/*
 * Gridscape backend — C (raw sockets + libcurl)
 *
 * Serves the built frontend from ../frontend/dist and exposes
 * POST /api/generate which calls an OpenAI-compatible chat completions
 * endpoint and parses the markdown response into {text, prompts}.
 *
 * Build:  see Makefile (Linux/macOS) or build command in README.md (Windows)
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_SOCKET closesocket
  #define SHUT_RDWR SD_BOTH
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define CLOSE_SOCKET close
  #define SHUT_RDWR SHUT_RDWR
  #define INVALID_SOCKET (-1)
#endif

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------------
 * Configuration
 * ----------------------------------------------------------------------- */

#define MAX_PROMPT_LENGTH 2000
#define BUFSZ (1024 * 1024)

static const char *SYSTEM_PROMPT =
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

static const char *CSP_HEADER =
    "default-src 'self'; script-src 'self'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src 'self' https://fonts.gstatic.com data:; "
    "img-src 'self' data:; connect-src 'self'";

/* Load .env file if present (sets env vars not already defined). */
static void load_dotenv(void) {
    FILE *f = fopen(".env", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (*key == '\0' || *key == '#') continue;
        /* Strip surrounding quotes from value */
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                          (val[0] == '\'' && val[vlen-1] == '\''))) {
            val[vlen-1] = '\0';
            val++;
        }
        if (!getenv(key)) {
#ifdef _WIN32
            _putenv_s(key, val);
#else
            setenv(key, val, 0);
#endif
        }
    }
    fclose(f);
}

/* Simple per-IP rate limiter (20 req/min). Single-threaded — no mutex needed. */
#define RATE_LIMIT_MAX 20
#define RATE_LIMIT_ENTRIES 256
typedef struct { char ip[64]; time_t timestamps[RATE_LIMIT_MAX]; int count; } RateBucket;
static RateBucket g_rate_buckets[RATE_LIMIT_ENTRIES];

static bool rate_limited(const char *ip) {
    time_t now = time(NULL);
    RateBucket *bucket = NULL;
    for (int i = 0; i < RATE_LIMIT_ENTRIES; i++) {
        if (g_rate_buckets[i].count > 0 && strcmp(g_rate_buckets[i].ip, ip) == 0) {
            bucket = &g_rate_buckets[i];
            break;
        }
    }
    if (!bucket) {
        for (int i = 0; i < RATE_LIMIT_ENTRIES; i++) {
            if (g_rate_buckets[i].count == 0) { bucket = &g_rate_buckets[i]; break; }
        }
    }
    if (!bucket) bucket = &g_rate_buckets[0]; /* overwrite oldest */

    strncpy(bucket->ip, ip, sizeof(bucket->ip) - 1);
    bucket->ip[sizeof(bucket->ip) - 1] = '\0';

    /* Expire old entries */
    int write_idx = 0;
    for (int i = 0; i < bucket->count; i++) {
        if (now - bucket->timestamps[i] < 60)
            bucket->timestamps[write_idx++] = bucket->timestamps[i];
    }
    bucket->count = write_idx;

    if (bucket->count >= RATE_LIMIT_MAX) return true;
    bucket->timestamps[bucket->count++] = now;
    return false;
}

static int PORT = 3000;

/* ---------------------------------------------------------------------------
 * String helpers
 * ----------------------------------------------------------------------- */

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static bool iequals(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *env_or(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v && *v) ? (char*)v : (char*)fallback;
}

/* JSON-escape a string into a malloc'd buffer. */
static char *json_escape(const char *s) {
    size_t len = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"': case '\\': len += 2; break;
            case '\n': len += 2; break;
            case '\r': len += 2; break;
            case '\t': len += 2; break;
            default: len++;
        }
    }
    char *out = malloc(len + 1);
    char *o = out;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  *o++ = '\\'; *o++ = '"'; break;
            case '\\': *o++ = '\\'; *o++ = '\\'; break;
            case '\n': *o++ = '\\'; *o++ = 'n'; break;
            case '\r': *o++ = '\\'; *o++ = 'r'; break;
            case '\t': *o++ = '\\'; *o++ = 't'; break;
            default: *o++ = *p;
        }
    }
    *o = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Markdown parser  (mirrors providers/contract.ts:parseMarkdownContent)
 * Manual line-by-line — no regex dependency.
 * ----------------------------------------------------------------------- */

typedef struct {
    char *text;
    char **prompts;
    int prompt_count;
} GenResult;

/* Check if a line is exactly "## explore further" (case-insensitive, trimmed). */
static bool is_heading(const char *line) {
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *t = trim(buf);
    if (t[0] != '#' || t[1] != '#') return false;
    t += 2;
    while (*t && isspace((unsigned char)*t)) t++;
    return iequals(t, "explore further");
}

/* Try to parse a bullet:  - [Label](url)  or  * [Label](url)
 * Returns malloc'd label on success, NULL on failure. */
static char *parse_bullet(const char *line) {
    char buf[1024];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '-' && *p != '*') return NULL;
    p++;
    if (!isspace((unsigned char)*p)) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '[') return NULL;
    p++;
    char *label_start = p;
    while (*p && *p != ']') p++;
    if (*p != ']') return NULL;
    char *label_end = p;
    p++; /* skip ] */
    if (*p != '(') return NULL;
    /* skip the rest — we only need the label */
    char *label = malloc(label_end - label_start + 1);
    memcpy(label, label_start, label_end - label_start);
    label[label_end - label_start] = '\0';
    return trim(label);
}

static GenResult parse_markdown_content(const char *raw) {
    GenResult result = {0};
    char *copy = strdup(raw);
    char *trimmed = trim(copy);

    /* Find heading line */
    char *heading_line_start = NULL;
    char *after_heading = NULL;

    char *line_start = trimmed;
    while (line_start && *line_start) {
        char *newline = strchr(line_start, '\n');
        char saved = 0;
        if (newline) { saved = *newline; *newline = '\0'; }

        if (is_heading(line_start)) {
            heading_line_start = line_start;
            after_heading = newline ? newline + 1 : line_start + strlen(line_start);
            if (newline) *newline = saved;
            break;
        }
        if (newline) *newline = saved;
        line_start = newline ? newline + 1 : NULL;
    }

    if (!heading_line_start) {
        result.text = strdup(trimmed);
        result.prompts = NULL;
        result.prompt_count = 0;
        free(copy);
        return result;
    }

    /* text = everything before heading */
    *heading_line_start = '\0';
    result.text = strdup(trim(trimmed));
    *heading_line_start = '#'; /* restore (not strictly needed) */

    /* Parse bullets from the section after heading */
    result.prompts = malloc(3 * sizeof(char*));
    result.prompt_count = 0;

    char *line = after_heading;
    while (line && *line && result.prompt_count < 3) {
        char *newline = strchr(line, '\n');
        char saved = 0;
        if (newline) { saved = *newline; *newline = '\0'; }

        char *label = parse_bullet(line);
        if (label) {
            result.prompts[result.prompt_count++] = label;
        }
        if (newline) *newline = saved;
        line = newline ? newline + 1 : NULL;
    }

    free(copy);
    return result;
}

static void free_gen_result(GenResult *r) {
    free(r->text);
    for (int i = 0; i < r->prompt_count; i++) free(r->prompts[i]);
    free(r->prompts);
}

/* ---------------------------------------------------------------------------
 * LLM provider  (OpenAI-compatible chat completions via libcurl)
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *base_url;
    const char *api_key_env;
    const char *model_env;
    const char *default_model;
} ProviderConfig;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    /* Append to a dynamically growing buffer */
    char **buf = (char**)userp;
    size_t old_len = *buf ? strlen(*buf) : 0;
    *buf = realloc(*buf, old_len + total + 1);
    memcpy(*buf + old_len, contents, total);
    (*buf)[old_len + total] = '\0';
    return total;
}

/* Extract choices[0].message.content from a chat completions JSON response.
 * Uses simple string search — robust enough for this well-defined format. */
static char *extract_content(const char *json_response) {
    const char *p = json_response;
    /* Find "content" inside a message object */
    while ((p = strstr(p, "\"content\""))) {
        p += 9; /* skip "content" */
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (*p == ':') {
            p++;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (*p == '"') {
                p++; /* skip opening quote */
                /* Find the closing quote (handle escaped quotes) */
                const char *start = p;
                const char *end = p;
                while (*end) {
                    if (*end == '\\' && end[1]) {
                        end += 2;
                    } else if (*end == '"') {
                        break;
                    } else {
                        end++;
                    }
                }
                size_t len = end - start;
                char *content = malloc(len + 1);
                /* Unescape basic sequences */
                size_t j = 0;
                for (const char *c = start; c < end; c++) {
                    if (*c == '\\' && c + 1 < end) {
                        c++;
                        switch (*c) {
                            case 'n': content[j++] = '\n'; break;
                            case 'r': content[j++] = '\r'; break;
                            case 't': content[j++] = '\t'; break;
                            case '"': content[j++] = '"'; break;
                            case '\\': content[j++] = '\\'; break;
                            default: content[j++] = *c; break;
                        }
                    } else {
                        content[j++] = *c;
                    }
                }
                content[j] = '\0';
                return content;
            }
        }
    }
    return strdup("");
}

static int call_chat_completions(
    const char *base_url,
    const char *api_key,
    const char *model,
    const char *prompt,
    GenResult *out
) {
    char *esc_prompt = json_escape(prompt);
    size_t body_len = strlen(SYSTEM_PROMPT) + strlen(esc_prompt) + strlen(model) + 256;
    char *body = malloc(body_len);
    snprintf(body, body_len,
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}]}",
        model, SYSTEM_PROMPT, esc_prompt);
    free(esc_prompt);

    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", base_url);

    char auth_header[1024];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return -1; }

    char *response = NULL;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        free(response);
        return -1;
    }
    if (http_code != 200) {
        fprintf(stderr, "Chat completions error: %ld %s\n", http_code, response ? response : "");
        free(response);
        return (int)http_code;
    }

    char *content = extract_content(response ? response : "");
    free(response);
    *out = parse_markdown_content(content);
    free(content);
    return 0;
}

static int generate_text(const char *prompt, GenResult *out) {
    const char *name = env_or("AI_PROVIDER", "openrouter");

    ProviderConfig cfg;
    if (iequals(name, "openai")) {
        cfg = (ProviderConfig){"https://api.openai.com/v1", "OPENAI_API_KEY", "OPENAI_MODEL", "gpt-4o-mini"};
    } else {
        if (!iequals(name, "openrouter"))
            fprintf(stderr, "Unknown AI_PROVIDER \"%s\", falling back to openrouter\n", name);
        cfg = (ProviderConfig){"https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "OPENROUTER_MODEL", "openai/gpt-oss-20b"};
    }

    return call_chat_completions(
        cfg.base_url,
        env_or(cfg.api_key_env, ""),
        env_or(cfg.model_env, cfg.default_model),
        prompt,
        out
    );
}

/* ---------------------------------------------------------------------------
 * MIME types
 * ----------------------------------------------------------------------- */

static const char *content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (iequals(ext, "html")) return "text/html";
    if (iequals(ext, "js"))   return "application/javascript";
    if (iequals(ext, "css"))  return "text/css";
    if (iequals(ext, "json")) return "application/json";
    if (iequals(ext, "svg"))  return "image/svg+xml";
    if (iequals(ext, "png"))  return "image/png";
    if (iequals(ext, "ico"))  return "image/x-icon";
    if (iequals(ext, "woff")) return "font/woff";
    if (iequals(ext, "woff2")) return "font/woff2";
    return "application/octet-stream";
}

/* ---------------------------------------------------------------------------
 * HTTP request handling
 * ----------------------------------------------------------------------- */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return buf;
}

/* Send an HTTP response */
static void send_response(SOCKET client, int status, const char *content_type_val,
                          const char *body, size_t body_len) {
    const char *status_text = "OK";
    switch (status) {
        case 200: status_text = "OK"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 429: status_text = "Too Many Requests"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 502: status_text = "Bad Gateway"; break;
    }

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Security-Policy: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type_val, body_len, CSP_HEADER);

    send(client, header, hlen, 0);
    send(client, body, body_len, 0);
}

static void send_json(SOCKET client, int status, const char *json_str) {
    send_response(client, status, "application/json", json_str, strlen(json_str));
}

static void send_error(SOCKET client, int status, const char *msg) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    send_json(client, status, buf);
}

/* Extract the "prompt" field value from a JSON body (simple search). */
static char *extract_prompt(const char *body) {
    const char *p = strstr(body, "\"prompt\"");
    if (!p) return NULL;
    p += 8;
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (*p != '"') return NULL;
    p++;
    const char *start = p;
    const char *end = p;
    while (*end) {
        if (*end == '\\' && end[1]) { end += 2; }
        else if (*end == '"') { break; }
        else { end++; }
    }
    size_t len = end - start;
    char *prompt = malloc(len + 1);
    size_t j = 0;
    for (const char *c = start; c < end; c++) {
        if (*c == '\\' && c + 1 < end) {
            c++;
            switch (*c) {
                case 'n': prompt[j++] = '\n'; break;
                case 't': prompt[j++] = '\t'; break;
                case '"': prompt[j++] = '"'; break;
                case '\\': prompt[j++] = '\\'; break;
                default: prompt[j++] = *c; break;
            }
        } else {
            prompt[j++] = *c;
        }
    }
    prompt[j] = '\0';
    return prompt;
}

/* Build the JSON response for a GenResult. Caller frees. */
static char *build_gen_json(GenResult *r) {
    char *esc_text = json_escape(r->text ? r->text : "");
    size_t total = strlen(esc_text) + 32;
    for (int i = 0; i < r->prompt_count; i++) {
        char *e = json_escape(r->prompts[i]);
        total += strlen(e) + 4;
        free(e);
    }

    char *json = malloc(total + 64);
    int pos = snprintf(json, total + 64, "{\"text\":\"%s\",\"prompts\":[", esc_text);
    free(esc_text);

    for (int i = 0; i < r->prompt_count; i++) {
        char *e = json_escape(r->prompts[i]);
        pos += snprintf(json + pos, total + 64 - pos, "%s\"%s\"", (i > 0 ? "," : ""), e);
        free(e);
    }
    pos += snprintf(json + pos, total + 64 - pos, "]}");

    return json;
}

/* Handle a single HTTP request */
static void handle_request(SOCKET client, const char *dist_path) {
    char buf[BUFSZ];
    int received = recv(client, buf, BUFSZ - 1, 0);
    if (received <= 0) return;
    buf[received] = '\0';

    /* Parse request line */
    char method[16], path[1024];
    method[0] = path[0] = '\0';
    sscanf(buf, "%15s %1023s", method, path);

    /* Find body (after \r\n\r\n) */
    char *body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/generate") == 0) {
        /* Per-IP rate limiting (20 req/min) */
        if (rate_limited("local")) {
            send_error(client, 429, "Too many requests. Please slow down.");
            return;
        }

        char *prompt = extract_prompt(body);
        if (!prompt || !*trim(prompt)) {
            send_error(client, 400, "Prompt is required");
            free(prompt);
            return;
        }
        if (strlen(prompt) > MAX_PROMPT_LENGTH) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Prompt is too long (max %d characters).", MAX_PROMPT_LENGTH);
            send_error(client, 400, msg);
            free(prompt);
            return;
        }

        GenResult result = {0};
        int rc = generate_text(prompt, &result);
        free(prompt);

        if (rc == 0) {
            char *json = build_gen_json(&result);
            send_json(client, 200, json);
            free(json);
            free_gen_result(&result);
        } else {
            /* Forward the upstream HTTP status (matches TS reference behavior) */
            int status = (rc > 0) ? rc : 500;
            send_error(client, status,
                status >= 400 && status < 500
                    ? "Generation failed. Please try again."
                    : "Failed to generate text content.");
            free_gen_result(&result);
        }
        return;
    }

    /* Static file serving */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        char file_path[2048];
        const char *req_path = strcmp(path, "/") == 0 ? "/index.html" : path;

        snprintf(file_path, sizeof(file_path), "%s%s", dist_path, req_path);

        /* Prevent path traversal */
        if (strstr(req_path, "..") != NULL) {
            send_error(client, 400, "Bad request");
            return;
        }

        size_t file_len = 0;
        char *content = read_file(file_path, &file_len);
        if (content) {
            send_response(client, 200, content_type(file_path), content, file_len);
            free(content);
        } else {
            /* SPA fallback: serve index.html */
            snprintf(file_path, sizeof(file_path), "%s/index.html", dist_path);
            content = read_file(file_path, &file_len);
            if (content) {
                send_response(client, 200, "text/html", content, file_len);
                free(content);
            } else {
                send_error(client, 404, "Frontend not built. Run npm run build in frontend/");
            }
        }
        return;
    }

    send_error(client, 404, "Not found");
}

/* ---------------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    load_dotenv();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    PORT = atoi(env_or("PORT", "3000"));

    /* Resolve dist path relative to CWD (../frontend/dist from c/ directory) */
    char dist_path[1024];
    snprintf(dist_path, sizeof(dist_path), "../frontend/dist");

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        perror("socket");
        return 1;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)PORT);

    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        CLOSE_SOCKET(server);
        return 1;
    }

    if (listen(server, 16) < 0) {
        perror("listen");
        CLOSE_SOCKET(server);
        return 1;
    }

    printf("Server running on http://localhost:%d\n", PORT);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        SOCKET client = accept(server, (struct sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) continue;

        handle_request(client, dist_path);
        CLOSE_SOCKET(client);
    }

    CLOSE_SOCKET(server);
    curl_global_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
