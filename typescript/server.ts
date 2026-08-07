import express from "express";
import path from "path";
import { fileURLToPath } from "url";
import helmet from "helmet";
import rateLimit from "express-rate-limit";
import dotenv from "dotenv";
import {
  createCanvas,
  getCanvas,
  generateNode,
  regenerateNode,
  deleteNode,
  setNodeVersion,
  setNodePosition,
  measureNode,
} from "./canvas";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

dotenv.config();

const MAX_PROMPT_LENGTH = 2000;

// Helper: validate canvas exists
function requireCanvas(req: express.Request, res: express.Response) {
  const canvas = getCanvas(req.params.id);
  if (!canvas) {
    res.status(404).json({ error: "Canvas not found" });
    return null;
  }
  return canvas;
}

// Helper: validate prompt
function validatePrompt(body: any, res: express.Response): string | null {
  const { prompt } = body;
  if (typeof prompt !== "string" || !prompt.trim()) {
    res.status(400).json({ error: "Prompt is required" });
    return null;
  }
  if (prompt.length > MAX_PROMPT_LENGTH) {
    res.status(400).json({ error: `Prompt is too long (max ${MAX_PROMPT_LENGTH} characters).` });
    return null;
  }
  return prompt;
}

async function startServer() {
  const app = express();
  const PORT = Number(process.env.PORT) || 3000;

  if (process.env.NODE_ENV === "production") {
    app.use(
      helmet({
        contentSecurityPolicy: {
          directives: {
            "default-src": ["'self'"],
            "script-src": ["'self'"],
            "style-src": ["'self'", "'unsafe-inline'", "https://fonts.googleapis.com"],
            "font-src": ["'self'", "https://fonts.gstatic.com", "data:"],
            "img-src": ["'self'", "data:"],
            "connect-src": ["'self'"],
          },
        },
      }),
    );
  }

  app.use(express.json({ limit: "1mb" }));

  // Rate-limit only endpoints that trigger LLM calls.
  const generateLimiter = rateLimit({
    windowMs: 60 * 1000,
    limit: 20,
    standardHeaders: "draft-7",
    legacyHeaders: false,
    message: { error: "Too many requests. Please slow down." },
  });

  // POST /api/canvas — create a new canvas session
  app.post("/api/canvas", (_req, res) => {
    const canvas = createCanvas();
    res.json({ canvasId: canvas.id });
  });

  // POST /api/canvas/:id/generate — create a root or child node
  app.post("/api/canvas/:id/generate", generateLimiter, async (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    const prompt = validatePrompt(req.body, res);
    if (!prompt) return;
    const { parentId } = req.body;
    try {
      const node = await generateNode(canvas, prompt, parentId);
      res.json({ node });
    } catch (error) {
      console.error(error);
      res.status(500).json({ error: "Failed to generate text content." });
    }
  });

  // POST /api/canvas/:id/nodes/:nid/regenerate — add a new version
  app.post("/api/canvas/:id/nodes/:nid/regenerate", generateLimiter, async (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    try {
      const node = await regenerateNode(canvas, req.params.nid);
      res.json({ node });
    } catch (error) {
      console.error(error);
      res.status(404).json({ error: "Node not found" });
    }
  });

  // DELETE /api/canvas/:id/nodes/:nid — delete node + descendants
  app.delete("/api/canvas/:id/nodes/:nid", (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    const deletedIds = deleteNode(canvas, req.params.nid);
    res.json({ deletedIds });
  });

  // PUT /api/canvas/:id/nodes/:nid/version — switch active version
  app.put("/api/canvas/:id/nodes/:nid/version", (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    const { versionIndex } = req.body;
    try {
      const node = setNodeVersion(canvas, req.params.nid, versionIndex);
      res.json({ node });
    } catch {
      res.status(404).json({ error: "Node not found" });
    }
  });

  // PUT /api/canvas/:id/nodes/:nid/position — persist user-adjusted position
  app.put("/api/canvas/:id/nodes/:nid/position", (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    const { x, y } = req.body;
    if (!Number.isFinite(x) || !Number.isFinite(y)) {
      res.status(400).json({ error: "Position requires numeric x and y" });
      return;
    }
    try {
      const node = setNodePosition(canvas, req.params.nid, x, y);
      res.json({ node });
    } catch {
      res.status(404).json({ error: "Node not found" });
    }
  });

  // PUT /api/canvas/:id/nodes/:nid/measure — update measured height
  app.put("/api/canvas/:id/nodes/:nid/measure", (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    measureNode(canvas, req.params.nid, req.body.height);
    res.json({ ok: true });
  });

  // GET /api/canvas/:id/nodes — get all nodes
  app.get("/api/canvas/:id/nodes", (req, res) => {
    const canvas = requireCanvas(req, res);
    if (!canvas) return;
    res.json({ nodes: canvas.nodes });
  });

  // Serve the built frontend (../frontend/dist). The frontend is a separate
  // React SPA; build it with `npm run build` in the frontend/ directory.
  const distPath = path.resolve(__dirname, "..", "frontend", "dist");
  app.use(express.static(distPath));
  app.get("*", (req, res) => {
    res.sendFile(path.join(distPath, "index.html"));
  });

  app.listen(PORT, "0.0.0.0", () => {
    console.log(`Server running on http://localhost:${PORT}`);
  });
}

startServer();
