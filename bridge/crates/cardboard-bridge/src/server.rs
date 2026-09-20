use std::sync::Arc;
use tiny_http::Server;
use crate::core::AppCore;
pub(super) mod handlers;
mod index;
pub(super) use index::ENDPOINT_INDEX;
/// REST control plane port (External -> bridge). Overridable for tests via
/// CARDBOARD_BRIDGE_PORT so parallel suites never collide.
const DEFAULT_PORT: u16 = 8567;
/// Binds 127.0.0.1:8567 and serves the endpoint index on a background thread.
/// Called once from main after the core is built; the loop never returns.
pub fn start(core: Arc<AppCore>) -> Result<(), String> {
    let port = std::env::var("CARDBOARD_BRIDGE_PORT")
        .ok()
        .and_then(|v| v.parse::<u16>().ok())
        .unwrap_or(DEFAULT_PORT);
    let addr = format!("127.0.0.1:{port}");
    let server = Server::http(&addr).map_err(|e| format!("control server bind {addr}: {e}"))?;
    core.push_log(format!("REST control plane on http://{addr}"));
    std::thread::spawn(move || {
        for request in server.incoming_requests() {
            handlers::handle(request, &core);
        }
    });
    Ok(())
}