//! Shared, UI-independent runtime state for the bridge plus the accounting
//! helpers that turn inbound phone telemetry into the metrics the UI and the
//! REST API show.

use std::sync::{Arc, Mutex};
use std::time::Instant;

/// Cheaply-clonable handle to the bridge state. Every worker thread (REST
/// requests, driver heartbeat, phone telemetry) holds one and performs short,
/// scoped updates. Poisoned-lock failures are deliberately ignored wherever a
/// dropped metric update is harmless; `.expect()` is reserved for the places
/// where core logic really cannot proceed without the state.
pub type SharedState = Arc<Mutex<AppState>>;

/// The log view (UI + `/logs`) only keeps this many newest lines.
const MAX_LOG_LINES: usize = 200;

/// Everything worth knowing about the current session. Plain data only — no
/// Slint or network types leak in here, which keeps the REST/UI views trivial.
#[derive(Debug)]
pub struct AppState {
    // -- public snapshot of connections & stream health --
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
    pub log: Vec<String>,
    // -- local preview (BRIDGE_PREVIEW / BRIDGE_STATS over the discovery socket) --
    pub preview_enabled: bool,
    pub preview_driver_fps: i32,   // encoder fps reported by the driver
    pub preview_bitrate_kbps: i32, // encoder bitrate reported by the driver
    pub preview_frames: u64,       // framed packets the driver has sent
    pub preview_drops: u64,        // packets the driver dropped on a full buffer
    // -- camera viewer (JPEG frames from phone on UDP 42072) --
    pub camera_connected: bool,
    pub camera_frame: Option<(u32, u32, Vec<u8>)>,
    pub camera_frame_time: Instant,
    /// Hands detected by the bridge-side ONNX pipeline (separate from phone telemetry).
    pub camera_detected_hands: usize,
    // -- private accounting used to derive the per-second fps figures above --
    gyro_pulse_count: u64,
    hand_pulse_count: u64,
    fps_window_started: Instant,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            driver_connected: false,
            encoder_active: false,
            encoder_name: "auto".into(),
            phone_connected: false,
            phone_ip: "0.0.0.0".into(),
            stream_fps: 0,
            latency_ms: 0,
            packets_total: 0,
            gyro_fps: 0,
            hand_fps: 0,
            hands_detected: 0,
            // The very first line of every bridge log.
            log: vec!["bridge started".to_string()],
            preview_enabled: false,
            preview_driver_fps: 0,
            preview_bitrate_kbps: 0,
            preview_frames: 0,
            preview_drops: 0,
            camera_connected: false,
            camera_frame: None,
            camera_frame_time: Instant::now(),
            camera_detected_hands: 0,
            gyro_pulse_count: 0,
            hand_pulse_count: 0,
            fps_window_started: Instant::now(),
        }
    }
}

impl AppState {
    /// Append a line to the ring log, trimming the oldest entries once the
    /// cap is exceeded so the log view never grows without bound.
    pub fn push_log(&mut self, line: String) {
        self.log.push(line);
        if self.log.len() > MAX_LOG_LINES {
            let excess = self.log.len() - MAX_LOG_LINES;
            self.log.drain(0..excess);
        }
    }

    /// Roll the per-second rate counters once at least one second has elapsed
    /// since the last roll. Called after every inbound telemetry packet so the
    /// fps figures stay fresh without a dedicated timing thread.
    pub fn recompute_fps(&mut self) {
        let now = Instant::now();
        let elapsed = now.duration_since(self.fps_window_started);
        if elapsed.as_millis() >= 1000 {
            // Clamp dt >= 1 ms so a re-roll in the same instant can't divide by zero.
            let dt = elapsed.as_secs_f32().max(0.001);
            self.gyro_fps = (self.gyro_pulse_count as f32 / dt) as i32;
            self.hand_fps = (self.hand_pulse_count as f32 / dt) as i32;
            self.gyro_pulse_count = 0;
            self.hand_pulse_count = 0;
            self.fps_window_started = now;
        }
    }

    /// A gyro sample arrived: counts toward both the gyro rate and the total
    /// telemetry packet tally.
    pub fn note_gyro(&mut self) {
        self.gyro_pulse_count += 1;
        self.packets_total += 1;
    }

    /// A hand-tracking frame arrived: counts toward the hand rate and records
    /// how many hands the phone currently sees.
    pub fn note_hand(&mut self, hands: u8) {
        self.hand_pulse_count += 1;
        self.hands_detected = hands as i32;
        self.packets_total += 1;
    }

    /// The driver's periodic BRIDGE_STATS landed: update the live preview
    /// numbers the UI shows. The frames counter coming from the driver is
    /// multi-target (it counts each phone copy too), which is fine for a
    /// monitoring display.
    pub fn note_preview_stats(&mut self, fps: i32, bitrate_kbps: i32, frames: u64, drops: u64) {
        self.preview_driver_fps = fps;
        self.preview_bitrate_kbps = bitrate_kbps;
        self.preview_frames = frames;
        self.preview_drops = drops;
    }

    /// Store the latest decoded camera frame (RGBA) for the viewer.
    pub fn note_camera_frame(&mut self, w: u32, h: u32, rgba: Vec<u8>) {
        self.camera_frame = Some((w, h, rgba));
        self.camera_frame_time = Instant::now();
        if !self.camera_connected {
            self.camera_connected = true;
            self.push_log("camera connected".into());
        }
    }

    /// Mark camera as disconnected if no frames have arrived recently.
    pub fn check_camera_liveness(&mut self) {
        if self.camera_connected && self.camera_frame_time.elapsed().as_secs() > 3 {
            self.camera_connected = false;
            self.push_log("camera disconnected".into());
        }
    }
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;

    #[test]
    fn log_ring_keeps_only_the_newest_lines() {
        let mut s = AppState::default();
        for i in 0..(MAX_LOG_LINES + 5) {
            s.push_log(format!("line {}", i));
        }
        assert_eq!(s.log.len(), MAX_LOG_LINES);
        assert_eq!(s.log.first().unwrap(), "line 5");
        assert_eq!(*s.log.last().unwrap(), format!("line {}", MAX_LOG_LINES + 4));
    }

    #[test]
    fn fps_counters_roll_after_a_second() {
        let mut s = AppState::default();
        // Pretend a two-second measurement window just closed.
        s.fps_window_started = Instant::now() - Duration::from_secs(2);
        s.gyro_pulse_count = 2000;
        s.hand_pulse_count = 4;
        s.recompute_fps();
        // ~1000 gyro/sec and ~2 hand/sec; allow generous slop for the wall
        // clock having ticked a bit past the nominal 2 s window.
        assert!((500..=2000).contains(&s.gyro_fps));
        assert!((1..=4).contains(&s.hand_fps));
        assert_eq!(s.gyro_pulse_count, 0);
        assert_eq!(s.hand_pulse_count, 0);
    }

    #[test]
    fn fps_counters_do_not_roll_within_the_first_second() {
        let mut s = AppState::default();
        s.gyro_pulse_count = 5;
        s.recompute_fps();
        assert_eq!(s.gyro_fps, 0);
        assert_eq!(s.gyro_pulse_count, 5);
    }

    #[test]
    fn every_telemetry_packet_counts_toward_the_tally() {
        let mut s = AppState::default();
        s.note_gyro();
        s.note_hand(2);
        assert_eq!(s.packets_total, 2);
        assert_eq!(s.hands_detected, 2);
    }
}