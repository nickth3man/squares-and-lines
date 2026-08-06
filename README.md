# Gridscape

An infinite spatial canvas for exploring interconnected concepts with AI-generated text. Start with any topic and branch non-linearly: every generated node contains explanatory markdown with **clickable key terms** and **suggested follow-up prompts** — click either to spawn a child node and grow a knowledge tree you can pan, zoom, drag, and re-generate.

## Features

- **Infinite canvas** — pan (drag background), zoom (Ctrl+wheel / toolbar buttons), draggable nodes, minimap with live viewport, collision-aware child placement.
- **AI text nodes** — each node is a markdown card where 2–4 key concepts are `[Term](Term)` links; clicking one branches off into a new node.
- **Follow-up prompts** — every node suggests 3 next topics; click to expand.
- **Versioning** — Regenerate appends a new version per node (stacked-card UI, toolbar switcher).
- **Provider-agnostic LLM layer** — no SDK, no vendor lock-in; swap models/providers via env vars.

## Stack

- **Client:** React 19, Vite 6, Tailwind CSS v4, motion, react-markdown, lucide-react
- **Server:** Express (`server.ts`) — API + static hosting, single process on port 3000

## Getting Started

**Prerequisites:** Node.js 18+ (for the built-in `fetch` used by the LLM layer)

1. `npm install`
2. Create `.env.local` (see [.env.example](.env.example)) with an `OPENROUTER_API_KEY` from [openrouter.ai/keys](https://openrouter.ai/keys) — the account needs credits.
3. `npm run dev` → http://localhost:3000

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `AI_PROVIDER` | `openrouter` | LLM provider: `openrouter` or `openai` |
| `OPENROUTER_API_KEY` | — | Required for the OpenRouter provider |
| `OPENROUTER_MODEL` | `openai/gpt-oss-20b` | Model id on OpenRouter |
| `OPENAI_API_KEY` | — | Required for the OpenAI provider |
| `OPENAI_MODEL` | `gpt-4o-mini` | Model id on OpenAI |

## Architecture

```
server.ts            Express route: POST /api/generate → { text, prompts }
providers/
  contract.ts        TextProvider interface + shared chat-completions caller + markdown parser
  openrouter.ts      OpenRouter provider (default)
  openai.ts          OpenAI provider
  index.ts           createProvider() factory (selects via AI_PROVIDER)
src/
  App.tsx            canvas state, pan/zoom, node spawning/versioning
  components/        NodeCard, ConnectingLines, Minimap
```

**The LLM contract is JSON-free by design.** The model returns plain markdown:

- `[Term](Term)` links inline in the text — the clickable branching mechanism;
- a trailing `## Explore further` section with 3 bullet-point markdown links — the follow-up prompts.

`parseMarkdownContent` splits the two deterministically. This works with any model — including small/free models that reject structured outputs — and degrades gracefully (missing section → text-only node) instead of failing on a malformed JSON envelope.

**Adding a provider:** implement `TextProvider` (`generateText(prompt) → { text, prompts }`) in `providers/`, register it in the factory, document its env vars.

## Scripts

| Script | What it does |
|---|---|
| `npm run dev` | Vite dev server + API via tsx |
| `npm run build` | Vite client build + esbuild server bundle → `dist/` |
| `npm start` | Run the production bundle |
| `npm run lint` | TypeScript check (`tsc --noEmit`) |

Image generation is intentionally not part of this app.
