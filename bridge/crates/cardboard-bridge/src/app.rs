use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;
/// Handle shared by every thread (net loops, UI pollers, REST): a mutex
/// around the single AppState. Threads lock briefly per update.
pub type SharedState = Arc<Mutex<AppState>>;
/// Ring-log cap: keeps memory flat no matter how chatty the net threads get.
const MAX_LOG_LINES: usize = 200;
static DEBUG_ENABLED: AtomicBool = AtomicBool::new(false);
/// Verbose logging switch: always on in debug builds, runtime-toggled in
/// release via --debug, CARDBOARD_DEBUG=1, or POST /debug.
pub fn debug_enabled() -> bool {
    cfg!(debug_assertions) || DEBUG_ENABLED.load(Ordering::Relaxed)
}
/// Flips verbose logging at runtime. Called from main's flag parsing and the
/// REST /debug endpoint.
pub fn set_debug_enabled(enabled: bool) {
    DEBUG_ENABLED.store(enabled, Ordering::Relaxed);
}
/// The whole bridge UI model in one struct: connection pills, live sensor
/// readouts, camera/hand state, applied stream settings, installer progress.
/// Every net thread folds updates in via the note_*/push_log methods below;
/// the Slint pollers and REST handlers only read snapshots out.
#[derive(Debug)]
pub struct AppState {
    pub driver_connected: bool,
    pub encoder_active: bool,
    pub encoder_name: String,
    pub driver_version: String,
    pub phone_version: String,
    pub phone_connected: bool,
    pub phone_ip: String,
    pub stream_fps: i32,
    pub latency_ms: i32,
    pub packets_total: u64,
    pub gyro_fps: i32,
    pub hand_fps: i32,
    pub hands_detected: i32,
    pub log: Vec<String>,
    pub preview_driver_fps: i32,
    pub preview_bitrate_kbps: i32,
    pub preview_frames: u64,
    pub preview_drops: u64,
    pub camera_connected: bool,
    pub camera_frame: Option<(u32, u32, Vec<u8>)>,
    pub camera_frame_time: Instant,
    pub camera_fps: i32,
    pub camera_detected_hands: usize,
    pub latest_gyro: [f32; 3],
    pub latest_accel: [f32; 3],
    pub latest_mag: [f32; 3],
    pub latest_timestamp_ms: u64,
    pub net_frames_decoded: u32,
    pub net_stalls: u32,
    pub net_decoded_fps: f32,
    pub applied_width: i32,
    pub applied_height: i32,
    pub applied_fps: i32,
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
    pub install_progress: f32,
    pub steamvr_running: bool,
    pub driver_present: bool,
    pub steamvr_note: String,
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
            driver_version: "unknown".into(),
            phone_version: "unknown".into(),
            phone_connected: false,
            phone_ip: "0.0.0.0".into(),
            stream_fps: 0,
            latency_ms: 0,
            packets_total: 0,
            gyro_fps: 0,
            hand_fps: 0,
            hands_detected: 0,
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
            applied_width: 2880,
            applied_height: 1620,
            applied_fps: 60,
            applied_bitrate_mbps: 20,
            link_test_active: false,
            link_test_result_mbps: 0,
            link_test_note: String::new(),
            hand_enabled: true,
            hand_overlay: true,
            hand_min_detection: 50,
            hand_min_presence: 50,
            hand_min_tracking: 50,
            install_busy: false,
            install_note: String::new(),
            install_progress: 0.0,
            steamvr_running: false,
            driver_present: false,
            steamvr_note: String::new(),
            gyro_pulse_count: 0,
            hand_pulse_count: 0,
            camera_pulse_count: 0,
            fps_window_started: Instant::now(),
        }
    }
}
impl AppState {
    /// Appends a ring-log line, dropping the oldest past MAX_LOG_LINES.
    /// All user-visible events (connects, installs, timeouts) flow through here.
    pub fn push_log(&mut self, line: String) {
        self.log.push(line);
        if self.log.len() > MAX_LOG_LINES {
            let excess = self.log.len() - MAX_LOG_LINES;
            self.log.drain(0..excess);
        }
    }
    /// Rolls the 1-second FPS window: converts pulse counts (gyro/hand/camera)
    /// into per-second rates and restarts the window. Called on every sample;
    /// cheap no-op until a full second elapsed.
    pub fn recompute_fps(&mut self) {
        let now = Instant::now();
        let elapsed = now.duration_since(self.fps_window_started);
        if elapsed.as_millis() >= 1000 {
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
    /// Folds one gyro sample in: latest values for the diagnostics tab plus
    /// pulse/packet counters for the FPS meter. Called from phone.rs.
    pub fn note_gyro(&mut self, sample: &crate::net::telemetry::GyroSample) {
        self.gyro_pulse_count += 1;
        self.packets_total += 1;
        self.latest_gyro = sample.angular_velocity;
        self.latest_accel = sample.acceleration;
        self.latest_mag = sample.magnetic_field;
        self.latest_timestamp_ms = sample.timestamp_ms;
    }
    /// Folds one hand hint in: visible-hand count plus counters. Called from
    /// phone.rs for trusted senders only.
    pub fn note_hand(&mut self, hands: u8) {
        self.hand_pulse_count += 1;
        self.hands_detected = hands as i32;
        self.packets_total += 1;
    }
    /// Adaptive-bitrate ladder (Mbps) the link test walks one step at a time.
    pub const BITRATE_TIERS_MBPS: [i32; 4] = [4, 8, 12, 20];
    /// Records phone decode health for the verdict in link_test_verdict.
    /// Never pushes bitrate by itself — only the explicit link test does.
    pub fn note_net_stats(&mut self, stats: &crate::net::telemetry::NetStats) {
        self.net_frames_decoded = stats.frames_decoded;
        self.net_stalls = stats.stalls;
        self.net_decoded_fps = stats.decoded_fps;
        self.packets_total += 1;
    }
    /// Remembers the last settings pushed to the driver so partial REST/UI
    /// updates can merge against them. Called by core.apply_settings.
    pub fn record_applied_settings(&mut self, width: i32, height: i32, fps: i32, bitrate_mbps: i32) {
        self.applied_width = width;
        self.applied_height = height;
        self.applied_fps = fps;
        self.applied_bitrate_mbps = bitrate_mbps;
    }
    /// Highest tier strictly below `current_mbps`. None at the bottom, in
    /// which case the verdict keeps the bottom tier.
    fn next_tier_down(current_mbps: i32) -> Option<i32> {
        let mut best: Option<i32> = None;
        for tier in Self::BITRATE_TIERS_MBPS {
            if tier < current_mbps {
                best = Some(tier);
            }
        }
        best
    }
    /// Lowest tier strictly above `current_mbps`. None at the top, in which
    /// case the verdict keeps the top tier.
    fn next_tier_up(current_mbps: i32) -> Option<i32> {
        for tier in Self::BITRATE_TIERS_MBPS {
            if tier > current_mbps {
                return Some(tier);
            }
        }
        None
    }
    /// Pure link-test verdict from stall delta + decoded fps vs the applied
    /// stream fps: new stalls step one tier down, a clean fast link steps one
    /// up, a slow-but-stall-free link holds. Returns (recommended_mbps, note).
    /// Called when the core's link test finishes; unit-tested below.
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
    /// Folds one BRIDGE_STATS line in: driver fps/bitrate/frames/drops for the
    /// preview readout. stream_fps mirrors the driver fps. Called from driver.rs.
    pub fn note_preview_stats(&mut self, fps: i32, bitrate_kbps: i32, frames: u64, drops: u64) {
        self.preview_driver_fps = fps;
        self.stream_fps = fps;
        self.preview_bitrate_kbps = bitrate_kbps;
        self.preview_frames = frames;
        self.preview_drops = drops;
    }
    /// Stores a decoded camera frame for the UI poller and counts it toward
    /// camera FPS. Called from the camera detect thread.
    pub fn note_camera_frame(&mut self, w: u32, h: u32, rgba: Vec<u8>) {
        self.camera_pulse_count += 1;
        self.store_camera_frame(w, h, rgba);
        self.recompute_fps();
    }
    /// Stores the frame without touching FPS counters: the overlay path where
    /// the frame was already counted. First frame also flips the connected
    /// pill and logs it.
    pub fn store_camera_frame(&mut self, w: u32, h: u32, rgba: Vec<u8>) {
        self.camera_frame = Some((w, h, rgba));
        self.camera_frame_time = Instant::now();
        if !self.camera_connected {
            self.camera_connected = true;
            self.push_log("camera connected".into());
        }
    }
    /// Drops the camera-connected pill when no frame arrived for 3s. Polled
    /// by the UI slow loop so a dead phone camera clears the indicator.
    pub fn check_camera_liveness(&mut self) {
        if self.camera_connected && self.camera_frame_time.elapsed().as_secs() > 3 {
            self.camera_connected = false;
            self.push_log("camera disconnected".into());
        }
    }
}
/// Gated log helper for net threads: pushes the formatted line only when
/// verbose logging is on. Locks the state briefly; drops the line on
/// contention instead of blocking the hot path.
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
    // State-method tests: ring-log cap, FPS window roll, per-packet tallies,
    // and the link-test verdict ladder (stalls step down, clean links step
    // up, top/bottom tiers clamp). Test names read as the spec.
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
        s.fps_window_started = Instant::now() - Duration::from_secs(2);
        s.gyro_pulse_count = 2000;
        s.hand_pulse_count = 4;
        s.recompute_fps();
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