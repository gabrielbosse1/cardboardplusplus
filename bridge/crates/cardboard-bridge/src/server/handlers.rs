use std::io::Read;
use tiny_http::{Header, Method, Request, Response, StatusCode};
use crate::core::AppCore;
use crate::server::ENDPOINT_INDEX;
/// Cap on POST bodies (settings/debug). Anything larger is truncated, keeping
/// a malicious client from growing bridge memory.
const MAX_BODY_BYTES: u64 = 64 * 1024;
/// Routes one HTTP request to its handler by (method, path) and responds.
/// Unknown routes get a 404 pointing at the endpoint index. Runs on the
/// server.rs background thread, one request at a time.
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
        (Method::Post, "/settings") => apply_settings(&mut request, core),
        (Method::Get, "/debug") => ok_json(&debug_payload()),
        (Method::Post, "/debug") => set_debug(&mut request),
        _ => text(StatusCode(404), NOT_FOUND_BODY),
    };
    let _ = request.respond(response);
}
/// GET /preview shape: the driver's self-reported stream stats (from
/// BRIDGE_STATS) that power the bridge preview readout.
fn preview_payload(core: &AppCore) -> serde_json::Value {
    let s = core.status();
    serde_json::json!({
        "driver": {
            "fps": s.preview_driver_fps,
            "bitrate_kbps": s.preview_bitrate_kbps,
            "frames": s.preview_frames,
            "drops": s.preview_drops,
        },
    })
}
/// GET /health shape: liveness plus the baked app version. Used by
/// installers and smoke tests to confirm the bridge is up.
fn health_payload() -> serde_json::Value {
    serde_json::json!({
        "ok": true,
        "app_version": crate::core::APP_VERSION,
    })
}
/// GET /logs?n=50 shape: newest-first ring-log lines, default 50. `query` is
/// the raw URL query string; unparsable n falls back to the default.
fn logs_payload(core: &AppCore, query: Option<&str>) -> serde_json::Value {
    let n = query
        .and_then(|q| q.split('&').find_map(|part| part.strip_prefix("n=")))
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(50);
    serde_json::json!({ "logs": core.logs(n) })
}
/// POST /settings handler. Accepts a partial JSON object (missing fields keep
/// the live session values), pushes the merged set to the driver via
/// CARDBOARD_CAP + BRIDGE_CFG, and echoes what was sent. 400 on non-JSON.
fn apply_settings(request: &mut Request, core: &AppCore) -> Response<std::io::Cursor<Vec<u8>>> {
    use serde::Deserialize;
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
                &serde_json::json!({"error": "body must be JSON like {\"width\":2880,\"height\":1620,\"fps\":60,\"bitrate\":20,\"encoder\":\"gpu\"}"}),
            );
        }
    };
    let live = core.applied_settings();
    let applied = core.apply_settings(
        parsed.width.unwrap_or(live.0),
        parsed.height.unwrap_or(live.1),
        parsed.fps.unwrap_or(live.2),
        parsed.bitrate_mbps.unwrap_or(live.3),
        parsed.encoder.as_deref().unwrap_or(&live.4),
    );
    ok_json(&serde_json::json!({
        "ok": true,
        "sent": applied,
        "message": "CARDBOARD_CAP + BRIDGE_CFG pushed to the driver over UDP",
    }))
}
/// Reads up to MAX_BODY_BYTES from a POST body. Short reads just yield
/// fewer bytes; over-long bodies are cut, never buffered whole.
fn read_body_bytes(request: &mut Request) -> Vec<u8> {
    let mut body = Vec::new();
    let _ = request.as_reader().take(MAX_BODY_BYTES).read_to_end(&mut body);
    body
}
/// Builds a JSON response with CORS open (the UI and local tools call from
/// any origin). Serialization cannot fail on the json! payloads used here.
fn json<T: serde::Serialize>(code: StatusCode, body: &T) -> Response<std::io::Cursor<Vec<u8>>> {
    let bytes = serde_json::to_string(body).unwrap_or_else(|_| "{}".into());
    Response::from_data(bytes)
        .with_status_code(code)
        .with_header(Header::from_bytes(&b"Content-Type"[..], &b"application/json"[..]).unwrap())
        .with_header(
            Header::from_bytes(&b"Access-Control-Allow-Origin"[..], &b"*"[..]).unwrap(),
        )
}
/// Builds a plain-text response (endpoint index, 404 body).
fn text(code: StatusCode, body: &str) -> Response<std::io::Cursor<Vec<u8>>> {
    Response::from_data(body.to_string()).with_status_code(code)
}
/// 200 + JSON shorthand for the success paths.
fn ok_json<T: serde::Serialize>(body: &T) -> Response<std::io::Cursor<Vec<u8>>> {
    json(StatusCode(200), body)
}
const NOT_FOUND_BODY: &str = "{\"error\":\"not found — GET / for the endpoint index\"}";
/// GET /debug shape: current verbose-logging flag plus the toggle hint.
fn debug_payload() -> serde_json::Value {
    serde_json::json!({
        "debug": crate::app::debug_enabled(),
        "hint": "POST /debug {\"enabled\":true} to toggle verbose logging",
    })
}
/// POST /debug handler: flips verbose logging at runtime from
/// {"enabled":bool}. Takes effect immediately for all debug_log! calls.
/// 400 on non-JSON.
fn set_debug(request: &mut Request) -> Response<std::io::Cursor<Vec<u8>>> {
    use serde::Deserialize;
    #[derive(Deserialize)]
    struct DebugPayload {
        enabled: bool,
    }
    let body = read_body_bytes(request);
    let parsed: DebugPayload = match serde_json::from_slice(&body) {
        Ok(p) => p,
        Err(_) => {
            return json(
                StatusCode(400),
                &serde_json::json!({"error": "body must be JSON like {\"enabled\":true}"}),
            );
        }
    };
    crate::app::set_debug_enabled(parsed.enabled);
    ok_json(&serde_json::json!({
        "ok": true,
        "debug": parsed.enabled,
    }))
}