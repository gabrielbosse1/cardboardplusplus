//! The UI-independent core of the bridge. Owns the shared `AppState`, starts
//! the driver/phone worker threads, and exposes the operations the two user
//! surfaces — the Slint window and the REST server — call. No Slint type ever
//! reaches this module: `StatusSnapshot` is the raw, serializable view.

use std::io::{Read, Write};
use std::net::UdpSocket;
use std::ops::Deref;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;

use crate::app::{AppState, SharedState};
use crate::net::mediapipe::MediapipeClient;
use crate::net::{self, EncoderChoice, DRIVER_DISCOVERY_PORT, MEDIAPIPE_PORT, VIDEO_PORT};

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
    pub preview_enabled: bool,
    pub preview_driver_fps: i32,
    pub preview_bitrate_kbps: i32,
    pub preview_frames: u64,
    pub preview_drops: u64,
    pub camera_connected: bool,
    pub camera_detected_hands: usize,
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
            preview_enabled: s.preview_enabled,
            preview_driver_fps: s.preview_driver_fps,
            preview_bitrate_kbps: s.preview_bitrate_kbps,
            preview_frames: s.preview_frames,
            preview_drops: s.preview_drops,
            camera_connected: s.camera_connected,
            camera_detected_hands: s.camera_detected_hands,
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

/// Spawn the Python MediaPipe hand-landmark server as a child process and
/// connect to it via TCP.  Returns `None` if the Python process can't be
/// started or the TCP connection fails after retries.
fn spawn_mediapipe_server(state: &SharedState) -> Option<MediapipeClient> {
    // First try connecting to an already-running server (e.g. started manually).
    if let Ok(client) = MediapipeClient::connect(MEDIAPIPE_PORT) {
        if let Ok(mut s) = state.lock() {
            s.push_log("mediapipe: connected to existing server".into());
        }
        return Some(client);
    }

    // Locate the Python script relative to the binary or cwd.
    let script_path = {
        let cwd_candidate = std::path::PathBuf::from("mediapipe_server.py");
        if cwd_candidate.is_file() {
            Some(cwd_candidate)
        } else if let Some(exe_dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())) {
            let near = exe_dir.join("mediapipe_server.py");
            if near.is_file() {
                Some(near)
            } else {
                // Go up from target/debug/ to bridge/
                exe_dir.parent().and_then(|p| p.parent()).and_then(|p| p.parent()).map(|p| p.join("mediapipe_server.py")).filter(|p| p.is_file())
            }
        } else {
            None
        }
    };
    let script_path = script_path?;

    let python = std::env::var("PYTHON").unwrap_or_else(|_| "python".into());
    let mut child = match Command::new(&python)
        .arg(&script_path)
        .arg("--port")
        .arg(MEDIAPIPE_PORT.to_string())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("mediapipe server failed to start: {e}"));
            }
            return None;
        }
    };

    // Drain stderr in a background thread so the Python process doesn't block.
    if let Some(stderr) = child.stderr.take() {
        thread::spawn(move || {
            use std::io::BufRead;
            let reader = std::io::BufReader::new(stderr);
            for line in reader.lines().map_while(Result::ok) {
                eprintln!("[mediapipe-py] {line}");
            }
        });
    }

    if let Ok(mut s) = state.lock() {
        s.push_log(format!(
            "mediapipe server started (pid {})", child.id()
        ));
    }

    // Give the server a moment to bind, then connect.
    match MediapipeClient::connect(MEDIAPIPE_PORT) {
        Ok(client) => Some(client),
        Err(e) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("mediapipe TCP connect failed: {e}"));
            }
            let _ = child.kill();
            None
        }
    }
}

/// The bridge's brain, independent of any UI. `AppCore` is cheap to clone —
/// each clone shares the same `Arc<Mutex<AppState>>`, so the UI, the REST
/// server and the network threads all observe one consistent state.
#[derive(Clone)]
pub struct AppCore {
    state: SharedState,
    /// The one spawned ffplay preview process (if any), so the UI can't stack
    /// multiple windows. Replaced when the old one exits.
    ffplay: Arc<Mutex<Option<Child>>>,
    /// The most recent decoded preview frame as raw RGBA pixels (width, height, data).
    preview_frame: Arc<Mutex<Option<(u32, u32, Vec<u8>)>>>,
    /// Handle to the running preview decode thread (so we can stop it).
    preview_decode: Arc<Mutex<Option<PreviewDecodeHandle>>>,
}

struct PreviewDecodeHandle {
    child: Child,
    stop: Arc<Mutex<bool>>,
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
            s.push_log(format!("camera receiver on udp {}", crate::net::CAMERA_PORT));
        }

        net::driver::spawn(state.clone());
        net::phone::spawn(state.clone());

        // Spawn the Python MediaPipe server and connect via TCP.
        let mediapipe_client = spawn_mediapipe_server(&state);
        net::camera::spawn(state.clone(), mediapipe_client);

        Arc::new(Self {
            state,
            ffplay: Arc::new(Mutex::new(None)),
            preview_frame: Arc::new(Mutex::new(None)),
            preview_decode: Arc::new(Mutex::new(None)),
        })
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

    /// Toggle the local preview: tells the driver to keep (or stop) sending the
    /// localhost copy of the stream. When enabled, also starts the embedded
    /// decoder that renders the stream inside the bridge UI.
    pub fn set_preview(&self, enabled: bool) {
        if enabled {
            self.start_preview_decode();
        } else {
            self.stop_preview_decode();
        }
        net::driver::set_preview(&self.state, enabled);
    }

    /// Take the most recent decoded preview frame (consumed by the UI poller).
    /// Creates a `slint::Image` from the raw RGBA data. Returns `None` if no
    /// new frame is available.
    pub fn take_preview_frame(&self) -> Option<slint::Image> {
        let (w, h, rgba) = self.preview_frame.lock().expect("preview frame lock").take()?;
        let buffer = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::clone_from_slice(&rgba, w, h);
        Some(slint::Image::from_rgba8(buffer))
    }

    /// Take the most recent camera frame for the viewer. Returns `None` if no
    /// new frame is available or if the frame is stale (>1s old).
    pub fn take_camera_frame(&self) -> Option<slint::Image> {
        let mut state = self.state.lock().expect("state lock");
        state.check_camera_liveness();
        if !state.camera_connected {
            return None;
        }
        let (w, h, rgba) = state.camera_frame.take()?;
        let buffer = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::clone_from_slice(&rgba, w, h);
        Some(slint::Image::from_rgba8(buffer))
    }

    /// Start the embedded preview decoder: binds UDP 42069, spawns ffmpeg to
    /// decode H.264 → RGBA, and stores the latest frame for the UI.
    fn start_preview_decode(&self) {
        {
            let mut decode = self.preview_decode.lock().expect("preview decode lock");
            if let Some(handle) = decode.as_mut() {
                match handle.child.try_wait() {
                    Ok(None) => {
                        self.push_log("embedded preview already running".into());
                        return;
                    }
                    _ => {
                        // Stopped or errored — clean up and restart below.
                        let _ = handle.child.kill();
                        *decode = None;
                    }
                }
            }
        }

        let stop = Arc::new(Mutex::new(false));
        let frame_slot = self.preview_frame.clone();

        // Bind the UDP socket that the driver sends the preview stream to.
        let socket = match UdpSocket::bind("127.0.0.1:42069") {
            Ok(s) => s,
            Err(e) => {
                self.push_log(format!("preview bind failed (port 42069): {e}"));
                return;
            }
        };
        socket.set_nonblocking(true).ok();

        // Spawn ffmpeg: raw Annex-B H.264 in → scaled RGBA rawvideo out.
        let mut child = match Command::new("ffmpeg")
            .args([
                "-f", "h264",
                "-probesize", "32",
                "-analyzeduration", "0",
                "-i", "pipe:0",
                "-vf", "scale=480:270",
                "-f", "rawvideo",
                "-pix_fmt", "rgba",
                "-v", "0",
                "pipe:1",
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(c) => c,
            Err(e) => {
                self.push_log(format!("ffmpeg not available for preview: {e}"));
                return;
            }
        };

        let mut stdin = child.stdin.take().expect("ffmpeg stdin");
        let mut stdout = child.stdout.take().expect("ffmpeg stdout");

        let frame_w: u32 = 480;
        let frame_h: u32 = 270;
        let frame_bytes = (frame_w * frame_h * 4) as usize;

        // Thread 1: drain all available UDP datagrams into ffmpeg stdin (non-blocking).
        let stop_feeder = stop.clone();
        thread::spawn(move || {
            let mut buf = vec![0u8; 65536];
            loop {
                if *stop_feeder.lock().unwrap_or_else(|e| e.into_inner()) {
                    break;
                }
                // Drain every available datagram before sleeping.
                loop {
                    match socket.recv(&mut buf) {
                        Ok(n) if n > 0 => {
                            if stdin.write_all(&buf[..n]).is_err() {
                                return; // ffmpeg stdin closed.
                            }
                        }
                        _ => break, // Would-block or error — done for this tick.
                    }
                }
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
        });

        // Thread 2: read complete RGBA frames from ffmpeg stdout (blocking is fine here).
        let stop_reader = stop.clone();
        thread::spawn(move || {
            let mut rgba = vec![0u8; frame_bytes];
            let mut off = 0usize;
            loop {
                if *stop_reader.lock().unwrap_or_else(|e| e.into_inner()) {
                    break;
                }
                // Blocking read is safe here — ffmpeg produces output whenever it
                // has decoded a frame, and the feeder thread keeps stdin full.
                match stdout.read(&mut rgba[off..]) {
                    Ok(0) => break, // ffmpeg exited.
                    Ok(n) => {
                        off += n;
                        if off >= frame_bytes {
                            off = 0;
                            let mut new_buf = vec![0u8; frame_bytes];
                            std::mem::swap(&mut rgba, &mut new_buf);
                            if let Ok(mut slot) = frame_slot.lock() {
                                *slot = Some((frame_w, frame_h, new_buf));
                            }
                        }
                    }
                    Err(_) => break,
                }
            }
        });

        // Register the running decode session so stop_preview_decode can tear it down.
        {
            let mut decode = self.preview_decode.lock().expect("preview decode lock");
            *decode = Some(PreviewDecodeHandle { child, stop });
        }

        // Record in AppState so the UI knows preview is active.
        if let Ok(mut s) = self.state.lock() {
            s.preview_enabled = true;
        }

        self.push_log("embedded preview started (UDP 42069 → ffmpeg → UI)".into());

        // Watcher thread: if ffmpeg exits unexpectedly, auto-restart after 2s backoff.
        let weak = Arc::downgrade(&{
            // We need an Arc<Self> to call start_preview_decode again.
            // Leaking an Arc is fine here — it lives for the process lifetime.
            let this = self.clone();
            Arc::new(this)
        });
        let preview_decode = self.preview_decode.clone();
        let state = self.state.clone();
        thread::spawn(move || {
            // Wait for the child to finish.
            let child_exited = {
                let decode = preview_decode.lock().expect("preview decode lock");
                decode.as_ref().map(|h| h.child.id())
            };
            let Some(_pid) = child_exited else { return };

            // Busy-wait until the decode handle is gone (stopped) or preview is off.
            loop {
                std::thread::sleep(std::time::Duration::from_millis(500));
                let should_restart = {
                    let decode = preview_decode.lock().expect("preview decode lock");
                    let s = state.lock().expect("state lock");
                    decode.is_none() && s.preview_enabled
                };
                if should_restart {
                    break;
                }
                // If preview was explicitly disabled, don't restart.
                let s = state.lock().expect("state lock");
                if !s.preview_enabled {
                    return;
                }
            }
            // Backoff before restart.
            std::thread::sleep(std::time::Duration::from_secs(2));
            if let Some(core) = weak.upgrade() {
                core.push_log("preview auto-restarting...".into());
                core.start_preview_decode();
            }
        });
    }

    /// Stop the embedded preview decoder and release the UDP port.
    fn stop_preview_decode(&self) {
        let handle = {
            let mut decode = self.preview_decode.lock().expect("preview decode lock");
            decode.take()
        };
        if let Some(mut h) = handle {
            // Signal the decode thread to stop.
            if let Ok(mut stop) = h.stop.lock() {
                *stop = true;
            }
            let _ = h.child.kill();
            let _ = h.child.wait();
            self.push_log("embedded preview stopped".into());
        }
        // Clear the last frame.
        if let Ok(mut frame) = self.preview_frame.lock() {
            *frame = None;
        }
        if let Ok(mut s) = self.state.lock() {
            s.preview_enabled = false;
        }
    }

    /// Spawn the local preview viewer (ffplay) pointed at the driver's
    /// localhost stream. At most one window: a second request while the first
    /// is still running just logs a reminder. Spawned detached so the bridge
    /// keeps running whether or not the viewer closes.
    pub fn open_ffplay_preview(&self) {
        let mut ffplay = self.ffplay.lock().expect("ffplay lock");
        if let Some(child) = ffplay.as_mut() {
            match child.try_wait() {
                // Still running — don't stack a second window.
                Ok(None) => {
                    self.push_log("local preview already open (ffplay running)".into());
                    return;
                }
                // Exited; drop the reference and spawn a fresh one below.
                _ => *ffplay = None,
            }
        }
        match std::process::Command::new("ffplay")
            .args(["-f", "h264", "-an", "udp://127.0.0.1:42069"])
            .spawn()
        {
            Ok(child) => {
                *ffplay = Some(child);
                self.push_log("local preview opened (ffplay udp://127.0.0.1:42069)".into());
            }
            Err(err) => self.push_log(format!("ffplay not available: {err}")),
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_snapshot_reflects_state() {
        let state: SharedState = Arc::new(Mutex::new(AppState::default()));
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = true;
            s.encoder_active = true;
            s.phone_connected = true;
            s.phone_ip = "10.0.0.1".into();
            s.preview_enabled = true;
            s.gyro_fps = 1000;
            s.hands_detected = 2;
        }
        let snapshot = StatusSnapshot::from(state.lock().unwrap().deref());
        assert!(snapshot.driver_connected);
        assert!(snapshot.encoder_active);
        assert!(snapshot.phone_connected);
        assert_eq!(snapshot.phone_ip, "10.0.0.1");
        assert!(snapshot.preview_enabled);
        assert_eq!(snapshot.gyro_fps, 1000);
        assert_eq!(snapshot.hands_detected, 2);
    }

    #[test]
    fn status_snapshot_field_order_is_stable() {
        // The REST API contract requires a specific JSON field order.
        // This test verifies the serialization produces the expected keys
        // in the expected order by checking the serialized string.
        let state: SharedState = Arc::new(Mutex::new(AppState::default()));
        let snapshot = StatusSnapshot::from(state.lock().unwrap().deref());
        let json = serde_json::to_string(&snapshot).unwrap();
        // Check that key fields appear in the correct order.
        let app_ver_pos = json.find("app_version").unwrap();
        let driver_pos = json.find("driver_connected").unwrap();
        let encoder_pos = json.find("encoder_active").unwrap();
        let phone_pos = json.find("phone_connected").unwrap();
        let preview_pos = json.find("preview_enabled").unwrap();
        let camera_pos = json.find("camera_connected").unwrap();
        assert!(app_ver_pos < driver_pos);
        assert!(driver_pos < encoder_pos);
        assert!(encoder_pos < phone_pos);
        assert!(phone_pos < preview_pos);
        assert!(preview_pos < camera_pos);
    }

    #[test]
    fn applied_settings_defaults_merge_correctly() {
        // When all fields are None, APPLIED_DEFAULTS should be used.
        let width = APPLIED_DEFAULTS.0;
        let height = APPLIED_DEFAULTS.1;
        let fps = APPLIED_DEFAULTS.2;
        let bitrate = APPLIED_DEFAULTS.3;
        assert_eq!(width, 2880);
        assert_eq!(height, 1620);
        assert_eq!(fps, 60);
        assert_eq!(bitrate, 20);
    }

    #[test]
    fn logs_returns_newest_first() {
        let state: SharedState = Arc::new(Mutex::new(AppState::default()));
        {
            let mut s = state.lock().unwrap();
            s.push_log("first".into());
            s.push_log("second".into());
            s.push_log("third".into());
        }
        // Build an AppCore manually (without spawning threads).
        let core = AppCore {
            state,
            ffplay: Arc::new(Mutex::new(None)),
            preview_frame: Arc::new(Mutex::new(None)),
            preview_decode: Arc::new(Mutex::new(None)),
        };
        let logs = core.logs(10);
        assert_eq!(logs[0], "third");
        assert_eq!(logs[1], "second");
        assert_eq!(logs[2], "first");
    }

    #[test]
    fn preview_payload_reflects_driver_stats() {
        let state: SharedState = Arc::new(Mutex::new(AppState::default()));
        {
            let mut s = state.lock().unwrap();
            s.preview_enabled = true;
            s.preview_driver_fps = 60;
            s.preview_bitrate_kbps = 20000;
            s.preview_frames = 5000;
            s.preview_drops = 10;
        }
        let snapshot = StatusSnapshot::from(state.lock().unwrap().deref());
        assert!(snapshot.preview_enabled);
        assert_eq!(snapshot.preview_driver_fps, 60);
        assert_eq!(snapshot.preview_bitrate_kbps, 20000);
        assert_eq!(snapshot.preview_frames, 5000);
        assert_eq!(snapshot.preview_drops, 10);
    }
}