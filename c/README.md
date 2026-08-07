# Gridscape — C Backend

Stateful canvas backend: owns the node model, spatial layout, versioning,
tree structure, and deletion cascades. Uses **raw sockets** (no HTTP
framework dependency) + **libcurl** for outbound HTTPS.

## Prerequisites

- A C11 compiler (GCC, Clang, or MSVC)
- libcurl (with HTTPS/TLS support)
- The frontend built to `../frontend/dist` (run `npm install && npm run build` in `frontend/`)

### Installing libcurl

- **Windows:** Install via [vcpkg](https://vcpkg.io): `vcpkg install curl`
- **macOS:** `brew install curl`
- **Linux:** `apt install libcurl4-openssl-dev` or equivalent

## Build & Run

### Linux / macOS

```bash
cd c
make
./gridscape
```

### Windows (MSVC)

```cmd
cd c
cl /O2 /std:c11 main.c /Fe:gridscape.exe /I"C:\vcpkg\installed\x64-windows\include" /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" libcurl.lib ws2_32.lib
copy "C:\vcpkg\installed\x64-windows\bin\libcurl.dll" . >nul
copy "C:\vcpkg\installed\x64-windows\bin\z.dll" . >nul
gridscape.exe
```

(Adjust the include/lib paths to match your libcurl installation.)

The server starts on `http://localhost:3000`.

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
