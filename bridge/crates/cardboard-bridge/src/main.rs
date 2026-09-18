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

    // Enable debug logging if CARDBOARD_DEBUG=1 or --debug flag.
    let debug = std::env::args().any(|a| a == "--debug")
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

/// Drive the Slint window: forward "apply settings" to the core and keep the
/// `BridgeState` global refreshed from the core every 250 ms.
fn run_window(core: Arc<AppCore>) {
    let ui = MainWindow::new().expect("failed to build the bridge UI");
    ui.global::<BridgeState>()
        .set_app_version(format!("v{}", core::APP_VERSION).into());

    wire_callbacks(&core, &ui);
    start_state_poller(core.clone(), ui.as_weak());

    println!("[bridge] running — press Ctrl+C to quit");
    ui.run().expect("bridge event loop failed");
    core.shutdown();
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

/// Poll the core into the UI on a timer. The timer is leaked for the process
/// lifetime (Slint timers are dropped when the Timer is dropped, so leaking
/// keeps stats flowing even after `run_window` would otherwise unwind).
fn start_state_poller(core: Arc<AppCore>, weak: slint::Weak<MainWindow>) {
    let timer = Timer::default();
    // Last delivered link-test result: a fresh one parks the bitrate slider
    // so the user fine-tunes from the recommendation instead of stale state.
    let mut last_test_result = 0i32;
    timer.start(TimerMode::Repeated, Duration::from_millis(50), move || {
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
        global.set_packets_total(snap.packets_total.min(i32::MAX as u64) as i32);
        global.set_gyro_fps(snap.gyro_fps);
        // Hand page shows the bridge-side MediaPipe pipeline (camera 42072
        // -> TCP 42073), not phone telemetry hands: `hands_detected`/`hand_fps`
        // count phone 0x11 packets, `camera_detected_hands` is the sidecar.
        global.set_hand_fps(snap.camera_fps);
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
        if snap.link_test_result_mbps != 0 && snap.link_test_result_mbps != last_test_result {
            last_test_result = snap.link_test_result_mbps;
            global.set_bitrate(snap.link_test_result_mbps);
        }
        if snap.link_test_result_mbps == 0 {
            last_test_result = 0;
        }
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