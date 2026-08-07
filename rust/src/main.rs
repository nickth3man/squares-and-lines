// Gridscape backend — Rust (axum) with stateful canvas management.
use axum::{
    extract::{Path, State},
    http::{HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    routing::{delete, get, post, put},
    Json, Router,
};
use parking_lot::Mutex as PLMutex;
use regex::Regex;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::env;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::{Arc, LazyLock};
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::Mutex;
use tower_http::services::{ServeDir, ServeFile};

const MAX_PROMPT_LENGTH: usize = 2000;
const NODE_WIDTH: f64 = 400.0;

static SYSTEM_PROMPT: &str = "You are an infinite spatial-knowledge-engine generator. Respond with markdown text about the user's topic: brief but impactful explanatory text. CRITICAL: wrap 2 to 4 key concepts or interesting terms as clickable markdown links in the exact format [Term](Term) with no spaces, for example 'Machine learning relies on [Supervised Learning](Supervised Learning) and [Neural Networks](Neural Networks).' Do not use bold or italics for those terms. End your response with a section headed exactly '## Explore further' containing exactly 3 bullet-point markdown links, each bullet like '- [A follow-up question](A follow-up question)'. Output only the markdown; no JSON, no code fences.";

static HEADING_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?im)^##\s+explore further\s*$").unwrap());
static BULLET_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$").unwrap());
static LINK_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\[([^\]]+)\]\(([^)]+)\)").unwrap());
static HTTP_CLIENT: LazyLock<reqwest::Client> = LazyLock::new(reqwest::Client::new);

#[derive(Clone, Serialize)]
struct InlineLink {
    label: String,
    target: String,
}

#[derive(Clone)]
struct GenResult {
    text: String,
    prompts: Vec<String>,
    links: Vec<InlineLink>,
}

fn parse_markdown_content(raw: &str) -> GenResult {
    let trimmed = raw.trim();
    let (text, section) = if let Some(heading) = HEADING_RE.find(trimmed) {
        (trimmed[..heading.start()].trim(), &trimmed[heading.end()..])
    } else {
        (trimmed, "")
    };
    let prompts = BULLET_RE
        .captures_iter(section)
        .map(|capture| capture[1].trim().to_string())
        .take(3)
        .collect();
    let links = LINK_RE
        .captures_iter(text)
        .map(|capture| InlineLink {
            label: capture[1].trim().to_string(),
            target: capture[2].trim().to_string(),
        })
        .collect();
    GenResult {
        text: text.to_string(),
        prompts,
        links,
    }
}

struct ProviderCfg {
    base_url: &'static str,
    api_key_env: &'static str,
    model_env: &'static str,
    default_model: &'static str,
}

fn providers() -> &'static [(&'static str, ProviderCfg)] {
    &[
        (
            "openrouter",
            ProviderCfg {
                base_url: "https://openrouter.ai/api/v1",
                api_key_env: "OPENROUTER_API_KEY",
                model_env: "OPENROUTER_MODEL",
                default_model: "openai/gpt-oss-20b",
            },
        ),
        (
            "openai",
            ProviderCfg {
                base_url: "https://api.openai.com/v1",
                api_key_env: "OPENAI_API_KEY",
                model_env: "OPENAI_MODEL",
                default_model: "gpt-4o-mini",
            },
        ),
    ]
}

async fn call_llm(prompt: &str) -> Result<GenResult, String> {
    let name = env::var("AI_PROVIDER").unwrap_or_else(|_| "openrouter".into());
    let config = providers()
        .iter()
        .find(|(provider, _)| *provider == name.to_lowercase())
        .map(|(_, config)| config)
        .ok_or_else(|| format!("Unknown AI_PROVIDER {name}"))?;
    let api_key = env::var(config.api_key_env).unwrap_or_default();
    let model = env::var(config.model_env).unwrap_or_else(|_| config.default_model.into());
    let body = serde_json::json!({
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ]
    });
    let response = HTTP_CLIENT
        .post(format!("{}/chat/completions", config.base_url))
        .header("Authorization", format!("Bearer {api_key}"))
        .json(&body)
        .send()
        .await
        .map_err(|error| error.to_string())?;
    if !response.status().is_success() {
        return Err(format!("LLM provider error ({})", response.status()));
    }
    let data: serde_json::Value = response.json().await.map_err(|error| error.to_string())?;
    let content = data["choices"][0]["message"]["content"]
        .as_str()
        .unwrap_or("");
    Ok(parse_markdown_content(content))
}

#[derive(Serialize, Clone)]
struct NodeVersion {
    prompt: String,
    text: String,
    prompts: Vec<String>,
    links: Vec<InlineLink>,
}

#[derive(Serialize, Clone)]
struct GridNodeData {
    id: String,
    x: f64,
    y: f64,
    width: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    height: Option<f64>,
    prompt: String,
    text: String,
    prompts: Vec<String>,
    links: Vec<InlineLink>,
    status: String,
    #[serde(rename = "versionIndex")]
    version_index: usize,
    versions: Vec<NodeVersion>,
    #[serde(rename = "parentId", skip_serializing_if = "Option::is_none")]
    parent_id: Option<String>,
}

struct Canvas {
    id: String,
    nodes: Vec<GridNodeData>,
}

type CanvasStore = Arc<Mutex<HashMap<String, Canvas>>>;

fn now_millis() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
}
fn gen_id(prefix: &str) -> String {
    format!("{prefix}-{}-{}", now_millis(), rand_u32() % 1000)
}
fn rand_u32() -> u32 {
    (now_millis() as u32)
        .wrapping_mul(1664525)
        .wrapping_add(1013904223)
}
fn rand_f64() -> f64 {
    (rand_u32() as f64) / (u32::MAX as f64)
}
fn abs(value: f64) -> f64 {
    if value < 0.0 {
        -value
    } else {
        value
    }
}

fn compute_child_position(parent: &GridNodeData, nodes: &[GridNodeData]) -> (f64, f64) {
    let new_x = parent.x + parent.width + 400.0;
    let initial_offset = if rand_f64() > 0.5 { 200.0 } else { -200.0 };
    let mut new_y = parent.y + initial_offset;
    let card_height = parent.height.unwrap_or(400.0);
    let mut occupied = true;
    let mut offset_multiplier = 1.0;
    let mut direction = if rand_f64() > 0.5 { 1.0 } else { -1.0 };
    while occupied {
        occupied = nodes.iter().any(|node| {
            let node_height = node.height.unwrap_or(400.0);
            abs(node.x + NODE_WIDTH / 2.0 - (new_x + NODE_WIDTH / 2.0)) < NODE_WIDTH
                && abs(node.y + node_height / 2.0 - (new_y + card_height / 2.0))
                    < (node_height + card_height) / 2.0
        });
        if occupied {
            new_y = parent.y + initial_offset + card_height * offset_multiplier * direction;
            direction *= -1.0;
            if direction == 1.0 {
                offset_multiplier += 1.0;
            }
        }
    }
    (new_x, new_y)
}

async fn canvas_generate(
    store: &CanvasStore,
    cid: &str,
    prompt: &str,
    parent_id: Option<&str>,
) -> Result<GridNodeData, String> {
    let mut canvases = store.lock().await;
    let canvas = canvases.get_mut(cid).ok_or("Canvas not found")?;
    let (x, y) = if let Some(parent_id) = parent_id {
        let parent = canvas
            .nodes
            .iter()
            .find(|node| node.id == parent_id)
            .ok_or("Parent not found")?;
        compute_child_position(parent, &canvas.nodes)
    } else {
        (0.0, 0.0)
    };
    let mut node = GridNodeData {
        id: gen_id("node"),
        x,
        y,
        width: NODE_WIDTH,
        height: None,
        prompt: prompt.to_string(),
        text: String::new(),
        prompts: vec![],
        links: vec![],
        status: "generating".into(),
        version_index: 0,
        versions: vec![],
        parent_id: parent_id.map(str::to_string),
    };
    canvas.nodes.push(node.clone());
    match call_llm(prompt).await {
        Ok(result) => {
            node.text = if result.text.is_empty() {
                "No text".into()
            } else {
                result.text
            };
            node.prompts = result.prompts;
            node.links = result.links;
            node.status = "ready".into();
            node.versions = vec![NodeVersion {
                prompt: prompt.to_string(),
                text: node.text.clone(),
                prompts: node.prompts.clone(),
                links: node.links.clone(),
            }];
        }
        Err(error) => {
            eprintln!("Generate error: {error}");
            node.status = "error".into();
        }
    }
    if let Some(stored) = canvas.nodes.iter_mut().find(|stored| stored.id == node.id) {
        *stored = node.clone();
    }
    Ok(node)
}

async fn canvas_regenerate(
    store: &CanvasStore,
    cid: &str,
    nid: &str,
) -> Result<GridNodeData, String> {
    let mut canvases = store.lock().await;
    let canvas = canvases.get_mut(cid).ok_or("Canvas not found")?;
    let node = canvas
        .nodes
        .iter_mut()
        .find(|node| node.id == nid)
        .ok_or("Node not found")?;
    node.status = "generating".into();
    let prompt = node.prompt.clone();
    match call_llm(&prompt).await {
        Ok(result) => {
            let text = if result.text.is_empty() {
                "No text".into()
            } else {
                result.text
            };
            let version = NodeVersion {
                prompt,
                text: text.clone(),
                prompts: result.prompts.clone(),
                links: result.links.clone(),
            };
            node.versions.push(version);
            node.version_index = node.versions.len() - 1;
            node.text = text;
            node.prompts = result.prompts;
            node.links = result.links;
            node.status = "ready".into();
        }
        Err(error) => {
            eprintln!("Regenerate error: {error}");
            node.status = "error".into();
        }
    }
    Ok(node.clone())
}

fn descendants(canvas: &Canvas, id: &str) -> Vec<String> {
    canvas
        .nodes
        .iter()
        .filter(|node| node.parent_id.as_deref() == Some(id))
        .flat_map(|node| {
            let mut result = vec![node.id.clone()];
            result.extend(descendants(canvas, &node.id));
            result
        })
        .collect()
}

#[derive(Clone)]
struct RateLimiter {
    log: Arc<PLMutex<HashMap<String, Vec<std::time::Instant>>>>,
}
impl RateLimiter {
    fn check(&self, ip: &str) -> bool {
        let mut log = self.log.lock();
        let now = std::time::Instant::now();
        let entry = log.entry(ip.to_string()).or_default();
        entry.retain(|time| now.duration_since(*time).as_secs() < 60);
        if entry.len() >= 20 {
            return true;
        }
        entry.push(now);
        false
    }
}

#[derive(Clone)]
struct AppState {
    store: CanvasStore,
    limiter: RateLimiter,
}

#[derive(Deserialize)]
struct GenerateBody {
    prompt: String,
    #[serde(rename = "parentId")]
    parent_id: Option<String>,
}
#[derive(Deserialize)]
struct VersionBody {
    #[serde(rename = "versionIndex")]
    version_index: usize,
}
#[derive(Deserialize)]
struct MeasureBody {
    height: f64,
}
#[derive(Deserialize)]
struct PositionBody {
    x: f64,
    y: f64,
}

async fn handle_create_canvas(State(state): State<AppState>) -> Response {
    let id = gen_id("canvas");
    state.store.lock().await.insert(
        id.clone(),
        Canvas {
            id: id.clone(),
            nodes: vec![],
        },
    );
    Json(serde_json::json!({"canvasId": id})).into_response()
}

async fn handle_generate(
    State(state): State<AppState>,
    Path(cid): Path<String>,
    body: Result<Json<GenerateBody>, axum::extract::rejection::JsonRejection>,
) -> Response {
    let Json(body) = match body {
        Ok(body) => body,
        Err(_) => {
            return (
                StatusCode::BAD_REQUEST,
                Json(serde_json::json!({"error": "Prompt is required"})),
            )
                .into_response()
        }
    };
    if state.limiter.check("global") {
        return (
            StatusCode::TOO_MANY_REQUESTS,
            Json(serde_json::json!({"error": "Too many requests. Please slow down."})),
        )
            .into_response();
    }
    if body.prompt.trim().is_empty() {
        return (
            StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"error": "Prompt is required"})),
        )
            .into_response();
    }
    if body.prompt.len() > MAX_PROMPT_LENGTH {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error": format!("Prompt is too long (max {MAX_PROMPT_LENGTH} characters.")}))).into_response();
    }
    match canvas_generate(&state.store, &cid, &body.prompt, body.parent_id.as_deref()).await {
        Ok(node) => Json(serde_json::json!({"node": node})).into_response(),
        Err(error) if error == "Canvas not found" => (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Canvas not found"})),
        )
            .into_response(),
        Err(error) => {
            eprintln!("Generate error: {error}");
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(serde_json::json!({"error": "Failed to generate text content."})),
            )
                .into_response()
        }
    }
}

async fn handle_regenerate(
    State(state): State<AppState>,
    Path((cid, nid)): Path<(String, String)>,
) -> Response {
    if state.limiter.check("global") {
        return (
            StatusCode::TOO_MANY_REQUESTS,
            Json(serde_json::json!({"error": "Too many requests. Please slow down."})),
        )
            .into_response();
    }
    match canvas_regenerate(&state.store, &cid, &nid).await {
        Ok(node) => Json(serde_json::json!({"node": node})).into_response(),
        Err(error) if error == "Canvas not found" || error == "Node not found" => (StatusCode::NOT_FOUND, Json(serde_json::json!({"error": if error == "Canvas not found" { "Canvas not found" } else { "Node not found" }}))).into_response(),
        Err(_) => (StatusCode::INTERNAL_SERVER_ERROR, Json(serde_json::json!({"error": "Failed to generate text content."}))).into_response(),
    }
}

async fn handle_delete(
    State(state): State<AppState>,
    Path((cid, nid)): Path<(String, String)>,
) -> Response {
    let mut canvases = state.store.lock().await;
    let Some(canvas) = canvases.get_mut(&cid) else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Canvas not found"})),
        )
            .into_response();
    };
    if !canvas.nodes.iter().any(|node| node.id == nid) {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Node not found"})),
        )
            .into_response();
    }
    let mut deleted = vec![nid.clone()];
    deleted.extend(descendants(canvas, &nid));
    canvas.nodes.retain(|node| !deleted.contains(&node.id));
    Json(serde_json::json!({"deletedIds": deleted})).into_response()
}

async fn handle_version(
    State(state): State<AppState>,
    Path((cid, nid)): Path<(String, String)>,
    Json(body): Json<VersionBody>,
) -> Response {
    let mut canvases = state.store.lock().await;
    let Some(canvas) = canvases.get_mut(&cid) else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Canvas not found"})),
        )
            .into_response();
    };
    let Some(node) = canvas.nodes.iter_mut().find(|node| node.id == nid) else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Node not found"})),
        )
            .into_response();
    };
    node.version_index = body.version_index;
    if let Some(version) = node.versions.get(body.version_index) {
        node.text = version.text.clone();
        node.prompts = version.prompts.clone();
        node.links = version.links.clone();
    }
    Json(serde_json::json!({"node": node})).into_response()
}

async fn handle_position(
    State(state): State<AppState>,
    Path((cid, nid)): Path<(String, String)>,
    Json(body): Json<PositionBody>,
) -> Response {
    let mut canvases = state.store.lock().await;
    let Some(canvas) = canvases.get_mut(&cid) else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Canvas not found"})),
        )
            .into_response();
    };
    let Some(node) = canvas.nodes.iter_mut().find(|node| node.id == nid) else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Node not found"})),
        )
            .into_response();
    };
    node.x = body.x;
    node.y = body.y;
    Json(serde_json::json!({"node": node})).into_response()
}

async fn handle_measure(
    State(state): State<AppState>,
    Path((cid, nid)): Path<(String, String)>,
    Json(body): Json<MeasureBody>,
) -> Response {
    let mut canvases = state.store.lock().await;
    let Some(canvas) = canvases.get_mut(&cid) else {
        return (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Canvas not found"})),
        )
            .into_response();
    };
    if let Some(node) = canvas.nodes.iter_mut().find(|node| node.id == nid) {
        node.height = Some(body.height);
    }
    Json(serde_json::json!({"ok": true})).into_response()
}

async fn handle_get_nodes(State(state): State<AppState>, Path(cid): Path<String>) -> Response {
    let canvases = state.store.lock().await;
    match canvases.get(&cid) {
        Some(canvas) => Json(serde_json::json!({"nodes": canvas.nodes})).into_response(),
        None => (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "Canvas not found"})),
        )
            .into_response(),
    }
}

async fn csp_middleware(request: axum::extract::Request, next: axum::middleware::Next) -> Response {
    let mut response = next.run(request).await;
    response.headers_mut().insert("Content-Security-Policy", HeaderValue::from_static("default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com data:; img-src 'self' data:; connect-src 'self'"));
    response
}

fn dist_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("frontend")
        .join("dist")
}

#[tokio::main]
async fn main() {
    let _ = dotenvy::dotenv();
    let port = env::var("PORT").unwrap_or_else(|_| "3000".into());
    let dist = dist_path();
    let serve_dir = ServeDir::new(&dist).fallback(ServeFile::new(dist.join("index.html")));
    let state = AppState {
        store: Arc::new(Mutex::new(HashMap::new())),
        limiter: RateLimiter {
            log: Arc::new(PLMutex::new(HashMap::new())),
        },
    };
    let app = Router::new()
        .route("/api/canvas", post(handle_create_canvas))
        .route("/api/canvas/{cid}/generate", post(handle_generate))
        .route(
            "/api/canvas/{cid}/nodes/{nid}/regenerate",
            post(handle_regenerate),
        )
        .route("/api/canvas/{cid}/nodes/{nid}", delete(handle_delete))
        .route("/api/canvas/{cid}/nodes/{nid}/version", put(handle_version))
        .route(
            "/api/canvas/{cid}/nodes/{nid}/position",
            put(handle_position),
        )
        .route("/api/canvas/{cid}/nodes/{nid}/measure", put(handle_measure))
        .route("/api/canvas/{cid}/nodes", get(handle_get_nodes))
        .fallback_service(serve_dir)
        .layer(axum::middleware::from_fn(csp_middleware))
        .with_state(state);
    let addr = format!("0.0.0.0:{port}");
    eprintln!("Server running on http://localhost:{port}");
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    axum::serve(
        listener,
        app.into_make_service_with_connect_info::<SocketAddr>(),
    )
    .await
    .unwrap();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parser_extracts_inline_links_and_prompts() {
        let result = parse_markdown_content(
            "Text with [Term](Term).\n\n## Explore further\n\n- [Prompt](Prompt)",
        );
        assert_eq!(result.text, "Text with [Term](Term).");
        assert_eq!(result.prompts, vec!["Prompt"]);
        assert_eq!(result.links[0].label, "Term");
        assert_eq!(result.links[0].target, "Term");
    }
}
