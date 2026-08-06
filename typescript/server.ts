import express from "express";
import path from "path";
import { fileURLToPath } from "url";
import helmet from "helmet";
import rateLimit from "express-rate-limit";
import dotenv from "dotenv";
import { createProvider, ProviderError } from "./providers";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

dotenv.config();

const MAX_PROMPT_LENGTH = 2000;

async function startServer() {
  const app = express();
  const PORT = Number(process.env.PORT) || 3000;

  // Security headers (production only). In dev, Vite injects inline scripts
  // for HMR/Fast Refresh that a strict CSP would block, so helmet is skipped
  // on localhost. CSP allows the Google Fonts in index.css and inline styles.
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

  const provider = createProvider();

  // Cap costly LLM calls per IP to protect the provider budget.
  const generateLimiter = rateLimit({
    windowMs: 60 * 1000, // 1 minute
    limit: 20, // max 20 requests per minute per IP
    standardHeaders: "draft-7",
    legacyHeaders: false,
    message: { error: "Too many requests. Please slow down." },
  });

  app.post("/api/generate", generateLimiter, async (req, res) => {
    try {
      const { prompt } = req.body;
      if (typeof prompt !== "string" || !prompt.trim()) {
        return res.status(400).json({ error: "Prompt is required" });
      }
      if (prompt.length > MAX_PROMPT_LENGTH) {
        return res.status(400).json({
          error: `Prompt is too long (max ${MAX_PROMPT_LENGTH} characters).`,
        });
      }

      res.json(await provider.generateText(prompt));
    } catch (error) {
      console.error(error);
      if (error instanceof ProviderError) {
        // Send a generic message to the client; the details are logged above.
        res.status(error.status).json({ error: "Generation failed. Please try again." });
      } else {
        res.status(500).json({ error: "Failed to generate text content." });
      }
    }
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
