// Gridscape backend — Go (net/http) with stateful canvas management.
//
// The backend owns all domain logic: node model, spatial layout, versioning,
// tree structure, and deletion cascades. Zero external dependencies.

package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math/rand/v2"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const maxPromptLength = 2000
const nodeWidth = 400.0

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

const cspHeader = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com data:; img-src 'self' data:; connect-src 'self'"

var (
	headingRE = regexp.MustCompile(`(?im)^##\s+explore further\s*$`)
	bulletRE  = regexp.MustCompile(`(?m)^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$`)
)

// ---------------------------------------------------------------------------
// Markdown parser
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
// LLM provider
// ---------------------------------------------------------------------------

type providerConfig struct {
	baseURL      string
	apiKeyEnv    string
	modelEnv     string
	defaultModel string
}

var providers = map[string]providerConfig{
	"openrouter": {"https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "OPENROUTER_MODEL", "openai/gpt-oss-20b"},
	"openai":     {"https://api.openai.com/v1", "OPENAI_API_KEY", "OPENAI_MODEL", "gpt-4o-mini"},
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func callLLM(prompt string) (genResult, error) {
	name := strings.ToLower(envOr("AI_PROVIDER", "openrouter"))
	cfg, ok := providers[name]
	if !ok {
		return genResult{}, fmt.Errorf("unknown AI_PROVIDER %q", name)
	}
	body, _ := json.Marshal(map[string]any{
		"model": envOr(cfg.modelEnv, cfg.defaultModel),
		"messages": []map[string]string{
			{"role": "system", "content": systemPrompt},
			{"role": "user", "content": prompt},
		},
	})
	req, _ := http.NewRequest("POST", cfg.baseURL+"/chat/completions", strings.NewReader(string(body)))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+os.Getenv(cfg.apiKeyEnv))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return genResult{}, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		errText, _ := io.ReadAll(resp.Body)
		log.Printf("Chat completions error: %d %s", resp.StatusCode, string(errText))
		return genResult{}, fmt.Errorf("LLM provider error (%d)", resp.StatusCode)
	}
	var data struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	json.NewDecoder(resp.Body).Decode(&data)
	content := ""
	if len(data.Choices) > 0 {
		content = data.Choices[0].Message.Content
	}
	return parseMarkdownContent(content), nil
}

// ---------------------------------------------------------------------------
// Canvas domain model
// ---------------------------------------------------------------------------

type NodeVersion struct {
	Prompt  string   `json:"prompt"`
	Text    string   `json:"text"`
	Prompts []string `json:"prompts"`
}

type GridNodeData struct {
	ID           string        `json:"id"`
	X            float64       `json:"x"`
	Y            float64       `json:"y"`
	Width        float64       `json:"width"`
	Height       float64       `json:"height,omitempty"`
	Prompt       string        `json:"prompt"`
	Text         string        `json:"text"`
	Prompts      []string      `json:"prompts"`
	Status       string        `json:"status"`
	VersionIndex int           `json:"versionIndex"`
	Versions     []NodeVersion `json:"versions"`
	ParentID     string        `json:"parentId,omitempty"`
}

type Canvas struct {
	ID    string
	Nodes []*GridNodeData
}

var (
	canvasStore = map[string]*Canvas{}
	canvasMu    sync.Mutex
)

func createCanvas() *Canvas {
	cid := fmt.Sprintf("canvas-%d-%d", time.Now().UnixMilli(), rand.IntN(100000))
	c := &Canvas{ID: cid}
	canvasMu.Lock()
	canvasStore[cid] = c
	canvasMu.Unlock()
	return c
}

func getCanvas(cid string) *Canvas {
	canvasMu.Lock()
	defer canvasMu.Unlock()
	return canvasStore[cid]
}

func genNodeID() string {
	return fmt.Sprintf("node-%d-%d", time.Now().UnixMilli(), rand.IntN(1000))
}

// Spatial layout — collision-aware child placement
func computeChildPosition(parent *GridNodeData, nodes []*GridNodeData) (float64, float64) {
	newX := parent.X + parent.Width + 400
	initialOffset := 200.0
	if rand.Float64() > 0.5 {
		initialOffset = -200
	}
	newY := parent.Y + initialOffset
	cardHeight := 400.0
	if parent.Height > 0 {
		cardHeight = parent.Height
	}
	occupied := true
	offsetMult := 1.0
	direction := 1.0
	if rand.Float64() > 0.5 {
		direction = -1
	}
	for occupied {
		occupied = false
		for _, n := range nodes {
			nH := 400.0
			if n.Height > 0 {
				nH = n.Height
			}
			xOverlap := abs(n.X+nodeWidth/2-(newX+nodeWidth/2)) < nodeWidth
			yOverlap := abs(n.Y+nH/2-(newY+cardHeight/2)) < (nH+cardHeight)/2
			if xOverlap && yOverlap {
				occupied = true
				break
			}
		}
		if occupied {
			newY = parent.Y + initialOffset + cardHeight*offsetMult*direction
			direction *= -1
			if direction == 1 {
				offsetMult++
			}
		}
	}
	return newX, newY
}

func abs(f float64) float64 {
	if f < 0 {
		return -f
	}
	return f
}

// Node operations

func canvasGenerateNode(c *Canvas, prompt, parentID string) (*GridNodeData, error) {
	var x, y float64
	if parentID != "" {
		var parent *GridNodeData
		for _, n := range c.Nodes {
			if n.ID == parentID {
				parent = n
				break
			}
		}
		if parent == nil {
			return nil, fmt.Errorf("parent node %s not found", parentID)
		}
		x, y = computeChildPosition(parent, c.Nodes)
	}
	node := &GridNodeData{
		ID: genNodeID(), X: x, Y: y, Width: nodeWidth,
		Prompt: prompt, Status: "generating", Prompts: []string{},
		Versions: []NodeVersion{}, ParentID: parentID,
	}
	c.Nodes = append(c.Nodes, node)
	result, err := callLLM(prompt)
	if err != nil {
		log.Printf("Generate error: %v", err)
		node.Status = "error"
	} else {
		if result.Text == "" {
			result.Text = "No text"
		}
		node.Text = result.Text
		node.Prompts = result.Prompts
		node.Status = "ready"
		node.Versions = []NodeVersion{{prompt, result.Text, result.Prompts}}
	}
	return node, nil
}

func canvasRegenerateNode(c *Canvas, nodeID string) (*GridNodeData, error) {
	var node *GridNodeData
	for _, n := range c.Nodes {
		if n.ID == nodeID {
			node = n
			break
		}
	}
	if node == nil {
		return nil, fmt.Errorf("node %s not found", nodeID)
	}
	node.Status = "generating"
	result, err := callLLM(node.Prompt)
	if err != nil {
		node.Status = "error"
	} else {
		if result.Text == "" {
			result.Text = "No text"
		}
		nv := NodeVersion{node.Prompt, result.Text, result.Prompts}
		node.Versions = append(node.Versions, nv)
		node.VersionIndex = len(node.Versions) - 1
		node.Text = nv.Text
		node.Prompts = nv.Prompts
		node.Status = "ready"
	}
	return node, nil
}

func canvasDeleteNode(c *Canvas, nodeID string) []string {
	var getDescendants func(id string) []string
	getDescendants = func(id string) []string {
		var desc []string
		for _, n := range c.Nodes {
			if n.ParentID == id {
				desc = append(desc, n.ID)
				desc = append(desc, getDescendants(n.ID)...)
			}
		}
		return desc
	}
	toDelete := map[string]bool{nodeID: true}
	for _, id := range getDescendants(nodeID) {
		toDelete[id] = true
	}
	filtered := c.Nodes[:0]
	for _, n := range c.Nodes {
		if !toDelete[n.ID] {
			filtered = append(filtered, n)
		}
	}
	c.Nodes = filtered
	var result []string
	for id := range toDelete {
		result = append(result, id)
	}
	return result
}

func canvasSetVersion(c *Canvas, nodeID string, versionIndex int) (*GridNodeData, error) {
	for _, n := range c.Nodes {
		if n.ID == nodeID {
			n.VersionIndex = versionIndex
			if versionIndex < len(n.Versions) {
				n.Text = n.Versions[versionIndex].Text
				n.Prompts = n.Versions[versionIndex].Prompts
			}
			return n, nil
		}
	}
	return nil, fmt.Errorf("node %s not found", nodeID)
}

func canvasMeasureNode(c *Canvas, nodeID string, height float64) {
	for _, n := range c.Nodes {
		if n.ID == nodeID {
			n.Height = height
			return
		}
	}
}

// ---------------------------------------------------------------------------
// Rate limiter
// ---------------------------------------------------------------------------

type ipEntry struct{ timestamps []time.Time }

var (
	rateMu  sync.Mutex
	rateLog = map[string]*ipEntry{}
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
// HTTP helpers
// ---------------------------------------------------------------------------

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

func handleCreateCanvas(w http.ResponseWriter, r *http.Request) {
	c := createCanvas()
	writeJSON(w, 200, map[string]string{"canvasId": c.ID})
}

func handleGenerate(w http.ResponseWriter, r *http.Request) {
	cid := r.PathValue("cid")
	c := getCanvas(cid)
	if c == nil {
		writeError(w, 404, "Canvas not found")
		return
	}
	if rateLimited(r.RemoteAddr) {
		writeError(w, 429, "Too many requests. Please slow down.")
		return
	}
	var body struct {
		Prompt   string `json:"prompt"`
		ParentID string `json:"parentId"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeError(w, 400, "Prompt is required")
		return
	}
	if strings.TrimSpace(body.Prompt) == "" {
		writeError(w, 400, "Prompt is required")
		return
	}
	if len(body.Prompt) > maxPromptLength {
		writeError(w, 400, fmt.Sprintf("Prompt is too long (max %d characters).", maxPromptLength))
		return
	}
	node, err := canvasGenerateNode(c, body.Prompt, body.ParentID)
	if err != nil {
		writeError(w, 500, "Failed to generate text content.")
		return
	}
	writeJSON(w, 200, map[string]any{"node": node})
}

func handleRegenerate(w http.ResponseWriter, r *http.Request) {
	c := getCanvas(r.PathValue("cid"))
	if c == nil {
		writeError(w, 404, "Canvas not found")
		return
	}
	if rateLimited(r.RemoteAddr) {
		writeError(w, 429, "Too many requests. Please slow down.")
		return
	}
	node, err := canvasRegenerateNode(c, r.PathValue("nid"))
	if err != nil {
		writeError(w, 404, "Node not found")
		return
	}
	writeJSON(w, 200, map[string]any{"node": node})
}

func handleDelete(w http.ResponseWriter, r *http.Request) {
	c := getCanvas(r.PathValue("cid"))
	if c == nil {
		writeError(w, 404, "Canvas not found")
		return
	}
	deleted := canvasDeleteNode(c, r.PathValue("nid"))
	writeJSON(w, 200, map[string][]string{"deletedIds": deleted})
}

func handleVersion(w http.ResponseWriter, r *http.Request) {
	c := getCanvas(r.PathValue("cid"))
	if c == nil {
		writeError(w, 404, "Canvas not found")
		return
	}
	var body struct{ VersionIndex int `json:"versionIndex"` }
	json.NewDecoder(r.Body).Decode(&body)
	node, err := canvasSetVersion(c, r.PathValue("nid"), body.VersionIndex)
	if err != nil {
		writeError(w, 404, "Node not found")
		return
	}
	writeJSON(w, 200, map[string]any{"node": node})
}

func handleMeasure(w http.ResponseWriter, r *http.Request) {
	c := getCanvas(r.PathValue("cid"))
	if c == nil {
		writeError(w, 404, "Canvas not found")
		return
	}
	var body struct{ Height float64 `json:"height"` }
	json.NewDecoder(r.Body).Decode(&body)
	canvasMeasureNode(c, r.PathValue("nid"), body.Height)
	writeJSON(w, 200, map[string]bool{"ok": true})
}

func handleGetNodes(w http.ResponseWriter, r *http.Request) {
	c := getCanvas(r.PathValue("cid"))
	if c == nil {
		writeError(w, 404, "Canvas not found")
		return
	}
	writeJSON(w, 200, map[string]any{"nodes": c.Nodes})
}

// ---------------------------------------------------------------------------
// Static file serving + SPA fallback
// ---------------------------------------------------------------------------

func staticHandler(distPath string) http.HandlerFunc {
	fileServer := http.FileServer(http.Dir(distPath))
	return func(w http.ResponseWriter, r *http.Request) {
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
		http.ServeFile(w, r, filepath.Join(distPath, "index.html"))
	}
}

func cspMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", cspHeader)
		next.ServeHTTP(w, r)
	})
}

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

func main() {
	loadDotEnv()
	port := envOr("PORT", "3000")

	distPath, _ := filepath.Abs(filepath.Join("..", "frontend", "dist"))

	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/canvas", handleCreateCanvas)
	mux.HandleFunc("POST /api/canvas/{cid}/generate", handleGenerate)
	mux.HandleFunc("POST /api/canvas/{cid}/nodes/{nid}/regenerate", handleRegenerate)
	mux.HandleFunc("DELETE /api/canvas/{cid}/nodes/{nid}", handleDelete)
	mux.HandleFunc("PUT /api/canvas/{cid}/nodes/{nid}/version", handleVersion)
	mux.HandleFunc("PUT /api/canvas/{cid}/nodes/{nid}/measure", handleMeasure)
	mux.HandleFunc("GET /api/canvas/{cid}/nodes", handleGetNodes)
	mux.HandleFunc("/", staticHandler(distPath))

	addr := ":" + port
	log.Printf("Server running on http://localhost:%s", port)
	if err := http.ListenAndServe(addr, cspMiddleware(mux)); err != nil {
		log.Fatal(err)
	}
}
