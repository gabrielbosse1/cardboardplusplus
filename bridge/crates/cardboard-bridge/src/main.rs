slint::include_modules!();

mod app;
mod core;
mod hand_overlay;
mod net;
mod server;

pub(crate) use app::debug_log;

use std::sync::Arc;
use std::time::Duration;

use core::AppCore;
use net::EncoderChoice;
use slint::{Timer, TimerMode};

fn main() {
    let headless = std::env::args().any(|a| a == "--headless");

    // Debug builds always debug; release builds default off and opt in via
    // env var or CLI flag (plus POST /debug at runtime).
    let debug = cfg!(debug_assertions)
        || std::env::args().any(|a| a == "--debug")
        || std::env::var("CARDBOARD_DEBUG").map(|v| v == "1").unwrap_or(false);
    app::set_debug_enabled(debug);

    // All logic lives in the UI-independent core; the server and the window
    // are two views over the same AppCore instance. The headless flag drops
    // the window, keeping only the control plane active.
    let core = AppCore::new();
    server::start(core.clone()).expect("failed to start REST control plane");

    println!("[bridge] control plane: http://127.0.0.1:8567 (GET / for the endpoint index)");

    if headless {
        keep_alive_without_ui();
    } else {
        run_window(core);
    }
}

/// `--headless` mode: the control plane threads keep running, and this loop
/// simply parks the process so it can be stopped with Ctrl+C.
fn keep_alive_without_ui() -> ! {
    println!("[bridge] headless mode — press Ctrl+C to quit");
    loop {
        std::thread::sleep(Duration::from_secs(3600));
    }
}

/// Drive the Slint windows: the small setup wizard first, then the hub.
/// One event loop serves both — the wizard hides itself and opens the hub.
fn run_window(core: Arc<AppCore>) {
    let wizard = WizardWindow::new().expect("failed to build the setup wizard");
    // One-time install: after the first completed setup, start on the
    // Connect-phone step (3) every launch — same screen, same Skip/Back.
    if bridge_core::paths::is_setup_done() {
        wizard.global::<BridgeState>().set_wizard_step(3);
    }
    // False once the hub is open (manual Skip/Open hub or phone auto-pass).
    let wizard_open = std::rc::Rc::new(std::cell::Cell::new(true));
    // True once the hub exists: re-finishing the wizard after a "back to
    // setup" round must reveal it, never build a second one.
    let hub_open = std::rc::Rc::new(std::cell::Cell::new(false));
    let reopen = WizardHandle {
        weak: wizard.as_weak(),
        open: wizard_open.clone(),
    };
    {
        let core = core.clone();
        let weak = wizard.as_weak();
        let reopen = reopen.clone();
        let hub_open = hub_open.clone();
        wizard.global::<BridgeState>().on_wizard_done(move || {
            if let Some(w) = weak.upgrade() {
                advance_from_wizard(&w, &core, &reopen, &hub_open);
            }
        });
    }
    {
        let c = core.clone();
        // Globals are singletons: wiring here serves the wizard window and,
        // later, the hub window alike.
        wizard.global::<BridgeState>().on_install_driver(move || {
            c.install_driver();
        });
        let c = core.clone();
        wizard.global::<BridgeState>().on_open_steamvr(move || {
            c.start_steamvr();
        });
    }
    start_wizard_watcher(core.clone(), reopen.clone(), hub_open.clone());
    wizard.show().expect("show setup wizard");

    println!("[bridge] running — press Ctrl+C to quit");
    // until_quit, not run_event_loop: the wizard hides during the hub
    // handoff (zero windows for a moment), which would end the loop early.
    slint::run_event_loop_until_quit().expect("bridge event loop failed");
    core.shutdown();
}

/// Re-show handle for the setup wizard: the hub's General / Diagnostics
/// buttons bring the wizard back at a chosen step instead of duplicating
/// the install flows.
#[derive(Clone)]
struct WizardHandle {
    weak: slint::Weak<WizardWindow>,
    open: std::rc::Rc<std::cell::Cell<bool>>,
}

/// Hide the wizard and open the hub exactly once, no matter which path
/// (Skip, Open hub, phone auto-pass) gets here first. The hub is shown
/// BEFORE the wizard hides: the event loop quits once no window is left,
/// so hiding first would end the process before the hub appears.
fn advance_from_wizard(
    w: &WizardWindow,
    core: &Arc<AppCore>,
    reopen: &WizardHandle,
    hub_open: &std::rc::Rc<std::cell::Cell<bool>>,
) {
    if reopen.open.replace(false) {
        bridge_core::paths::mark_setup_done();
        if !hub_open.replace(true) {
            eprintln!("[bridge] setup done — opening hub…");
            open_main(core, reopen);
        }
        let _ = w.hide();
        eprintln!("[bridge] hub shown");
    }
}

/// While the wizard is open, push install/phone state into it every 500 ms
/// and auto-pass to the hub once the phone app connects on the last step.
fn start_wizard_watcher(core: Arc<AppCore>, reopen: WizardHandle, hub_open: std::rc::Rc<std::cell::Cell<bool>>) {
    let timer = slint::Timer::default();
    timer.start(TimerMode::Repeated, Duration::from_millis(500), move || {
        if !reopen.open.get() {
            return;
        }
        core.refresh_driver_present();
        let snap = core.status();
        let Some(w) = reopen.weak.upgrade() else {
            reopen.open.set(false);
            return;
        };
        let global = w.global::<BridgeState>();
        global.set_phone_connected(snap.phone_connected);
        global.set_phone_ip(snap.phone_ip.clone().into());
        global.set_driver_present(snap.driver_present);
        global.set_install_busy(snap.install_busy);
        global.set_install_note(snap.install_note.clone().into());
        global.set_steamvr_note(snap.steamvr_note.clone().into());
        if snap.phone_connected && global.get_wizard_step() == 3 {
            advance_from_wizard(&w, &core, &reopen, &hub_open);
        }
    });
    Box::leak(Box::new(timer));
}

/// Build and show the hub window. The handle is leaked for the process
/// lifetime (same pattern as the state poller timer below).
fn open_main(core: &Arc<AppCore>, reopen: &WizardHandle) {
    let ui = MainWindow::new().expect("failed to build the bridge UI");
    ui.global::<BridgeState>()
        .set_app_version(format!("v{}", core::APP_VERSION).into());

    wire_callbacks(core, &ui, reopen);
    start_state_poller(core.clone(), ui.as_weak());

    ui.window().on_close_requested(|| {
        let _ = slint::quit_event_loop();
        slint::CloseRequestResponse::HideWindow
    });
    ui.show().expect("show bridge UI");
    Box::leak(Box::new(ui));
}

/// Delegate the UI's callbacks to the core, converting the Slint encoder index
/// into the encoder name string the driver expects.
fn wire_callbacks(core: &std::sync::Arc<AppCore>, ui: &MainWindow, reopen: &WizardHandle) {
    {
        let handle = reopen.clone();
        ui.global::<BridgeState>().on_reopen_wizard(move |step| {
            if let Some(w) = handle.weak.upgrade() {
                w.global::<BridgeState>().set_wizard_step(step);
                handle.open.set(true);
                let _ = w.show();
            }
        });
    }
    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>().on_install_driver(move || {
            core.install_driver();
        });
    }
    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>()
            .on_apply_settings(move |width, height, fps, bitrate, encoder_index| {
                let encoder = EncoderChoice::from(encoder_index);
                core.apply_settings(width, height, fps, bitrate, encoder.as_str());
            });
    }

    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>().on_test_link(move || {
            core.start_link_test();
        });
    }

    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>().on_hand_toggled(move |enabled| {
            core.set_hand_enabled(enabled);
        });
    }

    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>()
            .on_overlay_toggled(move |enabled| {
                core.set_hand_overlay(enabled);
            });
    }

    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>()
            .on_apply_hand_model(move |det, pres, track| {
                core.apply_hand_model(det, pres, track);
            });
    }
}

/// Poll the core into the UI on timers. The timers are leaked for the process
/// lifetime (Slint timers are dropped when the Timer is dropped, so leaking
/// keeps stats flowing even after `run_window` would otherwise unwind).
/// Split by cost: stats at 20 Hz, the 200-line log join at 1 Hz.
fn start_state_poller(core: Arc<AppCore>, weak: slint::Weak<MainWindow>) {
    // Fast poller (20 Hz): stats + frames.
    let fast = Timer::default();
    // Last delivered link-test result: a fresh one parks the bitrate slider
    // so the user fine-tunes from the recommendation instead of stale state.
    let mut last_test_result = 0i32;
    {
        let weak = weak.clone();
        let fast_core = core.clone();
        fast.start(TimerMode::Repeated, Duration::from_millis(50), move || {
            poll_stats(&fast_core, &weak, &mut last_test_result);
        });
    }
    Box::leak(Box::new(fast));
    // Slow poller (1 Hz): the log join only.
    let slow = Timer::default();
    {
        let weak = weak.clone();
        let slow_core = core.clone();
        slow.start(TimerMode::Repeated, Duration::from_secs(1), move || {
            let Some(ui) = weak.upgrade() else { return };
            slow_core.refresh_driver_present();
            ui.global::<BridgeState>()
                .set_log_text(slow_core.logs(200).join("\n").into());
        });
    }
    Box::leak(Box::new(slow));
}

fn poll_stats(core: &Arc<AppCore>, weak: &slint::Weak<MainWindow>, last_test_result: &mut i32) {
    let Some(ui) = weak.upgrade() else { return };
    let snap = core.status();
    let global = ui.global::<BridgeState>();
    global.set_driver_connected(snap.driver_connected);
    global.set_driver_present(snap.driver_present);
    global.set_install_busy(snap.install_busy);
    global.set_install_note(snap.install_note.clone().into());
    global.set_encoder_active(snap.encoder_active);
    global.set_encoder_name(snap.encoder_name.clone().into());
    global.set_phone_connected(snap.phone_connected);
    global.set_phone_ip(snap.phone_ip.clone().into());
    global.set_stream_fps(snap.stream_fps);
    global.set_latency_ms(snap.latency_ms);
    global.set_packets_total(snap.packets_total.min(i32::MAX as u64) as i32);
    global.set_gyro_fps(snap.gyro_fps);
    // `hand_fps` counts phone 0x11 packets; `hands_detected` is the
    // bridge-side sidecar count (`camera_detected_hands`).
    global.set_hand_fps(snap.hand_fps);
    global.set_hands_detected(snap.camera_detected_hands as i32);
    global.set_preview_driver_fps(snap.preview_driver_fps);
    global.set_preview_bitrate_kbps(snap.preview_bitrate_kbps);
    global.set_preview_frames(snap.preview_frames.min(i32::MAX as u64) as i32);
    global.set_preview_drops(snap.preview_drops.min(i32::MAX as u64) as i32);
    global.set_camera_connected(snap.camera_connected);
    global.set_camera_fps(snap.camera_fps);
    global.set_hand_enabled(snap.hand_enabled);
    global.set_hand_overlay(snap.hand_overlay);
    global.set_hand_det(snap.hand_min_detection);
    global.set_hand_pres(snap.hand_min_presence);
    global.set_hand_track(snap.hand_min_tracking);
    global.set_net_decoded_fps(snap.net_decoded_fps);
    global.set_net_stalls(snap.net_stalls as i32);
    global.set_gyro_x(format!("{:.2}", snap.latest_gyro_x).into());
    global.set_gyro_y(format!("{:.2}", snap.latest_gyro_y).into());
    global.set_gyro_z(format!("{:.2}", snap.latest_gyro_z).into());
    global.set_accel_x(format!("{:.2}", snap.latest_accel_x).into());
    global.set_accel_y(format!("{:.2}", snap.latest_accel_y).into());
    global.set_accel_z(format!("{:.2}", snap.latest_accel_z).into());
    global.set_mag_x(format!("{:.2}", snap.latest_mag_x).into());
    global.set_mag_y(format!("{:.2}", snap.latest_mag_y).into());
    global.set_mag_z(format!("{:.2}", snap.latest_mag_z).into());
    global.set_link_test_active(snap.link_test_active);
    global.set_link_test_note(snap.link_test_note.clone().into());
    if snap.link_test_result_mbps != 0 && snap.link_test_result_mbps != *last_test_result {
        *last_test_result = snap.link_test_result_mbps;
        global.set_bitrate(snap.link_test_result_mbps);
    }
    if snap.link_test_result_mbps == 0 {
        *last_test_result = 0;
    }
    if let Some(img) = core.take_preview_frame() {
        global.set_preview_frame(img);
    }
    if let Some(img) = core.take_camera_frame() {
        global.set_camera_frame(img);
    }
}