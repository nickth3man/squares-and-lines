"""
Gridscape backend — Python (Flask) with stateful canvas management.

The backend owns the node model, layout, generated content, presentation
metadata, versioning, tree structure, and deletion cascades.
"""

import os
import re
import random
import time
import threading
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import Optional
from collections import defaultdict, deque

import requests
from flask import Flask, request, jsonify, send_from_directory
from dotenv import load_dotenv

load_dotenv()

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
LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")

CSP_HEADER = (
    "default-src 'self'; script-src 'self'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src 'self' https://fonts.gstatic.com data:; "
    "img-src 'self' data:; connect-src 'self'"
)


def _extract_links(text: str) -> list:
    return [
        {"label": match.group(1).strip(), "target": match.group(2).strip()}
        for match in LINK_RE.finditer(text)
    ]


def parse_markdown_content(raw: str) -> dict:
    trimmed = raw.strip()
    match = HEADING_RE.search(trimmed)
    if not match:
        text = trimmed
        return {"text": text, "prompts": [], "links": _extract_links(text)}
    text = trimmed[: match.start()].strip()
    section = trimmed[match.start() + len(match.group(0)) :]
    prompts = [match.group(1).strip() for match in BULLET_RE.finditer(section)][:3]
    return {"text": text, "prompts": prompts, "links": _extract_links(text)}


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


class ProviderError(Exception):
    def __init__(self, status, message):
        self.status = status
        super().__init__(message)


def call_chat_completions(base_url, api_key, model, prompt):
    response = requests.post(
        f"{base_url}/chat/completions",
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"},
        json={
            "model": model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": prompt},
            ],
        },
        timeout=60,
    )
    if not response.ok:
        raise ProviderError(response.status_code, "LLM provider error")
    data = response.json()
    choices = data.get("choices") or []
    content = (choices[0].get("message") or {}).get("content", "") if choices else ""
    return parse_markdown_content(content)


def generate_text(prompt):
    name = os.environ.get("AI_PROVIDER", "openrouter").lower()
    config = PROVIDERS.get(name)
    if not config:
        raise ValueError(f'Unknown AI_PROVIDER "{name}"')
    return call_chat_completions(
        config["base_url"],
        os.environ.get(config["api_key_env"], ""),
        os.environ.get(config["model_env"], config["default_model"]),
        prompt,
    )


@dataclass
class NodeVersion:
    prompt: str
    text: str
    prompts: list
    links: list


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
    links: list = field(default_factory=list)
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


def canvas_generate_node(canvas: Canvas, prompt: str, parent_id: str = None) -> GridNodeData:
    x, y = 0.0, 0.0
    if parent_id:
        parent = next((node for node in canvas.nodes if node.id == parent_id), None)
        if not parent:
            raise ValueError(f"Parent node {parent_id} not found")
        x, y = compute_child_position(parent, canvas.nodes)
    node = GridNodeData(id=_gen_node_id(), x=x, y=y, prompt=prompt, parentId=parent_id)
    canvas.nodes.append(node)
    try:
        result = generate_text(prompt)
        node.text = result["text"] or "No text"
        node.prompts = result["prompts"]
        node.links = result["links"]
        node.status = "ready"
        node.versions = [NodeVersion(prompt, node.text, node.prompts, node.links)]
    except Exception as error:
        print(f"Generate error: {error}")
        node.status = "error"
    return node


def canvas_regenerate_node(canvas: Canvas, node_id: str) -> GridNodeData:
    node = next((candidate for candidate in canvas.nodes if candidate.id == node_id), None)
    if not node:
        raise ValueError(f"Node {node_id} not found")
    node.status = "generating"
    try:
        result = generate_text(node.prompt)
        version = NodeVersion(node.prompt, result["text"] or "No text", result["prompts"], result["links"])
        node.versions.append(version)
        node.versionIndex = len(node.versions) - 1
        node.text = version.text
        node.prompts = version.prompts
        node.links = version.links
        node.status = "ready"
    except Exception as error:
        print(f"Regenerate error: {error}")
        node.status = "error"
    return node


def canvas_delete_node(canvas: Canvas, node_id: str) -> list:
    def get_descendants(current_id):
        children = [node.id for node in canvas.nodes if node.parentId == current_id]
        descendants = list(children)
        for child_id in children:
            descendants.extend(get_descendants(child_id))
        return descendants

    to_delete = {node_id, *get_descendants(node_id)}
    canvas.nodes = [node for node in canvas.nodes if node.id not in to_delete]
    return list(to_delete)


def canvas_set_version(canvas: Canvas, node_id: str, version_index: int) -> GridNodeData:
    node = next((candidate for candidate in canvas.nodes if candidate.id == node_id), None)
    if not node:
        raise ValueError(f"Node {node_id} not found")
    node.versionIndex = version_index
    if version_index < len(node.versions):
        version = node.versions[version_index]
        node.text = version.text
        node.prompts = version.prompts
        node.links = version.links
    return node


def canvas_set_position(canvas: Canvas, node_id: str, x: float, y: float) -> GridNodeData:
    node = next((candidate for candidate in canvas.nodes if candidate.id == node_id), None)
    if not node:
        raise ValueError(f"Node {node_id} not found")
    node.x = x
    node.y = y
    return node


def canvas_measure_node(canvas: Canvas, node_id: str, height: float):
    node = next((candidate for candidate in canvas.nodes if candidate.id == node_id), None)
    if node:
        node.height = height


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
    except ProviderError as error:
        return jsonify({"error": "Generation failed. Please try again."}), error.status
    except Exception as error:
        print(f"Generate error: {error}")
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


@app.put("/api/canvas/<cid>/nodes/<nid>/position")
def api_position(cid, nid):
    canvas = get_canvas(cid)
    if not canvas:
        return jsonify({"error": "Canvas not found"}), 404
    body = request.get_json(silent=True) or {}
    x, y = body.get("x"), body.get("y")
    if not isinstance(x, (int, float)) or not isinstance(y, (int, float)):
        return jsonify({"error": "Position requires numeric x and y"}), 400
    try:
        node = canvas_set_position(canvas, nid, x, y)
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
    return jsonify({"nodes": [node.to_dict() for node in canvas.nodes]})


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
