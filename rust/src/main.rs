// Gridscape backend — Rust (axum)
//
// Serves the built frontend from ../frontend/dist and exposes
// POST /api/generate which calls an OpenAI-compatible chat completions
// endpoint and parses the markdown response into {text, prompts}.
//
// Run:  cargo run    (from this directory)

use axum::{
    extract::{ConnectInfo, State},
    http::{HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    routing::post,
    Json, Router,
};
use regex::Regex;
use serde::{Deserialize, Serialize};
use std::env;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::LazyLock;
use tower_http::services::{ServeDir, ServeFile};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

const MAX_PROMPT_LENGTH: usize = 2000;

static SYSTEM_PROMPT: &str = "You are an infinite spatial-knowledge-engine generator. Respond with markdown text about the user's topic: brief but impactful explanatory text. CRITICAL: wrap 2 to 4 key concepts or interesting terms as clickable markdown links in the exact format [Term](Term) with no spaces, for example 'Machine learning relies on [Supervised Learning](Supervised Learning) and [Neural Networks](Neural Networks).' Do not use bold or italics for those terms. End your response with a section headed exactly '## Explore further' containing exactly 3 bullet-point markdown links, each bullet like '- [A follow-up question](A follow-up question)'. Output only the markdown; no JSON, no code fences.";

static HEADING_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?im)^##\s+explore further\s*$").unwrap());
static BULLET_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$").unwrap());

// ---------------------------------------------------------------------------
// Markdown parser  (mirrors providers/contract.ts:parseMarkdownContent)
// ---------------------------------------------------------------------------

#[derive(Serialize)]
struct GenResult {
    text: String,
    prompts: Vec<String>,
}

fn parse_markdown_content(raw: &str) -> GenResult {
    let trimmed = raw.trim();
    if let Some(m) = HEADING_RE.find(trimmed) {
        let text = trimmed[..m.start()].trim().to_string();
        let section = &trimmed[m.end()..];
        let prompts: Vec<String> = BULLET_RE
            .captures_iter(section)
            .map(|c| c[1].trim().to_string())
            .take(3)
            .collect();
        GenResult { text, prompts }
    } else {
        GenResult {
            text: trimmed.to_string(),
            prompts: vec![],
        }
    }
}

// ---------------------------------------------------------------------------
// LLM provider  (OpenAI-compatible chat completions)
// ---------------------------------------------------------------------------

struct ProviderConfig {
    base_url: &'static str,
    api_key_env: &'static str,
    model_env: &'static str,
    default_model: &'static str,
}

fn providers() -> &'static [(&'static str, ProviderConfig)] {
    &[
        (
            "openrouter",
            ProviderConfig {
                base_url: "https://openrouter.ai/api/v1",
                api_key_env: "OPENROUTER_API_KEY",
                model_env: "OPENROUTER_MODEL",
                default_model: "openai/gpt-oss-20b",
            },
        ),
        (
            "openai",
            ProviderConfig {
                base_url: "https://api.openai.com/v1",
                api_key_env: "OPENAI_API_KEY",
                model_env: "OPENAI_MODEL",
                default_model: "gpt-4o-mini",
            },
        ),
    ]
}

#[derive(Deserialize)]
struct ChatResponse {
    choices: Option<Vec<ChatChoice>>,
}

#[derive(Deserialize)]
struct ChatChoice {
    message: Option<ChatMessage>,
}

#[derive(Deserialize)]
struct ChatMessage {
    content: Option<String>,
}

async fn generate_text(prompt: &str) -> Result<GenResult, (StatusCode, String)> {
    let name = env::var("AI_PROVIDER").unwrap_or_else(|_| "openrouter".into());
    let cfg = providers()
        .iter()
        .find(|(n, _)| *n == name.to_lowercase().as_str())
        .map(|(_, c)| c)
        .ok_or_else(|| {
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Unknown AI_PROVIDER \"{}\"", name),
            )
        })?;

    let api_key = env::var(cfg.api_key_env).unwrap_or_default();
    let model = env::var(cfg.model_env).unwrap_or_else(|_| cfg.default_model.into());

    let body = serde_json::json!({
        "model": model,
        "messages": [
            { "role": "system", "content": SYSTEM_PROMPT },
            { "role": "user", "content": prompt },
        ]
    });

    let client = reqwest::Client::new();
    let resp = client
        .post(format!("{}/chat/completions", cfg.base_url))
        .header("Authorization", format!("Bearer {}", api_key))
        .json(&body)
        .send()
        .await
        .map_err(|e| (StatusCode::BAD_GATEWAY, e.to_string()))?;

    if !resp.status().is_success() {
        let status = resp.status();
        let text = resp.text().await.unwrap_or_default();
        eprintln!("Chat completions error: {} {}", status, text);
        return Err((status, "LLM provider error".into()));
    }

    let data: ChatResponse = resp
        .json()
        .await
        .map_err(|e| (StatusCode::BAD_GATEWAY, e.to_string()))?;

    let content = data
        .choices
        .and_then(|c| c.into_iter().next())
        .and_then(|c| c.message)
        .and_then(|m| m.content)
        .unwrap_or_default();

    Ok(parse_markdown_content(&content))
}

// ---------------------------------------------------------------------------
// Rate limiter (simple, per-IP, 20 req/min)
// ---------------------------------------------------------------------------

use parking_lot::Mutex;
use std::collections::{HashMap, VecDeque};
use std::time::Instant;

#[derive(Default)]
struct RateLimiter {
    log: Mutex<HashMap<String, VecDeque<Instant>>>,
}

impl RateLimiter {
    fn check(&self, ip: &str) -> bool {
        let mut log = self.log.lock();
        let now = Instant::now();
        let entry = log.entry(ip.to_string()).or_default();
        while entry.front().map_or(false, |t| now.duration_since(*t).as_secs() >= 60) {
            entry.pop_front();
        }
        if entry.len() >= 20 {
            return true;
        }
        entry.push_back(now);
        false
    }
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

#[derive(Deserialize)]
struct GenerateBody {
    prompt: String,
}

#[derive(Clone)]
struct AppState {
    limiter: std::sync::Arc<RateLimiter>,
}

async fn handle_generate(
    State(state): State<AppState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    body: Result<Json<GenerateBody>, axum::extract::rejection::JsonRejection>,
) -> Response {
    let Json(body) = match body {
        Ok(b) => b,
        Err(_) => {
            return (
                StatusCode::BAD_REQUEST,
                Json(serde_json::json!({"error": "Prompt is required"})),
            )
                .into_response();
        }
    };

    // Per-IP rate limiting (20 req/min)
    let ip = addr.ip().to_string();
    if state.limiter.check(&ip) {
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
        return (
            StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"error": format!("Prompt is too long (max {} characters).", MAX_PROMPT_LENGTH)})),
        )
            .into_response();
    }

    match generate_text(&body.prompt).await {
        Ok(result) => Json(result).into_response(),
        Err((status, msg)) => {
            eprintln!("Generate error: {}", msg);
            // Forward the upstream HTTP status (matches TS reference behavior)
            let code = status.as_u16();
            let (resp_status, generic) = if code >= 400 && code < 500 {
                (status, "Generation failed. Please try again.")
            } else {
                (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    "Failed to generate text content.",
                )
            };
            (resp_status, Json(serde_json::json!({"error": generic}))).into_response()
        }
    }
}

// ---------------------------------------------------------------------------

fn dist_path() -> PathBuf {
    // CARGO_MANIFEST_DIR is the crate root at compile time — resolves
    // relative to source regardless of CWD or binary location.
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("frontend")
        .join("dist")
}

async fn csp_middleware(
    request: axum::extract::Request,
    next: axum::middleware::Next,
) -> Response {
    let mut response = next.run(request).await;
    response.headers_mut().insert(
        "Content-Security-Policy",
        HeaderValue::from_static(
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com data:; img-src 'self' data:; connect-src 'self'",
        ),
    );
    response
}

#[tokio::main]
async fn main() {
    let _ = dotenvy::dotenv();

    let port = env::var("PORT").unwrap_or_else(|_| "3000".into());
    let dist = dist_path();

    let serve_dir = ServeDir::new(&dist).fallback(ServeFile::new(dist.join("index.html")));

    let app = Router::new()
        .route("/api/generate", post(handle_generate))
        .fallback_service(serve_dir)
        .layer(axum::middleware::from_fn(csp_middleware))
        .with_state(AppState {
            limiter: std::sync::Arc::new(RateLimiter::default()),
        });

    let addr = format!("0.0.0.0:{}", port);
    eprintln!("Server running on http://localhost:{}", port);

    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    axum::serve(
        listener,
        app.into_make_service_with_connect_info::<SocketAddr>(),
    )
    .await
    .unwrap();
}
