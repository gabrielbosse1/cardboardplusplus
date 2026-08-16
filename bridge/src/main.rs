mod driver_connection;

use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use driver_connection::ConnectionManager;

slint::include_modules!();

#[derive(Default, Clone, Debug)]
struct UiState {
    compositor_fps: f32,
    encoder_fps: f32,
    driver_connected: bool,
    phone_connected: bool,
    phone_fps: f32,
    phone_fps_1low: f32,
    streaming: bool,
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();

    let conn = ConnectionManager::new();
    let state = conn.get_state();
    let handle = tokio::runtime::Handle::current();

    let ui_state = Arc::new(Mutex::new(UiState::default()));
    let streaming = Arc::new(AtomicBool::new(false));

    // Tokio task mirrors bridge state into shared UiState
    {
        let ui_state = ui_state.clone();
        let bridge_state = state.clone();
        let streaming = streaming.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_millis(200)).await;
                let s = bridge_state.read().await;
                let mut ui = ui_state.lock().unwrap();
                ui.compositor_fps = s.compositor_fps;
                ui.encoder_fps = s.encoder_fps;
                ui.driver_connected = s.driver_connected;
                ui.phone_connected = s.phone_connected;
                ui.phone_fps = s.phone_fps;
                ui.phone_fps_1low = s.phone_fps_1low;
                ui.streaming = streaming.load(Ordering::SeqCst);
            }
        });
    }

    // Run Slint UI on a dedicated std::thread
    let conn_for_ui = conn.clone();
    let streaming_for_ui = streaming.clone();
    let ui_state_for_ui = ui_state.clone();

    let ui_handle = std::thread::spawn(move || {
        let ui = MainWindow::new().unwrap();
        let ui_weak = ui.as_weak();

        {
            let conn = conn_for_ui.clone();
            let flag = streaming_for_ui.clone();
            let h = handle.clone();
            ui_weak.unwrap().on_start_stream(move || {
                tracing::info!("[UI] Start stream requested");
                flag.store(true, Ordering::SeqCst);
                let c = conn.clone();
                h.spawn(async move {
                    c.send_command("START_STREAM").await;
                });
            });
        }

        {
            let conn = conn_for_ui.clone();
            let flag = streaming_for_ui.clone();
            let h = handle.clone();
            ui_weak.unwrap().on_stop_stream(move || {
                tracing::info!("[UI] Stop stream requested");
                flag.store(false, Ordering::SeqCst);
                let c = conn.clone();
                h.spawn(async move {
                    c.send_command("STOP_STREAM").await;
                });
            });
        }

        let timer_state = ui_state_for_ui.clone();
        let timer_weak = ui_weak.clone();
        let timer = slint::Timer::default();
        timer.start(
            slint::TimerMode::Repeated,
            std::time::Duration::from_millis(250),
            move || {
                let ui_ref = match timer_weak.upgrade() {
                    Some(u) => u,
                    None => return,
                };
                let s = timer_state.lock().unwrap();
                ui_ref.set_compositor_fps(s.compositor_fps);
                ui_ref.set_encoder_fps(s.encoder_fps);
                ui_ref.set_driver_connected(s.driver_connected);
                ui_ref.set_phone_connected(s.phone_connected);
                ui_ref.set_phone_fps(s.phone_fps);
                ui_ref.set_phone_fps_1low(s.phone_fps_1low);
                ui_ref.set_streaming(s.streaming);
            },
        );

        tracing::info!("[BRIDGE] UI started");
        ui.run().unwrap();
    });

    ui_handle.join().unwrap();
}
