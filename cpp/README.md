# Gridscape — C++ Backend

Serves the Gridscape frontend and proxies LLM text generation via an
OpenAI-compatible chat completions API. Built with **cpp-httplib** (server)
and **libcurl** (outbound HTTPS).

## Prerequisites

- C++20 compiler (MSVC, GCC, or Clang)
- CMake 3.20+
- libcurl (with HTTPS/TLS support)
- The frontend built to `../frontend/dist` (run `npm install && npm run build` in `frontend/`)

### Installing libcurl

- **Windows (vcpkg):** `vcpkg install curl` then pass `-DCMAKE_TOOLCHAIN_PATH=.../vcpkg.cmake`
- **macOS:** `brew install curl`
- **Linux:** `apt install libcurl4-openssl-dev` or equivalent

## Build & Run

```bash
cd cpp
cmake -B build
cmake --build build --config Release
./build/gridscape        # Windows: build\Release\gridscape.exe
```

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

## Dev workflow

For frontend hot-reload during development, run this backend on `:3000`
and the Vite dev server (`npm run dev` in `frontend/`) on `:5173`.
The Vite config proxies `/api` calls to `:3000`.
