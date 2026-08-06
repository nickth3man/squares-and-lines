# Gridscape

An infinite spatial canvas for exploring interconnected concepts with AI-generated text. Start with any topic and branch non-linearly: every generated node contains explanatory markdown with **clickable key terms** and **suggested follow-up prompts** — click either to spawn a child node and grow a knowledge tree you can pan, zoom, drag, and re-generate.

## Architecture

The frontend and backend are **fully decoupled**. The frontend is a React 19 SPA (shared across all backends); the backend serves static files + a single API endpoint.

```
POST /api/generate   { "prompt": "string" }   →   { "text": "string", "prompts": ["string", ...] }
```

```
gridscape/
├── frontend/          Shared React SPA (Vite + Tailwind v4)
│   └── dist/          Built output — served by every backend
├── typescript/        Express backend (original reference impl)
├── python/            Flask backend
├── go/                net/http backend (zero external deps)
├── rust/              axum backend
├── cpp/               cpp-httplib + libcurl backend
└── c/                 raw sockets + libcurl backend
```

Each backend implements the **exact same contract**:

1. Serve static files from `../frontend/dist` with SPA fallback to `index.html`.
2. `POST /api/generate` — validate the prompt, call an OpenAI-compatible chat completions endpoint, parse the markdown response into `{ text, prompts }`.
3. Rate limiting (20 requests/min/IP) to protect the LLM budget.
4. Same env vars across all backends.

The LLM contract is **JSON-free by design** — the model returns plain markdown with `[Term](Term)` links and a trailing `## Explore further` section. `parseMarkdownContent` splits these deterministically. Works with any model, including small/free ones that reject structured outputs.

## Quick Start

### 1. Build the frontend (once, shared by all backends)

```bash
cd frontend
npm install
npm run build      # → frontend/dist/
```

### 2. Pick any backend

Each backend folder has its own README with setup instructions. Example with Go (zero external deps):

```bash
cd go
export OPENROUTER_API_KEY="your-key"
go run .
# → http://localhost:3000
```

### 3. Dev workflow (frontend hot-reload)

Run any backend on `:3000`, then:

```bash
cd frontend
npm run dev         # → http://localhost:5173 (proxies /api → :3000)
```

## Configuration

All backends read the same environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `AI_PROVIDER` | `openrouter` | LLM provider: `openrouter` or `openai` |
| `OPENROUTER_API_KEY` | — | Required for the OpenRouter provider |
| `OPENROUTER_MODEL` | `openai/gpt-oss-20b` | Model id on OpenRouter |
| `OPENAI_API_KEY` | — | Required for the OpenAI provider |
| `OPENAI_MODEL` | `gpt-4o-mini` | Model id on OpenAI |
| `PORT` | `3000` | Server port |

Copy `.env.example` into the backend folder you're using.

## Adding a Provider

Implement the provider in each backend's code: call `{baseUrl}/chat/completions` with `system` + `user` messages, extract `choices[0].message.content`, then run it through `parseMarkdownContent`.

## Stack

- **Frontend:** React 19, Vite 6, Tailwind CSS v4, motion, react-markdown, lucide-react
- **Backends:** TypeScript (Express), Python (Flask), Go (net/http), Rust (axum), C++ (cpp-httplib), C (raw sockets)
