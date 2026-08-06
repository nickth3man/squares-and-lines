"""
Gridscape backend — Python (Flask) with stateful canvas management.

The backend owns all domain logic: node model, spatial layout (collision-aware
child placement), versioning, tree structure, and deletion cascades.
The frontend is a thin renderer that calls these REST endpoints.

Run:  python app.py   (after: pip install -r requirements.txt)
"""

import os
import re
import json
import random
import time
import threading
import uuid
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import Optional

import requests
from flask import Flask, request, jsonify, send_from_directory
from collections import defaultdict, deque

from dotenv import load_dotenv

load_dotenv()

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

MAX_PROMPT_LENGTH = 2000
NODE_WIDTH = 400

SYSTEM_PROMPT = (
    "You are an infinite spatial-knowledge-engine generator. "
    "Respond with markdown text about the user's topic: brief but impactful "
    "explanatory text. CRITICAL: wrap 2 to 4 key concepts or interesting terms "
    "as clickable markdown links in the exact format [Term](Term) with no spaces, "
    "for example 'Machine learning relies on [Supervised Learning](Supervised Learning) "
    "and [Neural Networks](Neural Networks).' Do not use bold or italics for those terms. "
    "End your response with a section headed exactly '## Explore further' containing "
    "exactly 3 bullet-point markdown links, each bullet like "
    "'- [A follow-up question](A follow-up question)'. "
    "Output only the markdown; no JSON, no code fences."
)

HEADING_RE = re.compile(r"^##\s+explore further\s*$", re.IGNORECASE | re.MULTILINE)
BULLET_RE = re.compile(r"^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$", re.MULTILINE)

CSP_HEADER = (
    "default-src 'self'; script-src 'self'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src 'self' https://fonts.gstatic.com data:; "
    "img-src 'self' data:; connect-src 'self'"
)


# ---------------------------------------------------------------------------
# Markdown parser
# ---------------------------------------------------------------------------

def parse_markdown_content(raw: str) -> dict:
    trimmed = raw.strip()
    match = HEADING_RE.search(trimmed)
    if not match:
        return {"text": trimmed, "prompts": []}
    text = trimmed[: match.start()].strip()
    section = trimmed[match.start() + len(match.group(0)) :]
    prompts = [m.group(1).strip() for m in BULLET_RE.finditer(section)][:3]
    return {"text": text, "prompts": prompts}


# ---------------------------------------------------------------------------
# LLM provider
# ---------------------------------------------------------------------------

PROVIDERS = {
    "openrouter": {
        "base_url": "https://openrouter.ai/api/v1",
        "api_key_env": "OPENROUTER_API_KEY",
        "model_env": "OPENROUTER_MODEL",
        "default_model": "openai/gpt-oss-20b",
    },
    "openai": {
        "base_url": "https://api.openai.com/v1",
        "api_key_env": "OPENAI_API_KEY",
        "model_env": "OPENAI_MODEL",
        "default_model": "gpt-4o-mini",
    },
}


def call_chat_completions(base_url, api_key, model, prompt):
    resp = requests.post(
        f"{base_url}/chat/completions",
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"},
        json={"model": model, "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ]},
        timeout=60,
    )
    if not resp.ok:
        print(f"Chat completions error: {resp.status_code} {resp.text}")
        raise ProviderError(resp.status_code, "LLM provider error")
    data = resp.json()
    content = ""
    choices = data.get("choices") or []
    if choices:
        message = choices[0].get("message") or {}
        content = message.get("content") or ""
    return parse_markdown_content(content)


def generate_text(prompt):
    name = os.environ.get("AI_PROVIDER", "openrouter").lower()
    cfg = PROVIDERS.get(name)
    if not cfg:
        raise ValueError(f'Unknown AI_PROVIDER "{name}"')
    return call_chat_completions(
        cfg["base_url"],
        os.environ.get(cfg["api_key_env"], ""),
        os.environ.get(cfg["model_env"], cfg["default_model"]),
        prompt,
    )


class ProviderError(Exception):
    def __init__(self, status, message):
        self.status = status
        super().__init__(message)


# ---------------------------------------------------------------------------
# Canvas domain model (backend-owned)
# ---------------------------------------------------------------------------

@dataclass
class NodeVersion:
    prompt: str
    text: str
    prompts: list


@dataclass
class GridNodeData:
    id: str
    x: float
    y: float
    width: int = NODE_WIDTH
    height: Optional[float] = None
    prompt: str = ""
    text: str = ""
    prompts: list = field(default_factory=list)
    status: str = "generating"
    versionIndex: int = 0
    versions: list = field(default_factory=list)
    parentId: Optional[str] = None

    def to_dict(self):
        return asdict(self)


@dataclass
class Canvas:
    id: str
    nodes: list = field(default_factory=list)


# In-memory canvas store
_canvases: dict[str, Canvas] = {}
_canvas_lock = threading.Lock()


def create_canvas() -> Canvas:
    cid = f"canvas-{int(time.time() * 1000)}-{random.randint(0, 99999)}"
    canvas = Canvas(id=cid)
    _canvases[cid] = canvas
    return canvas


def get_canvas(cid: str) -> Optional[Canvas]:
    return _canvases.get(cid)


def _gen_node_id() -> str:
    return f"node-{int(time.time() * 1000)}-{random.randint(0, 999)}"


# ---------------------------------------------------------------------------
# Spatial layout — collision-aware child placement
# (Exact replication of App.tsx handleExpand algorithm)
# ---------------------------------------------------------------------------

def compute_child_position(parent: GridNodeData, nodes: list) -> tuple:
    new_x = parent.x + parent.width + 400
    initial_offset = 200 if random.random() > 0.5 else -200
    new_y = parent.y + initial_offset
    card_height = parent.height or 400

    occupied = True
    offset_mult = 1
    direction = 1 if random.random() > 0.5 else -1

    while occupied:
        occupied = any(
            abs(n.x + NODE_WIDTH / 2 - (new_x + NODE_WIDTH / 2)) < NODE_WIDTH
            and abs((n.y + (n.height or 400) / 2) - (new_y + card_height / 2))
            < ((n.height or 400) + card_height) / 2
            for n in nodes
        )
        if occupied:
            new_y = parent.y + initial_offset + card_height * offset_mult * direction
            direction *= -1
            if direction == 1:
                offset_mult += 1

    return new_x, new_y


# ---------------------------------------------------------------------------
# Node operations
# ---------------------------------------------------------------------------

def canvas_generate_node(canvas: Canvas, prompt: str, parent_id: str = None) -> GridNodeData:
    x, y = 0.0, 0.0
    if parent_id:
        parent = next((n for n in canvas.nodes if n.id == parent_id), None)
        if not parent:
            raise ValueError(f"Parent node {parent_id} not found")
        x, y = compute_child_position(parent, canvas.nodes)

    node = GridNodeData(id=_gen_node_id(), x=x, y=y, prompt=prompt, parentId=parent_id)
    canvas.nodes.append(node)

    try:
        result = generate_text(prompt)
        node.text = result["text"] or "No text"
        node.prompts = result["prompts"]
        node.status = "ready"
        node.versions = [NodeVersion(prompt=prompt, text=node.text, prompts=node.prompts)]
    except Exception as e:
        print(f"Generate error: {e}")
        node.status = "error"

    return node


def canvas_regenerate_node(canvas: Canvas, node_id: str) -> GridNodeData:
    node = next((n for n in canvas.nodes if n.id == node_id), None)
    if not node:
        raise ValueError(f"Node {node_id} not found")

    node.status = "generating"
    try:
        result = generate_text(node.prompt)
        nv = NodeVersion(prompt=node.prompt, text=result["text"] or "No text", prompts=result["prompts"])
        node.versions.append(nv)
        node.versionIndex = len(node.versions) - 1
        node.text = nv.text
        node.prompts = nv.prompts
        node.status = "ready"
    except Exception as e:
        print(f"Regenerate error: {e}")
        node.status = "error"
    return node


def canvas_delete_node(canvas: Canvas, node_id: str) -> list:
    def get_descendants(nid):
        children = [n.id for n in canvas.nodes if n.parentId == nid]
        desc = list(children)
        for cid in children:
            desc.extend(get_descendants(cid))
        return desc

    to_delete = {node_id, *get_descendants(node_id)}
    canvas.nodes = [n for n in canvas.nodes if n.id not in to_delete]
    return list(to_delete)


def canvas_set_version(canvas: Canvas, node_id: str, version_index: int) -> GridNodeData:
    node = next((n for n in canvas.nodes if n.id == node_id), None)
    if not node:
        raise ValueError(f"Node {node_id} not found")
    node.versionIndex = version_index
    if version_index < len(node.versions):
        v = node.versions[version_index]
        node.text = v.text
        node.prompts = v.prompts
    return node


def canvas_measure_node(canvas: Canvas, node_id: str, height: float):
    node = next((n for n in canvas.nodes if n.id == node_id), None)
    if node:
        node.height = height


# ---------------------------------------------------------------------------
# Rate limiter (20 req/min per IP)
# ---------------------------------------------------------------------------

_rate_log: dict[str, deque] = defaultdict(deque)
_rate_lock = threading.Lock()


def rate_limited(ip: str) -> bool:
    now = time.time()
    with _rate_lock:
        log = _rate_log[ip]
        while log and now - log[0] > 60:
            log.popleft()
        if len(log) >= 20:
            return True
        log.append(now)
    return False


# ---------------------------------------------------------------------------
# Flask app
# ---------------------------------------------------------------------------

app = Flask(__name__, static_folder=None)
DIST_PATH = Path(__file__).resolve().parent.parent / "frontend" / "dist"


@app.after_request
def set_csp(response):
    response.headers["Content-Security-Policy"] = CSP_HEADER
    return response


@app.post("/api/canvas")
def api_create_canvas():
    canvas = create_canvas()
    return jsonify({"canvasId": canvas.id})


@app.post("/api/canvas/<cid>/generate")
def api_generate(cid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    if rate_limited(request.remote_addr or "unknown"):
        return jsonify({"error": "Too many requests. Please slow down."}), 429
    body = request.get_json(silent=True) or {}
    prompt = body.get("prompt")
    if not isinstance(prompt, str) or not prompt.strip():
        return jsonify({"error": "Prompt is required"}), 400
    if len(prompt) > MAX_PROMPT_LENGTH:
        return jsonify({"error": f"Prompt is too long (max {MAX_PROMPT_LENGTH} characters)."}), 400
    try:
        node = canvas_generate_node(canvas, prompt, body.get("parentId"))
        return jsonify({"node": node.to_dict()})
    except ProviderError as e:
        return jsonify({"error": "Generation failed. Please try again."}), e.status
    except Exception as e:
        print(f"Generate error: {e}")
        return jsonify({"error": "Failed to generate text content."}), 500


@app.post("/api/canvas/<cid>/nodes/<nid>/regenerate")
def api_regenerate(cid, nid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    if rate_limited(request.remote_addr or "unknown"):
        return jsonify({"error": "Too many requests. Please slow down."}), 429
    try:
        node = canvas_regenerate_node(canvas, nid)
        return jsonify({"node": node.to_dict()})
    except ValueError:
        return jsonify({"error": "Node not found"}), 404


@app.delete("/api/canvas/<cid>/nodes/<nid>")
def api_delete(cid, nid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    deleted = canvas_delete_node(canvas, nid)
    return jsonify({"deletedIds": deleted})


@app.put("/api/canvas/<cid>/nodes/<nid>/version")
def api_version(cid, nid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    body = request.get_json(silent=True) or {}
    try:
        node = canvas_set_version(canvas, nid, body.get("versionIndex", 0))
        return jsonify({"node": node.to_dict()})
    except ValueError:
        return jsonify({"error": "Node not found"}), 404


@app.put("/api/canvas/<cid>/nodes/<nid>/measure")
def api_measure(cid, nid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    body = request.get_json(silent=True) or {}
    canvas_measure_node(canvas, nid, body.get("height", 0))
    return jsonify({"ok": True})


@app.get("/api/canvas/<cid>/nodes")
def api_get_nodes(cid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    return jsonify({"nodes": [n.to_dict() for n in canvas.nodes]})


# Static file serving + SPA fallback
@app.get("/")
def index():
    return send_from_directory(DIST_PATH, "index.html")


@app.get("/<path:path>")
def static_files(path):
    full = DIST_PATH / path
    if full.is_file():
        return send_from_directory(DIST_PATH, path)
    return send_from_directory(DIST_PATH, "index.html")


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "3000"))
    print(f"Server running on http://localhost:{port}")
    app.run(host="0.0.0.0", port=port)
