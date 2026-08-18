//! REST control-plane bootstrap: resolve the port, bind the loopback HTTP
//! server and hand each request to `handlers::handle`. All endpoint logic
//! lives in `handlers`; the index page text lives in `index`.

use std::sync::Arc;

use tiny_http::Server;

use crate::core::AppCore;

pub(super) mod handlers;
mod index;

pub(super) use index::ENDPOINT_INDEX;

/// Loopback control-plane port (overridable with CARDBOARD_BRIDGE_PORT).
const DEFAULT_PORT: u16 = 8567;

/// Start the REST server on its own thread and return once it is bound.
/// Binding happens synchronously so a bind failure (e.g. port already taken)
/// is reported here rather than on some background thread.
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