// Gridscape backend — Go (net/http, zero external dependencies)
//
// Serves the built frontend from ../frontend/dist and exposes
// POST /api/generate which calls an OpenAI-compatible chat completions
// endpoint and parses the markdown response into {text, prompts}.
//
// Run:  go run .     (from this directory)

package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

const cspHeader = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com data:; img-src 'self' data:; connect-src 'self'"

// loadDotEnv reads a .env file (if present) and sets env vars that aren't
// already defined. Keeps Go at zero external dependencies.
func loadDotEnv() {
	data, err := os.ReadFile(".env")
	if err != nil {
		return
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) == 2 {
			key := strings.TrimSpace(parts[0])
			val := strings.Trim(strings.TrimSpace(parts[1]), `"'`)
			if os.Getenv(key) == "" {
				os.Setenv(key, val)
			}
		}
	}
}
// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

const maxPromptLength = 2000

const systemPrompt = "You are an infinite spatial-knowledge-engine generator. " +
	"Respond with markdown text about the user's topic: brief but impactful " +
	"explanatory text. CRITICAL: wrap 2 to 4 key concepts or interesting terms " +
	"as clickable markdown links in the exact format [Term](Term) with no spaces, " +
	"for example 'Machine learning relies on [Supervised Learning](Supervised Learning) " +
	"and [Neural Networks](Neural Networks).' Do not use bold or italics for those terms. " +
	"End your response with a section headed exactly '## Explore further' containing " +
	"exactly 3 bullet-point markdown links, each bullet like " +
	"'- [A follow-up question](A follow-up question)'. " +
	"Output only the markdown; no JSON, no code fences."

var (
	headingRE = regexp.MustCompile(`(?im)^##\s+explore further\s*$`)
	bulletRE  = regexp.MustCompile(`(?m)^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$`)
)

// ---------------------------------------------------------------------------
// Markdown parser  (mirrors providers/contract.ts:parseMarkdownContent)
// ---------------------------------------------------------------------------

type genResult struct {
	Text    string   `json:"text"`
	Prompts []string `json:"prompts"`
}

func parseMarkdownContent(raw string) genResult {
	trimmed := strings.TrimSpace(raw)
	loc := headingRE.FindStringIndex(trimmed)
	if loc == nil {
		return genResult{Text: trimmed, Prompts: []string{}}
	}

	text := strings.TrimSpace(trimmed[:loc[0]])
	section := trimmed[loc[1]:]
	matches := bulletRE.FindAllStringSubmatch(section, -1)

	prompts := []string{}
	for _, m := range matches {
		prompts = append(prompts, strings.TrimSpace(m[1]))
		if len(prompts) >= 3 {
			break
		}
	}
	return genResult{Text: text, Prompts: prompts}
}

// ---------------------------------------------------------------------------
// LLM provider  (OpenAI-compatible chat completions)
// ---------------------------------------------------------------------------

type providerConfig struct {
	baseURL      string
	apiKeyEnv    string
	modelEnv     string
	defaultModel string
}

var providers = map[string]providerConfig{
	"openrouter": {
		baseURL:      "https://openrouter.ai/api/v1",
		apiKeyEnv:    "OPENROUTER_API_KEY",
		modelEnv:     "OPENROUTER_MODEL",
		defaultModel: "openai/gpt-oss-20b",
	},
	"openai": {
		baseURL:      "https://api.openai.com/v1",
		apiKeyEnv:    "OPENAI_API_KEY",
		modelEnv:     "OPENAI_MODEL",
		defaultModel: "gpt-4o-mini",
	},
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func generateText(prompt string) (genResult, int, error) {
	name := strings.ToLower(envOr("AI_PROVIDER", "openrouter"))
	cfg, ok := providers[name]
	if !ok {
		return genResult{}, 500, fmt.Errorf("unknown AI_PROVIDER %q", name)
	}

	body := map[string]any{
		"model": envOr(cfg.modelEnv, cfg.defaultModel),
		"messages": []map[string]string{
			{"role": "system", "content": systemPrompt},
			{"role": "user", "content": prompt},
		},
	}
	bodyBytes, _ := json.Marshal(body)

	req, err := http.NewRequest("POST", cfg.baseURL+"/chat/completions", strings.NewReader(string(bodyBytes)))
	if err != nil {
		return genResult{}, 500, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+os.Getenv(cfg.apiKeyEnv))

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return genResult{}, 502, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		errText, _ := io.ReadAll(resp.Body)
		log.Printf("Chat completions error: %d %s", resp.StatusCode, string(errText))
		return genResult{}, resp.StatusCode, fmt.Errorf("LLM provider error (%d)", resp.StatusCode)
	}

	var data struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return genResult{}, 502, err
	}

	content := ""
	if len(data.Choices) > 0 {
		content = data.Choices[0].Message.Content
	}
	return parseMarkdownContent(content), 0, nil
}

// ---------------------------------------------------------------------------
// Simple in-memory rate limiter (20 req/min per IP)
// ---------------------------------------------------------------------------

type ipEntry struct {
	timestamps []time.Time
}

var (
	rateMu    sync.Mutex
	rateLog   = map[string]*ipEntry{}
)

func rateLimited(ip string) bool {
	rateMu.Lock()
	defer rateMu.Unlock()
	now := time.Now()
	entry, ok := rateLog[ip]
	if !ok {
		entry = &ipEntry{}
		rateLog[ip] = entry
	}
	cutoff := now.Add(-time.Minute)
	filtered := entry.timestamps[:0]
	for _, t := range entry.timestamps {
		if t.After(cutoff) {
			filtered = append(filtered, t)
		}
	}
	if len(filtered) >= 20 {
		entry.timestamps = filtered
		return true
	}
	entry.timestamps = append(filtered, now)
	return false
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

func handleGenerate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"Method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	ip, _, _ := strings.Cut(r.RemoteAddr, ":")
	if rateLimited(ip) {
		writeJSON(w, 429, map[string]string{"error": "Too many requests. Please slow down."})
		return
	}

	var body struct {
		Prompt string `json:"prompt"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON body"})
		return
	}

	if strings.TrimSpace(body.Prompt) == "" {
		writeJSON(w, 400, map[string]string{"error": "Prompt is required"})
		return
	}
	if len(body.Prompt) > maxPromptLength {
		writeJSON(w, 400, map[string]string{"error": fmt.Sprintf("Prompt is too long (max %d characters).", maxPromptLength)})
		return
	}

	result, status, err := generateText(body.Prompt)
	if err != nil {
		log.Printf("Generate error: %v", err)
		if status >= 400 {
			writeJSON(w, status, map[string]string{"error": "Generation failed. Please try again."})
		} else {
			writeJSON(w, 500, map[string]string{"error": "Failed to generate text content."})
		}
		return
	}
	writeJSON(w, 200, result)
}

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data)
}

// ---------------------------------------------------------------------------
// Static file serving + SPA fallback
// ---------------------------------------------------------------------------

func staticHandler(distPath string) http.HandlerFunc {
	fileServer := http.FileServer(http.Dir(distPath))
	return func(w http.ResponseWriter, r *http.Request) {
		// API route — let it 404 naturally
		if strings.HasPrefix(r.URL.Path, "/api/") {
			http.NotFound(w, r)
			return
		}
		fullPath := filepath.Join(distPath, filepath.Clean(r.URL.Path))
		info, err := os.Stat(fullPath)
		if err == nil && !info.IsDir() {
			fileServer.ServeHTTP(w, r)
			return
		}
		// SPA fallback
		http.ServeFile(w, r, filepath.Join(distPath, "index.html"))
	}
}

// ---------------------------------------------------------------------------

func cspMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", cspHeader)
		next.ServeHTTP(w, r)
	})
}

func main() {
	loadDotEnv()
	port := envOr("PORT", "3000")

	// Resolve dist path: try CWD first (covers `go run .`), then executable.
	distPath, _ := filepath.Abs(filepath.Join("..", "frontend", "dist"))
	if _, err := os.Stat(distPath); os.IsNotExist(err) {
		exe, _ := os.Executable()
		distPath = filepath.Join(filepath.Dir(exe), "..", "frontend", "dist")
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/api/generate", handleGenerate)
	mux.HandleFunc("/", staticHandler(distPath))

	addr := ":" + port
	log.Printf("Server running on http://localhost:%s", port)
	if err := http.ListenAndServe(addr, cspMiddleware(mux)); err != nil {
		log.Fatal(err)
	}
}
