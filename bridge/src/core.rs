//! The UI-independent core of the bridge. Owns the shared `AppState`, starts
//! the driver/phone worker threads, and exposes the operations the two user
//! surfaces — the Slint window and the REST server — call. No Slint type ever
//! reaches this module: `StatusSnapshot` is the raw, serializable view.

use std::ops::Deref;
use std::sync::{Arc, Mutex};

use crate::app::{AppState, SharedState};
use crate::net::{self, EncoderChoice, DRIVER_DISCOVERY_PORT, VIDEO_PORT};

/// Version read from Cargo.toml at compile time.
pub const APP_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Default stream settings used when a request omits a field.
/// Order: (width, height, fps, bitrate_mbps).
pub const APPLIED_DEFAULTS: (i32, i32, i32, i32) = (2880, 1620, 60, 20);

/// A plain, serializable snapshot of the bridge's live state; what both the UI
/// and the REST API render. Field *order is part of the wire contract* (the
/// harness diffs the serialized JSON byte-for-byte) so do not reorder these.
#[derive(Debug, Clone, serde::Serialize)]
pub struct StatusSnapshot {
    pub app_version: String,
    pub driver_connected: bool,
    pub encoder_active: bool,
    pub encoder_name: String,
    pub phone_connected: bool,
    pub phone_ip: String,
    pub stream_fps: i32,
    pub latency_ms: i32,
    pub packets_total: u64,
    pub gyro_fps: i32,
    pub hand_fps: i32,
    pub hands_detected: i32,
}

impl From<&AppState> for StatusSnapshot {
    fn from(s: &AppState) -> Self {
        Self {
            app_version: APP_VERSION.to_string(),
            driver_connected: s.driver_connected,
            encoder_active: s.encoder_active,
            encoder_name: s.encoder_name.clone(),
            phone_connected: s.phone_connected,
            phone_ip: s.phone_ip.clone(),
            stream_fps: s.stream_fps,
            latency_ms: s.latency_ms,
            packets_total: s.packets_total,
            gyro_fps: s.gyro_fps,
            hand_fps: s.hand_fps,
            hands_detected: s.hands_detected,
        }
    }
}

/// The settings that were just pushed to the driver, echoed back by the REST
/// API so the client can confirm exactly what was applied.
#[derive(Debug, Clone, serde::Serialize)]
pub struct AppliedSettings {
    pub width: i32,
    pub height: i32,
    pub fps: i32,
    pub bitrate_mbps: i32,
    pub encoder: String,
}

/// The bridge's brain, independent of any UI. `AppCore` is cheap to clone —
/// each clone shares the same `Arc<Mutex<AppState>>`, so the UI, the REST
/// server and the network threads all observe one consistent state.
#[derive(Clone)]
pub struct AppCore {
    state: SharedState,
}

impl AppCore {
    /// Build the core, lay down the startup log banner (so an operator reading
    /// the log immediately knows the topology), and kick off the driver +
    /// phone control-plane loops.
    pub fn new() -> Arc<Self> {
        let state: SharedState = Arc::new(Mutex::new(AppState::default()));
        {
            let mut s = state.lock().expect("state lock");
            s.push_log(format!("bridge v{APP_VERSION}"));
            s.push_log(format!(
                "driver discovery: sending BRIDGE_HELLO on udp {DRIVER_DISCOVERY_PORT} (waits for BRIDGE_ACK protocol support)"
            ));
            s.push_log(format!("video stays direct: PC -> phone on udp {VIDEO_PORT}"));
        }

        net::driver::spawn(state.clone());
        net::phone::spawn(state.clone());

        Arc::new(Self { state })
    }

    /// Append a line to the shared ring log. Best-effort: a poisoned lock just
    /// drops the line rather than killing the calling thread.
    pub fn push_log(&self, line: String) {
        if let Ok(mut s) = self.state.lock() {
            s.push_log(line);
        }
    }

    /// Push stream settings to the driver (CARDBOARD_CAP + BRIDGE_CFG over
    /// UDP), then record the applied encoder and the log line.
    pub fn apply_settings(
        &self,
        width: i32,
        height: i32,
        fps: i32,
        bitrate_mbps: i32,
        encoder: &str,
    ) -> AppliedSettings {
        let choice = EncoderChoice::from_name(encoder);
        net::driver::send_config(width, height, fps, bitrate_mbps, choice);

        if let Ok(mut s) = self.state.lock() {
            s.encoder_name = choice.as_str().to_string();
            // Encoding only makes sense while the driver is present; if it is,
            // mark the encoder live again so a re-apply re-activates it.
            if s.driver_connected {
                s.encoder_active = true;
            }
            s.push_log(format!(
                "config applied: {width}x{height} @{fps}fps {bitrate_mbps}mbps encoder={}",
                choice.as_str()
            ));
        }

        AppliedSettings {
            width,
            height,
            fps,
            bitrate_mbps,
            encoder: choice.as_str().to_string(),
        }
    }

    /// Live status snapshot: a single read of the shared state.
    pub fn status(&self) -> StatusSnapshot {
        let s = self.state.lock().expect("state lock");
        StatusSnapshot::from(s.deref())
    }

    /// Newest-first log lines. The REST/UI consumers reverse the ring so the
    /// most recent entry is always first.
    pub fn logs(&self, n: usize) -> Vec<String> {
        let s = self.state.lock().expect("state lock");
        s.log.iter().rev().take(n).cloned().collect()
    }
}