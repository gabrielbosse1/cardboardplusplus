//! Telemetry uplink from the phone.
//!
//! Listens on UDP 42071 and folds each parsed packet (hello / gyro / hand /
//! ping) into the shared state. The phone is considered connected until it has
//! been silent for `PHONE_TIMEOUT`, at which point the flag is cleared in the
//! same way the driver link decays.

use std::net::{SocketAddr, UdpSocket};
use std::time::{Duration, Instant};

use crate::app::SharedState;
use crate::net::telemetry::{self, TelemetryPacket};
use crate::net::TELEMETRY_PORT;

/// Poll cadence while waiting for the next packet / liveness re-check.
const POLL_INTERVAL: Duration = Duration::from_millis(50);
/// A phone silent for this long is considered timed out.
const PHONE_TIMEOUT: Duration = Duration::from_secs(4);

/// Bind the telemetry socket and start the receive loop. Called by
/// `AppCore::new`; a bind failure (e.g. port already taken) is logged and the
/// rest of the bridge keeps running, just without a phone link.
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
    // Block until a datagram lands; liveness is handled by the timeout below
    // rather than by a read timeout, so no packet is ever dropped by one.
    let _ = sock.set_read_timeout(Option::<Duration>::None);

    if let Ok(mut s) = state.lock() {
        s.push_log(format!("telemetry listener on udp {TELEMETRY_PORT}"));
    }

    std::thread::spawn(move || telemetry_loop(sock, state));
}

/// Receive packets forever: each one updates the connection timestamp and the
/// shared metrics; every pass re-checks the phone timeout.
fn telemetry_loop(sock: UdpSocket, state: SharedState) {
    let mut buf = [0u8; 65535];
    let mut last_seen = None::<Instant>;

    loop {
        if let Ok((n, src)) = sock.recv_from(&mut buf) {
            last_seen = Some(Instant::now());
            let packet = telemetry::parse_packet(&buf[..n]);
            apply_packet(&state, packet, src);
        }

        mark_phone_gone_if_stale(&state, last_seen);
        std::thread::sleep(POLL_INTERVAL);
    }
}

/// Fold one parsed telemetry packet into shared state. Every packet — even a
/// bare ping — refreshes the "phone is alive" flag; only the hello announces
/// the phone's address in the log (once per connection).
fn apply_packet(state: &SharedState, packet: TelemetryPacket, src: SocketAddr) {
    if let Ok(mut s) = state.lock() {
        match packet {
            TelemetryPacket::Hello => {
                if !s.phone_connected {
                    s.phone_ip = src.ip().to_string();
                    s.push_log(format!("phone hello from {src}"));
                }
                s.phone_connected = true;
            }
            TelemetryPacket::Ping => {
                s.packets_total += 1;
                s.phone_connected = true;
            }
            TelemetryPacket::Gyro(_sample) => {
                s.note_gyro();
                s.phone_connected = true;
            }
            TelemetryPacket::Hand(frame) => {
                s.note_hand(frame.hands);
                s.phone_connected = true;
            }
            TelemetryPacket::Unknown => {}
        }
        s.recompute_fps();
    }
}

/// Clear the phone-connected flag (with a single transition log line) once the
/// phone has been silent for `PHONE_TIMEOUT`.
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

/// Best-effort downlink to the phone (not currently used by the session, kept
/// for future control messages).
#[allow(dead_code)]
pub fn send_to_phone(addr: SocketAddr, data: &[u8]) {
    if let Ok(sock) = UdpSocket::bind("0.0.0.0:0") {
        let _ = sock.send_to(data, addr);
    }
}

#[cfg(test)]
mod tests {
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

    // --- apply_packet: Hello ---

    #[test]
    fn hello_sets_phone_connected() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello, fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
    }

    #[test]
    fn hello_records_phone_ip() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello, fake_src());
        let s = state.lock().unwrap();
        assert_eq!(s.phone_ip, "192.168.1.100");
    }

    #[test]
    fn hello_logs_on_first_connect() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello, fake_src());
        let s = state.lock().unwrap();
        assert!(s.log.iter().any(|l| l.contains("phone hello")));
    }

    #[test]
    fn hello_does_not_log_on_repeat() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Hello, fake_src());
        let count_first = state.lock().unwrap().log.len();
        apply_packet(&state, TelemetryPacket::Hello, fake_src());
        let count_second = state.lock().unwrap().log.len();
        assert_eq!(count_first, count_second);
    }

    // --- apply_packet: Gyro ---

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

    // --- apply_packet: Hand ---

    #[test]
    fn hand_sets_phone_connected_and_records_hands() {
        let state = fresh_state();
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

    // --- apply_packet: Ping ---

    #[test]
    fn ping_sets_phone_connected() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Ping, fake_src());
        let s = state.lock().unwrap();
        assert!(s.phone_connected);
        assert_eq!(s.packets_total, 1);
    }

    // --- apply_packet: Unknown ---

    #[test]
    fn unknown_packet_does_not_set_phone_connected() {
        let state = fresh_state();
        apply_packet(&state, TelemetryPacket::Unknown, fake_src());
        let s = state.lock().unwrap();
        assert!(!s.phone_connected);
    }

    // --- mark_phone_gone_if_stale ---

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

    // --- Full lifecycle: hello -> timeout -> hello ---

    #[test]
    fn phone_lifecycle_connect_timeout_reconnect() {
        let state = fresh_state();
        let src = fake_src();

        // Connect
        apply_packet(&state, TelemetryPacket::Hello, src);
        assert!(state.lock().unwrap().phone_connected);

        // Timeout
        let stale = Some(Instant::now() - Duration::from_secs(5));
        mark_phone_gone_if_stale(&state, stale);
        assert!(!state.lock().unwrap().phone_connected);

        // Reconnect
        apply_packet(&state, TelemetryPacket::Hello, src);
        assert!(state.lock().unwrap().phone_connected);
    }
}