# Gridscape — Go Backend

Stateful canvas backend: owns the node model, spatial layout, versioning,
tree structure, and deletion cascades. **Zero external dependencies** —
uses only the Go standard library.

## Prerequisites

- Go 1.22+
- The frontend built to `../frontend/dist` (run `npm install && npm run build` in `frontend/`)

## Run

```bash
cd go
go run .
```

The server starts on `http://localhost:3000`.

## Build

```bash
go build -o gridscape.exe .
./gridscape
```

## Configuration

Set environment variables before running (copy from `../typescript/.env.example`):

| Variable | Default | Purpose |
|---|---|---|
| `AI_PROVIDER` | `openrouter` | LLM provider: `openrouter` or `openai` |
| `OPENROUTER_API_KEY` | — | Required for the OpenRouter provider |
| `OPENROUTER_MODEL` | `openai/gpt-oss-20b` | Model id on OpenRouter |
| `OPENAI_API_KEY` | — | Required for the OpenAI provider |
| `OPENAI_MODEL` | `gpt-4o-mini` | Model id on OpenAI |
| `PORT` | `3000` | Server port |

## API

Stateful canvas sessions (same contract as every backend):

| Method | Endpoint | Body | Returns |
|---|---|---|---|
| `POST` | `/api/canvas` | — | `{ canvasId }` |
| `POST` | `/api/canvas/:id/generate` | `{ prompt, parentId? }` | `{ node }` |
| `POST` | `/api/canvas/:id/nodes/:nid/regenerate` | — | `{ node }` |
| `DELETE` | `/api/canvas/:id/nodes/:nid` | — | `{ deletedIds }` |
| `PUT` | `/api/canvas/:id/nodes/:nid/version` | `{ versionIndex }` | `{ node }` |
| `PUT` | `/api/canvas/:id/nodes/:nid/position` | `{ x, y }` | `{ node }` |
| `GET` | `/api/canvas/:id/nodes` | — | `{ nodes }` |

## Dev workflow

For frontend hot-reload during development, run this backend on `:3000`
and the Vite dev server (`npm run dev` in `frontend/`) on `:5173`.
The Vite config proxies `/api` calls to `:3000`.
