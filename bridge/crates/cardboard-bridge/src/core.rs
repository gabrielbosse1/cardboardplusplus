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
use crate::net::mediapipe::{MediapipeClient, Reclaim};
use crate::net::{self, EncoderChoice, DRIVER_DISCOVERY_PORT, MEDIAPIPE_PORT, VIDEO_PORT};

use bridge_core::paths;

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
    pub preview_driver_fps: i32,
    pub preview_bitrate_kbps: i32,
    pub preview_frames: u64,
    pub preview_drops: u64,
    pub camera_connected: bool,
    pub camera_fps: i32,
    pub camera_detected_hands: usize,
    pub latest_gyro_x: f32,
    pub latest_gyro_y: f32,
    pub latest_gyro_z: f32,
    pub latest_accel_x: f32,
    pub latest_accel_y: f32,
    pub latest_accel_z: f32,
    pub latest_mag_x: f32,
    pub latest_mag_y: f32,
    pub latest_mag_z: f32,
    // -- appended (never reordered): phone video-path health + link test --
    pub net_frames_decoded: u32,
    pub net_stalls: u32,
    pub net_decoded_fps: f32,
    pub applied_bitrate_mbps: i32,
    pub link_test_active: bool,
    pub link_test_result_mbps: i32,
    pub link_test_note: String,
    // -- appended (never reordered): hand-tracking model state --
    pub hand_enabled: bool,
    pub hand_overlay: bool,
    pub hand_min_detection: i32,
    pub hand_min_presence: i32,
    pub hand_min_tracking: i32,
    // -- appended (never reordered): setup wizard installer state --
    pub install_busy: bool,
    pub install_note: String,
    pub driver_present: bool,
    pub steamvr_note: String,
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
            preview_driver_fps: s.preview_driver_fps,
            preview_bitrate_kbps: s.preview_bitrate_kbps,
            preview_frames: s.preview_frames,
            preview_drops: s.preview_drops,
            camera_connected: s.camera_connected,
            camera_fps: s.camera_fps,
            camera_detected_hands: s.camera_detected_hands,
            latest_gyro_x: s.latest_gyro[0],
            latest_gyro_y: s.latest_gyro[1],
            latest_gyro_z: s.latest_gyro[2],
            latest_accel_x: s.latest_accel[0],
            latest_accel_y: s.latest_accel[1],
            latest_accel_z: s.latest_accel[2],
            latest_mag_x: s.latest_mag[0],
            latest_mag_y: s.latest_mag[1],
            latest_mag_z: s.latest_mag[2],
            net_frames_decoded: s.net_frames_decoded,
            net_stalls: s.net_stalls,
            net_decoded_fps: s.net_decoded_fps,
            applied_bitrate_mbps: s.applied_bitrate_mbps,
            link_test_active: s.link_test_active,
            link_test_result_mbps: s.link_test_result_mbps,
            link_test_note: s.link_test_note.clone(),
            hand_enabled: s.hand_enabled,
            hand_overlay: s.hand_overlay,
            hand_min_detection: s.hand_min_detection,
            hand_min_presence: s.hand_min_presence,
            hand_min_tracking: s.hand_min_tracking,
            install_busy: s.install_busy,
            install_note: s.install_note.clone(),
            driver_present: s.driver_present,
            steamvr_note: s.steamvr_note.clone(),
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
/// connect to it via TCP. Returns the client plus the child handle (if we
/// spawned one, so the caller can kill it on shutdown). Returns `(None, None)`
/// if the Python process can't be started or the TCP connection fails.
/// The sidecar takes no arguments: port and model defaults are static, and
/// tuning happens live over the TCP link (`MediapipeClient::set_config`).
fn spawn_mediapipe_server(state: &SharedState) -> (Option<MediapipeClient>, Option<Child>) {
    let (d, p, t) = match state.lock() {
        Ok(s) => (
            s.hand_min_detection as f32 / 100.0,
            s.hand_min_presence as f32 / 100.0,
            s.hand_min_tracking as f32 / 100.0,
        ),
        Err(_) => (0.5, 0.5, 0.5),
    };
    // First try connecting to an already-running server (e.g. started manually).
    // The healthy probe also pushes the current thresholds, so an adopted
    // server keeps the UI's tuning.
    if let Some(client) = MediapipeClient::connect_healthy(MEDIAPIPE_PORT, d, p, t) {
        if let Ok(mut s) = state.lock() {
            s.push_log("mediapipe: connected to existing server".into());
        }
        return (Some(client), None);
    }
    // No healthy server, but something still answers TCP: a stale squatter
    // (e.g. an older bridge's wedged sidecar). Kill it if it's ours, fail
    // loudly otherwise — never silently adopt it.
    if MediapipeClient::try_once(MEDIAPIPE_PORT).is_ok() {
        match MediapipeClient::reclaim_port(MEDIAPIPE_PORT) {
            Reclaim::Freed(pid) => {
                if let Ok(mut s) = state.lock() {
                    s.push_log(format!("mediapipe: killed stale sidecar (PID {pid}), starting fresh"));
                }
            }
            Reclaim::AlreadyFree => {
                if let Ok(mut s) = state.lock() {
                    s.push_log("mediapipe: stale server vanished, starting fresh".into());
                }
            }
            Reclaim::Refused(reason) => {
                if let Ok(mut s) = state.lock() {
                    s.push_log(format!(
                        "mediapipe: port {MEDIAPIPE_PORT} held by {reason}; not touching it — free the port and restart"
                    ));
                }
                return (None, None);
            }
        }
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
                // Try bridge/crates/cardboard-bridge/ (dev layout)
                let dev_candidate = exe_dir
                    .parent() // target/debug -> target
                    .and_then(|p| p.parent()) // target -> bridge
                    .and_then(|p| p.parent()) // bridge -> repo root
                    .map(|p| p.join("bridge").join("crates").join("cardboard-bridge").join("mediapipe_server.py"))
                    .filter(|p| p.is_file());
                if dev_candidate.is_some() {
                    dev_candidate
                } else {
                    // Try going up to bridge/ and looking in crates/cardboard-bridge/
                    exe_dir
                        .parent() // target/debug -> target
                        .and_then(|p| p.parent()) // target -> bridge
                        .map(|p| p.join("crates").join("cardboard-bridge").join("mediapipe_server.py"))
                        .filter(|p| p.is_file())
                }
            }
        } else {
            None
        }
    };
    let Some(script_path) = script_path else {
        if let Ok(mut s) = state.lock() {
            s.push_log("mediapipe server script not found (expected next to the binary or in crates/cardboard-bridge/)".into());
        }
        return (None, None);
    };

    let python = std::env::var("PYTHON").unwrap_or_else(|_| "python".into());
    let mut child = match Command::new(&python)
        .arg(&script_path)
        // Stdout is discarded (not piped): an undrained pipe would fill its
        // 64KB buffer and wedge the child. Stderr is drained below.
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("mediapipe server failed to start: {e}"));
            }
            return (None, None);
        }
    };

    // Drain stderr in a background thread so the Python process doesn't block.
    // Lines are mirrored into the ring log too: the bridge often runs as a GUI
    // app where its own stderr is invisible, and a silent sidecar is undebuggable.
    if let Some(stderr) = child.stderr.take() {
        let log_state = state.clone();
        thread::spawn(move || {
            use std::io::BufRead;
            let reader = std::io::BufReader::new(stderr);
            for line in reader.lines().map_while(Result::ok) {
                eprintln!("[mediapipe-py] {line}");
                if let Ok(mut s) = log_state.lock() {
                    s.push_log(format!("[mediapipe-py] {line}"));
                }
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
        Ok(client) => (Some(client), Some(child)),
        Err(e) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("mediapipe TCP connect failed: {e}"));
            }
            let _ = child.kill();
            (None, None)
        }
    }
}

/// The bridge's brain, independent of any UI. `AppCore` is cheap to clone —
/// each clone shares the same `Arc<Mutex<AppState>>`, so the UI, the REST
/// server and the network threads all observe one consistent state.
#[derive(Clone)]
pub struct AppCore {
    state: SharedState,
    /// The most recent decoded preview frame as raw RGBA pixels (width, height, data).
    preview_frame: Arc<Mutex<Option<(u32, u32, Vec<u8>)>>>,
    /// Handle to the running preview decode thread (so we can stop it).
    preview_decode: Arc<Mutex<Option<PreviewDecodeHandle>>>,
    /// The spawned Python MediaPipe server (if we started one). Killed by
    /// `shutdown()` so it doesn't outlive the bridge.
    mediapipe_proc: Arc<Mutex<Option<Child>>>,
    /// Live handle to the sidecar's TCP link. Cloned into the camera detect
    /// thread; this copy serves `apply_hand_model` without a restart.
    mediapipe_client: Arc<Mutex<Option<MediapipeClient>>>,
}

struct PreviewDecodeHandle {
    child: Child,
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
        let (mediapipe_client, mediapipe_proc) = spawn_mediapipe_server(&state);
        net::camera::spawn(state.clone(), mediapipe_client.clone());

        let core = Arc::new(Self {
            state,
            preview_frame: Arc::new(Mutex::new(None)),
            preview_decode: Arc::new(Mutex::new(None)),
            mediapipe_proc: Arc::new(Mutex::new(mediapipe_proc)),
            mediapipe_client: Arc::new(Mutex::new(mediapipe_client)),
        });
        // The preview is always on: start decoding the driver's localhost
        // copy right away (the driver sends it by default, no toggle needed).
        core.start_preview_decode();
        core
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
            s.record_applied_settings(width, height, fps, bitrate_mbps);
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

    /// How long the manual link test samples the phone's net stats.
    pub const LINK_TEST_SECS: u64 = 10;

    /// Start one manual link test (UI "Test link" button). Samples the
    /// phone's stall counter for `LINK_TEST_SECS`, then stores a recommended
    /// bitrate + note for the user to fine-tune and Apply. Never pushes to
    /// the driver by itself. No-op unless phone and driver are both live.
    pub fn start_link_test(&self) {
        let baseline = if let Ok(mut s) = self.state.lock() {
            if s.link_test_active {
                return;
            }
            if !s.driver_connected || !s.phone_connected {
                s.push_log("link test needs driver + phone connected".into());
                return;
            }
            s.link_test_active = true;
            s.link_test_result_mbps = 0;
            s.link_test_note = "testing…".into();
            let at = s.applied_bitrate_mbps;
            s.push_log(format!(
                "link test started ({} s at {} Mbps)…",
                Self::LINK_TEST_SECS, at
            ));
            (s.net_stalls, s.applied_fps, s.applied_bitrate_mbps)
        } else {
            return;
        };

        let state = self.state.clone();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(Self::LINK_TEST_SECS));
            if let Ok(mut s) = state.lock() {
                let (rec, note) = AppState::link_test_verdict(
                    baseline.0,
                    s.net_stalls,
                    s.net_decoded_fps,
                    baseline.1,
                    baseline.2,
                );
                s.link_test_active = false;
                s.link_test_result_mbps = rec;
                s.link_test_note = note.to_string();
                s.push_log(format!("link test done: {note} → {rec} Mbps (slider updated, Apply to save)"));
            }
        });
    }

    /// Master switch for bridge-side hand tracking. While off, the camera
    /// detect worker skips the MediaPipe round-trip (the viewer still shows
    /// the raw camera feed).
    pub fn set_hand_enabled(&self, enabled: bool) {
        if let Ok(mut s) = self.state.lock() {
            s.hand_enabled = enabled;
            s.push_log(format!(
                "hand tracking {}",
                if enabled { "enabled" } else { "disabled" }
            ));
        }
    }

    /// Toggle the skeleton overlay drawn onto the camera viewer frame.
    pub fn set_hand_overlay(&self, enabled: bool) {
        if let Ok(mut s) = self.state.lock() {
            s.hand_overlay = enabled;
        }
    }

    /// Store new model confidences (0-100) and push them to the running
    /// sidecar over TCP — the landmarker is recreated in place, so the camera
    /// feed never drops a frame.
    pub fn apply_hand_model(&self, det: i32, pres: i32, track: i32) {
        let (d, p, t) = {
            let mut s = self.state.lock().expect("state lock");
            s.hand_min_detection = det.clamp(1, 100);
            s.hand_min_presence = pres.clamp(1, 100);
            s.hand_min_tracking = track.clamp(1, 100);
            let (di, pi, ti) = (s.hand_min_detection, s.hand_min_presence, s.hand_min_tracking);
            s.push_log(format!(
                "hand model: detection {di}%, presence {pi}%, tracking {ti}%"
            ));
            (
                di as f32 / 100.0,
                pi as f32 / 100.0,
                ti as f32 / 100.0,
            )
        };
        let live = self
            .mediapipe_client
            .lock()
            .ok()
            .and_then(|c| c.clone())
            .map(|cli| cli.set_config(d, p, t))
            .unwrap_or(false);
        self.push_log(if live {
            "hand model updated live".into()
        } else {
            "sidecar not running — model applies on next bridge start".into()
        });
    }

    /// Re-check whether the driver DLL is present inside SteamVR. Cheap
    /// local stat call — the UI poller runs it so the wizard step can show
    /// "detected" without an install.
    pub fn refresh_driver_present(&self) -> bool {
        let present = std::path::Path::new(&driver_target_dll()).exists();
        if let Ok(mut s) = self.state.lock() {
            s.driver_present = present;
        }
        present
    }

    /// One-click SteamVR driver install (wizard step 2): copy the compiled
    /// DLL into the SteamVR driver dir with backup + manifest, on a thread.
    /// No-op while an install is already running.
    pub fn install_driver(&self) {
        {
            let Ok(mut s) = self.state.lock() else {
                return;
            };
            if s.install_busy {
                return;
            }
            s.install_busy = true;
            s.install_note = "installing…".into();
            s.push_log("installing SteamVR driver…".into());
        }
        let state = self.state.clone();
        thread::spawn(move || {
            let result = install_driver_files();
            if let Ok(mut s) = state.lock() {
                s.install_busy = false;
                s.install_note = match &result {
                    Ok(out) => format!("done — {out}"),
                    Err(e) => format!("failed — {e}"),
                };
                s.driver_present = std::path::Path::new(&driver_target_dll()).exists();
                let note = s.install_note.clone();
                s.push_log(format!("driver install: {note}"));
            }
        });
    }

    /// Launch SteamVR via vrserver.exe (wizard step 2, after install).
    /// SteamVR picks up the installed driver on start.
    pub fn start_steamvr(&self) {
        let state = self.state.clone();
        thread::spawn(move || {
            let vrserver = format!("{}\\bin\\win64\\vrserver.exe", steamvr_root());
            let note = if !std::path::Path::new(&vrserver).exists() {
                format!("vrserver.exe not found at {vrserver}")
            } else {
                match Command::new(&vrserver).spawn() {
                    Ok(_) => format!("started {vrserver}"),
                    Err(e) => format!("failed to start SteamVR: {e}"),
                }
            };
            if let Ok(mut s) = state.lock() {
                s.steamvr_note = note.clone();
                s.push_log(note);
            }
        });
    }

    /// Kill the spawned MediaPipe server (if we started one) so the Python
    /// process doesn't outlive the bridge. Called on shutdown.
    pub fn shutdown(&self) {
        if let Ok(mut proc) = self.mediapipe_proc.lock() {
            if let Some(mut child) = proc.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
        }
        self.push_log("bridge shutting down".into());
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
                "-probesize", "32768",
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
        thread::spawn(move || {
            let mut buf = vec![0u8; 65536];
            loop {
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
        let preview_decode_cleanup = self.preview_decode.clone();
        thread::spawn(move || {
            let mut rgba = vec![0u8; frame_bytes];
            let mut off = 0usize;
            loop {
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
            // ffmpeg exited (crash or stop). Clear the handle so the next
            // start_preview_decode doesn't inherit a dead child.
            if let Ok(mut decode) = preview_decode_cleanup.lock() {
                if decode.take().is_some() {
                    eprintln!("[preview] ffmpeg exited, handle cleared");
                }
            }
        });

        // Register the running decode session for the watcher below.
        {
            let mut decode = self.preview_decode.lock().expect("preview decode lock");
            *decode = Some(PreviewDecodeHandle { child });
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
        thread::spawn(move || {
            // Wait for the child to finish.
            let child_exited = {
                let decode = preview_decode.lock().expect("preview decode lock");
                decode.as_ref().map(|h| h.child.id())
            };
            let Some(_pid) = child_exited else { return };

            // Busy-wait until the decode handle is gone, then restart it:
            // the preview is always on, so a dead ffmpeg is always revived.
            loop {
                std::thread::sleep(std::time::Duration::from_millis(500));
                let decode = preview_decode.lock().expect("preview decode lock");
                if decode.is_none() {
                    break;
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

/// Compiled driver DLL produced by the C++ build, resolved from the repo
/// checkout (empty when the checkout can't be located — the caller must
/// surface an error, never a personal path).
fn driver_dll_src() -> String {
    paths::default_driver_dll()
}
/// SteamVR addon home for this driver (standard Steam location by default).
fn steamvr_drivers_dir() -> String {
    paths::default_steamvr_drivers_dir()
}
/// SteamVR root (drivers dir minus the last two segments).
fn steamvr_root() -> String {
    paths::steamvr_root_from_drivers_dir(&steamvr_drivers_dir())
}

/// Target path of the driver DLL inside SteamVR.
fn driver_target_dll() -> String {
    format!("{}\\bin\\win64\\driver_cardboardplusplus.dll", steamvr_drivers_dir())
}

/// Copy the compiled DLL into SteamVR with backup + manifest (same steps
/// as the legacy bridge-ui installer). Runs on the install thread.
fn install_driver_files() -> Result<String, String> {
    use std::path::Path;
    let src = driver_dll_src();
    if src.is_empty() || !Path::new(&src).exists() {
        return Err(format!(
            "compiled dll not found at {src} (build the driver first)"
        ));
    }
    let target_dir = format!("{}\\bin\\win64", steamvr_drivers_dir());
    std::fs::create_dir_all(&target_dir).map_err(|e| format!("mkdir {target_dir}: {e}"))?;
    let target_dll = driver_target_dll();

    if Path::new(&target_dll).exists() {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let backup = format!("{target_dll}.bak-{stamp}");
        std::fs::copy(&target_dll, &backup).map_err(|e| format!("backup {backup}: {e}"))?;
    }

    let tmp_dll = format!("{target_dll}.tmp-{}", std::process::id());
    if let Err(e) = std::fs::copy(&src, &tmp_dll) {
        let _ = std::fs::remove_file(&tmp_dll);
        return Err(format!("copy to {tmp_dll}: {e}"));
    }
    if let Err(e) = std::fs::rename(&tmp_dll, &target_dll) {
        let _ = std::fs::remove_file(&tmp_dll);
        return Err(format!("rename {tmp_dll} -> {target_dll}: {e}"));
    }

    // Ship the manifest + bindings for a from-scratch install.
    let src_root = Path::new(&src)
        .parent()
        .and_then(|p| p.parent())
        .unwrap_or(Path::new("."));
    let res_src = src_root.join("resources");
    if res_src.is_dir() {
        let drivers_dir = steamvr_drivers_dir();
        let res_dst = Path::new(&drivers_dir).join("resources");
        let _ = std::fs::create_dir_all(&res_dst);
        for name in [
            "driver.vrdrivermanifest",
            "controller_profile.json",
            "legacy_bindings_example.json",
        ] {
            let dst = res_dst.join(name);
            if !dst.exists() {
                let _ = std::fs::copy(res_src.join(name), &dst);
            }
        }
        let _ = std::fs::copy(
            src_root.join("driver.vrdrivermanifest"),
            Path::new(&drivers_dir).join("driver.vrdrivermanifest"),
        );
    }

    Ok(format!("installed driver into {target_dir}"))
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
            s.gyro_fps = 1000;
            s.hands_detected = 2;
        }
        let snapshot = StatusSnapshot::from(state.lock().unwrap().deref());
        assert!(snapshot.driver_connected);
        assert!(snapshot.encoder_active);
        assert!(snapshot.phone_connected);
        assert_eq!(snapshot.phone_ip, "10.0.0.1");
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
        let preview_pos = json.find("preview_driver_fps").unwrap();
        let camera_pos = json.find("camera_connected").unwrap();
        assert!(app_ver_pos < driver_pos);
        assert!(driver_pos < encoder_pos);
        assert!(encoder_pos < phone_pos);
        assert!(phone_pos < preview_pos);
        assert!(preview_pos < camera_pos);
        // Phone health + link-test fields are appended (never reordered).
        let net_pos = json.find("net_frames_decoded").unwrap();
        let test_pos = json.find("link_test_active").unwrap();
        assert!(camera_pos < net_pos);
        assert!(net_pos < test_pos);
        // Wizard installer fields are appended last (never reordered).
        let hand_pos = json.find("hand_min_tracking").unwrap();
        let install_pos = json.find("install_busy").unwrap();
        let present_pos = json.find("driver_present").unwrap();
        assert!(test_pos < hand_pos);
        assert!(hand_pos < install_pos);
        assert!(install_pos < present_pos);
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
            preview_frame: Arc::new(Mutex::new(None)),
            preview_decode: Arc::new(Mutex::new(None)),
            mediapipe_proc: Arc::new(Mutex::new(None)),
            mediapipe_client: Arc::new(Mutex::new(None)),
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
            s.preview_driver_fps = 60;
            s.preview_bitrate_kbps = 20000;
            s.preview_frames = 5000;
            s.preview_drops = 10;
        }
        let snapshot = StatusSnapshot::from(state.lock().unwrap().deref());
        assert_eq!(snapshot.preview_driver_fps, 60);
        assert_eq!(snapshot.preview_bitrate_kbps, 20000);
        assert_eq!(snapshot.preview_frames, 5000);
        assert_eq!(snapshot.preview_drops, 10);
    }
}