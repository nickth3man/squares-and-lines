/*
 * Gridscape backend — C (raw sockets + libcurl) with stateful canvas management.
 *
 * The backend owns all domain logic: node model, spatial layout, versioning,
 * tree structure, and deletion cascades. In-memory canvas store.
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_SOCKET closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #define INVALID_SOCKET (-1)
  #define CLOSE_SOCKET close
#endif

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define MAX_PROMPT_LENGTH 2000
#define BUFSZ (1024 * 1024)
#define NODE_WIDTH 400.0
#define MAX_PROMPTS 3
#define MAX_VERSIONS 32
#define INITIAL_CAPACITY 8

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

static int PORT = 3000;

/* ---------------------------------------------------------------------------
 * String helpers
 * ----------------------------------------------------------------------- */

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0'; return s;
}

static bool iequals(const char *a, const char *b) {
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; a++; b++; }
    return *a == '\0' && *b == '\0';
}

static char *env_or(const char *key, const char *fallback) {
    const char *v = getenv(key); return (v && *v) ? (char*)v : (char*)fallback;
}

static char *json_escape(const char *s) {
    size_t len = 0;
    for (const char *p = s; *p; p++) { len += (*p=='"'||*p=='\\'||*p=='\n'||*p=='\r'||*p=='\t') ? 2 : 1; }
    char *out = malloc(len + 1); char *o = out;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"': *o++='\\'; *o++='"'; break; case '\\': *o++='\\'; *o++='\\'; break;
            case '\n': *o++='\\'; *o++='n'; break; case '\r': *o++='\\'; *o++='r'; break;
            case '\t': *o++='\\'; *o++='t'; break; default: *o++=*p;
        }
    }
    *o = '\0'; return out;
}

static char *str_dup(const char *s) { char *d = malloc(strlen(s)+1); strcpy(d, s); return d; }

static void gen_id(char *buf, size_t bufsz, const char *prefix) {
    snprintf(buf, bufsz, "%s-%lld-%d", prefix, (long long)(time(NULL) * 1000), rand() % 1000);
}

/* ---------------------------------------------------------------------------
 * .env loader
 * ----------------------------------------------------------------------- */

static void load_dotenv(void) {
    FILE *f = fopen(".env", "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = '\0'; char *key = trim(line); char *val = trim(eq + 1);
        if (*key == '\0' || *key == '#') continue;
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0]=='\"'&&val[vlen-1]=='\"') || (val[0]=='\''&&val[vlen-1]=='\''))) { val[vlen-1]='\0'; val++; }
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

/* ---------------------------------------------------------------------------
 * Markdown parser
 * ----------------------------------------------------------------------- */

static bool is_heading(const char *line) {
    char buf[256]; strncpy(buf, line, 255); buf[255]='\0'; char *t = trim(buf);
    if (t[0]!='#'||t[1]!='#') return false; t+=2;
    while (*t && isspace((unsigned char)*t)) t++;
    return iequals(t, "explore further");
}

static char *parse_bullet(const char *line) {
    char buf[1024]; strncpy(buf, line, 1023); buf[1023]='\0';
    char *p = buf; while (*p && isspace((unsigned char)*p)) p++;
    if (*p!='-' && *p!='*') return NULL; p++;
    if (!isspace((unsigned char)*p)) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p!='[') return NULL; p++;
    char *ls = p; while (*p && *p!=']') p++;
    if (*p!=']') return NULL; char *le = p; p++;
    if (*p!='(') return NULL;
    char *label = malloc(le - ls + 1); memcpy(label, ls, le-ls); label[le-ls]='\0';
    return trim(label);
}

#define MAX_LINKS 16
typedef struct { char *label; char *target; } InlineLink;
typedef struct { char *text; char **prompts; int prompt_count; InlineLink *links; int link_count; } GenResult;

static void extract_inline_links(const char *text, InlineLink **out_links, int *out_count) {
    *out_links = calloc(MAX_LINKS, sizeof(InlineLink));
    *out_count = 0;
    const char *p = text;
    while (*p && *out_count < MAX_LINKS) {
        const char *start = strchr(p, '[');
        if (!start) break;
        const char *label_end = strchr(start + 1, ']');
        if (!label_end || label_end[1] != '(') break;
        const char *target_end = strchr(label_end + 2, ')');
        if (!target_end) break;
        size_t label_len = (size_t)(label_end - start - 1);
        size_t target_len = (size_t)(target_end - label_end - 2);
        (*out_links)[*out_count].label = malloc(label_len + 1);
        (*out_links)[*out_count].target = malloc(target_len + 1);
        memcpy((*out_links)[*out_count].label, start + 1, label_len); (*out_links)[*out_count].label[label_len] = '\0';
        memcpy((*out_links)[*out_count].target, label_end + 2, target_len); (*out_links)[*out_count].target[target_len] = '\0';
        (*out_count)++;
        p = target_end + 1;
    }
}

static GenResult parse_markdown_content(const char *raw) {
    GenResult r = {0};
    char *copy = str_dup(raw); char *trimmed = trim(copy);
    char *heading_start = NULL, *after_heading = NULL;
    char *ls = trimmed;
    while (ls && *ls) {
        char *nl = strchr(ls, '\n'); char sv = 0; if (nl) { sv = *nl; *nl = '\0'; }
        if (is_heading(ls)) { heading_start = ls; after_heading = nl ? nl+1 : ls+strlen(ls); if (nl) *nl = sv; break; }
        if (nl) *nl = sv; ls = nl ? nl+1 : NULL;
    }
    if (!heading_start) {
        r.text = str_dup(trimmed);
        extract_inline_links(r.text, &r.links, &r.link_count);
        free(copy); return r;
    }
    *heading_start = '\0'; r.text = str_dup(trim(trimmed));
    r.prompts = malloc(MAX_PROMPTS * sizeof(char*)); r.prompt_count = 0;
    char *line = after_heading;
    while (line && *line && r.prompt_count < MAX_PROMPTS) {
        char *nl = strchr(line, '\n'); char sv = 0; if (nl) { sv = *nl; *nl = '\0'; }
        char *label = parse_bullet(line); if (label) r.prompts[r.prompt_count++] = label;
        if (nl) *nl = sv; line = nl ? nl+1 : NULL;
    }
    extract_inline_links(r.text, &r.links, &r.link_count);
    free(copy); return r;
}

static void free_gen_result(GenResult *r) {
    free(r->text); for (int i = 0; i < r->prompt_count; i++) free(r->prompts[i]);
    for (int i = 0; i < r->link_count; i++) { free(r->links[i].label); free(r->links[i].target); }
    free(r->prompts); free(r->links);
}

/* ---------------------------------------------------------------------------
 * LLM provider (libcurl)
 * ----------------------------------------------------------------------- */

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    char **buf = (char**)userp; size_t old = *buf ? strlen(*buf) : 0;
    *buf = realloc(*buf, old + size*nmemb + 1); memcpy(*buf+old, contents, size*nmemb); (*buf)[old+size*nmemb] = '\0';
    return size * nmemb;
}

static char *extract_content(const char *json_resp) {
    const char *p = json_resp;
    while ((p = strstr(p, "\"content\""))) {
        p += 9; while (*p && (*p==' '||*p=='\t')) p++;
        if (*p == ':') { p++; while (*p && (*p==' '||*p=='\t')) p++;
            if (*p == '"') { p++; const char *start = p, *end = p;
                while (*end) { if (*end=='\\'&&end[1]) end+=2; else if (*end=='"') break; else end++; }
                size_t len = end - start; char *c = malloc(len+1); size_t j=0;
                for (const char *s=start; s<end; s++) {
                    if (*s=='\\'&&s+1<end) { s++; switch(*s){case 'n':c[j++]='\n';break;case 'r':c[j++]='\r';break;case 't':c[j++]='\t';break;case '"':c[j++]='"';break;case '\\':c[j++]='\\';break;default:c[j++]=*s;break;} }
                    else c[j++]=*s;
                }
                c[j]='\0'; return c;
            }
        }
    }
    return str_dup("");
}

static bool call_llm(const char *prompt, GenResult *out) {
    char *esc = json_escape(prompt);
    const char *name = env_or("AI_PROVIDER", "openrouter");
    const char *base, *key_env, *model_env, *def_model;
    if (iequals(name, "openai")) { base="https://api.openai.com/v1"; key_env="OPENAI_API_KEY"; model_env="OPENAI_MODEL"; def_model="gpt-4o-mini"; }
    else { base="https://openrouter.ai/api/v1"; key_env="OPENROUTER_API_KEY"; model_env="OPENROUTER_MODEL"; def_model="openai/gpt-oss-20b"; }

    size_t blen = strlen(SYSTEM_PROMPT) + strlen(esc) + 256;
    char *body = malloc(blen);
    snprintf(body, blen, "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},{\"role\":\"user\",\"content\":\"%s\"}]}",
             env_or(model_env, def_model), SYSTEM_PROMPT, esc);
    free(esc);

    char url[512]; snprintf(url, sizeof(url), "%s/chat/completions", base);
    char auth[1024]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", env_or(key_env, ""));

    CURL *curl = curl_easy_init(); if (!curl) { free(body); return false; }
    char *resp = NULL; struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth);
    curl_easy_setopt(curl, CURLOPT_URL, url); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs); curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp); curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl); free(body);
    if (res != CURLE_OK || code != 200) { free(resp); return false; }
    char *content = extract_content(resp ? resp : ""); free(resp);
    *out = parse_markdown_content(content); free(content);
    return true;
}

/* ---------------------------------------------------------------------------
 * Canvas domain model
 * ----------------------------------------------------------------------- */

typedef struct {
    char *prompt, *text;
    char **prompts; int prompt_count;
    InlineLink *links; int link_count;
} NodeVer;

typedef struct {
    char id[64]; double x, y, width, height; char prompt[1024];
    char *text; char **prompts; int prompt_count;
    InlineLink *links; int link_count;
    char status[16]; int versionIndex; NodeVer versions[MAX_VERSIONS]; int version_count;
    char parentId[64];
} Node;

static InlineLink *clone_links(const InlineLink *source, int count) {
    if (count == 0) return NULL;
    InlineLink *copy = calloc((size_t)count, sizeof(InlineLink));
    for (int i = 0; i < count; i++) {
        copy[i].label = str_dup(source[i].label);
        copy[i].target = str_dup(source[i].target);
    }
    return copy;
}
static char **clone_prompts(char **source, int count) {
    if (count == 0) return NULL;
    char **copy = calloc((size_t)count, sizeof(char*));
    for (int i = 0; i < count; i++) copy[i] = str_dup(source[i]);
    return copy;
}


typedef struct { char id[64]; Node *nodes; int count, capacity; } Canvas;

static Canvas *g_canvases = NULL; static int g_canvas_count = 0, g_canvas_cap = 0;

static Canvas *find_canvas(const char *cid) {
    for (int i = 0; i < g_canvas_count; i++) if (strcmp(g_canvases[i].id, cid) == 0) return &g_canvases[i];
    return NULL;
}

static Canvas *create_canvas(void) {
    if (g_canvas_count >= g_canvas_cap) {
        g_canvas_cap = g_canvas_cap ? g_canvas_cap * 2 : 4;
        g_canvases = realloc(g_canvases, g_canvas_cap * sizeof(Canvas));
    }
    Canvas *c = &g_canvases[g_canvas_count++];
    gen_id(c->id, sizeof(c->id), "canvas");
    c->nodes = NULL; c->count = 0; c->capacity = 0;
    return c;
}

static Node *find_node(Canvas *c, const char *nid) {
    for (int i = 0; i < c->count; i++) if (strcmp(c->nodes[i].id, nid) == 0) return &c->nodes[i];
    return NULL;
}

static Node *add_node(Canvas *c) {
    if (c->count >= c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : INITIAL_CAPACITY;
        c->nodes = realloc(c->nodes, c->capacity * sizeof(Node));
    }
    return &c->nodes[c->count++];
}

/* Spatial layout */
static void compute_child_pos(Node *parent, Node *all, int count, double *ox, double *oy) {
    double newX = parent->x + parent->width + 400;
    double ioff = (rand() > RAND_MAX/2) ? 200 : -200;
    double newY = parent->y + ioff;
    double cardH = parent->height > 0 ? parent->height : 400;
    bool occ = true; double mult = 1, dir = (rand() > RAND_MAX/2) ? 1 : -1;
    while (occ) {
        occ = false;
        for (int i = 0; i < count; i++) {
            double nH = all[i].height > 0 ? all[i].height : 400;
            if (fabs(all[i].x + NODE_WIDTH/2 - (newX + NODE_WIDTH/2)) < NODE_WIDTH &&
                fabs(all[i].y + nH/2 - (newY + cardH/2)) < (nH + cardH)/2) { occ = true; break; }
        }
        if (occ) { newY = parent->y + ioff + cardH * mult * dir; dir *= -1; if (dir == 1) mult++; }
    }
    *ox = newX; *oy = newY;
}

/* Node JSON serialization */
static char *node_to_json(Node *n) {
    size_t sz = 1024 + strlen(n->id) + strlen(n->prompt) + (n->text ? strlen(n->text) : 0) + n->prompt_count * 128 + n->link_count * 256;
    char *esc_text = json_escape(n->text ? n->text : "");
    char *buf = malloc(sz + 8192); int pos = snprintf(buf, sz + 8192,
        "{\"id\":\"%s\",\"x\":%.0f,\"y\":%.0f,\"width\":%.0f,\"height\":%.0f,\"prompt\":\"%s\",\"text\":\"%s\",\"prompts\":[",
        n->id, n->x, n->y, n->width, n->height, n->prompt, esc_text);
    free(esc_text);
    for (int i = 0; i < n->prompt_count; i++) { char *e = json_escape(n->prompts[i]); pos += snprintf(buf+pos, sz+8192-pos, "%s\"%s\"", i ? "," : "", e); free(e); }
    pos += snprintf(buf+pos, sz+8192-pos, "],\"links\":[");
    for (int i = 0; i < n->link_count; i++) {
        char *el = json_escape(n->links[i].label), *et = json_escape(n->links[i].target);
        pos += snprintf(buf+pos, sz+8192-pos, "%s{\"label\":\"%s\",\"target\":\"%s\"}", i ? "," : "", el, et);
        free(el); free(et);
    }
    pos += snprintf(buf+pos, sz+8192-pos, "],\"status\":\"%s\",\"versionIndex\":%d,\"versions\":[", n->status, n->versionIndex);
    for (int i = 0; i < n->version_count; i++) {
        NodeVer *v = &n->versions[i]; char *ep = json_escape(v->prompt), *et = json_escape(v->text ? v->text : "");
        pos += snprintf(buf+pos, sz+8192-pos, "%s{\"prompt\":\"%s\",\"text\":\"%s\",\"prompts\":[", i ? "," : "", ep, et); free(ep); free(et);
        for (int j = 0; j < v->prompt_count; j++) { char *e = json_escape(v->prompts[j]); pos += snprintf(buf+pos, sz+8192-pos, "%s\"%s\"", j ? "," : "", e); free(e); }
        pos += snprintf(buf+pos, sz+8192-pos, "],\"links\":[");
        for (int j = 0; j < v->link_count; j++) {
            char *el = json_escape(v->links[j].label), *etarget = json_escape(v->links[j].target);
            pos += snprintf(buf+pos, sz+8192-pos, "%s{\"label\":\"%s\",\"target\":\"%s\"}", j ? "," : "", el, etarget);
            free(el); free(etarget);
        }
        pos += snprintf(buf+pos, sz+8192-pos, "]}");
    }
    pos += snprintf(buf+pos, sz+8192-pos, "]");
    if (n->parentId[0]) pos += snprintf(buf+pos, sz+8192-pos, ",\"parentId\":\"%s\"", n->parentId);
    snprintf(buf+pos, sz+8192-pos, "}");
    return buf;
}

/* ---------------------------------------------------------------------------
 * HTTP response helpers
 * ----------------------------------------------------------------------- */

static void send_response(SOCKET client, int status, const char *ct, const char *body, size_t blen) {
    const char *st = "OK";
    switch(status) { case 400: st="Bad Request"; break; case 404: st="Not Found"; break;
        case 429: st="Too Many Requests"; break; case 500: st="Internal Server Error"; break; }
    char hdr[1024];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Content-Security-Policy: %s\r\nConnection: close\r\n\r\n",
        status, st, ct, blen, CSP_HEADER);
    send(client, hdr, hl, 0); send(client, body, blen, 0);
}

static void send_json(SOCKET client, int status, const char *json) {
    send_response(client, status, "application/json", json, strlen(json));
}

static void send_error(SOCKET client, int status, const char *msg) {
    char buf[512]; int l = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    send_json(client, status, buf);
}

/* JSON field extraction helpers */
static char *extract_field(const char *body, const char *field) {
    char pattern[64]; snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(body, pattern); if (!p) return NULL;
    p += strlen(pattern); while (*p && (*p==' '||*p=='\t'||*p==':')) p++;
    if (*p != '"') return NULL; p++;
    const char *start = p, *end = p;
    while (*end) { if (*end=='\\'&&end[1]) end+=2; else if (*end=='"') break; else end++; }
    size_t len = end - start; char *val = malloc(len+1); size_t j=0;
    for (const char *s=start; s<end; s++) {
        if (*s=='\\'&&s+1<end) { s++; switch(*s){case 'n':val[j++]='\n';break;case 't':val[j++]='\t';break;case '"':val[j++]='"';break;case '\\':val[j++]='\\';break;default:val[j++]=*s;break;} }
        else val[j++]=*s;
    }
    val[j]='\0'; return val;
}

static double extract_num(const char *body, const char *field) {
    char pattern[64]; snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(body, pattern); if (!p) return 0;
    p += strlen(pattern); while (*p && (*p==' '||*p=='\t'||*p==':')) p++;
    return strtod(p, NULL);
}

/* ---------------------------------------------------------------------------
 * Rate limiter (simple, single-threaded)
 * ----------------------------------------------------------------------- */

#define RATE_MAX 20
typedef struct { char ip[64]; time_t ts[RATE_MAX]; int cnt; } RateBucket;
static RateBucket g_rate[64];

static bool rate_limited(const char *ip) {
    time_t now = time(NULL); RateBucket *b = NULL;
    for (int i = 0; i < 64; i++) if (g_rate[i].cnt > 0 && strcmp(g_rate[i].ip, ip) == 0) { b = &g_rate[i]; break; }
    if (!b) for (int i = 0; i < 64; i++) if (g_rate[i].cnt == 0) { b = &g_rate[i]; break; }
    if (!b) b = &g_rate[0];
    strncpy(b->ip, ip, 63); b->ip[63]='\0';
    int w = 0; for (int i = 0; i < b->cnt; i++) if (now - b->ts[i] < 60) b->ts[w++] = b->ts[i];
    b->cnt = w; if (b->cnt >= RATE_MAX) return true; b->ts[b->cnt++] = now; return false;
}

/* ---------------------------------------------------------------------------
 * Static file serving
 * ----------------------------------------------------------------------- */

static const char *content_type(const char *path) {
    const char *ext = strrchr(path, '.'); if (!ext) return "application/octet-stream"; ext++;
    if (iequals(ext,"html")) return "text/html"; if (iequals(ext,"js")) return "application/javascript";
    if (iequals(ext,"css")) return "text/css"; if (iequals(ext,"json")) return "application/json";
    if (iequals(ext,"svg")) return "image/svg+xml"; if (iequals(ext,"png")) return "image/png";
    return "application/octet-stream";
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(len+1); fread(buf, 1, len, f); buf[len]='\0'; fclose(f);
    if (out_len) *out_len = (size_t)len; return buf;
}

/* ---------------------------------------------------------------------------
 * URL path segment parser — fills segs[] with pointers into a mutable copy
 * Returns segment count.
 * ----------------------------------------------------------------------- */

static int parse_path_segs(const char *path, char segs[][128], int max_segs) {
    char tmp[2048]; strncpy(tmp, path, 2047); tmp[2047]='\0';
    int count = 0; char *p = tmp;
    while (*p == '/') p++; // skip leading /
    while (*p && count < max_segs) {
        char *next = strchr(p, '/');
        if (next) { *next = '\0'; strncpy(segs[count], p, 127); segs[count][127]='\0'; count++; p = next+1; }
        else { strncpy(segs[count], p, 127); segs[count][127]='\0'; count++; break; }
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Handle canvas API requests
 * ----------------------------------------------------------------------- */

static void handle_api(SOCKET client, const char *method, const char *path, const char *body) {
    char segs[8][128];
    int nseg = parse_path_segs(path, segs, 8);

    /* POST /api/canvas */
    if (strcmp(method, "POST") == 0 && nseg == 2 && strcmp(segs[0], "api")==0 && strcmp(segs[1], "canvas")==0) {
        Canvas *c = create_canvas();
        char json[256]; snprintf(json, sizeof(json), "{\"canvasId\":\"%s\"}", c->id);
        send_json(client, 200, json); return;
    }

    /* All other canvas routes need at least /api/canvas/{cid} */
    if (nseg < 3 || strcmp(segs[0], "api")!=0 || strcmp(segs[1], "canvas")!=0) {
        send_error(client, 404, "Not found"); return;
    }

    const char *cid = segs[2];
    Canvas *canvas = find_canvas(cid);
    if (!canvas) { send_error(client, 404, "Canvas not found"); return; }

    /* POST /api/canvas/:cid/generate */
    if (strcmp(method, "POST")==0 && nseg==4 && strcmp(segs[3], "generate")==0) {
        if (rate_limited("local")) { send_error(client, 429, "Too many requests. Please slow down."); return; }
        char *prompt = extract_field(body, "prompt");
        if (!prompt || !*trim(prompt)) { send_error(client, 400, "Prompt is required"); free(prompt); return; }
        if (strlen(prompt) > MAX_PROMPT_LENGTH) { send_error(client, 400, "Prompt too long"); free(prompt); return; }
        char *parentId = extract_field(body, "parentId");

        Node *node = add_node(canvas);
        memset(node, 0, sizeof(Node));
        gen_id(node->id, sizeof(node->id), "node");
        node->width = NODE_WIDTH; node->height = 0;
        if (parentId && parentId[0]) { strncpy(node->parentId, parentId, sizeof(node->parentId)-1); }
        if (parentId && parentId[0]) {
            Node *parent = find_node(canvas, parentId);
            if (parent) compute_child_pos(parent, canvas->nodes, canvas->count, &node->x, &node->y);
        }
        strcpy(node->status, "generating");
        GenResult gr;
        if (call_llm(prompt, &gr)) {
            node->text = gr.text[0] ? gr.text : str_dup("No text");
            node->prompts = gr.prompts; node->prompt_count = gr.prompt_count;
            node->links = gr.links; node->link_count = gr.link_count;
            strcpy(node->status, "ready");
            if (node->version_count < MAX_VERSIONS) {
                NodeVer *v = &node->versions[node->version_count++];
                v->prompt = str_dup(prompt); v->text = str_dup(node->text);
                v->prompts = clone_prompts(gr.prompts, gr.prompt_count); v->prompt_count = gr.prompt_count;
                v->links = clone_links(gr.links, gr.link_count); v->link_count = gr.link_count;
            }
        } else { strcpy(node->status, "error"); node->text = str_dup(""); node->prompts = NULL; node->prompt_count = 0; node->links = NULL; node->link_count = 0; }

        char *json = node_to_json(node);
        char *wrap = malloc(strlen(json) + 16);
        sprintf(wrap, "{\"node\":%s}", json);
        send_json(client, 200, wrap);
        free(wrap); free(json); free(prompt); free(parentId);
        return;
    }

    /* nseg >= 5 for /api/canvas/:cid/nodes/:nid/... */
    if (nseg < 5 || strcmp(segs[3], "nodes") != 0) { send_error(client, 404, "Not found"); return; }
    const char *nid = segs[4];

    /* POST /api/canvas/:cid/nodes/:nid/regenerate */
    if (strcmp(method, "POST")==0 && nseg==6 && strcmp(segs[5], "regenerate")==0) {
        if (rate_limited("local")) { send_error(client, 429, "Too many requests. Please slow down."); return; }
        Node *node = find_node(canvas, nid);
        if (!node) { send_error(client, 404, "Node not found"); return; }
        strcpy(node->status, "generating");
        GenResult gr;
        if (call_llm(node->prompt, &gr)) {
            char *text = gr.text[0] ? gr.text : str_dup("No text");
            if (node->version_count < MAX_VERSIONS) {
                NodeVer *v = &node->versions[node->version_count++];
                v->prompt = str_dup(node->prompt); v->text = str_dup(text);
                v->prompts = clone_prompts(gr.prompts, gr.prompt_count); v->prompt_count = gr.prompt_count;
                v->links = clone_links(gr.links, gr.link_count); v->link_count = gr.link_count;
            }
            node->versionIndex = node->version_count - 1;
            free(node->text); node->text = text;
            free(node->prompts); node->prompts = gr.prompts; node->prompt_count = gr.prompt_count;
            free(node->links); node->links = gr.links; node->link_count = gr.link_count;
            strcpy(node->status, "ready");
        } else strcpy(node->status, "error");
        char *json = node_to_json(node);
        char *wrap = malloc(strlen(json)+16); sprintf(wrap, "{\"node\":%s}", json);
        send_json(client, 200, wrap); free(wrap); free(json); return;
    }

    /* DELETE /api/canvas/:cid/nodes/:nid */
    if (strcmp(method, "DELETE")==0 && nseg==5) {
        /* Find node + descendants */
        bool *del = calloc(canvas->count, sizeof(bool));
        /* Mark target */
        int target = -1;
        for (int i = 0; i < canvas->count; i++) if (strcmp(canvas->nodes[i].id, nid)==0) { target=i; del[i]=true; break; }
        if (target < 0) { free(del); send_error(client, 404, "Node not found"); return; }
        /* Find descendants iteratively */
        bool changed = true;
        while (changed) { changed = false;
            for (int i = 0; i < canvas->count; i++) {
                if (!del[i] && canvas->nodes[i].parentId[0]) {
                    for (int j = 0; j < canvas->count; j++) {
                        if (del[j] && strcmp(canvas->nodes[i].parentId, canvas->nodes[j].id)==0) { del[i]=true; changed=true; break; }
                    }
                }
            }
        }
        /* Build deleted IDs JSON */
        size_t jsz = 256; char *json = malloc(jsz); int pos = 0;
        pos += snprintf(json+pos, jsz-pos, "{\"deletedIds\":[");
        int first = 1;
        for (int i = 0; i < canvas->count; i++) {
            if (del[i]) {
                while (pos + 128 > jsz) { jsz*=2; json=realloc(json, jsz); }
                pos += snprintf(json+pos, jsz-pos, "%s\"%s\"", first?"":",", canvas->nodes[i].id); first=0;
            }
        }
        pos += snprintf(json+pos, jsz-pos, "]}");
        /* Compact nodes array */
        int w = 0;
        for (int i = 0; i < canvas->count; i++) if (!del[i]) { if (w != i) canvas->nodes[w] = canvas->nodes[i]; w++; }
        canvas->count = w;
        send_json(client, 200, json); free(json); free(del); return;
    }

    /* PUT /api/canvas/:cid/nodes/:nid/version */
    if (strcmp(method, "PUT")==0 && nseg==6 && strcmp(segs[5], "version")==0) {
        Node *node = find_node(canvas, nid);
        if (!node) { send_error(client, 404, "Node not found"); return; }
        int vi = (int)extract_num(body, "versionIndex");
        node->versionIndex = vi;
        if (vi >= 0 && vi < node->version_count) {
            free(node->text); node->text = str_dup(node->versions[vi].text);
            free(node->prompts); node->prompts = NULL; node->prompt_count = 0;
            for (int j = 0; j < node->versions[vi].prompt_count; j++) {
                node->prompts = realloc(node->prompts, (node->prompt_count+1)*sizeof(char*));
                node->prompts[node->prompt_count++] = str_dup(node->versions[vi].prompts[j]);
            }
            free(node->links); node->links = clone_links(node->versions[vi].links, node->versions[vi].link_count);
            node->link_count = node->versions[vi].link_count;
        }
        char *json = node_to_json(node);
        char *wrap = malloc(strlen(json)+16); sprintf(wrap, "{\"node\":%s}", json);
        send_json(client, 200, wrap); free(wrap); free(json); return;
    }

    /* PUT /api/canvas/:cid/nodes/:nid/position */
    if (strcmp(method, "PUT")==0 && nseg==6 && strcmp(segs[5], "position")==0) {
        Node *node = find_node(canvas, nid);
        if (!node) { send_error(client, 404, "Node not found"); return; }
        const char *x_marker = strstr(body, "\"x\"");
        const char *y_marker = strstr(body, "\"y\"");
        if (!x_marker || !y_marker) { send_error(client, 400, "Position requires numeric x and y"); return; }
        node->x = extract_num(body, "x"); node->y = extract_num(body, "y");
        char *json = node_to_json(node); char *wrap = malloc(strlen(json)+16);
        sprintf(wrap, "{\"node\":%s}", json); send_json(client, 200, wrap);
        free(wrap); free(json); return;
    }

    /* PUT /api/canvas/:cid/nodes/:nid/measure */
    if (strcmp(method, "PUT")==0 && nseg==6 && strcmp(segs[5], "measure")==0) {
        Node *node = find_node(canvas, nid);
        if (node) node->height = extract_num(body, "height");
        send_json(client, 200, "{\"ok\":true}"); return;
    }

    send_error(client, 404, "Not found");
}

/* GET /api/canvas/:cid/nodes */
static void handle_get_nodes(SOCKET client, Canvas *canvas) {
    size_t jsz = 256; char *json = malloc(jsz); int pos = 0;
    pos += snprintf(json+pos, jsz-pos, "{\"nodes\":[");
    for (int i = 0; i < canvas->count; i++) {
        char *nj = node_to_json(&canvas->nodes[i]);
        while (pos + strlen(nj) + 4 > jsz) { jsz *= 2; json = realloc(json, jsz); }
        pos += snprintf(json+pos, jsz-pos, "%s%s", i>0?",":"", nj); free(nj);
    }
    pos += snprintf(json+pos, jsz-pos, "]}");
    send_json(client, 200, json); free(json);
}

/* ---------------------------------------------------------------------------
 * Main request handler
 * ----------------------------------------------------------------------- */

static void handle_request(SOCKET client, const char *dist_path) {
    char *buf = malloc(BUFSZ);
    int total = 0;
    /* Loop recv until we have full headers + body (based on Content-Length) */
    while (total < BUFSZ - 1) {
        int n = recv(client, buf + total, BUFSZ - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end) continue;
        int hdr_len = (int)(hdr_end - buf) + 4;
        int cl = 0;
        char *p = strstr(buf, "Content-Length:");
        if (p) cl = atoi(p + 15);
        if (total >= hdr_len + cl) break;
    }
    if (total <= 0) { free(buf); return; }
    buf[total] = '\0';

    char method[16], path[1024];
    method[0] = path[0] = '\0';
    sscanf(buf, "%15s %1023s", method, path);
    char *body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : "";

    /* API routes */
    if (strncmp(path, "/api/canvas", 11) == 0) {
        /* Special case: GET /api/canvas/:cid/nodes */
        char segs[8][128]; int nseg = parse_path_segs(path, segs, 8);
        if (strcmp(method, "GET")==0 && nseg==4 && strcmp(segs[3],"nodes")==0) {
            Canvas *c = find_canvas(segs[2]);
            if (c) handle_get_nodes(client, c);
            else send_error(client, 404, "Canvas not found");
        } else {
            handle_api(client, method, path, body);
        }
        free(buf); return;
    }

    /* Static file serving */
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        char fp[2048];
        const char *rp = strcmp(path, "/") == 0 ? "/index.html" : path;
        snprintf(fp, sizeof(fp), "%s%s", dist_path, rp);
        if (strstr(rp, "..") != NULL) { send_error(client, 400, "Bad request"); free(buf); return; }
        size_t flen = 0; char *content = read_file(fp, &flen);
        if (content) { send_response(client, 200, content_type(fp), content, flen); free(content); }
        else {
            snprintf(fp, sizeof(fp), "%s/index.html", dist_path);
            content = read_file(fp, &flen);
            if (content) { send_response(client, 200, "text/html", content, flen); free(content); }
            else send_error(client, 404, "Frontend not built");
        }
        free(buf); return;
    }
    send_error(client, 404, "Not found");
    free(buf);
}

/* ---------------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

static int server_main(void) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    load_dotenv(); curl_global_init(CURL_GLOBAL_DEFAULT);
    srand((unsigned)time(NULL));
    PORT = atoi(env_or("PORT", "3000"));

    const char *dist_path = "../frontend/dist";
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) { perror("socket"); return 1; }
    int opt = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((unsigned short)PORT);
    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server, 16) < 0) { perror("listen"); return 1; }
    printf("Server running on http://localhost:%d\n", PORT); fflush(stdout);

    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        SOCKET client = accept(server, (struct sockaddr*)&ca, &cl);
        if (client == INVALID_SOCKET) continue;
        handle_request(client, dist_path);
        CLOSE_SOCKET(client);
    }
    CLOSE_SOCKET(server); curl_global_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

#ifdef GRIDSCAPE_CONTRACT_TEST
int main(void) {
    GenResult result = parse_markdown_content("Text with [Term](Term).\n\n## Explore further\n\n- [Prompt](Prompt)");
    int ok = strcmp(result.text, "Text with [Term](Term).") == 0 && result.prompt_count == 1 && strcmp(result.prompts[0], "Prompt") == 0 && result.link_count == 1 && strcmp(result.links[0].label, "Term") == 0 && strcmp(result.links[0].target, "Term") == 0;
    free_gen_result(&result);
    return ok ? 0 : 1;
}
#else
int main(void) { return server_main(); }
#endif
