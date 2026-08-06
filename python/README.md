# Gridscape — Python Backend

Serves the Gridscape frontend and proxies LLM text generation via an
OpenAI-compatible chat completions API.

## Prerequisites

- Python 3.10+
- The frontend built to `../frontend/dist` (run `npm install && npm run build` in `frontend/`)

## Setup

```bash
cd python
python -m venv .venv
.venv\Scripts\activate        # Windows
# source .venv/bin/activate   # macOS/Linux
pip install -r requirements.txt
```

## Run

```bash
# Copy and fill in environment variables
copy ..\typescript\.env.example .env   # Windows
# cp ../typescript/.env.example .env    # macOS/Linux

python app.py
```

The server starts on `http://localhost:3000`.

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `AI_PROVIDER` | `openrouter` | LLM provider: `openrouter` or `openai` |
| `OPENROUTER_API_KEY` | — | Required for the OpenRouter provider |
| `OPENROUTER_MODEL` | `openai/gpt-oss-20b` | Model id on OpenRouter |
| `OPENAI_API_KEY` | — | Required for the OpenAI provider |
| `OPENAI_MODEL` | `gpt-4o-mini` | Model id on OpenAI |
| `PORT` | `3000` | Server port |

## Dev workflow

For frontend hot-reload during development, run this backend on `:3000`
and the Vite dev server (`npm run dev` in `frontend/`) on `:5173`.
The Vite config proxies `/api` calls to `:3000`.
