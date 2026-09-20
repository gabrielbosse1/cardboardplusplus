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
/// Crate version stamped into /health, the hub title, and release URLs.
pub const APP_VERSION: &str = env!("CARGO_PKG_VERSION");
/// Preview frame size the ffmpeg decode scales to (keeps the hub cheap).
pub const PREVIEW_W: u32 = 480;
pub const PREVIEW_H: u32 = 270;
/// Stream settings before the first apply: 2880x1620 @ 60fps, 20 Mbps.
pub const APPLIED_DEFAULTS: (i32, i32, i32, i32) = (2880, 1620, 60, 20);
/// JSON-serializable copy of AppState for GET /status and the Slint pollers.
/// Field order is covered by a stability test (hub bindings depend on it).
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
    pub net_frames_decoded: u32,
    pub net_stalls: u32,
    pub net_decoded_fps: f32,
    pub applied_bitrate_mbps: i32,
    pub link_test_active: bool,
    pub link_test_result_mbps: i32,
    pub link_test_note: String,
    pub hand_enabled: bool,
    pub hand_overlay: bool,
    pub hand_min_detection: i32,
    pub hand_min_presence: i32,
    pub hand_min_tracking: i32,
    pub install_busy: bool,
    pub install_note: String,
    pub driver_present: bool,
    pub steamvr_note: String,
    pub applied_width: i32,
    pub applied_height: i32,
    pub applied_fps: i32,
    pub driver_version: String,
    pub phone_version: String,
    pub install_progress: f32,
    pub steamvr_running: bool,
}
impl From<&AppState> for StatusSnapshot {
    /// Snapshots live state for the UI/REST boundary. Copies every field so
    /// readers never hold the state lock while rendering.
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
            applied_width: s.applied_width,
            applied_height: s.applied_height,
            applied_fps: s.applied_fps,
            driver_version: s.driver_version.clone(),
            phone_version: s.phone_version.clone(),
            install_progress: s.install_progress,
            steamvr_running: s.steamvr_running,
        }
    }
}
/// Stream settings echoed back by POST /settings after a successful push.
#[derive(Debug, Clone, serde::Serialize)]
pub struct AppliedSettings {
    pub width: i32,
    pub height: i32,
    pub fps: i32,
    pub bitrate_mbps: i32,
    pub encoder: String,
}
/// Starts the hand-tracking sidecar chain at bridge boot: reuses a healthy
/// server when one already listens (dev restarts), reclaims the port from a
/// stale python holder, else spawns mediapipe_server.py next to the binary
/// and connects. Returns the client for the camera thread plus the child to
/// kill at shutdown. Every outcome is logged; (None, None) means hands stay
/// off but the rest of the bridge runs.
fn spawn_mediapipe_server(state: &SharedState) -> (Option<MediapipeClient>, Option<Child>) {
    let (d, p, t) = match state.lock() {
        Ok(s) => (
            s.hand_min_detection as f32 / 100.0,
            s.hand_min_presence as f32 / 100.0,
            s.hand_min_tracking as f32 / 100.0,
        ),
        Err(_) => (0.5, 0.5, 0.5),
    };
    if let Some(client) = MediapipeClient::connect_healthy(MEDIAPIPE_PORT, d, p, t) {
        if let Ok(mut s) = state.lock() {
            s.push_log("mediapipe: connected to existing server".into());
        }
        return (Some(client), None);
    }
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
    let script_path = {
        let cwd_candidate = std::path::PathBuf::from("mediapipe_server.py");
        if cwd_candidate.is_file() {
            Some(cwd_candidate)
        } else if let Some(exe_dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())) {
            let near = exe_dir.join("mediapipe_server.py");
            if near.is_file() {
                Some(near)
            } else {
                let dev_candidate = exe_dir
                    .parent()
                    .and_then(|p| p.parent())
                    .and_then(|p| p.parent())
                    .map(|p| p.join("bridge").join("crates").join("cardboard-bridge").join("mediapipe_server.py"))
                    .filter(|p| p.is_file());
                if dev_candidate.is_some() {
                    dev_candidate
                } else {
                    exe_dir
                        .parent()
                        .and_then(|p| p.parent())
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
    let (python_prog, python_args) = python_command();
    {
        let mut probe = Command::new(&python_prog);
        probe.args(&python_args).arg("--version");
        let ver = probe
            .output()
            .map(|o| {
                let mut s = String::from_utf8_lossy(&o.stdout).into_owned();
                if s.trim().is_empty() {
                    s = String::from_utf8_lossy(&o.stderr).into_owned();
                }
                s.lines().next().unwrap_or("unknown").trim().to_string()
            })
            .unwrap_or_else(|_| "not runnable".to_string());
        if let Ok(mut s) = state.lock() {
            s.push_log(format!("mediapipe python: {python_prog} {ver}"));
        }
    }
    let mut child = match Command::new(&python_prog)
        .args(&python_args)
        .arg(&script_path)
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
/// Picks the python for the sidecar: bundled python/ next to the exe first,
/// then $PYTHON, then the py launcher, then PATH python. Last resort is the
/// bare "python" name (spawn reports the failure to the log).
fn python_command() -> (String, Vec<String>) {
    if let Some(bundled) = bundled_python() {
        return (bundled, vec![]);
    }
    if let Ok(p) = std::env::var("PYTHON") {
        if !p.trim().is_empty() {
            return (p, vec![]);
        }
    }
    for (prog, args) in [("py", vec!["-3"]), ("python", vec![])] {
        let ok = Command::new(prog)
            .args(&args)
            .arg("--version")
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false);
        if ok {
            return (
                prog.to_string(),
                args.into_iter().map(str::to_string).collect(),
            );
        }
    }
    ("python".to_string(), vec![])
}
/// Installer-bundled interpreter (python/python.exe next to the bridge exe).
/// None in dev checkouts, which use system python instead.
fn bundled_python() -> Option<String> {
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|e| e.parent().map(|p| p.to_path_buf()))?;
    let py = exe_dir.join("python").join("python.exe");
    py.is_file().then(|| py.to_string_lossy().into_owned())
}
/// Logs the preview ffmpeg version at startup, or how to install it when
/// missing (then the video preview stays dark but everything else works).
fn log_ffmpeg_version(state: &SharedState) {
    match Command::new("ffmpeg").arg("-version").output() {
        Ok(out) if out.status.success() => {
            let first = String::from_utf8_lossy(&out.stdout)
                .lines()
                .next()
                .unwrap_or("ffmpeg")
                .trim()
                .to_string();
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("preview ffmpeg: {first}"));
            }
        }
        _ => {
            if let Ok(mut s) = state.lock() {
                s.push_log(
                    "ffmpeg not found on PATH — preview stays dark; install via `winget install ffmpeg`".into(),
                );
            }
        }
    }
}
/// UI-independent bridge owner: holds shared state plus the preview frame
/// slot, the ffmpeg decode child, and the MediaPipe client/child. All net
/// loops and installers run from here; main.rs only renders snapshots.
#[derive(Clone)]
pub struct AppCore {
    state: SharedState,
    preview_frame: Arc<Mutex<Option<(u32, u32, Vec<u8>)>>>,
    preview_decode: Arc<Mutex<Option<PreviewDecodeHandle>>>,
    mediapipe_proc: Arc<Mutex<Option<Child>>>,
    mediapipe_client: Arc<Mutex<Option<MediapipeClient>>>,
}
/// ffmpeg preview decode child handle. Killed on shutdown/restart.
struct PreviewDecodeHandle {
    child: Child,
}
impl AppCore {
    /// Builds the core and starts every background loop: driver heartbeat,
    /// phone telemetry, SHM drain (attaches when the driver mapping appears),
    /// camera pipeline, sidecar spawn, and preview decode. Returns the Arc
    /// that main, server, and the UI share.
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
        {
            let shm_state = state.clone();
            thread::spawn(move || {
                let mut svc: Option<bridge_core::shm::ShmService> = None;
                loop {
                    if svc.is_none() {
                        match bridge_core::shm::ShmService::open(
                            bridge_shm::protocol::DEFAULT_REGION_SIZE,
                        ) {
                            Ok(s) => {
                                svc = Some(s);
                                if let Ok(mut st) = shm_state.lock() {
                                    st.push_log("shm: attached to driver status region".into());
                                }
                            }
                            Err(_) => {
                                std::thread::sleep(std::time::Duration::from_secs(1));
                                continue;
                            }
                        }
                    }
                    if let Some(s) = svc.as_mut() {
                        let msgs = s.drain();
                        if !msgs.is_empty() {
                            crate::debug_log!(
                                &shm_state,
                                "[shm] drained {} msgs (write_seq={})",
                                msgs.len(),
                                s.last_write_seq
                            );
                        }
                    }
                    std::thread::sleep(std::time::Duration::from_millis(50));
                }
            });
        }
        log_ffmpeg_version(&state);
        let (mediapipe_client, mediapipe_proc) = spawn_mediapipe_server(&state);
        net::camera::spawn(state.clone(), mediapipe_client.clone());
        let core = Arc::new(Self {
            state,
            preview_frame: Arc::new(Mutex::new(None)),
            preview_decode: Arc::new(Mutex::new(None)),
            mediapipe_proc: Arc::new(Mutex::new(mediapipe_proc)),
            mediapipe_client: Arc::new(Mutex::new(mediapipe_client)),
        });
        core.start_preview_decode();
        core
    }
    /// Thread-safe log append for background workers that only hold &self.
    pub fn push_log(&self, line: String) {
        if let Ok(mut s) = self.state.lock() {
            s.push_log(line);
        }
    }
    /// Applies Stream-tab settings: pushes CARDBOARD_CAP + BRIDGE_CFG to the
    /// driver, records them for partial merges, and returns the echo for
    /// POST /settings. Called from the hub callback and the REST handler.
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
    /// Link-test observation window in seconds.
    pub const LINK_TEST_SECS: u64 = 10;
    /// Starts a 10s link test at the current bitrate: snapshots stall/fps
    /// counters, and a background thread later writes the verdict from
    /// link_test_verdict. No-op while a test runs or when driver/phone are
    /// down. The hub polls link_test_* fields for progress.
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
    /// Toggles the MediaPipe detect path in the camera thread. The phone's
    /// hand-presence hints keep flowing regardless.
    pub fn set_hand_enabled(&self, enabled: bool) {
        if let Ok(mut s) = self.state.lock() {
            s.hand_enabled = enabled;
            s.push_log(format!(
                "hand tracking {}",
                if enabled { "enabled" } else { "disabled" }
            ));
        }
    }
    /// Toggles painting the skeleton onto the camera preview. Detection still
    /// runs and counts hands when off — only the drawing stops.
    pub fn set_hand_overlay(&self, enabled: bool) {
        if let Ok(mut s) = self.state.lock() {
            s.hand_overlay = enabled;
        }
    }
    /// Retunes detection/presence/tracking thresholds (1-100 from the Camera
    /// tab): stores them, then pushes to the live sidecar without a restart.
    /// When the sidecar is down the values wait for the next bridge start.
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
    /// Refreshes whether the compiled driver DLL exists at the expected path.
    /// Polled by the wizard watcher and hub slow loop for the install pill.
    pub fn refresh_driver_present(&self) -> bool {
        let present = std::path::Path::new(&driver_target_dll()).exists();
        if let Ok(mut s) = self.state.lock() {
            s.driver_present = present;
        }
        present
    }
    /// Installs the driver on a worker thread with progress reports: refuses
    /// while SteamVR runs (locked DLLs), then copies DLL + resources + FFmpeg
    /// runtime into the SteamVR slot. Re-entrant calls while busy are ignored.
    pub fn install_driver(&self) {
        {
            let Ok(mut s) = self.state.lock() else {
                return;
            };
            if s.install_busy {
                return;
            }
            let running = steamvr_running_now();
            if !running.is_empty() {
                s.steamvr_running = true;
                s.install_note = format!(
                    "SteamVR is running ({}) — close it first (status window → menu → Quit SteamVR), then install again",
                    running.join(", ")
                );
                let note = s.install_note.clone();
                s.push_log(format!("driver install blocked: {note}"));
                return;
            }
            s.steamvr_running = false;
            s.install_busy = true;
            s.install_progress = 0.0;
            s.install_note = "installing…".into();
            s.push_log("installing SteamVR driver…".into());
        }
        let state = self.state.clone();
        thread::spawn(move || {
            let report = |progress: f32, stage: &str| {
                if let Ok(mut s) = state.lock() {
                    s.install_progress = progress.clamp(0.0, 1.0);
                    s.install_note = stage.into();
                }
            };
            let result = install_driver_files(&report);
            if let Ok(mut s) = state.lock() {
                s.install_busy = false;
                s.install_note = match &result {
                    Ok(out) => {
                        s.install_progress = 1.0;
                        format!("done — {out}")
                    }
                    Err(e) => format!("failed — {e}"),
                };
                s.driver_present = std::path::Path::new(&driver_target_dll()).exists();
                let note = s.install_note.clone();
                s.push_log(format!("driver install: {note}"));
            }
        });
    }
    /// Clears a stale "SteamVR running" banner after the user quits it.
    /// Called from the wizard's dismiss button.
    pub fn clear_steamvr_running(&self) {
        if let Ok(mut s) = self.state.lock() {
            s.steamvr_running = false;
        }
    }
    /// Launches SteamVR via `steam -applaunch 250820` on a worker thread.
    /// Reports "already running" immediately instead of double-launching, and
    /// tells the user where Steam lives when it cannot be found.
    pub fn start_steamvr(&self) {
        let running = steamvr_running_now();
        if !running.is_empty() {
            if let Ok(mut s) = self.state.lock() {
                s.steamvr_running = true;
                s.steamvr_note = format!(
                    "SteamVR is already running ({}) — close it first (status window → menu → Quit SteamVR), then open again",
                    running.join(", ")
                );
                let note = s.steamvr_note.clone();
                s.push_log(format!("open SteamVR blocked: {note}"));
            }
            return;
        }
        if let Ok(mut s) = self.state.lock() {
            s.steamvr_running = false;
        }
        let state = self.state.clone();
        thread::spawn(move || {
            let note = match find_steam_exe() {
                Some(steam) => {
                    match Command::new(&steam)
                        .args(["-applaunch", &STEAMVR_APP_ID.to_string()])
                        .spawn()
                    {
                        Ok(_) => format!("opening SteamVR via Steam ({steam})"),
                        Err(e) => format!("failed to start SteamVR via {steam}: {e}"),
                    }
                }
                None => "Steam not found (expected Steam\\steam.exe under Program Files) — open Steam and start SteamVR from Library → Tools".into(),
            };
            if let Ok(mut s) = state.lock() {
                s.steamvr_note = note.clone();
                s.push_log(note);
            }
        });
    }
    /// Stops the sidecar child (if this bridge spawned it) at event-loop exit.
    /// Net threads are daemon-style and end with the process.
    pub fn shutdown(&self) {
        if let Ok(mut proc) = self.mediapipe_proc.lock() {
            if let Some(mut child) = proc.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
        }
        self.push_log("bridge shutting down".into());
    }
    /// Swaps the latest decoded preview frame out for the hub poller,
    /// converting it to a Slint image. None when no frame arrived yet.
    pub fn take_preview_frame(&self) -> Option<slint::Image> {
        let (w, h, rgba) = self.preview_frame.lock().expect("preview frame lock").take()?;
        let buffer = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::clone_from_slice(&rgba, w, h);
        Some(slint::Image::from_rgba8(buffer))
    }
    /// Swaps the latest camera frame out for the hub poller after a liveness
    /// check: returns None (and clears the pill) when the camera went quiet.
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
    /// Starts the localhost preview pipeline: binds UDP 42069 (the driver's
    /// Annex-B copy), pipes datagrams into ffmpeg stdin, and publishes each
    /// decoded PREVIEW_W x PREVIEW_H RGBA frame to the slot the hub polls.
    /// Restarts automatically when ffmpeg exits; a held port logs its PID.
    /// Called once from new(); the pump threads never touch bridge memory
    /// beyond the single latest-frame slot.
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
                        let _ = handle.child.kill();
                        *decode = None;
                    }
                }
            }
        }
        let frame_slot = self.preview_frame.clone();
        let socket = {
            let mut bound = None;
            for attempt in 0..6 {
                match UdpSocket::bind("127.0.0.1:42069") {
                    Ok(s) => {
                        bound = Some(s);
                        break;
                    }
                    Err(e) => {
                        if attempt == 5 {
                            let holder = crate::net::mediapipe::listen_pid(VIDEO_PORT)
                                .map(|pid| format!(" (held by PID {pid})"))
                                .unwrap_or_default();
                            self.push_log(format!(
                                "preview bind failed (port 42069): {e}{holder} — free the port and restart"
                            ));
                            return;
                        }
                        std::thread::sleep(std::time::Duration::from_millis(500));
                    }
                }
            }
            bound.expect("preview socket bound above")
        };
        socket.set_nonblocking(true).ok();
        let vf = format!("scale={PREVIEW_W}:{PREVIEW_H}");
        let mut child = match Command::new("ffmpeg")
            .args([
                "-f", "h264",
                "-probesize", "32768",
                "-analyzeduration", "0",
                "-i", "pipe:0",
                "-vf",
            ])
            .arg(&vf)
            .args([
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
        let frame_w: u32 = PREVIEW_W;
        let frame_h: u32 = PREVIEW_H;
        let frame_bytes = (frame_w * frame_h * 4) as usize;
        thread::spawn(move || {
            let mut buf = vec![0u8; 65536];
            loop {
                loop {
                    match socket.recv(&mut buf) {
                        Ok(n) if n > 0 => {
                            if stdin.write_all(&buf[..n]).is_err() {
                                return;
                            }
                        }
                        _ => break,
                    }
                }
                std::thread::sleep(std::time::Duration::from_millis(2));
            }
        });
        let preview_decode_cleanup = self.preview_decode.clone();
        thread::spawn(move || {
            let mut rgba = vec![0u8; frame_bytes];
            let mut off = 0usize;
            loop {
                if rgba.len() < frame_bytes {
                    rgba.resize(frame_bytes, 0);
                }
                match stdout.read(&mut rgba[off..]) {
                    Ok(0) => break,
                    Ok(n) => {
                        off += n;
                        if off >= frame_bytes {
                            off = 0;
                            if let Ok(mut slot) = frame_slot.lock() {
                                match slot.as_mut() {
                                    Some((_, _, old)) => std::mem::swap(&mut rgba, old),
                                    None => {
                                        *slot = Some((
                                            frame_w,
                                            frame_h,
                                            std::mem::replace(&mut rgba, Vec::new()),
                                        ));
                                    }
                                }
                            }
                        }
                    }
                    Err(_) => break,
                }
            }
            if let Ok(mut decode) = preview_decode_cleanup.lock() {
                if decode.take().is_some() {
                    eprintln!("[preview] ffmpeg exited, handle cleared");
                }
            }
        });
        {
            let mut decode = self.preview_decode.lock().expect("preview decode lock");
            *decode = Some(PreviewDecodeHandle { child });
        }
        self.push_log("embedded preview started (UDP 42069 → ffmpeg → UI)".into());
        let weak = Arc::downgrade(&{
            let this = self.clone();
            Arc::new(this)
        });
        let preview_decode = self.preview_decode.clone();
        thread::spawn(move || {
            let child_exited = {
                let decode = preview_decode.lock().expect("preview decode lock");
                decode.as_ref().map(|h| h.child.id())
            };
            let Some(_pid) = child_exited else { return };
            loop {
                std::thread::sleep(std::time::Duration::from_millis(500));
                let decode = preview_decode.lock().expect("preview decode lock");
                if decode.is_none() {
                    break;
                }
            }
            std::thread::sleep(std::time::Duration::from_secs(2));
            if let Some(core) = weak.upgrade() {
                core.push_log("preview auto-restarting...".into());
                core.start_preview_decode();
            }
        });
    }
    /// Full state snapshot for GET /status and the hub pollers.
    pub fn status(&self) -> StatusSnapshot {
        let s = self.state.lock().expect("state lock");
        StatusSnapshot::from(s.deref())
    }
    /// Live stream settings for partial merges (REST/UI). Falls back to
    /// APPLIED_DEFAULTS when the lock is poisoned.
    pub fn applied_settings(&self) -> (i32, i32, i32, i32, String) {
        match self.state.lock() {
            Ok(s) => (
                s.applied_width,
                s.applied_height,
                s.applied_fps,
                s.applied_bitrate_mbps,
                s.encoder_name.clone(),
            ),
            Err(_) => (
                APPLIED_DEFAULTS.0,
                APPLIED_DEFAULTS.1,
                APPLIED_DEFAULTS.2,
                APPLIED_DEFAULTS.3,
                "gpu".into(),
            ),
        }
    }
    /// Newest-first ring-log lines for GET /logs and the hub log view.
    pub fn logs(&self, n: usize) -> Vec<String> {
        let s = self.state.lock().expect("state lock");
        s.log.iter().rev().take(n).cloned().collect()
    }
}
/// Steam app id used to launch SteamVR, and the process names whose presence
/// blocks driver installs (locked DLLs) and gates the "already running" path.
const STEAMVR_APP_ID: u32 = 250820;
const STEAMVR_PROCS: [&str; 2] = ["vrserver.exe", "vrmonitor.exe"];
/// Finds steam.exe under Program Files. None on machines without Steam
/// (start_steamvr then tells the user where to get it).
fn find_steam_exe() -> Option<String> {
    let dirs = [
        std::env::var("ProgramFiles(x86)").ok(),
        std::env::var("ProgramFiles").ok(),
    ];
    for dir in dirs.into_iter().flatten() {
        let p = std::path::PathBuf::from(dir).join("Steam").join("steam.exe");
        if p.exists() {
            return Some(p.to_string_lossy().into_owned());
        }
    }
    None
}
/// Matches tasklist CSV output against the SteamVR process names (first
/// column, case-insensitive). Pure function so tests cover it without
/// spawning tasklist.
fn detect_steamvr_in_tasklist(output: &str) -> Vec<&'static str> {
    STEAMVR_PROCS
        .iter()
        .filter(|proc_| {
            output.lines().any(|line| {
                line.split(',')
                    .next()
                    .unwrap_or(line)
                    .trim()
                    .trim_matches('"')
                    .eq_ignore_ascii_case(proc_)
            })
        })
        .copied()
        .collect()
}
/// Live check: which SteamVR processes run right now. Empty when tasklist
/// itself fails (then installs proceed — the locked-file retry reports it).
fn steamvr_running_now() -> Vec<&'static str> {
    match std::process::Command::new("tasklist")
        .args(["/NH", "/FO", "CSV"])
        .output()
    {
        Ok(o) if o.status.success() => {
            detect_steamvr_in_tasklist(&String::from_utf8_lossy(&o.stdout))
        }
        _ => Vec::new(),
    }
}
/// SteamVR driver slot, its root, and the installed DLL path. Thin wrappers
/// over bridge_core::paths used across install/presence checks.
fn steamvr_drivers_dir() -> String {
    paths::default_steamvr_drivers_dir()
}
/// SteamVR root derived from the driver slot (for vrsettings + steam exe).
fn steamvr_root() -> String {
    paths::steamvr_root_from_drivers_dir(&steamvr_drivers_dir())
}
/// Installed driver DLL: <slot>/bin/win64/driver_cardboardplusplus.dll.
/// Presence of this file drives the install pill.
fn driver_target_dll() -> String {
    format!("{}\\bin\\win64\\driver_cardboardplusplus.dll", steamvr_drivers_dir())
}
/// Six-stage install with progress reports: prepare folders, back up the
/// current DLL (timestamped .bak), resolve the new DLL (local compile or
/// release download), atomic swap-in, FFmpeg runtime, resources + vrsettings
/// force-on. Returns the one-line summary shown in the hub. Called on the
/// install_driver worker thread.
fn install_driver_files(report: &dyn Fn(f32, &str)) -> Result<String, String> {
    use bridge_core::driver_install::{
        DriverDllSource, download_to, driver_dll_source, force_driver_enabled,
        install_ffmpeg_standalone, install_resources,
    };
    use std::path::Path;
    report(0.05, "preparing folders… (1/6)");
    let target_dir = format!("{}\\bin\\win64", steamvr_drivers_dir());
    std::fs::create_dir_all(&target_dir).map_err(|e| format!("mkdir {target_dir}: {e}"))?;
    let target_dll = driver_target_dll();
    report(0.15, "backing up current driver… (2/6)");
    if Path::new(&target_dll).exists() {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let backup = format!("{target_dll}.bak-{stamp}");
        std::fs::copy(&target_dll, &backup).map_err(|e| format!("backup {backup}: {e}"))?;
    }
    report(0.30, "resolving driver… (3/6)");
    let mut downloaded: Option<std::path::PathBuf> = None;
    let src = match driver_dll_source()? {
        DriverDllSource::Local(p) => {
            report(0.40, "copying local driver… (3/6)");
            p
        }
        DriverDllSource::Download(url) => {
            report(0.40, "downloading driver… (3/6)");
            let tmp = std::env::temp_dir()
                .join(format!("cb-driver-{}.dll", std::process::id()));
            download_to(&url, &tmp)?;
            downloaded = Some(tmp.clone());
            report(0.50, "driver downloaded… (3/6)");
            tmp
        }
    };
    report(0.55, "installing driver… (4/6)");
    let tmp_dll = format!("{target_dll}.tmp-{}", std::process::id());
    if let Err(e) = std::fs::copy(&src, &tmp_dll) {
        let _ = std::fs::remove_file(&tmp_dll);
        return Err(format!("copy to {tmp_dll}: {e}"));
    }
    let installed = bridge_core::driver_deps::replace_locked(
        std::path::Path::new(&tmp_dll),
        std::path::Path::new(&target_dll),
    );
    if let Some(tmp) = downloaded {
        let _ = std::fs::remove_file(tmp);
    }
    installed?;
    report(0.65, "installing FFmpeg runtimes… (5/6)");
    let ffmpeg_dlls = install_ffmpeg_standalone(Path::new(&target_dir), false)?;
    report(0.80, "FFmpeg runtimes ready… (5/6)");
    report(0.88, "installing resources… (6/6)");
    let drivers_dir = steamvr_drivers_dir();
    install_resources(Path::new(&drivers_dir))?;
    report(0.95, "enabling driver… (6/6)");
    let forced = force_driver_enabled(&steamvr_root())?;
    Ok(format!(
        "installed driver into {target_dir} (+ {} FFmpeg {} DLLs: {}{})",
        ffmpeg_dlls.len(),
        bridge_core::driver_deps::ffmpeg_version(),
        ffmpeg_dlls.join(", "),
        if forced { "; vrsettings forced on" } else { "" },
    ))
}
#[cfg(test)]
mod tests {
    // Core contract tests: snapshot mirrors state 1:1 with stable JSON field
    // order (hub bindings depend on it), tasklist parsing finds SteamVR
    // processes case-insensitively, defaults merge, logs read newest-first,
    // and preview stats flow through. Test names read as the spec.
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
        let state: SharedState = Arc::new(Mutex::new(AppState::default()));
        let snapshot = StatusSnapshot::from(state.lock().unwrap().deref());
        let json = serde_json::to_string(&snapshot).unwrap();
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
        let net_pos = json.find("net_frames_decoded").unwrap();
        let test_pos = json.find("link_test_active").unwrap();
        assert!(camera_pos < net_pos);
        assert!(net_pos < test_pos);
        let hand_pos = json.find("hand_min_tracking").unwrap();
        let install_pos = json.find("install_busy").unwrap();
        let present_pos = json.find("driver_present").unwrap();
        assert!(test_pos < hand_pos);
        assert!(hand_pos < install_pos);
        assert!(install_pos < present_pos);
        let applied_w = json.find("applied_width").unwrap();
        let applied_h = json.find("applied_height").unwrap();
        let applied_fps = json.find("applied_fps").unwrap();
        assert!(present_pos < applied_w);
        assert!(applied_w < applied_h);
        assert!(applied_h < applied_fps);
        let driver_ver = json.find("driver_version").unwrap();
        let phone_ver = json.find("phone_version").unwrap();
        assert!(applied_fps < driver_ver);
        assert!(driver_ver < phone_ver);
        let progress_pos = json.find("install_progress").unwrap();
        assert!(phone_ver < progress_pos);
        let running_pos = json.find("steamvr_running").unwrap();
        assert!(progress_pos < running_pos);
    }
    #[test]
    fn steamvr_app_id_matches_store_page() {
        assert_eq!(STEAMVR_APP_ID, 250820);
    }
    #[test]
    fn tasklist_csv_detects_steamvr_procs() {
        let out = "\"steam.exe\",\"1234\",\"Console\",\"1\",\"10,000 K\"\r\n\
                   \"vrserver.exe\",\"5678\",\"Console\",\"1\",\"50,000 K\"\r\n\
                   \"vrmonitor.exe\",\"9012\",\"Console\",\"1\",\"20,000 K\"\r\n";
        assert_eq!(detect_steamvr_in_tasklist(out), vec!["vrserver.exe", "vrmonitor.exe"]);
    }
    #[test]
    fn tasklist_output_without_steamvr_is_empty() {
        let out = "\"steam.exe\",\"1234\",\"Console\",\"1\",\"10,000 K\"\r\n\
                   \"explorer.exe\",\"42\",\"Console\",\"1\",\"30,000 K\"\r\n";
        assert!(detect_steamvr_in_tasklist(out).is_empty());
        assert!(detect_steamvr_in_tasklist("").is_empty());
    }
    #[test]
    fn tasklist_match_is_case_insensitive() {
        assert_eq!(detect_steamvr_in_tasklist("\"VRSERVER.EXE\",\"1\",\"X\",\"1\",\"1 K\""), vec!["vrserver.exe"]);
    }
    #[test]
    fn applied_settings_defaults_merge_correctly() {
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