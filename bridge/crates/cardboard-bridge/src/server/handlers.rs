//! Endpoint implementations for the REST control plane. Each handler builds a
//! `Response` from the `AppCore` it shares with the UI, so the REST API is
//! just another view over the same state the window renders.

use std::io::Read;

use tiny_http::{Header, Method, Request, Response, StatusCode};

use crate::core::{AppCore, APPLIED_DEFAULTS};
use crate::server::ENDPOINT_INDEX;

/// Body size cap for requests (settings payloads are a few hundred bytes).
const MAX_BODY_BYTES: u64 = 64 * 1024;

/// Route one HTTP request and answer it. The path/query split happens once so
/// the matches are simple and the query string stays available for per-route
/// parameters such as `n=`.
pub fn handle(mut request: Request, core: &AppCore) {
    let method = request.method().clone();
    let (path, query) = match request.url().split_once('?') {
        Some((p, q)) => (p.to_string(), Some(q.to_string())),
        None => (request.url().to_string(), None),
    };

    let response = match (method, path.as_str()) {
        (_, "/") | (_, "/index") => text(StatusCode(200), ENDPOINT_INDEX),
        (Method::Get, "/health") => ok_json(&health_payload()),
        (Method::Get, "/status") => ok_json(&core.status()),
        (Method::Get, "/logs") => ok_json(&logs_payload(core, query.as_deref())),
        (Method::Get, "/preview") => ok_json(&preview_payload(core)),
        (Method::Post, "/preview") => set_preview(&mut request, core),
        (Method::Post, "/settings") => apply_settings(&mut request, core),
        _ => text(StatusCode(404), NOT_FOUND_BODY),
    };

    let _ = request.respond(response);
}

/// Current preview state: whether it is enabled and the driver's latest stats.
fn preview_payload(core: &AppCore) -> serde_json::Value {
    let s = core.status();
    serde_json::json!({
        "enabled": s.preview_enabled,
        "driver": {
            "fps": s.preview_driver_fps,
            "bitrate_kbps": s.preview_bitrate_kbps,
            "frames": s.preview_frames,
            "drops": s.preview_drops,
        },
    })
}

/// POST /preview — toggle the local preview. Accepts `{"enabled": bool}` to
/// switch the driver's localhost stream and/or `{"ffplay": true}` to open the
/// ffplay viewer window.
fn set_preview(request: &mut Request, core: &AppCore) -> Response<std::io::Cursor<Vec<u8>>> {
    use serde::Deserialize;

    #[derive(Deserialize)]
    struct PreviewPayload {
        enabled: Option<bool>,
        #[serde(rename = "ffplay")]
        open_ffplay: Option<bool>,
    }

    let body = read_body_bytes(request);
    let parsed: PreviewPayload = match serde_json::from_slice(&body) {
        Ok(p) => p,
        Err(_) => {
            return json(
                StatusCode(400),
                &serde_json::json!({"error": "body must be JSON like {\"enabled\":true} or {\"ffplay\":true}"}),
            );
        }
    };

    if let Some(enabled) = parsed.enabled {
        core.set_preview(enabled);
    }
    if parsed.open_ffplay.unwrap_or(false) {
        core.open_ffplay_preview();
    }

    ok_json(&serde_json::json!({
        "ok": true,
        "preview": preview_payload(core),
    }))
}

/// The endpoint index behaves like the page literal: served as raw text.
fn health_payload() -> serde_json::Value {
    serde_json::json!({
        "ok": true,
        "app_version": crate::core::APP_VERSION,
    })
}

/// `{"logs": [...]}` with the requested `n` (default 50) newest-first lines.
fn logs_payload(core: &AppCore, query: Option<&str>) -> serde_json::Value {
    let n = query
        .and_then(|q| q.strip_prefix("n="))
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(50);
    serde_json::json!({ "logs": core.logs(n) })
}

/// POST /settings — parse an optional-fields payload, push the merged config
/// to the driver, and echo exactly what was applied.
fn apply_settings(request: &mut Request, core: &AppCore) -> Response<std::io::Cursor<Vec<u8>>> {
    use serde::Deserialize;

    /// Optional settings fields; absent ones fall back to `APPLIED_DEFAULTS`.
    /// `bitrate` (mbps) has a dedicated wire alias matching the public API.
    #[derive(Deserialize, Default)]
    struct SettingsPayload {
        width: Option<i32>,
        height: Option<i32>,
        fps: Option<i32>,
        #[serde(rename = "bitrate")]
        bitrate_mbps: Option<i32>,
        encoder: Option<String>,
    }

    let body = read_body_bytes(request);
    let parsed: SettingsPayload = match serde_json::from_slice(&body) {
        Ok(p) => p,
        Err(_) => {
            return json(
                StatusCode(400),
                &serde_json::json!({"error": "body must be JSON like {\"width\":2880,\"height\":1620,\"fps\":60,\"bitrate\":20,\"encoder\":\"auto\"}"}),
            );
        }
    };

    let applied = core.apply_settings(
        parsed.width.unwrap_or(APPLIED_DEFAULTS.0),
        parsed.height.unwrap_or(APPLIED_DEFAULTS.1),
        parsed.fps.unwrap_or(APPLIED_DEFAULTS.2),
        parsed.bitrate_mbps.unwrap_or(APPLIED_DEFAULTS.3),
        parsed.encoder.as_deref().unwrap_or("auto"),
    );

    ok_json(&serde_json::json!({
        "ok": true,
        "sent": applied,
        "message": "CARDBOARD_CAP + BRIDGE_CFG pushed to the driver over UDP",
    }))
}

/// Read the request body up to `MAX_BODY_BYTES`; read errors are ignored so a
/// truncated upload just yields an empty body (and usually a 400 from the
/// parse above).
fn read_body_bytes(request: &mut Request) -> Vec<u8> {
    let mut body = Vec::new();
    let _ = request.as_reader().take(MAX_BODY_BYTES).read_to_end(&mut body);
    body
}

// -------------------------------------------------------- response helpers

/// JSON response with CORS, so a browser-based control page can read it too.
fn json<T: serde::Serialize>(code: StatusCode, body: &T) -> Response<std::io::Cursor<Vec<u8>>> {
    let bytes = serde_json::to_string(body).unwrap_or_else(|_| "{}".into());
    Response::from_data(bytes)
        .with_status_code(code)
        .with_header(Header::from_bytes(&b"Content-Type"[..], &b"application/json"[..]).unwrap())
        .with_header(
            Header::from_bytes(&b"Access-Control-Allow-Origin"[..], &b"*"[..]).unwrap(),
        )
}

/// Plain-text response with status; deliberately no Content-Type header, so
/// consumers get the raw body bytes exactly as authored (e.g. the index page).
fn text(code: StatusCode, body: &str) -> Response<std::io::Cursor<Vec<u8>>> {
    Response::from_data(body.to_string()).with_status_code(code)
}

/// Convenience for 200 + JSON.
fn ok_json<T: serde::Serialize>(body: &T) -> Response<std::io::Cursor<Vec<u8>>> {
    json(StatusCode(200), body)
}

/// Fallback for unknown routes (kept as raw text, matching `/`).
const NOT_FOUND_BODY: &str = "{\"error\":\"not found — GET / for the endpoint index\"}";