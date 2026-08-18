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