use std::net::{SocketAddr, UdpSocket};
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use crate::app::SharedState;
use crate::net::telemetry::{self, TelemetryPacket};
use crate::net::{SENSOR_PORT, TELEMETRY_PORT};
/// Lazily-bound socket for the sensor forward path (bridge -> driver, UDP
/// 42074). Created on first relay so unit tests using apply_packet never bind.
static SENSOR_SOCK: OnceLock<UdpSocket> = OnceLock::new();
fn sensor_sock() -> &'static UdpSocket {
    SENSOR_SOCK.get_or_init(|| UdpSocket::bind("0.0.0.0:0").expect("sensor forward socket"))
}
const POLL_INTERVAL: Duration = Duration::from_millis(50);
const PHONE_TIMEOUT: Duration = Duration::from_secs(4);
/// Starts the telemetry listener (UDP 42071) on a background thread. Binds the
/// socket, logs-and-returns on failure, and hands ownership to telemetry_loop.
pub fn spawn(state: SharedState) {
    let sock = match UdpSocket::bind(format!("0.0.0.0:{TELEMETRY_PORT}")) {
        Ok(sock) => sock,
        Err(err) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("telemetry bind on {TELEMETRY_PORT} failed: {err}"));
            }
            return;
        }
    };
    let _ = sock.set_read_timeout(Some(POLL_INTERVAL));
    if let Ok(mut s) = state.lock() {
        s.push_log(format!("telemetry listener on udp {TELEMETRY_PORT}"));
    }
    std::thread::spawn(move || telemetry_loop(sock, state));
}
/// Receive loop: parses each datagram, relays raw gyro/rotation frames to the
/// driver (latest-wins, no decode needed), and folds the parsed packet into
/// shared state. Read timeouts just re-arm the loop; after 10s of total
/// silence it logs a one-time firewall hint, and every tick refreshes the
/// phone-timeout check so a vanished phone clears the connected pill.
fn telemetry_loop(sock: UdpSocket, state: SharedState) {
    let mut buf = [0u8; 65535];
    let mut last_seen = None::<Instant>;
    let started = Instant::now();
    let mut silence_hinted = false;
    loop {
        match sock.recv_from(&mut buf) {
            Ok((n, src)) => {
                let packet = telemetry::parse_packet(&buf[..n]);
                if matches!(packet, TelemetryPacket::Unknown) {
                    crate::debug_log!(
                        state,
                        "[phone] ignored unknown {}-byte datagram from {src}",
                        n
                    );
                } else {
                    last_seen = Some(Instant::now());
                }
                let raw = &buf[..n];
                if !raw.is_empty()
                    && ((raw[0] == 0x10 && n == 45) || (raw[0] == 0x12 && n == 25))
                {
                    relay_raw_to_driver(raw);
                }
                apply_packet(&state, packet, src);
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock
                || e.kind() == std::io::ErrorKind::TimedOut =>
            {
            }
            Err(_) => {
            }
        }
        if !silence_hinted && last_seen.is_none() && started.elapsed() > Duration::from_secs(10) {
            silence_hinted = true;
            if let Ok(mut s) = state.lock() {
                s.push_log(format!(
                    "telemetry bound but silent after 10s — check Windows firewall inbound UDP {TELEMETRY_PORT} and the phone's Wi-Fi"
                ));
            }
        }
        mark_phone_gone_if_stale(&state, last_seen);
    }
}
/// Folds one parsed packet into shared state. Hello/gyro/rotation adopt the
/// sender IP (the phone roams DHCP); hand/net-stats from a non-connected IP
/// are ignored so a second phone on the LAN cannot inject input. Gyro samples
/// feed the diagnostics FPS meter; rotation samples are debug-logged here
/// (the actual forward to the driver happens unparsed in telemetry_loop).
fn apply_packet(state: &SharedState, packet: TelemetryPacket, src: SocketAddr) {
    let mut debug_msg: Option<String> = None;
    if let Ok(mut s) = state.lock() {
        match packet {
            TelemetryPacket::Hello(version) => {
                let new_ip = src.ip().to_string();
                if s.phone_ip != new_ip {
                    s.phone_ip = new_ip;
                    s.push_log(format!("phone hello from {src}"));
                } else if !s.phone_connected {
                    s.push_log(format!("phone hello from {src}"));
                }
                if !version.is_empty() && s.phone_version != version {
                    s.phone_version = version.clone();
                    s.push_log(format!("phone version {version}"));
                }
                s.phone_connected = true;
            }
            TelemetryPacket::Ping => {
                s.packets_total += 1;
                s.phone_connected = true;
            }
            TelemetryPacket::Gyro(sample) => {
                let ip = src.ip().to_string();
                if s.phone_ip != ip {
                    s.phone_ip = ip;
                    s.push_log(format!("phone IP updated to {src} (gyro)"));
                }
                s.note_gyro(&sample);
                s.phone_connected = true;
            }
            TelemetryPacket::Hand(frame) => {
                if s.phone_connected && s.phone_ip == src.ip().to_string() {
                    s.note_hand(frame.hands);
                } else {
                    debug_msg = Some(format!(
                        "[phone] ignored hand frame from {src} (not the connected phone)"
                    ));
                }
            }
            TelemetryPacket::Rotation(sample) => {
                let ip = src.ip().to_string();
                if s.phone_ip != ip {
                    s.phone_ip = ip;
                    s.push_log(format!("phone IP updated to {src} (rotation)"));
                }
                s.phone_connected = true;
                debug_msg = Some(format!(
                    "[phone] rotation quat=({:.4},{:.4},{:.4},{:.4}) ts={}",
                    sample.quat[0], sample.quat[1], sample.quat[2], sample.quat[3],
                    sample.timestamp_ms
                ));
            }
            TelemetryPacket::NetStats(stats) => {
                if s.phone_connected && s.phone_ip == src.ip().to_string() {
                    s.note_net_stats(&stats);
                } else {
                    debug_msg = Some(format!(
                        "[phone] ignored net-stats from {src} (not the connected phone)"
                    ));
                }
            }
            TelemetryPacket::Unknown => {}
        }
        s.recompute_fps();
    }
    if let Some(m) = debug_msg {
        crate::debug_log!(state, "{m}");
    }
}
/// Clears the phone-connected pill when nothing decodable arrived within
/// PHONE_TIMEOUT. Logs the transition once; Unknown datagrams never count as
/// activity so noise cannot hold the connection open.
fn mark_phone_gone_if_stale(state: &SharedState, last_seen: Option<Instant>) {
    let stale = last_seen
        .map(|t| t.elapsed() > PHONE_TIMEOUT)
        .unwrap_or(true);
    if stale {
        if let Ok(mut s) = state.lock() {
            if s.phone_connected {
                s.push_log("phone timed out".into());
            }
            s.phone_connected = false;
        }
    }
}
/// Forwards one raw gyro/rotation datagram to the driver (UDP 42074) without
/// parsing it. Best-effort: a dropped forward is superseded by the next phone
/// sample within milliseconds, so failures only log to stderr.
fn relay_raw_to_driver(raw: &[u8]) {
    if let Err(e) = sensor_sock().send_to(raw, format!("127.0.0.1:{SENSOR_PORT}")) {
        eprintln!("[sensor-fwd] send_to 42074 failed: {e}");
    }
}
#[cfg(test)]
mod tests {
    // State-transition tests over apply_packet with a fake phone IP: hello
    // adopts IP/version, gyro feeds the FPS meter, hand/net-stats from
    // strangers are ignored, stalls never auto-push bitrate, and the stale
    // check clears the pill exactly once. Test names read as the spec.
    use super::*;
    use crate::app::AppState;
    use crate::net::telemetry::{GyroSample, HandFrame, TelemetryPacket};
    use std::sync::{Arc, Mutex};
    fn fresh_state() -> SharedState {
        Arc::new(Mutex::new(AppState::default()))
    }
    fn fake_src() -> SocketAddr {
        "192.168.1.100:12345".parse().unwrap()
    }
    #[test]
    fn hello_sets_phone_connected() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
    }
    #[test]
    fn hello_records_phone_ip() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let s = state.lock().unwrap();
        assert_eq!(s.phone_ip, "192.168.1.100");
    }
    #[test]
    fn hello_logs_on_first_connect() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let s = state.lock().unwrap();
        assert!(s.log.iter().any(|l| l.contains("phone hello")));
    }
    #[test]
    fn hello_records_phone_version_and_logs_once() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello("542".into()), fake_src());
        assert_eq!(state.lock().unwrap().phone_version, "542");
        assert!(state.lock().unwrap().log.iter().any(|l| l.contains("phone version 542")));
        let count_first = state.lock().unwrap().log.len();
        apply_packet(&state, TelemetryPacket::Hello("542".into()), fake_src());
        assert_eq!(state.lock().unwrap().log.len(), count_first);
        apply_packet(&state, TelemetryPacket::Hello("543".into()), fake_src());
        assert_eq!(state.lock().unwrap().phone_version, "543");
    }
    #[test]
    fn hello_does_not_log_on_repeat() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let count_first = state.lock().unwrap().log.len();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let count_second = state.lock().unwrap().log.len();
        assert_eq!(count_first, count_second);
    }
    #[test]
    fn gyro_sets_phone_connected() {
        let state = fresh_state();
        let sample = GyroSample::default();
        apply_packet(&state, TelemetryPacket::Gyro(sample), fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
    }
    #[test]
    fn gyro_increments_packets_total() {
        let state = fresh_state();
        let sample = GyroSample::default();
        apply_packet(&state, TelemetryPacket::Gyro(sample), fake_src());
        apply_packet(&state, TelemetryPacket::Gyro(sample), fake_src());
        let s = state.lock().unwrap();
        assert_eq!(s.packets_total, 2);
    }
    #[test]
    fn hand_sets_phone_connected_and_records_hands() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let frame = HandFrame {
            timestamp_ms: 100,
            hands: 2,
            landmarks_per_hand: 21,
            confidence: 0.9,
        };
        apply_packet(&state, TelemetryPacket::Hand(frame), fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
        assert_eq!(s.hands_detected, 2);
    }
    #[test]
    fn hand_from_unknown_phone_is_ignored() {
        let state = fresh_state();
        let frame = HandFrame {
            timestamp_ms: 100,
            hands: 2,
            landmarks_per_hand: 21,
            confidence: 0.9,
        };
        apply_packet(&state, TelemetryPacket::Hand(frame), fake_src());
        let s = state.lock().unwrap();
        assert!(!s.phone_connected);
        assert_eq!(s.hands_detected, 0);
    }
    #[test]
    fn hand_from_roamed_ip_is_ignored_until_hello() {
        use std::net::SocketAddr;
        let state = fresh_state();
        let home: SocketAddr = fake_src();
        let rogue: SocketAddr = "192.168.1.200:9999".parse().unwrap();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), home);
        let frame = HandFrame {
            timestamp_ms: 100,
            hands: 2,
            landmarks_per_hand: 21,
            confidence: 0.9,
        };
        apply_packet(&state, TelemetryPacket::Hand(frame), rogue);
        let s = state.lock().unwrap();
        assert_eq!(s.phone_ip, "192.168.1.100");
        assert_eq!(s.hands_detected, 0);
    }
    #[test]
    fn ping_never_moves_phone_ip() {
        use std::net::SocketAddr;
        let state = fresh_state();
        let home: SocketAddr = fake_src();
        let rogue: SocketAddr = "192.168.1.200:9999".parse().unwrap();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), home);
        apply_packet(&state, TelemetryPacket::Ping, rogue);
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
        assert_eq!(s.phone_ip, "192.168.1.100");
    }
    #[test]
    fn ping_sets_phone_connected() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Ping, fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
        assert_eq!(s.packets_total, 1);
    }
    #[test]
    fn unknown_packet_does_not_set_phone_connected() {        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Unknown, fake_src());
        let s = state.lock().unwrap();
        assert!(!s.phone_connected);
    }
    #[test]
    fn net_stats_sets_phone_connected_and_records_counters() {
        use crate::net::telemetry::NetStats;
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = true;
        }
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        let stats = NetStats {
            timestamp_ms: 1,
            frames_decoded: 120,
            stalls: 0,
            decoded_fps: 60.0,
        };
        apply_packet(&state, TelemetryPacket::NetStats(stats), fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
        assert_eq!(s.net_frames_decoded, 120);
        assert_eq!(s.net_decoded_fps, 60.0);
    }
    #[test]
    fn net_stats_from_unknown_phone_is_ignored() {
        use crate::net::telemetry::NetStats;
        let state = fresh_state();
        let stats = NetStats {
            timestamp_ms: 1,
            frames_decoded: 120,
            stalls: 0,
            decoded_fps: 60.0,
        };
        apply_packet(&state, TelemetryPacket::NetStats(stats), fake_src());
        let s = state.lock().unwrap();
        assert!(!s.phone_connected);
        assert_eq!(s.net_frames_decoded, 0);
    }
    #[test]
    fn net_stats_stall_never_pushes_by_itself() {
        use crate::net::telemetry::NetStats;
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = true;
            assert_eq!(s.applied_bitrate_mbps, 20);
        }
        let stats = NetStats {
            timestamp_ms: 1,
            frames_decoded: 10,
            stalls: 2,
            decoded_fps: 5.0,
        };
        apply_packet(&state, TelemetryPacket::Hello("1".into()), fake_src());
        apply_packet(&state, TelemetryPacket::NetStats(stats), fake_src());
        let s = state.lock().unwrap();
        assert_eq!(s.net_stalls, 2);
        assert_eq!(s.applied_bitrate_mbps, 20);
    }
    #[test]
    fn stale_phone_clears_connected_flag() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.phone_connected = true;
        }
        let stale_time = Some(Instant::now() - Duration::from_secs(10));
        mark_phone_gone_if_stale(&state, stale_time);
        let s = state.lock().unwrap();
        assert!(!s.phone_connected);
    }
    #[test]
    fn fresh_phone_keeps_connected_flag() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.phone_connected = true;
        }
        let fresh_time = Some(Instant::now());
        mark_phone_gone_if_stale(&state, fresh_time);
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
    }
    #[test]
    fn never_received_hello_marks_phone_gone() {
        let state = fresh_state();
        mark_phone_gone_if_stale(&state, None);
        let s = state.lock().unwrap();
        assert!(!s.phone_connected);
    }
    #[test]
    fn stale_phone_timeout_logs_once() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.phone_connected = true;
        }
        let stale_time = Some(Instant::now() - Duration::from_secs(10));
        mark_phone_gone_if_stale(&state, stale_time);
        let log_count_first = state.lock().unwrap().log.len();
        mark_phone_gone_if_stale(&state, stale_time);
        let log_count_second = state.lock().unwrap().log.len();
        assert_eq!(log_count_first, log_count_second);
    }
    #[test]
    fn phone_lifecycle_connect_timeout_reconnect() {
        let state = fresh_state();
        let src = fake_src();
        apply_packet(&state, TelemetryPacket::Hello("1".into()), src);
        assert!(state.lock().unwrap().phone_connected);
        let stale = Some(Instant::now() - Duration::from_secs(5));
        mark_phone_gone_if_stale(&state, stale);
        assert!(!state.lock().unwrap().phone_connected);
        apply_packet(&state, TelemetryPacket::Hello("1".into()), src);
        assert!(state.lock().unwrap().phone_connected);
    }
}