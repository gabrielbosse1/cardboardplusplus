slint::include_modules!();

mod app;
mod core;
mod hand_overlay;
mod net;
mod server;

use std::sync::Arc;
use std::time::Duration;

use core::AppCore;
use net::EncoderChoice;
use slint::{Timer, TimerMode};

fn main() {
    let headless = std::env::args().any(|a| a == "--headless");

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

/// Drive the Slint window: forward "apply settings" to the core and keep the
/// `BridgeState` global refreshed from the core every 250 ms.
fn run_window(core: Arc<AppCore>) {
    let ui = MainWindow::new().expect("failed to build the bridge UI");
    ui.global::<BridgeState>()
        .set_app_version(format!("v{}", core::APP_VERSION).into());

    wire_callbacks(&core, &ui);
    start_state_poller(core, ui.as_weak());

    println!("[bridge] running — press Ctrl+C to quit");
    ui.run().expect("bridge event loop failed");
}

/// Delegate the UI's callbacks to the core, converting the Slint encoder index
/// into the encoder name string the driver expects.
fn wire_callbacks(core: &std::sync::Arc<AppCore>, ui: &MainWindow) {
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
        ui.global::<BridgeState>()
            .on_toggle_preview(move |enabled| {
                core.set_preview(enabled);
            });
    }

    {
        let core = std::sync::Arc::clone(core);
        ui.global::<BridgeState>().on_open_preview(move || {
            core.open_ffplay_preview();
        });
    }
}

/// Poll the core into the UI on a timer. The timer is leaked for the process
/// lifetime (Slint timers are dropped when the Timer is dropped, so leaking
/// keeps stats flowing even after `run_window` would otherwise unwind).
fn start_state_poller(core: Arc<AppCore>, weak: slint::Weak<MainWindow>) {
    let timer = Timer::default();
    timer.start(TimerMode::Repeated, Duration::from_millis(250), move || {
        let Some(ui) = weak.upgrade() else { return };
        let snap = core.status();
        let global = ui.global::<BridgeState>();
        global.set_driver_connected(snap.driver_connected);
        global.set_encoder_active(snap.encoder_active);
        global.set_encoder_name(snap.encoder_name.clone().into());
        global.set_phone_connected(snap.phone_connected);
        global.set_phone_ip(snap.phone_ip.clone().into());
        global.set_stream_fps(snap.stream_fps);
        global.set_latency_ms(snap.latency_ms);
        global.set_packets_total(snap.packets_total as i32);
        global.set_gyro_fps(snap.gyro_fps);
        global.set_hand_fps(snap.hand_fps);
        global.set_hands_detected(snap.hands_detected);
        global.set_preview_enabled(snap.preview_enabled);
        global.set_preview_driver_fps(snap.preview_driver_fps);
        global.set_preview_bitrate_kbps(snap.preview_bitrate_kbps);
        global.set_preview_frames(snap.preview_frames as i32);
        global.set_preview_drops(snap.preview_drops as i32);
        global.set_camera_connected(snap.camera_connected);
        if let Some(img) = core.take_preview_frame() {
            global.set_preview_frame(img);
        }
        if let Some(img) = core.take_camera_frame() {
            global.set_camera_frame(img);
        }
        global.set_log_text(core.logs(200).join("\n").into());
    });
    Box::leak(Box::new(timer));
}