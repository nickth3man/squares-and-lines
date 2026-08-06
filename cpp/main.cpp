// Gridscape backend — C++ (cpp-httplib + libcurl) with stateful canvas management.
//
// The backend owns all domain logic: node model, spatial layout, versioning,
// tree structure, and deletion cascades.

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const size_t MAX_PROMPT_LENGTH = 2000;
static const double NODE_WIDTH = 400.0;

static const std::string SYSTEM_PROMPT =
    "You are an infinite spatial-knowledge-engine generator. "
    "Respond with markdown text about the user's topic: brief but impactful "
    "explanatory text. CRITICAL: wrap 2 to 4 key concepts or interesting terms "
    "as clickable markdown links in the exact format [Term](Term) with no spaces, "
    "for example 'Machine learning relies on [Supervised Learning](Supervised Learning) "
    "and [Neural Networks](Neural Networks).' Do not use bold or italics for those terms. "
    "End your response with a section headed exactly '## Explore further' containing "
    "exactly 3 bullet-point markdown links, each bullet like "
    "'- [A follow-up question](A follow-up question)'. "
    "Output only the markdown; no JSON, no code fences.";

static const char* CSP_HEADER =
    "default-src 'self'; script-src 'self'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src 'self' https://fonts.gstatic.com data:; "
    "img-src 'self' data:; connect-src 'self'";

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool iequals(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](char c1, char c2) { return std::tolower((unsigned char)c1) == std::tolower((unsigned char)c2); });
}

static std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

static double absf(double f) { return f < 0 ? -f : f; }

static std::string genId(const std::string& prefix) {
    static std::mt19937 rng(std::random_device{}());
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix + "-" + std::to_string(ms) + "-" + std::to_string(rng() % 1000);
}

static double randFloat() {
    static std::mt19937 rng(std::random_device{}());
    return (double)rng() / rng.max();
}

// ---------------------------------------------------------------------------
// .env loader
// ---------------------------------------------------------------------------

static void loadDotEnv() {
    std::ifstream f(".env");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\'')))
            val = val.substr(1, val.size() - 2);
        if (key.empty() || key[0] == '#') continue;
        if (!std::getenv(key.c_str())) {
#ifdef _WIN32
            _putenv_s(key.c_str(), val.c_str());
#else
            setenv(key.c_str(), val.c_str(), 0);
#endif
        }
    }
}

// ---------------------------------------------------------------------------
// Rate limiter
// ---------------------------------------------------------------------------

struct RateLimiter {
    std::mutex mtx;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> log;
    bool check(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();
        auto& dq = log[ip];
        while (!dq.empty() && std::chrono::duration_cast<std::chrono::seconds>(now - dq.front()).count() >= 60)
            dq.pop_front();
        if (dq.size() >= 20) return true;
        dq.push_back(now);
        return false;
    }
};

// ---------------------------------------------------------------------------
// Markdown parser
// ---------------------------------------------------------------------------

struct GenResult { std::string text; std::vector<std::string> prompts; };

static GenResult parseMarkdownContent(const std::string& raw) {
    std::string trimmed = trim(raw);
    size_t headingPos = std::string::npos, headingEnd = std::string::npos;
    size_t pos = 0;
    while (pos < trimmed.size()) {
        size_t lineEnd = trimmed.find('\n', pos);
        std::string line = (lineEnd == std::string::npos) ? trimmed.substr(pos) : trimmed.substr(pos, lineEnd - pos);
        std::string stripped = trim(line);
        if (stripped.size() > 3 && stripped.substr(0, 2) == "##") {
            if (iequals(trim(stripped.substr(2)), "explore further")) {
                headingPos = pos;
                headingEnd = (lineEnd == std::string::npos) ? trimmed.size() : lineEnd + 1;
                break;
            }
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 1;
    }
    if (headingPos == std::string::npos) return { trimmed, {} };
    std::string text = trim(trimmed.substr(0, headingPos));
    std::string section = trimmed.substr(headingEnd);
    std::vector<std::string> prompts;
    std::regex bulletRe(R"(^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$)", std::regex::multiline);
    for (std::sregex_iterator it(section.begin(), section.end(), bulletRe), end; it != end && prompts.size() < 3; ++it)
        prompts.push_back(trim((*it)[1].str()));
    return { text, prompts };
}

// ---------------------------------------------------------------------------
// LLM provider
// ---------------------------------------------------------------------------

static size_t writeCb(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static bool callLLM(const std::string& prompt, GenResult& out) {
    std::string name = envOr("AI_PROVIDER", "openrouter");
    std::string baseUrl, apiKeyEnv, modelEnv, defaultModel;
    if (name == "openai") {
        baseUrl = "https://api.openai.com/v1"; apiKeyEnv = "OPENAI_API_KEY"; modelEnv = "OPENAI_MODEL"; defaultModel = "gpt-4o-mini";
    } else { baseUrl = "https://openrouter.ai/api/v1"; apiKeyEnv = "OPENROUTER_API_KEY"; modelEnv = "OPENROUTER_MODEL"; defaultModel = "openai/gpt-oss-20b"; }

    json body = { {"model", envOr(modelEnv.c_str(), defaultModel)}, {"messages", {
        {{"role", "system"}, {"content", SYSTEM_PROMPT}},
        {{"role", "user"}, {"content", prompt}},
    }} };
    std::string bodyStr = body.dump();
    std::string url = baseUrl + "/chat/completions";
    std::string auth = "Authorization: Bearer " + envOr(apiKeyEnv.c_str(), "");

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (res != CURLE_OK || code != 200) return false;
    json data = json::parse(resp, nullptr, false);
    if (data.is_discarded()) return false;
    std::string content = data.value(json::json_pointer("/choices/0/message/content"), "");
    out = parseMarkdownContent(content);
    return true;
}

// ---------------------------------------------------------------------------
// Canvas domain model
// ---------------------------------------------------------------------------

struct NodeVersion { std::string prompt, text; std::vector<std::string> prompts; };

struct GridNodeData {
    std::string id;
    double x, y, width;
    std::optional<double> height;
    std::string prompt, text, status;
    std::vector<std::string> prompts;
    int versionIndex;
    std::vector<NodeVersion> versions;
    std::string parentId;

    json toJson() const {
        json j = { {"id", id}, {"x", x}, {"y", y}, {"width", width},
            {"prompt", prompt}, {"text", text}, {"prompts", prompts},
            {"status", status}, {"versionIndex", versionIndex} };
        if (height) j["height"] = *height;
        if (!parentId.empty()) j["parentId"] = parentId;
        json vers = json::array();
        for (auto& v : versions)
            vers.push_back({{"prompt", v.prompt}, {"text", v.text}, {"prompts", v.prompts}});
        j["versions"] = vers;
        return j;
    }
};

struct Canvas { std::string id; std::vector<GridNodeData> nodes; };

static std::map<std::string, Canvas> g_canvases;
static std::mutex g_canvasMutex;
static RateLimiter g_rateLimiter;

// Spatial layout — collision-aware child placement
static std::pair<double, double> computeChildPosition(const GridNodeData& parent, const std::vector<GridNodeData>& nodes) {
    double newX = parent.x + parent.width + 400;
    double initialOffset = randFloat() > 0.5 ? 200 : -200;
    double newY = parent.y + initialOffset;
    double cardH = parent.height.value_or(400);
    bool occupied = true;
    double mult = 1; double dir = randFloat() > 0.5 ? 1 : -1;
    while (occupied) {
        occupied = false;
        for (auto& n : nodes) {
            double nH = n.height.value_or(400);
            if (absf(n.x + NODE_WIDTH/2 - (newX + NODE_WIDTH/2)) < NODE_WIDTH &&
                absf(n.y + nH/2 - (newY + cardH/2)) < (nH + cardH)/2) { occupied = true; break; }
        }
        if (occupied) {
            newY = parent.y + initialOffset + cardH * mult * dir;
            dir *= -1; if (dir == 1) mult++;
        }
    }
    return {newX, newY};
}

// ---------------------------------------------------------------------------
// Static file helpers
// ---------------------------------------------------------------------------

static const char* contentTypeFor(const fs::path& p) {
    std::string ext = p.extension().string();
    if (ext == ".html") return "text/html"; if (ext == ".js") return "application/javascript";
    if (ext == ".css") return "text/css"; if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml"; if (ext == ".png") return "image/png";
    return "application/octet-stream";
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    loadDotEnv();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int port = std::stoi(envOr("PORT", "3000"));
    fs::path distPath = fs::path(__FILE__).parent_path().parent_path() / "frontend" / "dist";
    httplib::Server svr;
    svr.set_default_headers({{"Content-Security-Policy", CSP_HEADER}});

    // POST /api/canvas
    svr.Post("/api/canvas", [](const auto& req, auto& res) {
        std::string cid = genId("canvas");
        { std::lock_guard<std::mutex> lock(g_canvasMutex); g_canvases[cid] = {cid, {}}; }
        res.set_content(json{{"canvasId", cid}}.dump(), "application/json");
    });

    // POST /api/canvas/:cid/generate
    svr.Post(R"(/api/canvas/([^/]+)/generate)", [](const auto& req, auto& res) {
        std::string cid = req.matches[1];
        if (g_rateLimiter.check(req.remote_addr)) { res.status = 429; res.set_content(R"({"error":"Too many requests. Please slow down."})", "application/json"); return; }
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("prompt") || !body["prompt"].is_string()) { res.status = 400; res.set_content(R"({"error":"Prompt is required"})", "application/json"); return; }
        std::string prompt = body["prompt"].get<std::string>();
        if (trim(prompt).empty()) { res.status = 400; res.set_content(R"({"error":"Prompt is required"})", "application/json"); return; }
        std::string parentId = body.value("parentId", "");

        std::lock_guard<std::mutex> lock(g_canvasMutex);
        auto it = g_canvases.find(cid);
        if (it == g_canvases.end()) { res.status = 404; res.set_content(R"({"error":"Canvas not found"})", "application/json"); return; }
        auto& canvas = it->second;
        double x = 0, y = 0;
        if (!parentId.empty()) {
            auto pit = std::find_if(canvas.nodes.begin(), canvas.nodes.end(), [&](const auto& n){ return n.id == parentId; });
            if (pit == canvas.nodes.end()) { res.status = 500; res.set_content(R"({"error":"Parent not found"})", "application/json"); return; }
            auto [px, py] = computeChildPosition(*pit, canvas.nodes); x = px; y = py;
        }
        GridNodeData node{genId("node"), x, y, NODE_WIDTH, std::nullopt, prompt, "", "generating", {}, 0, {}, parentId};
        canvas.nodes.push_back(node);
        GenResult result;
        if (callLLM(prompt, result)) {
            node.text = result.text.empty() ? "No text" : result.text;
            node.prompts = result.prompts; node.status = "ready";
            node.versions.push_back({prompt, node.text, node.prompts});
        } else { node.status = "error"; }
        auto& stored = canvas.nodes.back(); stored = node;
        res.set_content(json{{"node", node.toJson()}}.dump(), "application/json");
    });

    // POST /api/canvas/:cid/nodes/:nid/regenerate
    svr.Post(R"(/api/canvas/([^/]+)/nodes/([^/]+)/regenerate)", [](const auto& req, auto& res) {
        if (g_rateLimiter.check(req.remote_addr)) { res.status = 429; res.set_content(R"({"error":"Too many requests. Please slow down."})", "application/json"); return; }
        std::lock_guard<std::mutex> lock(g_canvasMutex);
        auto it = g_canvases.find(req.matches[1]);
        if (it == g_canvases.end()) { res.status = 404; res.set_content(R"({"error":"Canvas not found"})", "application/json"); return; }
        auto& canvas = it->second;
        auto nit = std::find_if(canvas.nodes.begin(), canvas.nodes.end(), [&](const auto& n){ return n.id == req.matches[2]; });
        if (nit == canvas.nodes.end()) { res.status = 404; res.set_content(R"({"error":"Node not found"})", "application/json"); return; }
        auto& node = *nit; node.status = "generating";
        GenResult result;
        if (callLLM(node.prompt, result)) {
            std::string text = result.text.empty() ? "No text" : result.text;
            node.versions.push_back({node.prompt, text, result.prompts});
            node.versionIndex = node.versions.size() - 1;
            node.text = text; node.prompts = result.prompts; node.status = "ready";
        } else { node.status = "error"; }
        res.set_content(json{{"node", node.toJson()}}.dump(), "application/json");
    });

    // DELETE /api/canvas/:cid/nodes/:nid
    svr.Delete(R"(/api/canvas/([^/]+)/nodes/([^/]+))", [](const auto& req, auto& res) {
        std::lock_guard<std::mutex> lock(g_canvasMutex);
        auto it = g_canvases.find(req.matches[1]);
        if (it == g_canvases.end()) { res.status = 404; res.set_content(R"({"error":"Canvas not found"})", "application/json"); return; }
        auto& canvas = it->second;
        std::string nid = req.matches[2];
        // Find all descendants
        std::vector<std::string> toDelete = {nid};
        std::function<void(const std::string&)> findDesc = [&](const std::string& id) {
            for (auto& n : canvas.nodes) if (n.parentId == id) { toDelete.push_back(n.id); findDesc(n.id); }
        };
        findDesc(nid);
        std::set<std::string> delSet(toDelete.begin(), toDelete.end());
        canvas.nodes.erase(std::remove_if(canvas.nodes.begin(), canvas.nodes.end(),
            [&](const auto& n){ return delSet.count(n.id); }), canvas.nodes.end());
        res.set_content(json{{"deletedIds", toDelete}}.dump(), "application/json");
    });

    // PUT /api/canvas/:cid/nodes/:nid/version
    svr.Put(R"(/api/canvas/([^/]+)/nodes/([^/]+)/version)", [](const auto& req, auto& res) {
        std::lock_guard<std::mutex> lock(g_canvasMutex);
        auto it = g_canvases.find(req.matches[1]);
        if (it == g_canvases.end()) { res.status = 404; res.set_content(R"({"error":"Canvas not found"})", "application/json"); return; }
        auto& canvas = it->second;
        auto nit = std::find_if(canvas.nodes.begin(), canvas.nodes.end(), [&](const auto& n){ return n.id == req.matches[2]; });
        if (nit == canvas.nodes.end()) { res.status = 404; res.set_content(R"({"error":"Node not found"})", "application/json"); return; }
        json body = json::parse(req.body, nullptr, false);
        int vi = body.value("versionIndex", 0);
        auto& node = *nit; node.versionIndex = vi;
        if (vi < (int)node.versions.size()) { node.text = node.versions[vi].text; node.prompts = node.versions[vi].prompts; }
        res.set_content(json{{"node", node.toJson()}}.dump(), "application/json");
    });

    // PUT /api/canvas/:cid/nodes/:nid/measure
    svr.Put(R"(/api/canvas/([^/]+)/nodes/([^/]+)/measure)", [](const auto& req, auto& res) {
        std::lock_guard<std::mutex> lock(g_canvasMutex);
        auto it = g_canvases.find(req.matches[1]);
        if (it == g_canvases.end()) { res.status = 404; res.set_content(R"({"error":"Canvas not found"})", "application/json"); return; }
        auto& canvas = it->second;
        json body = json::parse(req.body, nullptr, false);
        double h = body.value("height", 0.0);
        for (auto& n : canvas.nodes) if (n.id == req.matches[2]) { n.height = h; break; }
        res.set_content(R"({"ok":true})", "application/json");
    });

    // GET /api/canvas/:cid/nodes
    svr.Get(R"(/api/canvas/([^/]+)/nodes)", [](const auto& req, auto& res) {
        std::lock_guard<std::mutex> lock(g_canvasMutex);
        auto it = g_canvases.find(req.matches[1]);
        if (it == g_canvases.end()) { res.status = 404; res.set_content(R"({"error":"Canvas not found"})", "application/json"); return; }
        json nodes = json::array();
        for (auto& n : it->second.nodes) nodes.push_back(n.toJson());
        res.set_content(json{{"nodes", nodes}}.dump(), "application/json");
    });

    // Static file serving + SPA fallback
    svr.set_mount_point("/", distPath.string());
    svr.set_error_handler([&distPath](const auto& req, auto& res) {
        if (res.status == 404) {
            fs::path idx = distPath / "index.html";
            if (fs::exists(idx)) { res.status = 200; res.set_content(readFile(idx), "text/html"); }
        }
    });

    std::cout << "Server running on http://localhost:" << port << std::endl;
    svr.listen("0.0.0.0", port);
    curl_global_cleanup();
    return 0;
}
