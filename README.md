# Gridscape

An infinite spatial canvas for exploring interconnected concepts with AI-generated text. Start with any topic and branch non-linearly: every generated node contains explanatory markdown with **clickable key terms** and **suggested follow-up prompts** — click either to spawn a child node and grow a knowledge tree you can pan, zoom, drag, and re-generate.

## Architecture

The frontend and backend are **fully decoupled**. The frontend is a React 19 SPA (thin renderer); each backend owns **all domain logic** — the node model, spatial layout (collision-aware child placement), versioning, tree structure, and deletion cascades — and maintains the canvas state in memory.

```
gridscape/
├── frontend/          Shared React SPA (thin renderer, Vite + Tailwind v4)
│   └── dist/          Built output — served by every backend
├── typescript/        Express backend (original reference impl)
├── python/            Flask backend
├── go/                net/http backend (zero external deps)
├── rust/              axum backend
├── cpp/               cpp-httplib + libcurl backend
└── c/                 raw sockets + libcurl backend
```

## API — Stateful Canvas Sessions

Every backend implements the **exact same contract**. The frontend creates a canvas session, then calls REST endpoints for every node operation. The backend returns complete `GridNodeData` objects:

```json
{
  "id": "node-...",
  "x": 800, "y": 200, "width": 400, "height": 400,
  "prompt": "event horizon",
  "text": "...markdown with [Term](Term) links...",
  "prompts": ["follow-up 1", "follow-up 2", "follow-up 3"],
  "links": [{ "label": "Term", "target": "Term" }],
  "status": "ready",
  "versionIndex": 1,
  "versions": [{ "prompt": "...", "text": "...", "prompts": ["..."], "links": [{ "label": "Term", "target": "Term" }] }],
  "parentId": "node-..."
}
```

| Method | Endpoint | Body | Returns |
|---|---|---|---|
| `POST` | `/api/canvas` | — | `{ canvasId }` |
| `POST` | `/api/canvas/:id/generate` | `{ prompt, parentId? }` | `{ node }` |
| `POST` | `/api/canvas/:id/nodes/:nid/regenerate` | — | `{ node }` |
| `DELETE` | `/api/canvas/:id/nodes/:nid` | — | `{ deletedIds: [...] }` |
| `PUT` | `/api/canvas/:id/nodes/:nid/version` | `{ versionIndex }` | `{ node }` |
| `PUT` | `/api/canvas/:id/nodes/:nid/measure` | `{ height }` | `{ ok: true }` |
| `PUT` | `/api/canvas/:id/nodes/:nid/position` | `{ x, y }` | `{ node }` |

- `generate` without `parentId` creates a root node at (0, 0); with `parentId` it places a collision-free child to the right of the parent.
- `regenerate` adds a new version to the node and switches to it.
- `delete` removes the node **and all its descendants** (cascade).
- `measure` records the rendered height so collision avoidance uses real dimensions.

- `links` contains ordered `{ label, target }` metadata for inline clickable terms, so clients do not need to parse markdown links to implement branching.
- `position` persists user-dragged node coordinates; pan and zoom remain client-side viewport state.
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
