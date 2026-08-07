// Gridscape backend — Go (net/http) with stateful canvas management.
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
	linkRE    = regexp.MustCompile(`\[([^\]]+)\]\(([^)]+)\)`)
)

type inlineLink struct {
	Label  string `json:"label"`
	Target string `json:"target"`
}

type genResult struct {
	Text    string       `json:"text"`
	Prompts []string     `json:"prompts"`
	Links   []inlineLink `json:"links"`
}

func parseMarkdownContent(raw string) genResult {
	trimmed := strings.TrimSpace(raw)
	text := trimmed
	section := ""
	if loc := headingRE.FindStringIndex(trimmed); loc != nil {
		text = strings.TrimSpace(trimmed[:loc[0]])
		section = trimmed[loc[1]:]
	}
	prompts := []string{}
	for _, match := range bulletRE.FindAllStringSubmatch(section, -1) {
		prompts = append(prompts, strings.TrimSpace(match[1]))
		if len(prompts) == 3 {
			break
		}
	}
	links := []inlineLink{}
	for _, match := range linkRE.FindAllStringSubmatch(text, -1) {
		links = append(links, inlineLink{Label: strings.TrimSpace(match[1]), Target: strings.TrimSpace(match[2])})
	}
	return genResult{Text: text, Prompts: prompts, Links: links}
}

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
	if value := os.Getenv(key); value != "" {
		return value
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
	if resp.StatusCode != http.StatusOK {
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
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return genResult{}, err
	}
	content := ""
	if len(data.Choices) > 0 {
		content = data.Choices[0].Message.Content
	}
	return parseMarkdownContent(content), nil
}

type NodeVersion struct {
	Prompt  string       `json:"prompt"`
	Text    string       `json:"text"`
	Prompts []string     `json:"prompts"`
	Links   []inlineLink `json:"links"`
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
	Links        []inlineLink  `json:"links"`
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
	canvas := &Canvas{ID: cid}
	canvasMu.Lock()
	canvasStore[cid] = canvas
	canvasMu.Unlock()
	return canvas
}

func getCanvas(cid string) *Canvas {
	canvasMu.Lock()
	defer canvasMu.Unlock()
	return canvasStore[cid]
}

func genNodeID() string {
	return fmt.Sprintf("node-%d-%d", time.Now().UnixMilli(), rand.IntN(1000))
}

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
	offsetMultiplier := 1.0
	direction := 1.0
	if rand.Float64() > 0.5 {
		direction = -1
	}
	for occupied {
		occupied = false
		for _, n := range nodes {
			nodeHeight := 400.0
			if n.Height > 0 {
				nodeHeight = n.Height
			}
			xOverlap := abs(n.X+nodeWidth/2-(newX+nodeWidth/2)) < nodeWidth
			yOverlap := abs(n.Y+nodeHeight/2-(newY+cardHeight/2)) < (nodeHeight+cardHeight)/2
			if xOverlap && yOverlap {
				occupied = true
				break
			}
		}
		if occupied {
			newY = parent.Y + initialOffset + cardHeight*offsetMultiplier*direction
			direction *= -1
			if direction == 1 {
				offsetMultiplier++
			}
		}
	}
	return newX, newY
}

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
		Prompt: prompt, Status: "generating", Prompts: []string{}, Links: []inlineLink{},
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
		node.Links = result.Links
		node.Status = "ready"
		node.Versions = []NodeVersion{{Prompt: prompt, Text: node.Text, Prompts: node.Prompts, Links: node.Links}}
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
		nv := NodeVersion{Prompt: node.Prompt, Text: result.Text, Prompts: result.Prompts, Links: result.Links}
		node.Versions = append(node.Versions, nv)
		node.VersionIndex = len(node.Versions) - 1
		node.Text = nv.Text
		node.Prompts = nv.Prompts
		node.Links = nv.Links
		node.Status = "ready"
	}
	return node, nil
}

func canvasDeleteNode(c *Canvas, nodeID string) []string {
	var descendants func(string) []string
	descendants = func(id string) []string {
		var result []string
		for _, n := range c.Nodes {
			if n.ParentID == id {
				result = append(result, n.ID)
				result = append(result, descendants(n.ID)...)
			}
		}
		return result
	}
	toDelete := map[string]bool{nodeID: true}
	for _, id := range descendants(nodeID) {
		toDelete[id] = true
	}
	filtered := c.Nodes[:0]
	for _, n := range c.Nodes {
		if !toDelete[n.ID] {
			filtered = append(filtered, n)
		}
	}
	c.Nodes = filtered
	result := []string{}
	for id := range toDelete {
		result = append(result, id)
	}
	return result
}

func canvasSetVersion(c *Canvas, nodeID string, versionIndex int) (*GridNodeData, error) {
	for _, n := range c.Nodes {
		if n.ID == nodeID {
			n.VersionIndex = versionIndex
			if versionIndex >= 0 && versionIndex < len(n.Versions) {
				n.Text = n.Versions[versionIndex].Text
				n.Prompts = n.Versions[versionIndex].Prompts
				n.Links = n.Versions[versionIndex].Links
			}
			return n, nil
		}
	}
	return nil, fmt.Errorf("node %s not found", nodeID)
}

func canvasSetPosition(c *Canvas, nodeID string, x, y float64) (*GridNodeData, error) {
	for _, n := range c.Nodes {
		if n.ID == nodeID {
			n.X, n.Y = x, y
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

func abs(f float64) float64 {
	if f < 0 {
		return -f
	}
	return f
}

type ipEntry struct{ timestamps []time.Time }

var (
	rateMu  sync.Mutex
	rateLog = map[string]*ipEntry{}
)

func rateLimited(ip string) bool {
	rateMu.Lock()
	defer rateMu.Unlock()
	now := time.Now()
	entry := rateLog[ip]
	if entry == nil {
		entry = &ipEntry{}
		rateLog[ip] = entry
	}
	cutoff := now.Add(-time.Minute)
	filtered := entry.timestamps[:0]
	for _, timestamp := range entry.timestamps {
		if timestamp.After(cutoff) {
			filtered = append(filtered, timestamp)
		}
	}
	if len(filtered) >= 20 {
		entry.timestamps = filtered
		return true
	}
	entry.timestamps = append(filtered, now)
	return false
}

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(data)
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}

func handleCreateCanvas(w http.ResponseWriter, r *http.Request) {
	canvas := createCanvas()
	writeJSON(w, http.StatusOK, map[string]string{"canvasId": canvas.ID})
}

func handleGenerate(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	if rateLimited(r.RemoteAddr) {
		writeError(w, http.StatusTooManyRequests, "Too many requests. Please slow down.")
		return
	}
	var body struct {
		Prompt   string `json:"prompt"`
		ParentID string `json:"parentId"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil || strings.TrimSpace(body.Prompt) == "" {
		writeError(w, http.StatusBadRequest, "Prompt is required")
		return
	}
	if len(body.Prompt) > maxPromptLength {
		writeError(w, http.StatusBadRequest, fmt.Sprintf("Prompt is too long (max %d characters).", maxPromptLength))
		return
	}
	node, err := canvasGenerateNode(canvas, body.Prompt, body.ParentID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "Failed to generate text content.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"node": node})
}

func handleRegenerate(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	if rateLimited(r.RemoteAddr) {
		writeError(w, http.StatusTooManyRequests, "Too many requests. Please slow down.")
		return
	}
	node, err := canvasRegenerateNode(canvas, r.PathValue("nid"))
	if err != nil {
		writeError(w, http.StatusNotFound, "Node not found")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"node": node})
}

func handleDelete(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	writeJSON(w, http.StatusOK, map[string][]string{"deletedIds": canvasDeleteNode(canvas, r.PathValue("nid"))})
}

func handleVersion(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	var body struct {
		VersionIndex int `json:"versionIndex"`
	}
	_ = json.NewDecoder(r.Body).Decode(&body)
	node, err := canvasSetVersion(canvas, r.PathValue("nid"), body.VersionIndex)
	if err != nil {
		writeError(w, http.StatusNotFound, "Node not found")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"node": node})
}

func handlePosition(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	var body struct {
		X float64 `json:"x"`
		Y float64 `json:"y"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "Position requires numeric x and y")
		return
	}
	node, err := canvasSetPosition(canvas, r.PathValue("nid"), body.X, body.Y)
	if err != nil {
		writeError(w, http.StatusNotFound, "Node not found")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"node": node})
}

func handleMeasure(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	var body struct {
		Height float64 `json:"height"`
	}
	_ = json.NewDecoder(r.Body).Decode(&body)
	canvasMeasureNode(canvas, r.PathValue("nid"), body.Height)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func handleGetNodes(w http.ResponseWriter, r *http.Request) {
	canvas := getCanvas(r.PathValue("cid"))
	if canvas == nil {
		writeError(w, http.StatusNotFound, "Canvas not found")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"nodes": canvas.Nodes})
}

func staticHandler(distPath string) http.HandlerFunc {
	fileServer := http.FileServer(http.Dir(distPath))
	return func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") {
			http.NotFound(w, r)
			return
		}
		fullPath := filepath.Join(distPath, filepath.Clean(r.URL.Path))
		if info, err := os.Stat(fullPath); err == nil && !info.IsDir() {
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
			value := strings.Trim(strings.TrimSpace(parts[1]), `"'`)
			if os.Getenv(key) == "" {
				_ = os.Setenv(key, value)
			}
		}
	}
}

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
	mux.HandleFunc("PUT /api/canvas/{cid}/nodes/{nid}/position", handlePosition)
	mux.HandleFunc("PUT /api/canvas/{cid}/nodes/{nid}/measure", handleMeasure)
	mux.HandleFunc("GET /api/canvas/{cid}/nodes", handleGetNodes)
	mux.HandleFunc("/", staticHandler(distPath))
	addr := ":" + port
	log.Printf("Server running on http://localhost:%s", port)
	if err := http.ListenAndServe(addr, cspMiddleware(mux)); err != nil {
		log.Fatal(err)
	}
}
