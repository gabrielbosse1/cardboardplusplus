//! Control-plane link to the SteamVR driver.
//!
//! Two jobs share one ephemeral-socket singleton:
//!  * `send_config` pushes the stream settings (`CARDBOARD_CAP` + `BRIDGE_CFG`)
//!    whenever the user applies them.
//!  * a heartbeat loop sends `BRIDGE_HELLO v1` on the discovery port every
//!    500 ms and treats an incoming `BRIDGE_ACK` as proof the driver is alive.
//!
//! The heartbeat is what drives the `driver_connected`/`encoder_active` flags
//! by allowing write access to the shared state.

use std::net::{SocketAddr, UdpSocket};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::app::SharedState;
use crate::net::{EncoderChoice, DRIVER_DISCOVERY_PORT};

/// How often the discovery heartbeat is re-sent.
const HEARTBEAT_INTERVAL: Duration = Duration::from_millis(500);
/// How long each recv blocks while polling for a BRIDGE_ACK reply.
const ACK_POLL_TIMEOUT: Duration = Duration::from_millis(200);
/// Missing an ACK for this long marks the driver (and encoder) as lost.
const DRIVER_ACK_TIMEOUT: Duration = Duration::from_secs(5);

/// The bound discovery socket plus the driver address it talks to.
struct DriverConn {
    sock: UdpSocket,
    addr: SocketAddr,
}

/// Bridge -> driver wire commands for the local preview toggle.
const PREVIEW_WIRE_ON: &[u8] = b"BRIDGE_PREVIEW 1";
const PREVIEW_WIRE_OFF: &[u8] = b"BRIDGE_PREVIEW 0";

/// Fixed prefix of the driver's periodic stats packet.
const STATS_PREFIX: &str = "BRIDGE_STATS";

/// Process-global, set once by `spawn`; `send_config` reads it on demand.
static DRIVER_CONN: OnceLock<Mutex<DriverConn>> = OnceLock::new();

/// Register the discovery socket and start the heartbeat loop. Called early by
/// `AppCore::new`; if binding fails the failure is logged and everything else
/// stays consistent (the bridge just reports a disconnected driver).
pub fn spawn(state: SharedState) {
    let sock = match UdpSocket::bind("0.0.0.0:0") {
        Ok(sock) => sock,
        Err(err) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("driver socket failed: {err}"));
            }
            return;
        }
    };
    // Non-blocking-ish polls so the heartbeat cadence stays independent of the
    // driver's reply latency.
    let _ = sock.set_read_timeout(Some(ACK_POLL_TIMEOUT));

    let addr: SocketAddr = format!("127.0.0.1:{DRIVER_DISCOVERY_PORT}")
        .parse()
        .expect("driver discovery address is a literal");

    let _ = DRIVER_CONN.set(Mutex::new(DriverConn { sock, addr }));
    let conn = DRIVER_CONN.get().expect("driver conn set just above");

    std::thread::spawn(move || heartbeat_loop(&state, conn));
}

/// Periodic handshake: say hello, watch for the ACK, and declare the driver
/// gone once it has been silent for `DRIVER_ACK_TIMEOUT`.
fn heartbeat_loop(state: &SharedState, conn: &Mutex<DriverConn>) {
    let mut last_ack = None::<Instant>;
    let mut buf = [0u8; 512];

    loop {
        send_heartbeat(conn);
        poll_for_ack(conn, &mut buf, &mut last_ack, state);
        mark_driver_gone_if_stale(state, last_ack);
        std::thread::sleep(HEARTBEAT_INTERVAL);
    }
}

/// Re-send the discovery greeting. The driver answers with BRIDGE_ACK when it
/// speaks the protocol; silence keeps the loop pinging.
fn send_heartbeat(conn: &Mutex<DriverConn>) {
    let conn = conn.lock().expect("driver conn lock");
    let _ = conn.sock.send_to(b"BRIDGE_HELLO v1", conn.addr);
}

/// One heartbeat pass: re-send the greeting, then drain every datagram the
/// driver has queued for us (in practice an ACK may be followed by a STATS
/// packet in the same pass).
fn poll_for_ack(
    conn: &Mutex<DriverConn>,
    buf: &mut [u8; 512],
    last_ack: &mut Option<Instant>,
    state: &SharedState,
) {
    loop {
        let Ok((n, _src)) = conn.lock().expect("driver conn lock").sock.recv_from(buf) else {
            return; // poll timed out — no more packets from the driver
        };
        match handle_bridge_datagram(&buf[..n], last_ack, state) {
            DatagramHandled::Continue => continue, // drain the next one if any
            DatagramHandled::Stop => return,
        }
    }
}

/// One inbound datagram from the discovery socket: BRIDGE_ACK proves the driver
/// is alive; BRIDGE_STATS updates the live preview numbers. Anything else is
/// ignored and we keep draining so a stray packet can't stall the heartbeat.
enum DatagramHandled {
    Continue,
    Stop,
}

fn handle_bridge_datagram(data: &[u8], last_ack: &mut Option<Instant>, state: &SharedState) -> DatagramHandled {
    let msg = String::from_utf8_lossy(data);
    if msg.trim_start().starts_with("BRIDGE_ACK") {
        *last_ack = Some(Instant::now());
        if let Ok(mut s) = state.lock() {
            if !s.driver_connected {
                s.push_log("driver handshake established".into());
            }
            s.driver_connected = true;
            // A live driver implies an active encoder until told otherwise.
            s.encoder_active = true;
        }
        return DatagramHandled::Continue;
    }
    if msg.starts_with(STATS_PREFIX) {
        if let Some((fps, kbps, frames, drops)) = parse_stats(&msg) {
            if let Ok(mut s) = state.lock() {
                s.note_preview_stats(fps, kbps, frames, drops);
            }
        }
        return DatagramHandled::Stop; // stats is the last packet of a pass
    }
    DatagramHandled::Continue
}

/// Parse `BRIDGE_STATS fps=<n> bitrate=<kbps> frames=<n> drops=<n>` into
/// (fps, bitrate_kbps, frames, drops). Missing or malformed fields default to 0
/// so a partial packet never poisons the counters the UI shows. Control bytes
/// (e.g. a stray NUL from an older driver build) are stripped first so a field
/// like "frames=\0 10" still parses.
fn parse_stats(msg: &str) -> Option<(i32, i32, u64, u64)> {
    let rest = msg.strip_prefix(STATS_PREFIX)?;
    let clean: String = rest.chars().filter(|c| !c.is_control()).collect();
    let mut fps = 0i32;
    let mut kbps = 0i32;
    let mut frames = 0u64;
    let mut drops = 0u64;
    for field in clean.split_whitespace() {
        let (key, value) = field.split_once('=')?;
        match key {
            "fps" => fps = value.parse().unwrap_or(0),
            "bitrate" => kbps = value.parse().unwrap_or(0),
            "frames" => frames = value.parse().unwrap_or(0),
            "drops" => drops = value.parse().unwrap_or(0),
            _ => {}
        }
    }
    Some((fps, kbps, frames, drops))
}

/// Tell the driver whether to keep sending the localhost preview stream, then
/// record the choice so the UI/REST reflect what was actually asked.
pub fn set_preview(state: &SharedState, enabled: bool) {
    let Some(conn) = DRIVER_CONN.get() else {
        if let Ok(mut s) = state.lock() {
            s.preview_enabled = enabled;
            s.push_log(format!("preview set to {} (driver link down; retried on next connect)", if enabled { "on" } else { "off" }));
        }
        return;
    };
    let wire = if enabled { PREVIEW_WIRE_ON } else { PREVIEW_WIRE_OFF };
    let conn = conn.lock().expect("driver conn lock");
    let _ = conn.sock.send_to(wire, conn.addr);
    if let Ok(mut s) = state.lock() {
        s.preview_enabled = enabled;
        s.push_log(format!("local preview {} (BRIDGE_PREVIEW sent to driver)", if enabled { "enabled" } else { "disabled" }));
    }
}

/// Drop the connected flags once the ACK has been missing too long — this is
/// what backs the UI/REST "driver" indicator going red. The single log line is
/// emitted on the transition only (the flag read guards it).
fn mark_driver_gone_if_stale(state: &SharedState, last_ack: Option<Instant>) {
    let stale = last_ack
        .map(|t| t.elapsed() > DRIVER_ACK_TIMEOUT)
        .unwrap_or(true);
    if stale {
        if let Ok(mut s) = state.lock() {
            if s.driver_connected {
                s.driver_connected = false;
                s.encoder_active = false;
                s.push_log("driver heartbeat lost".into());
            }
        }
    }
}

/// Push stream settings to the driver as two datagrams:
///   `CARDBOARD_CAP {width} {height}`                 — what the phone can decode
///   `BRIDGE_CFG {fps} {bitrate_kbps} {encoder}`      — how to encode it
///
/// Bitrate is converted to kilobits per second here (the REST API speaks in
/// megabits). No-op until `spawn` has registered the socket.
pub fn send_config(width: i32, height: i32, fps: i32, bitrate_mbps: i32, encoder: EncoderChoice) {
    let Some(conn) = DRIVER_CONN.get() else {
        return;
    };
    let conn = conn.lock().expect("driver conn lock");
    let (cap, cfg) = config_wire_packets(width, height, fps, bitrate_mbps, encoder);
    let _ = conn.sock.send_to(&cap, conn.addr);
    let _ = conn.sock.send_to(&cfg, conn.addr);
}

/// Serialize the two configuration datagrams. Extracted from `send_config` so
/// the wire bytes are unit-testable.
fn config_wire_packets(
    width: i32,
    height: i32,
    fps: i32,
    bitrate_mbps: i32,
    encoder: EncoderChoice,
) -> (Vec<u8>, Vec<u8>) {
    let cap = format!("CARDBOARD_CAP {} {}", width, height).into_bytes();
    let cfg = format!(
        "BRIDGE_CFG {} {} {}",
        fps,
        bitrate_mbps * 1000,
        encoder.as_str()
    )
    .into_bytes();
    (cap, cfg)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_wire_bytes_match_the_driver_protocol() {
        let (cap, cfg) = config_wire_packets(1600, 900, 60, 8, EncoderChoice::Nvenc);
        assert_eq!(cap, b"CARDBOARD_CAP 1600 900");
        assert_eq!(cfg, b"BRIDGE_CFG 60 8000 h264_nvenc");
    }

    #[test]
    fn bitrate_is_scaled_to_kilobits_on_the_wire() {
        let (_, cfg) = config_wire_packets(2880, 1620, 30, 20, EncoderChoice::Auto);
        assert!(String::from_utf8_lossy(&cfg).ends_with("20000 auto"));
    }

    #[test]
    fn capacity_messages_keep_field_padding_stable() {
        let (cap, _) = config_wire_packets(1, 2, 3, 4, EncoderChoice::Amf);
        assert_eq!(cap, b"CARDBOARD_CAP 1 2");
    }

    #[test]
    fn preview_wire_bytes_match_the_driver_protocol() {
        assert_eq!(PREVIEW_WIRE_ON, b"BRIDGE_PREVIEW 1");
        assert_eq!(PREVIEW_WIRE_OFF, b"BRIDGE_PREVIEW 0");
    }

    #[test]
    fn stats_parser_fills_every_field() {
        assert_eq!(
            parse_stats("BRIDGE_STATS fps=60 bitrate=20000 frames=1234 drops=2"),
            Some((60, 20000, 1234, 2))
        );
    }

    #[test]
    fn stats_parser_tolerates_partial_and_extra_fields() {
        // Missing fields default to 0; unknown keys are skipped.
        assert_eq!(parse_stats("BRIDGE_STATS fps=30"), Some((30, 0, 0, 0)));
        assert_eq!(
            parse_stats("BRIDGE_STATS fps=1 bitrate=2 frames=3 drops=4 future=none"),
            Some((1, 2, 3, 4))
        );
    }

    #[test]
    fn stats_parser_rejects_wrong_prefix() {
        assert_eq!(parse_stats("BRIDGE_ACK v1"), None);
        assert_eq!(parse_stats("hello"), None);
    }

    // --- handle_bridge_datagram state lifecycle tests ---

    use crate::app::AppState;
    use std::sync::{Arc, Mutex};

    fn fresh_state() -> SharedState {
        Arc::new(Mutex::new(AppState::default()))
    }

    #[test]
    fn bridge_ack_sets_driver_connected_and_encoder_active() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"BRIDGE_ACK v1", &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert!(last_ack.is_some());
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
        assert!(s.encoder_active);
    }

    #[test]
    fn bridge_ack_with_leading_whitespace_still_connects() {
        let state = fresh_state();
        let mut last_ack = None;
        handle_bridge_datagram(b"  BRIDGE_ACK v1", &mut last_ack, &state);
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
    }

    #[test]
    fn bridge_ack_transitions_from_disconnected() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = false;
            s.encoder_active = false;
        }
        let mut last_ack = None;
        handle_bridge_datagram(b"BRIDGE_ACK v1", &mut last_ack, &state);
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
        assert!(s.encoder_active);
    }

    #[test]
    fn bridge_stats_updates_preview_metrics() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(
            b"BRIDGE_STATS fps=60 bitrate=20000 frames=1234 drops=2",
            &mut last_ack,
            &state,
        );
        assert!(matches!(result, DatagramHandled::Stop));
        let s = state.lock().unwrap();
        assert_eq!(s.preview_driver_fps, 60);
        assert_eq!(s.preview_bitrate_kbps, 20000);
        assert_eq!(s.preview_frames, 1234);
        assert_eq!(s.preview_drops, 2);
    }

    #[test]
    fn bridge_stats_does_not_touch_connection_flags() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = false;
            s.encoder_active = false;
        }
        let mut last_ack = None;
        handle_bridge_datagram(
            b"BRIDGE_STATS fps=30 bitrate=8000 frames=100 drops=0",
            &mut last_ack,
            &state,
        );
        let s = state.lock().unwrap();
        assert!(!s.driver_connected);
        assert!(!s.encoder_active);
    }

    #[test]
    fn unknown_datagram_does_not_change_state() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"random garbage", &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert!(last_ack.is_none());
        let s = state.lock().unwrap();
        assert!(!s.driver_connected);
    }

    #[test]
    fn empty_datagram_does_not_change_state() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"", &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert!(last_ack.is_none());
    }

    #[test]
    fn multiple_acks_in_a_row_keep_driver_connected() {
        let state = fresh_state();
        let mut last_ack = None;
        for _ in 0..5 {
            handle_bridge_datagram(b"BRIDGE_ACK v1", &mut last_ack, &state);
        }
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
        assert!(s.encoder_active);
    }

    #[test]
    fn ack_then_stats_drain_returns_stop() {
        let state = fresh_state();
        let mut last_ack = None;
        let r1 = handle_bridge_datagram(b"BRIDGE_ACK v1", &mut last_ack, &state);
        assert!(matches!(r1, DatagramHandled::Continue));
        let r2 = handle_bridge_datagram(b"BRIDGE_STATS fps=0 bitrate=0 frames=0 drops=0", &mut last_ack, &state);
        assert!(matches!(r2, DatagramHandled::Stop));
    }

    // --- mark_driver_gone_if_stale tests ---

    #[test]
    fn stale_driver_clears_connected_flag() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = true;
            s.encoder_active = true;
        }
        let stale_time = Some(std::time::Instant::now() - std::time::Duration::from_secs(10));
        mark_driver_gone_if_stale(&state, stale_time);
        let s = state.lock().unwrap();
        assert!(!s.driver_connected);
        assert!(!s.encoder_active);
    }

    #[test]
    fn fresh_driver_keeps_connected_flag() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = true;
            s.encoder_active = true;
        }
        let fresh_time = Some(std::time::Instant::now());
        mark_driver_gone_if_stale(&state, fresh_time);
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
        assert!(s.encoder_active);
    }

    #[test]
    fn never_received_ack_marks_driver_gone() {
        let state = fresh_state();
        mark_driver_gone_if_stale(&state, None);
        let s = state.lock().unwrap();
        assert!(!s.driver_connected);
    }

    #[test]
    fn stale_driver_clears_only_once_logs_single_line() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_connected = true;
            s.encoder_active = true;
        }
        let stale_time = Some(std::time::Instant::now() - std::time::Duration::from_secs(10));
        mark_driver_gone_if_stale(&state, stale_time);
        let log_count_first = state.lock().unwrap().log.len();
        // Second call should NOT add another log line (flag already false).
        mark_driver_gone_if_stale(&state, stale_time);
        let log_count_second = state.lock().unwrap().log.len();
        assert_eq!(log_count_first, log_count_second);
    }
}