"""
Gridscape backend — Python (Flask)

Serves the built frontend from ../frontend/dist and exposes
POST /api/generate which calls an OpenAI-compatible chat completions
endpoint and parses the markdown response into {text, prompts}.

Run:  python app.py          (after: pip install -r requirements.txt)
"""

import os
import re
import json
import requests
from pathlib import Path
from dotenv import load_dotenv
from flask import Flask, request, jsonify, send_from_directory
from collections import defaultdict, deque
import threading
import time

load_dotenv()

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

MAX_PROMPT_LENGTH = 2000

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


# ---------------------------------------------------------------------------
# Markdown parser  (mirrors providers/contract.ts:parseMarkdownContent)
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
# LLM provider  (OpenAI-compatible chat completions)
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


def call_chat_completions(base_url: str, api_key: str, model: str, prompt: str) -> dict:
    resp = requests.post(
        f"{base_url}/chat/completions",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        json={
            "model": model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": prompt},
            ],
        },
        timeout=60,
    )
    if not resp.ok:
        print(f"Chat completions error: {resp.status_code} {resp.text}")
        raise ProviderError(
            resp.status_code,
            "LLM provider error. Check API key and account credits.",
        )

    data = resp.json()
    content = ""
    choices = data.get("choices") or []
    if choices:
        message = choices[0].get("message") or {}
        content = message.get("content") or ""

    return parse_markdown_content(content)


def generate_text(prompt: str) -> dict:
    name = os.environ.get("AI_PROVIDER", "openrouter").lower()
    cfg = PROVIDERS.get(name)
    if not cfg:
        raise ValueError(f'Unknown AI_PROVIDER "{name}"')

    return call_chat_completions(
        base_url=cfg["base_url"],
        api_key=os.environ.get(cfg["api_key_env"], ""),
        model=os.environ.get(cfg["model_env"], cfg["default_model"]),
        prompt=prompt,
    )


class ProviderError(Exception):
    def __init__(self, status: int, message: str):
        self.status = status
        super().__init__(message)


# ---------------------------------------------------------------------------
# Simple in-memory rate limiter (20 req/min per IP)
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
# App
# ---------------------------------------------------------------------------
app = Flask(__name__, static_folder=None)

DIST_PATH = Path(__file__).resolve().parent.parent / "frontend" / "dist"

CSP_HEADER = (
    "default-src 'self'; "
    "script-src 'self'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src 'self' https://fonts.gstatic.com data:; "
    "img-src 'self' data:; "
    "connect-src 'self'"
)


@app.after_request
def set_csp(response):
    response.headers["Content-Security-Policy"] = CSP_HEADER
    return response


@app.post("/api/generate")
def api_generate():
    client_ip = request.remote_addr or "unknown"
    if rate_limited(client_ip):
        return jsonify({"error": "Too many requests. Please slow down."}), 429

    body = request.get_json(silent=True) or {}
    prompt = body.get("prompt")
    if not isinstance(prompt, str) or not prompt.strip():
        return jsonify({"error": "Prompt is required"}), 400
    if len(prompt) > MAX_PROMPT_LENGTH:
        return jsonify({"error": f"Prompt is too long (max {MAX_PROMPT_LENGTH} characters)."}), 400

    try:
        return jsonify(generate_text(prompt))
    except ProviderError as e:
        return jsonify({"error": "Generation failed. Please try again."}), e.status
    except Exception as exc:
        print(f"Generate error: {exc}")
        return jsonify({"error": "Failed to generate text content."}), 500


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
