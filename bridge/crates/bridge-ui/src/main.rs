//! Slint desktop UI for the Cardboard++ Bridge — the product.
//!
//! - **Top bar**: connection status (Connected/Idle/Error from bridge-shm
//!   liveness), live framerate, stream state, client state.
//! - **Stream** side-bar section: resolution/FPS/bitrate/encoder settings owned
//!   by the bridge and pushed down to the SteamVR driver over the command
//!   channel (`bridge_shm::CmdProducer`), plus the start/stop switch (the
//!   single on/off for the whole product).
//! - **Camera** section: placeholder for phone camera passthrough + MediaPipe
//!   hand tracking.
//! - **General** section: automatic installer for the SteamVR driver (DLL
//!   backup first) and ADB install of the Android APK.
//! - **Diagnostics** pane: readout of the shared-memory status region.

slint::include_modules!();

use log::{info, warn};
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use bridge_core::paths;
use bridge_core::shm::{SettingsChannel, ShmService};
use bridge_shm::protocol::{BridgeMessage, DEFAULT_REGION_SIZE};

const APP_DIR: &str = "CardboardPlusPlus";
const CONFIG_FILE: &str = "bridge.json";

/// Persistent settings. The Bridge owns all configuration; nothing about the
/// stream lives in the driver or the app.
#[derive(Clone, Serialize, Deserialize)]
struct BridgeConfig {
    width: u32,
    height: u32,
    fps: u32,
    bitrate_kbps: u32,
    encoder: u32,
    stream_enabled: bool,
    driver_dll_src: String,
    steamvr_drivers_dir: String,
    apk_path: String,
    adb_path: String,
    phone_endpoint: String,
}

impl Default for BridgeConfig {
    fn default() -> Self {
        Self {
            width: 2880,
            height: 1620,
            fps: 60,
            bitrate_kbps: 20000,
            encoder: 0,
            stream_enabled: true,
            driver_dll_src: paths::default_driver_dll(),
            steamvr_drivers_dir: paths::default_steamvr_drivers_dir(),
            apk_path: paths::default_apk(),
            adb_path: paths::default_adb(),
            // No default: the user enters their phone's ADB endpoint.
            phone_endpoint: String::new(),
        }
    }
}

fn config_path() -> std::path::PathBuf {
    let base = std::env::var_os("LOCALAPPDATA")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    base.join(APP_DIR).join(CONFIG_FILE)
}

fn load_config() -> BridgeConfig {
    let path = config_path();
    let mut cfg = BridgeConfig::default();
    if let Ok(s) = std::fs::read_to_string(&path) {
        if let Ok(loaded) = serde_json::from_str::<BridgeConfig>(&s) {
            cfg = loaded;
        } else {
            warn!("config parse failed; using defaults");
        }
    }
    cfg
}

fn save_config(cfg: &BridgeConfig) {
    let path = config_path();
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    match serde_json::to_string_pretty(cfg) {
        Ok(s) => {
            if let Err(e) = std::fs::write(&path, s) {
                warn!("could not save config {}: {}", path.display(), e);
            }
        }
        Err(e) => {
            warn!("could not serialize config: {}", e);
        }
    }
}

/// Shared between the worker loop and the UI callbacks.
struct Shared {
    channel: Mutex<Option<SettingsChannel>>,
    cfg: Mutex<BridgeConfig>,
    // last pushed fingerprint so we don't spam the driver with no-ops.
    last_pushed: Mutex<Option<(u32, u32, u32, u32, u32, u32)>>,
}

fn push_settings(shared: &Shared) {
    let cfg = shared.cfg.lock().unwrap().clone();
    let fingerprint = (
        cfg.width,
        cfg.height,
        cfg.fps,
        cfg.bitrate_kbps,
        cfg.encoder,
        if cfg.stream_enabled { 1 } else { 0 },
    );
    let mut last = shared.last_pushed.lock().unwrap();
    if *last == Some(fingerprint) {
        return;
    }
    let mut chan = shared.channel.lock().unwrap();
    if chan.is_none() {
        match SettingsChannel::open() {
            Ok(c) => {
                info!("command region open; pushing settings to driver");
                *chan = Some(c);
            }
            Err(e) => {
                warn!("command region unavailable (driver not polling yet): {e}");
                return;
            }
        }
    }
    if let Some(c) = chan.as_mut() {
        c.push(cfg.width, cfg.height, cfg.fps, cfg.bitrate_kbps, cfg.encoder,
               if cfg.stream_enabled { 1 } else { 0 });
        *last = Some(fingerprint);
        info!(
            "pushed settings: {}x{} @{}fps, {}kbps, encoder={}, stream={}",
            cfg.width, cfg.height, cfg.fps, cfg.bitrate_kbps, cfg.encoder, cfg.stream_enabled
        );
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let cfg = load_config();
    info!("loaded config: {}x{} @{}fps, {}kbps", cfg.width, cfg.height, cfg.fps, cfg.bitrate_kbps);

    let ui = MainWindow::new()?;

    let shared = Arc::new(Shared {
        channel: Mutex::new(None),
        cfg: Mutex::new(cfg.clone()),
        last_pushed: Mutex::new(None),
    });

    apply_config_to_ui(&ui, &cfg);

    // ---- UI callbacks ----
    {
        let shared = shared.clone();
        let ui_weak = ui.as_weak();
        ui.on_apply_settings(move || {
            let ui = ui_weak.clone().unwrap();
            let mut cfg = shared.cfg.lock().unwrap().clone();
            cfg.width = ui.get_stream_width_s().to_string().parse::<u32>().unwrap_or(2880).max(320).min(7680);
            cfg.height = ui.get_stream_height_s().to_string().parse::<u32>().unwrap_or(1620).max(180).min(4320);
            // Align down to 16 (H.264 macroblock requirement).
            cfg.width -= cfg.width % 16;
            cfg.height -= cfg.height % 16;
            cfg.fps = ui.get_stream_fps().clamp(30, 120) as u32;
            cfg.bitrate_kbps = ui.get_stream_bitrate_kbps().clamp(2000, 60000) as u32;
            cfg.encoder = (ui.get_stream_encoder().clamp(0, 1)) as u32;
            cfg.driver_dll_src = ui.get_driver_dll_src().to_string();
            cfg.steamvr_drivers_dir = ui.get_steamvr_drivers_dir().to_string();
            cfg.apk_path = ui.get_apk_path().to_string();
            cfg.adb_path = ui.get_adb_path().to_string();
            cfg.phone_endpoint = ui.get_phone_endpoint().to_string();
            *shared.cfg.lock().unwrap() = cfg.clone();
            save_config(&cfg);
            push_settings(&shared);
            let ui_weak2 = ui_weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(u) = ui_weak2.upgrade() {
                    u.set_general_status(
                        format!("Settings pushed to driver ({}x{} @{} fps, {} kbps mbs alignment).",
                            cfg.width, cfg.height, cfg.fps, cfg.bitrate_kbps).into(),
                    );
                }
            });
        });
    }

    {
        let shared = shared.clone();
        let ui_weak = ui.as_weak();
        ui.on_toggle_stream(move || {
            let mut cfg = shared.cfg.lock().unwrap().clone();
            cfg.stream_enabled = !cfg.stream_enabled;
            *shared.cfg.lock().unwrap() = cfg.clone();
            save_config(&cfg);
            push_settings(&shared);
            let handle = ui_weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(u) = handle.upgrade() {
                    u.set_stream_enabled(cfg.stream_enabled);
                    u.set_general_status(
                        format!("Stream {}", if cfg.stream_enabled { "STARTED (switch pushed to driver)" } else { "STOPPED (switch pushed to driver)" }).into(),
                    );
                }
            });
        });
    }

    {
        let shared = shared.clone();
        let ui_weak = ui.as_weak();
        ui.on_build_driver(move || {
            let shared = shared.clone();
            let ui = ui_weak.clone();
            spawn_install("build-driver", shared, ui,
                |cfg| build_driver(cfg), "Building SteamVR driver (MSVC Release|x64)...");
        });
    }

    {
        let shared = shared.clone();
        let ui_weak = ui.as_weak();
        ui.on_install_driver(move || {
            let shared = shared.clone();
            let ui = ui_weak.clone();
            spawn_install("install-driver", shared, ui,
                |cfg| install_driver(cfg), "Installing / updating SteamVR driver...");
        });
    }

    {
        let shared = shared.clone();
        let ui_weak = ui.as_weak();
        ui.on_install_apk(move || {
            let shared = shared.clone();
            let ui = ui_weak.clone();
            spawn_install("install-apk", shared, ui,
                |cfg| install_apk(cfg), "Installing APK on phone over ADB...");
        });
    }

    {
        let shared = shared.clone();
        let ui_weak = ui.as_weak();
        ui.on_start_steamvr(move || {
            let drivers_dir = shared.cfg.lock().unwrap().steamvr_drivers_dir.clone();
            spawn_start_steamvr(ui_weak.clone(), drivers_dir);
        });
    }

    // ---- Worker: drain the status ring, compute FPS/telemetry, drive UI ----
    {
        let ui_handle = ui.as_weak();
        let shared = shared.clone();
        std::thread::spawn(move || worker_loop(ui_handle, shared));
    }

    ui.run()?;
    Ok(())
}

fn apply_config_to_ui(ui: &MainWindow, cfg: &BridgeConfig) {
    ui.set_stream_width_s(cfg.width.to_string().into());
    ui.set_stream_height_s(cfg.height.to_string().into());
    ui.set_stream_fps(cfg.fps as i32);
    ui.set_stream_bitrate_kbps(cfg.bitrate_kbps as i32);
    ui.set_stream_encoder(cfg.encoder as i32);
    ui.set_stream_enabled(cfg.stream_enabled);
    ui.set_driver_dll_src(cfg.driver_dll_src.clone().into());
    ui.set_steamvr_drivers_dir(cfg.steamvr_drivers_dir.clone().into());
    ui.set_apk_path(cfg.apk_path.clone().into());
    ui.set_adb_path(cfg.adb_path.clone().into());
    ui.set_phone_endpoint(cfg.phone_endpoint.clone().into());
}

fn worker_loop(ui_handle: slint::Weak<MainWindow>, shared: Arc<Shared>) {
    info!("bridge-ui worker: waiting for driver shared-memory region...");

    let mut service: Option<ShmService> = None;
    let mut ever_connected = false;
    let mut last_write_at = Instant::now();

    let mut msg_history: VecDeque<String> = VecDeque::with_capacity(14);
    let mut frames_bucket = 0u64;
    let mut bucket_start = Instant::now();

    let mut last_telemetry: Option<String> = None;
    let mut cap_seen_at = Instant::now() - Duration::from_secs(30);
    let mut prev_write_seq = 0u64;

    loop {
        let now = Instant::now();

        // (Re)attach to the driver's region.
        if service.is_none() {
            match ShmService::open(DEFAULT_REGION_SIZE) {
                Ok(svc) => {
                    info!("bridge-ui: connected to driver region");
                    service = Some(svc);
                    ever_connected = true;
                    last_write_at = now;
                    prev_write_seq = 0;
                    // Welcome push so the driver inherits persisted settings on
                    // first contact (the fingerprint guards against re-pushes).
                    push_settings(&shared);
                }
                Err(_) => {
                    // Driver not up yet; keep the UI honest.
                    update_status(&ui_handle,
                        "Idle",
                        &format!("{:.1} fps", 0.0),
                        "Stopped",
                        "No client",
                        "Waiting for SteamVR driver to create the region...",
                        &String::new());
                    std::thread::sleep(Duration::from_millis(1000));
                    continue;
                }
            }
        }

        if let Some(svc) = service.as_mut() {
            let msgs = svc.drain();
            for m in &msgs {
                push_tag(&mut msg_history, bridge_core::shm::msg_tag(m).to_string());
                match m {
                    BridgeMessage::FrameSubmitted(_) => frames_bucket += 1,
                    BridgeMessage::Telemetry(t) => {
                        // The driver heartbeats ~1/s with a frames=0 telemetry to
                        // keep the region alive while SteamVR is idle. Don't let
                        // that clobber the real encoder readout below.
                        if !(t.frames == 0 && t.avg_encode_us == 0) {
                            let enc_fps = if t.avg_interval_us > 0 {
                                1_000_000.0 / t.avg_interval_us as f64
                            } else {
                                0.0
                            };
                            let dup_pct = if t.summary_frames > 0 {
                                t.dup_count as f64 / t.summary_frames as f64 * 100.0
                            } else {
                                0.0
                            };
                            last_telemetry = Some(format!(
                                "encoder: {:.1} fps avg, encode avg {:.3} ms (max {:.3} ms), dup {:.1}%",
                                enc_fps,
                                t.avg_encode_us as f64 / 1000.0,
                                t.max_encode_us as f64 / 1000.0,
                                dup_pct
                            ));
                        }
                    }
                    BridgeMessage::CapReported(_) => cap_seen_at = now,
                    _ => {}
                }
            }
            if svc.last_write_seq != prev_write_seq {
                // write_seq went backwards: the driver restarted (SteamVR
                // cycle). It re-initialized with hardcoded defaults, so force a
                // settings re-push and re-anchor liveness.
                if svc.last_write_seq < prev_write_seq {
                    *shared.last_pushed.lock().unwrap() = None;
                    push_settings(&shared);
                    info!("bridge-ui: driver restarted; re-pushed persisted settings");
                }
                prev_write_seq = svc.last_write_seq;
                last_write_at = now;
            }

            // Liveness: the driver heartbeats ~1/s while SteamVR runs.
            let fresh = now.duration_since(last_write_at) < Duration::from_secs(8);
            let status = if fresh {
                "Connected"
            } else if ever_connected {
                "Error"
            } else {
                "Idle"
            };

            // Periodic UI refresh (1 Hz).
            if now.duration_since(bucket_start) >= Duration::from_secs(1) {
                let fps = frames_bucket as f64
                    / now.duration_since(bucket_start).as_secs_f64().max(1e-3);
                frames_bucket = 0;
                bucket_start = now;

                let cfg = shared.cfg.lock().unwrap().clone();
                let stream_state = if cfg.stream_enabled {
                    if fps > 0.0 { "Streaming" } else { "Enabled (no frames)" }
                } else {
                    "Stopped"
                };
                let client_state = if now.duration_since(cap_seen_at) < Duration::from_secs(15) {
                    "Connected"
                } else {
                    "No client"
                };

                let diag = format!(
                    "write_seq: {}\nqueued: {}\nframe messages/s: {:.0}\ndropped total: {}\nlast messages: {}",
                    svc.last_write_seq,
                    svc.pending(),
                    fps,
                    svc.dropped_total,
                    msg_history.iter().rev().take(9).cloned().collect::<Vec<_>>().join(", "),
                );

                update_status(&ui_handle, status, &format!("{fps:.1} fps"), stream_state,
                    client_state, &diag, last_telemetry.as_deref().unwrap_or(""));
                push_settings(&shared);
            }
        }

        std::thread::sleep(Duration::from_millis(50));
    }
}

fn push_tag(history: &mut VecDeque<String>, tag: String) {
    history.push_back(tag);
    while history.len() > 14 {
        history.pop_front();
    }
}

fn update_status(
    ui_handle: &slint::Weak<MainWindow>,
    connection: &str,
    fps: &str,
    stream_state: &str,
    client: &str,
    diagnostics: &str,
    telemetry: &str,
) {
    let connection = connection.to_string();
    let fps = fps.to_string();
    let stream_state = stream_state.to_string();
    let client = client.to_string();
    let diagnostics = diagnostics.to_string();
    let telemetry = telemetry.to_string();
    let weak = ui_handle.clone();
    let _ = slint::invoke_from_event_loop(move || {
        if let Some(ui) = weak.upgrade() {
            ui.set_connection_status(connection.into());
            ui.set_live_fps(fps.into());
            ui.set_stream_state(stream_state.into());
            ui.set_client_state(client.into());
            ui.set_diagnostics(diagnostics.into());
            if !telemetry.is_empty() {
                ui.set_telemetry_line(telemetry.into());
            }
        }
    });
}

fn spawn_install<F>(
    what: &str,
    shared: Arc<Shared>,
    ui: slint::Weak<MainWindow>,
    f: F,
    running_msg: &str,
) where
    F: FnOnce(BridgeConfig) -> Result<String, String> + Send + 'static,
{
    let what = what.to_string();
    let running_msg = running_msg.to_string();
    let ui2 = ui.clone();
    let _ = slint::invoke_from_event_loop(move || {
        if let Some(u) = ui2.upgrade() {
            u.set_general_status(running_msg.into());
        }
    });

    std::thread::spawn(move || {
        let cfg = shared.cfg.lock().unwrap().clone();
        let result = f(cfg);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(u) = ui.upgrade() {
                let msg = match &result {
                    Ok(out) => format!("{what}: OK — {out}"),
                    Err(e) => format!("{what}: FAILED — {e}"),
                };
                u.set_general_status(msg.into());
            }
        });
    });
}

fn run_capture(prog: &str, args: &[&str]) -> Result<String, String> {
    info!("run: {} {}", prog, args.join(" "));
    let out = std::process::Command::new(prog)
        .args(args)
        .output()
        .map_err(|e| format!("failed to start {}: {e}", prog))?;
    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    if !out.status.success() {
        return Err(format!(
            "{} exited {}:\n{}{}",
            prog,
            out.status,
            stdout.trim(),
            stderr.trim()
        ));
    }
    Ok(format!("{}\n{}", stdout.trim(), stderr.trim()).trim().to_string())
}

fn dll_install_dir(cfg: &BridgeConfig) -> String {
    format!("{}\\bin\\win64", cfg.steamvr_drivers_dir.trim_end_matches('\\'))
}

fn build_driver(cfg: BridgeConfig) -> Result<String, String> {
    let sln = {
        let dll = Path::new(&cfg.driver_dll_src);
        let dir = dll.parent().and_then(|p| p.parent()).unwrap_or(Path::new("."));
        dir.join("driver_cardboardplusplus.sln")
    };
    if !sln.exists() {
        return Err(format!("solution not found at {}", sln.display()));
    }
    let sol = sln.to_str().unwrap_or("").to_string();
    let Some(msbuild) = paths::find_msbuild() else {
        return Err("MSBuild not found - install the Visual Studio C++ build tools".into());
    };
    run_capture(
        &msbuild,
        &[
            &sol,
            "/p:Configuration=Release",
            "/p:Platform=x64",
            "/m",
            "/v:minimal",
            "/nologo",
        ],
    )?;
    if !Path::new(&cfg.driver_dll_src).exists() {
        return Err(format!("build finished but DLL missing at {}", cfg.driver_dll_src));
    }
    Ok(format!("built {}", cfg.driver_dll_src))
}

fn install_driver(cfg: BridgeConfig) -> Result<String, String> {
    use bridge_core::driver_install::{
        DriverDllSource, download_to, driver_dll_source, force_driver_enabled,
        install_ffmpeg_standalone, install_resources,
    };
    // Configured DLL wins when present; otherwise the shared standalone
    // resolver (local checkout build, then version-matched GitHub release).
    let mut downloaded: Option<std::path::PathBuf> = None;
    let src = if Path::new(&cfg.driver_dll_src).exists() {
        std::path::PathBuf::from(&cfg.driver_dll_src)
    } else {
        match driver_dll_source()? {
            DriverDllSource::Local(p) => p,
            DriverDllSource::Download(url) => {
                let tmp = std::env::temp_dir()
                    .join(format!("cb-driver-{}.dll", std::process::id()));
                download_to(&url, &tmp)?;
                downloaded = Some(tmp.clone());
                tmp
            }
        }
    };
    let target = dll_install_dir(&cfg);
    std::fs::create_dir_all(&target).map_err(|e| format!("mkdir {}: {e}", target))?;
    let target_dll = format!("{}\\driver_cardboardplusplus.dll", target);

    // Backup existing DLL first (kept: rename below replaces atomically).
    if Path::new(&target_dll).exists() {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let backup = format!("{}.bak-{}", target_dll, stamp);
        std::fs::copy(&target_dll, &backup).map_err(|e| format!("backup {}: {e}", backup))?;
        info!("backed up existing driver to {}", backup);
    }

    // Atomic install: copy to a temp file in the same directory, then rename
    // over the target. A failed copy never leaves a missing/half-written DLL.
    let tmp_dll = format!("{}.tmp-{}", target_dll, std::process::id());
    if let Err(e) = std::fs::copy(&src, &tmp_dll) {
        let _ = std::fs::remove_file(&tmp_dll);
        return Err(format!("copy to {}: {e}", tmp_dll));
    }
    // Retry while SteamVR holds the loaded DLL open; the error names the
    // lock so "access denied" becomes actionable.
    let installed =
        bridge_core::driver_deps::replace_locked(Path::new(&tmp_dll), Path::new(&target_dll));
    if let Some(tmp) = downloaded {
        let _ = std::fs::remove_file(tmp);
    }
    installed?;

    // Runtime deps: the pinned FFmpeg set — vendored, or fetched from the
    // pinned zip when there is no checkout (fails with the expected names
    // when the pin itself is stale, never a silent dead driver).
    let ffmpeg_dlls = install_ffmpeg_standalone(Path::new(&target), false)?;
    info!("installed FFmpeg runtimes: {}", ffmpeg_dlls.join(", "));

    // Ship the exe-baked manifest + bindings for a from-scratch install.
    install_resources(Path::new(&cfg.steamvr_drivers_dir))?;

    // Force the driver on in steamvr.vrsettings (repairs a user-disabled or
    // safe-mode-blocked state from an earlier crash).
    let root = paths::steamvr_root_from_drivers_dir(&cfg.steamvr_drivers_dir);
    let forced = force_driver_enabled(&root)?;

    Ok(format!(
        "installed driver into {} (+ {} FFmpeg {} DLLs{})",
        target,
        ffmpeg_dlls.len(),
        bridge_core::driver_deps::ffmpeg_version(),
        if forced { "; vrsettings forced on" } else { "" },
    ))
}

fn install_apk(cfg: BridgeConfig) -> Result<String, String> {
    if !Path::new(&cfg.adb_path).exists() {
        return Err(format!("adb not found at {}", cfg.adb_path));
    }
    if !Path::new(&cfg.apk_path).exists() {
        return Err(format!("apk not found at {}", cfg.apk_path));
    }
    if cfg.phone_endpoint.trim().is_empty() {
        return Err("no phone endpoint - enter the phone's ADB address (ip:port)".into());
    }
    run_capture(&cfg.adb_path, &["connect", &cfg.phone_endpoint])?;
    run_capture(&cfg.adb_path, &["-s", &cfg.phone_endpoint, "install", "-r", &cfg.apk_path])?;
    Ok(format!("APK installed on {}", cfg.phone_endpoint))
}

fn spawn_start_steamvr(ui: slint::Weak<MainWindow>, drivers_dir: String) {
    std::thread::spawn(move || {
        // Derive the SteamVR root from the configured drivers dir.
        let root = paths::steamvr_root_from_drivers_dir(&drivers_dir);
        let vrserver = format!("{root}\\bin\\win64\\vrserver.exe");
        let msg = if Path::new(&vrserver).exists() {
            match std::process::Command::new(&vrserver).spawn() {
                Ok(_) => format!("Started {}", vrserver),
                Err(e) => format!("Failed to start SteamVR: {e}"),
            }
        } else {
            format!("vrserver.exe not found at {}", vrserver)
        };
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(u) = ui.upgrade() {
                u.set_general_status(msg.into());
            }
        });
    });
}