//! Shared, UI-independent runtime state for the bridge plus the accounting
//! helpers that turn inbound phone telemetry into the metrics the UI and the
//! REST API show.

use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

/// Cheaply-clonable handle to the bridge state. Every worker thread (REST
/// requests, driver heartbeat, phone telemetry) holds one and performs short,
/// scoped updates. Poisoned-lock failures are deliberately ignored wherever a
/// dropped metric update is harmless; `.expect()` is reserved for the places
/// where core logic really cannot proceed without the state.
pub type SharedState = Arc<Mutex<AppState>>;

/// The log view (UI + `/logs`) only keeps this many newest lines.
const MAX_LOG_LINES: usize = 200;

/// Global debug flag. Toggle via `CARDBOARD_DEBUG=1` env var or `POST /debug`.
static DEBUG_ENABLED: AtomicBool = AtomicBool::new(false);

/// Check if debug logging is enabled.
pub fn debug_enabled() -> bool {
    DEBUG_ENABLED.load(Ordering::Relaxed)
}

/// Toggle debug logging at runtime.
pub fn set_debug_enabled(enabled: bool) {
    DEBUG_ENABLED.store(enabled, Ordering::Relaxed);
}

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
    // -- local preview (BRIDGE_STATS over the discovery socket; always decoding) --
    pub preview_driver_fps: i32,   // encoder fps reported by the driver
    pub preview_bitrate_kbps: i32, // encoder bitrate reported by the driver
    pub preview_frames: u64,       // framed packets the driver has sent
    pub preview_drops: u64,        // packets the driver dropped on a full buffer
    // -- camera viewer (JPEG frames from phone on UDP 42072) --
    pub camera_connected: bool,
    pub camera_frame: Option<(u32, u32, Vec<u8>)>,
    pub camera_frame_time: Instant,
    pub camera_fps: i32,
    /// Hands detected by the bridge-side MediaPipe pipeline (separate from phone telemetry).
    pub camera_detected_hands: usize,
    // -- latest sensor sample (for UI display and driver forwarding) --
    pub latest_gyro: [f32; 3],
    pub latest_accel: [f32; 3],
    pub latest_mag: [f32; 3],
    pub latest_timestamp_ms: u64,
    // -- video-path health reported by the phone (tag 0x13, ~every 2 s) --
    pub net_frames_decoded: u32, // frames decoded since the last report
    pub net_stalls: u32,         // monotonic stall count from the phone
    pub net_decoded_fps: f32,    // decode rate observed on the phone
    // -- last settings pushed to the driver (manual Apply or Test Link recommendation) --
    pub applied_width: i32,
    pub applied_height: i32,
    pub applied_fps: i32,
    pub applied_bitrate_mbps: i32,
    // -- manual link test (UI "Test link" button): one 10 s sample, no auto-push --
    pub link_test_active: bool,
    pub link_test_result_mbps: i32, // 0 = no result yet
    pub link_test_note: String,
    // -- hand-tracking model (bridge-side MediaPipe sidecar, TCP 42073) --
    pub hand_enabled: bool,   // master switch: detect loop skips work while off
    pub hand_overlay: bool,   // draw the skeleton onto the camera viewer frame
    pub hand_min_detection: i32, // 0-100, MediaPipe min_hand_detection_confidence
    pub hand_min_presence: i32,  // 0-100, MediaPipe min_hand_presence_confidence
    pub hand_min_tracking: i32,  // 0-100, MediaPipe min_tracking_confidence
    // -- private accounting used to derive the per-second fps figures above --
    gyro_pulse_count: u64,
    hand_pulse_count: u64,
    camera_pulse_count: u64,
    fps_window_started: Instant,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            driver_connected: false,
            encoder_active: false,
            encoder_name: "gpu".into(),
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
            preview_driver_fps: 0,
            preview_bitrate_kbps: 0,
            preview_frames: 0,
            preview_drops: 0,
            camera_connected: false,
            camera_frame: None,
            camera_frame_time: Instant::now(),
            camera_fps: 0,
            camera_detected_hands: 0,
            latest_gyro: [0.0; 3],
            latest_accel: [0.0; 3],
            latest_mag: [0.0; 3],
            latest_timestamp_ms: 0,
            net_frames_decoded: 0,
            net_stalls: 0,
            net_decoded_fps: 0.0,
            // Matches the driver's boot settings (EncoderSetup.cpp) so the
            // link test verdict is relative to reality, not zeros.
            applied_width: 2880,
            applied_height: 1620,
            applied_fps: 60,
            applied_bitrate_mbps: 20,
            link_test_active: false,
            link_test_result_mbps: 0,
            link_test_note: String::new(),
            // On by default: stream + hand tracking ship together, and the
            // toggle stays available to opt out at runtime.
            hand_enabled: true,
            hand_overlay: true,
            hand_min_detection: 50,
            hand_min_presence: 50,
            hand_min_tracking: 50,
            gyro_pulse_count: 0,
            hand_pulse_count: 0,
            camera_pulse_count: 0,
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
    /// since the last roll. Called after every inbound telemetry packet and
    /// every camera frame so the fps figures stay fresh without a dedicated
    /// timing thread.
    pub fn recompute_fps(&mut self) {
        let now = Instant::now();
        let elapsed = now.duration_since(self.fps_window_started);
        if elapsed.as_millis() >= 1000 {
            // Clamp dt >= 1 ms so a re-roll in the same instant can't divide by zero.
            let dt = elapsed.as_secs_f32().max(0.001);
            self.gyro_fps = (self.gyro_pulse_count as f32 / dt) as i32;
            self.hand_fps = (self.hand_pulse_count as f32 / dt) as i32;
            self.camera_fps = (self.camera_pulse_count as f32 / dt) as i32;
            self.gyro_pulse_count = 0;
            self.hand_pulse_count = 0;
            self.camera_pulse_count = 0;
            self.fps_window_started = now;
        }
    }

    /// A gyro sample arrived: counts toward both the gyro rate and the total
    /// telemetry packet tally; stores the latest values for UI/driver.
    pub fn note_gyro(&mut self, sample: &crate::net::telemetry::GyroSample) {
        self.gyro_pulse_count += 1;
        self.packets_total += 1;
        self.latest_gyro = sample.angular_velocity;
        self.latest_accel = sample.acceleration;
        self.latest_mag = sample.magnetic_field;
        self.latest_timestamp_ms = sample.timestamp_ms;
    }

    /// A hand-tracking frame arrived: counts toward the hand rate and records
    /// how many hands the phone currently sees.
    pub fn note_hand(&mut self, hands: u8) {
        self.hand_pulse_count += 1;
        self.hands_detected = hands as i32;
        self.packets_total += 1;
    }

    /// Bitrate tiers (Mbps) the manual link test recommends along. One knob
    /// only: resolution/fps stay manual so a congested link never silently
    /// shrinks the picture, it just gets fewer bits.
    pub const BITRATE_TIERS_MBPS: [i32; 4] = [4, 8, 12, 20];

    /// Fold a phone net-stats report into the counters the UI shows. Never
    /// pushes to the driver by itself — bitrate changes only via manual Apply
    /// (optionally seeded by the link test recommendation).
    pub fn note_net_stats(&mut self, stats: &crate::net::telemetry::NetStats) {
        self.net_frames_decoded = stats.frames_decoded;
        self.net_stalls = stats.stalls;
        self.net_decoded_fps = stats.decoded_fps;
        self.packets_total += 1;
    }

    /// Record a settings push (manual Apply) so the link test verdict stays
    /// relative to what the driver actually got.
    pub fn record_applied_settings(&mut self, width: i32, height: i32, fps: i32, bitrate_mbps: i32) {
        self.applied_width = width;
        self.applied_height = height;
        self.applied_fps = fps;
        self.applied_bitrate_mbps = bitrate_mbps;
    }

    fn next_tier_down(current_mbps: i32) -> Option<i32> {
        let mut best: Option<i32> = None;
        for tier in Self::BITRATE_TIERS_MBPS {
            if tier < current_mbps {
                best = Some(tier);
            }
        }
        best
    }

    fn next_tier_up(current_mbps: i32) -> Option<i32> {
        for tier in Self::BITRATE_TIERS_MBPS {
            if tier > current_mbps {
                return Some(tier);
            }
        }
        None
    }

    /// Verdict for one manual link test: compare the phone's stall counter
    /// across the sample window plus its decode rate against the encoder
    /// target. Pure so the UI thread and unit tests share it. Returns
    /// (recommended_mbps, one-line note).
    pub fn link_test_verdict(
        stalls_before: u32,
        stalls_after: u32,
        decoded_fps: f32,
        applied_fps: i32,
        applied_mbps: i32,
    ) -> (i32, &'static str) {
        if stalls_after > stalls_before {
            let rec = Self::next_tier_down(applied_mbps).unwrap_or(Self::BITRATE_TIERS_MBPS[0]);
            return (rec, "stalls seen during test — recommend lower");
        }
        if decoded_fps >= 0.9 * applied_fps.max(1) as f32 {
            match Self::next_tier_up(applied_mbps) {
                Some(higher) => (higher, "link clean — can try higher"),
                None => (applied_mbps, "link clean at top tier"),
            }
        } else {
            (applied_mbps, "no stalls but decode below target — keep current")
        }
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
    /// Counts toward the camera fps rate.
    pub fn note_camera_frame(&mut self, w: u32, h: u32, rgba: Vec<u8>) {
        self.camera_pulse_count += 1;
        self.store_camera_frame(w, h, rgba);
        self.recompute_fps();
    }

    /// Store the latest decoded camera frame without counting fps.
    /// Used by the MediaPipe worker for annotated (overlay) frames so the
    /// fps pill measures the display rate, not detect completions.
    pub fn store_camera_frame(&mut self, w: u32, h: u32, rgba: Vec<u8>) {
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

/// Append a debug-only line to the ring log. Only fires when debug is enabled.
macro_rules! debug_log {
    ($state:expr, $($arg:tt)*) => {
        if $crate::app::debug_enabled() {
            if let Ok(mut s) = $state.lock() {
                s.push_log(format!($($arg)*));
            }
        }
    };
}
pub(crate) use debug_log;

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
        s.note_gyro(&crate::net::telemetry::GyroSample::default());
        s.note_hand(2);
        assert_eq!(s.packets_total, 2);
        assert_eq!(s.hands_detected, 2);
    }

    #[test]
    fn hand_tracking_is_on_by_default() {
        // Stream + tracking ship together: a fresh bridge detects on camera
        // frames immediately, no toggle needed. The UI toggle opts out.
        assert!(AppState::default().hand_enabled);
    }

    fn net_stats(stalls: u32, fps: f32) -> crate::net::telemetry::NetStats {
        crate::net::telemetry::NetStats {
            timestamp_ms: 1,
            frames_decoded: 120,
            stalls,
            decoded_fps: fps,
        }
    }

    fn live_state() -> AppState {
        let mut s = AppState::default();
        s.driver_connected = true;
        s
    }

    #[test]
    fn net_stats_report_only_updates_counters() {
        let mut s = live_state();
        s.note_net_stats(&net_stats(3, 55.0));
        assert_eq!(s.net_stalls, 3);
        assert_eq!(s.net_decoded_fps, 55.0);
        assert_eq!(s.net_frames_decoded, 120);
        // No automatic push: the applied settings are untouched.
        assert_eq!(s.applied_bitrate_mbps, 20);
    }

    #[test]
    fn record_applied_settings_stores_the_push() {
        let mut s = live_state();
        s.record_applied_settings(2880, 1620, 60, 12);
        assert_eq!(s.applied_width, 2880);
        assert_eq!(s.applied_height, 1620);
        assert_eq!(s.applied_fps, 60);
        assert_eq!(s.applied_bitrate_mbps, 12);
    }

    #[test]
    fn link_test_stalls_recommend_one_tier_down() {
        assert_eq!(
            AppState::link_test_verdict(3, 5, 20.0, 60, 20),
            (12, "stalls seen during test — recommend lower")
        );
    }

    #[test]
    fn link_test_stalls_at_bottom_recommend_bottom() {
        assert_eq!(
            AppState::link_test_verdict(0, 1, 5.0, 60, 4),
            (4, "stalls seen during test — recommend lower")
        );
    }

    #[test]
    fn link_test_clean_recommends_one_tier_up() {
        assert_eq!(
            AppState::link_test_verdict(2, 2, 59.0, 60, 12),
            (20, "link clean — can try higher")
        );
    }

    #[test]
    fn link_test_clean_at_top_stays() {
        assert_eq!(
            AppState::link_test_verdict(0, 0, 60.0, 60, 20),
            (20, "link clean at top tier")
        );
    }

    #[test]
    fn link_test_low_decode_without_stalls_keeps_current() {
        assert_eq!(
            AppState::link_test_verdict(1, 1, 20.0, 60, 12),
            (12, "no stalls but decode below target — keep current")
        );
    }
}