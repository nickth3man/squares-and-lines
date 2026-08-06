// Gridscape backend — C++ (cpp-httplib + libcurl)
//
// Serves the built frontend from ../frontend/dist and exposes
// POST /api/generate which calls an OpenAI-compatible chat completions
// endpoint and parses the markdown response into {text, prompts}.
//
// Build:  see CMakeLists.txt / README.md

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

static const char* CSP_HEADER =
    "default-src 'self'; script-src 'self'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src 'self' https://fonts.gstatic.com data:; "
    "img-src 'self' data:; connect-src 'self'";

// Load .env file if present (sets env vars not already defined).
static void loadDotEnv() {
    std::ifstream f(".env");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        // Strip surrounding quotes
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                 (val.front() == '\'' && val.back() == '\'')))
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

// Simple per-IP rate limiter (20 req/min).
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

// Error carrying the upstream HTTP status so routes can surface it.
struct ProviderError : std::runtime_error {
    int status;
    ProviderError(int s, const std::string& msg) : std::runtime_error(msg), status(s) {}
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static const size_t MAX_PROMPT_LENGTH = 2000;

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

// ---------------------------------------------------------------------------
// Markdown parser  (mirrors providers/contract.ts:parseMarkdownContent)
// ---------------------------------------------------------------------------

struct GenResult {
    std::string text;
    std::vector<std::string> prompts;
};

static std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Case-insensitive ASCII string comparison for the heading match.
static bool iequals(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) ==
                   std::tolower(static_cast<unsigned char>(c2));
        });
}

static GenResult parseMarkdownContent(const std::string& raw) {
    std::string trimmed = trim(raw);

    // Find the "## Explore further" heading line (case-insensitive).
    // We scan line by line to match the original regex ^##\s+explore further\s*$
    size_t headingPos = std::string::npos;
    size_t headingEnd = std::string::npos;

    size_t pos = 0;
    while (pos < trimmed.size()) {
        size_t lineEnd = trimmed.find('\n', pos);
        std::string line = (lineEnd == std::string::npos)
            ? trimmed.substr(pos)
            : trimmed.substr(pos, lineEnd - pos);

        std::string stripped = trim(line);
        if (stripped.size() > 3 && stripped.substr(0, 2) == "##") {
            std::string rest = trim(stripped.substr(2));
            if (iequals(rest, "explore further")) {
                headingPos = pos;
                headingEnd = (lineEnd == std::string::npos) ? trimmed.size() : lineEnd + 1;
                break;
            }
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 1;
    }

    if (headingPos == std::string::npos) {
        return { trimmed, {} };
    }

    std::string text = trim(trimmed.substr(0, headingPos));
    std::string section = trimmed.substr(headingEnd);

    // Match bullets: ^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$
    std::vector<std::string> prompts;
    std::regex bulletRe(R"(^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$)", std::regex::multiline);
    for (std::sregex_iterator it(section.begin(), section.end(), bulletRe), end; it != end && prompts.size() < 3; ++it) {
        prompts.push_back(trim((*it)[1].str()));
    }

    return { text, prompts };
}

// ---------------------------------------------------------------------------
// LLM provider  (OpenAI-compatible chat completions via libcurl)
// ---------------------------------------------------------------------------

struct ProviderConfig {
    std::string baseUrl;
    std::string apiKeyEnv;
    std::string modelEnv;
    std::string defaultModel;
};

static std::string envOr(const char* key, const std::string& fallback) {
    const char* val = std::getenv(key);
    return (val && *val) ? std::string(val) : fallback;
}

// libcurl write callback
static size_t writeCb(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t total = size * nmemb;
    userp->append(static_cast<char*>(contents), total);
    return total;
}

static GenResult callChatCompletions(
    const std::string& baseUrl,
    const std::string& apiKey,
    const std::string& model,
    const std::string& prompt
) {
    json body = {
        {"model", model},
        {"messages", {
            {{"role", "system"}, {"content", SYSTEM_PROMPT}},
            {{"role", "user"}, {"content", prompt}},
        }}
    };
    std::string bodyStr = body.dump();

    std::string url = baseUrl + "/chat/completions";
    std::string authHeader = "Authorization: Bearer " + apiKey;

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to init libcurl");

    std::string responseStr;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw ProviderError(502, std::string("libcurl error: ") + curl_easy_strerror(res));
    }
    if (httpCode != 200) {
        std::cerr << "Chat completions error: " << httpCode << " " << responseStr << std::endl;
        throw ProviderError(static_cast<int>(httpCode), "LLM provider error");
    }

    json resp = json::parse(responseStr, nullptr, false);
    if (resp.is_discarded() || !resp.contains("choices")) {
        return { "", {} };
    }

    std::string content = "";
    try {
        content = resp["choices"][0]["message"]["content"].get<std::string>();
    } catch (...) {}

    return parseMarkdownContent(content);
}

static GenResult generateText(const std::string& prompt) {
    std::string name = envOr("AI_PROVIDER", "openrouter");

    ProviderConfig cfg;
    if (name == "openai") {
        cfg = {"https://api.openai.com/v1", "OPENAI_API_KEY", "OPENAI_MODEL", "gpt-4o-mini"};
    } else {
        if (name != "openrouter") {
            std::cerr << "Unknown AI_PROVIDER \"" << name << "\", falling back to openrouter" << std::endl;
        }
        cfg = {"https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "OPENROUTER_MODEL", "openai/gpt-oss-20b"};
    }

    return callChatCompletions(
        cfg.baseUrl,
        envOr(cfg.apiKeyEnv.c_str(), ""),
        envOr(cfg.modelEnv.c_str(), cfg.defaultModel),
        prompt
    );
}

// ---------------------------------------------------------------------------
// Static file serving + SPA fallback helpers
// ---------------------------------------------------------------------------

static const char* contentTypeFor(const fs::path& p) {
    std::string ext = p.extension().string();
    if (ext == ".html") return "text/html";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".css")  return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".png")  return "image/png";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
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
    // Resolve relative to source file (works regardless of CWD).
    fs::path distPath = fs::path(__FILE__).parent_path().parent_path() / "frontend" / "dist";

    httplib::Server svr;
    svr.set_default_headers({{"Content-Security-Policy", CSP_HEADER}});

    static RateLimiter rateLimiter;

    // POST /api/generate
    svr.Post("/api/generate", [&distPath](const httplib::Request& req, httplib::Response& res) {
        // Per-IP rate limiting (20 req/min)
        std::string ip = req.remote_addr;
        if (rateLimiter.check(ip)) {
            res.status = 429;
            res.set_content(R"({"error":"Too many requests. Please slow down."})", "application/json");
            return;
        }

        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("prompt") || !body["prompt"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"Prompt is required"})", "application/json");
            return;
        }

        std::string prompt = body["prompt"].get<std::string>();
        if (trim(prompt).empty()) {
            res.status = 400;
            res.set_content(R"({"error":"Prompt is required"})", "application/json");
            return;
        }
        if (prompt.size() > MAX_PROMPT_LENGTH) {
            res.status = 400;
            res.set_content(R"({"error":"Prompt is too long (max 2000 characters)."})", "application/json");
            return;
        }

        try {
            GenResult result = generateText(prompt);
            json out = {{"text", result.text}, {"prompts", result.prompts}};
            res.set_content(out.dump(), "application/json");
        } catch (const ProviderError& e) {
            std::cerr << "Generate error: " << e.what() << std::endl;
            res.status = e.status;
            res.set_content(R"({"error":"Generation failed. Please try again."})", "application/json");
        } catch (const std::exception& e) {
            std::cerr << "Generate error: " << e.what() << std::endl;
            res.status = 500;
            res.set_content(R"({"error":"Failed to generate text content."})", "application/json");
        }
    });

    // Static file serving + SPA fallback
    svr.set_mount_point("/", distPath.string());
    svr.set_error_handler([&distPath](const httplib::Request& req, httplib::Response& res) {
        if (res.status == 404) {
            fs::path indexFile = distPath / "index.html";
            if (fs::exists(indexFile)) {
                res.status = 200;
                res.set_content(readFile(indexFile), "text/html");
                return;
            }
        }
    });

    std::cout << "Server running on http://localhost:" << port << std::endl;
    svr.listen("0.0.0.0", port);

    curl_global_cleanup();
    return 0;
}
