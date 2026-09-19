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
use std::sync::{Arc, OnceLock};
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
/// The socket is shared lock-free (`UdpSocket: Send + Sync`): the heartbeat
/// thread recvs while `send_config` sends, and neither ever blocks the other.
struct DriverConn {
    sock: Arc<UdpSocket>,
    addr: SocketAddr,
}

/// Fixed prefix of the driver's periodic stats packet.
const STATS_PREFIX: &str = "BRIDGE_STATS";

/// Malformed STATS datagrams rejected so far (surfaced via debug log).
static STATS_REJECTS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// Process-global, set once by `spawn`; `send_config` reads it on demand.
static DRIVER_CONN: OnceLock<DriverConn> = OnceLock::new();

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

    let _ = DRIVER_CONN.set(DriverConn {
        sock: Arc::new(sock),
        addr,
    });
    let conn = DRIVER_CONN.get().expect("driver conn set just above");

    std::thread::spawn(move || heartbeat_loop(&state, conn));
}

/// Periodic handshake: say hello, watch for the ACK, and declare the driver
/// gone once it has been silent for `DRIVER_ACK_TIMEOUT`.
fn heartbeat_loop(state: &SharedState, conn: &DriverConn) {
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
fn send_heartbeat(conn: &DriverConn) {
    let _ = conn.sock.send_to(b"BRIDGE_HELLO v1", conn.addr);
}

/// One heartbeat pass: re-send the greeting, then drain every datagram the
/// driver has queued for us (in practice an ACK may be followed by a STATS
/// packet in the same pass).
fn poll_for_ack(
    conn: &DriverConn,
    buf: &mut [u8; 512],
    last_ack: &mut Option<Instant>,
    state: &SharedState,
) {
    loop {
        let Ok((n, src)) = conn.sock.recv_from(buf) else {
            return; // poll timed out — no more packets from the driver
        };
        match handle_bridge_datagram(&buf[..n], src, last_ack, state) {
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
    // Kept for the drain match: STATS used to end a pass before it learned
    // to keep draining (an ACK queued behind STATS was delayed a heartbeat).
    #[allow(dead_code)]
    Stop,
}

fn handle_bridge_datagram(
    data: &[u8],
    src: SocketAddr,
    last_ack: &mut Option<Instant>,
    state: &SharedState,
) -> DatagramHandled {
    let msg = String::from_utf8_lossy(data);
    crate::debug_log!(state, "[driver] recv: {}", msg.trim());
    // Liveness only from the real driver socket: any local process can spray
    // `BRIDGE_STATS` at us, and that must never keep `driver_connected` true
    // after the real driver died.
    let trusted =
        src.ip() == std::net::IpAddr::from([127, 0, 0, 1]) && src.port() == DRIVER_DISCOVERY_PORT;
    if msg.trim_start().starts_with("BRIDGE_ACK") {
        if !trusted {
            return DatagramHandled::Continue;
        }
        *last_ack = Some(Instant::now());
        if let Ok(mut s) = state.lock() {
            if !s.driver_connected {
                s.push_log("driver handshake established".into());
            }
            s.driver_connected = true;
            // A live driver implies an active encoder until told otherwise.
            s.encoder_active = true;
            // Trailing commit-count suffix ("BRIDGE_ACK v1 <n>"); old
            // drivers send none, so an empty suffix keeps the old value.
            let version = ack_version(&msg);
            if !version.is_empty() && s.driver_version != version {
                s.driver_version = version.to_string();
                s.push_log(format!("driver version {version}"));
            }
        }
        return DatagramHandled::Continue;
    }
    if msg.trim_start().starts_with(STATS_PREFIX) {
        if !trusted {
            return DatagramHandled::Continue;
        }
        match parse_stats(&msg) {
            Some((fps, kbps, frames, drops)) => {
                if let Ok(mut s) = state.lock() {
                    s.note_preview_stats(fps, kbps, frames, drops);
                }
            }
            None => {
                let n = STATS_REJECTS.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
                crate::debug_log!(
                    state,
                    "[driver] rejected malformed STATS ({} bytes, reject #{})",
                    data.len(),
                    n
                );
            }
        }
        // STATS is routine traffic, not the end of a pass: keep draining so
        // an ACK queued behind it is still seen in the same heartbeat.
        return DatagramHandled::Continue;
    }
    DatagramHandled::Continue
}

/// Extract the driver's commit-count version from `BRIDGE_ACK v1 <n>`.
/// Returns "" when the driver sent no suffix (pre-version builds).
fn ack_version(msg: &str) -> &str {
    let rest = msg.trim_start().strip_prefix("BRIDGE_ACK").unwrap_or("").trim_start();
    // tok0 = "v1" (protocol), tok1 = commit-count version (if present).
    let mut toks = rest.split_whitespace();
    toks.next();
    toks.next().unwrap_or("")
}

/// Parse `BRIDGE_STATS fps=<n> bitrate=<kbps> frames=<n> drops=<n>` into
/// (fps, bitrate_kbps, frames, drops). Returns `None` when zero of the four
/// fields parse, so a malformed packet is distinguishable from an idle
/// driver (all zeros). Control bytes (e.g. a stray NUL from an older driver
/// build) are stripped first so a field like "frames=\0 10" still parses.
fn parse_stats(msg: &str) -> Option<(i32, i32, u64, u64)> {
    let rest = msg.trim_start().strip_prefix(STATS_PREFIX)?;
    let clean: String = rest.chars().filter(|c| !c.is_control()).collect();
    let mut fps = 0i32;
    let mut kbps = 0i32;
    let mut frames = 0u64;
    let mut drops = 0u64;
    let mut parsed = 0u32;
    for field in clean.split_whitespace() {
        let Some((key, value)) = field.split_once('=') else {
            continue;
        };
        match key {
            "fps" => {
                if let Ok(v) = value.parse() {
                    fps = v;
                    parsed += 1;
                }
            }
            "bitrate" => {
                if let Ok(v) = value.parse() {
                    kbps = v;
                    parsed += 1;
                }
            }
            "frames" => {
                if let Ok(v) = value.parse() {
                    frames = v;
                    parsed += 1;
                }
            }
            "drops" => {
                if let Ok(v) = value.parse() {
                    drops = v;
                    parsed += 1;
                }
            }
            _ => {}
        }
    }
    if parsed == 0 {
        return None;
    }
    Some((fps, kbps, frames, drops))
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
    fn stats_parser_fills_every_field() {
        assert_eq!(
            parse_stats("BRIDGE_STATS fps=60 bitrate=20000 frames=1234 drops=2"),
            Some((60, 20000, 1234, 2))
        );
    }

    #[test]
    fn stats_parser_tolerates_partial_and_extra_fields() {
        // Missing fields stay 0 as long as at least one field parses;
        // unknown keys are skipped.
        assert_eq!(parse_stats("BRIDGE_STATS fps=30"), Some((30, 0, 0, 0)));
        assert_eq!(
            parse_stats("BRIDGE_STATS fps=1 bitrate=2 frames=3 drops=4 future=none"),
            Some((1, 2, 3, 4))
        );
    }

    #[test]
    fn stats_parser_rejects_zero_valid_fields() {
        // Malformed must be distinguishable from an idle driver (all zeros).
        assert_eq!(parse_stats("BRIDGE_STATS"), None);
        assert_eq!(parse_stats("BRIDGE_STATS future=none"), None);
        assert_eq!(parse_stats("BRIDGE_STATS fps=abc"), None);
        assert_eq!(parse_stats("BRIDGE_STATS fps"), None);
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

    fn driver_src() -> SocketAddr {
        "127.0.0.1:42070".parse().unwrap()
    }

    fn spoofed_src() -> SocketAddr {
        "192.168.1.5:42070".parse().unwrap()
    }

    #[test]
    fn ack_version_extracts_commit_count_suffix() {
        assert_eq!(ack_version("BRIDGE_ACK v1 542"), "542");
        assert_eq!(ack_version("BRIDGE_ACK v1"), "");
        assert_eq!(ack_version("  BRIDGE_ACK v1 7"), "7");
        assert_eq!(ack_version("garbage"), "");
    }

    #[test]
    fn bridge_ack_with_version_suffix_stores_driver_version() {
        let state = fresh_state();
        let mut last_ack = None;
        handle_bridge_datagram(b"BRIDGE_ACK v1 542", driver_src(), &mut last_ack, &state);
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
        assert_eq!(s.driver_version, "542");
        assert!(s.log.iter().any(|l| l.contains("driver version 542")));
    }

    #[test]
    fn bridge_ack_without_suffix_keeps_old_driver_version() {
        let state = fresh_state();
        {
            let mut s = state.lock().unwrap();
            s.driver_version = "100".into();
        }
        let mut last_ack = None;
        handle_bridge_datagram(b"BRIDGE_ACK v1", driver_src(), &mut last_ack, &state);
        assert_eq!(state.lock().unwrap().driver_version, "100");
    }

    #[test]
    fn bridge_ack_sets_driver_connected_and_encoder_active() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"BRIDGE_ACK v1", driver_src(), &mut last_ack, &state);
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
        handle_bridge_datagram(b"  BRIDGE_ACK v1", driver_src(), &mut last_ack, &state);
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
    }

    #[test]
    fn spoofed_ack_does_not_touch_connection_flags() {
        // Any local process can spray BRIDGE_ACK; only 127.0.0.1:42070 counts.
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"BRIDGE_ACK v1", spoofed_src(), &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert!(last_ack.is_none());
        let s = state.lock().unwrap();
        assert!(!s.driver_connected);
        assert!(!s.encoder_active);
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
        handle_bridge_datagram(b"BRIDGE_ACK v1", driver_src(), &mut last_ack, &state);
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
            driver_src(),
            &mut last_ack,
            &state,
        );
        // STATS keeps draining: an ACK queued behind it is seen in the same pass.
        assert!(matches!(result, DatagramHandled::Continue));
        let s = state.lock().unwrap();
        assert_eq!(s.preview_driver_fps, 60);
        assert_eq!(s.preview_bitrate_kbps, 20000);
        assert_eq!(s.preview_frames, 1234);
        assert_eq!(s.preview_drops, 2);
    }

    #[test]
    fn bridge_stats_with_leading_whitespace_still_parses() {
        let state = fresh_state();
        let mut last_ack = None;
        handle_bridge_datagram(
            b"  BRIDGE_STATS fps=30 bitrate=8000 frames=100 drops=0",
            driver_src(),
            &mut last_ack,
            &state,
        );
        assert_eq!(state.lock().unwrap().preview_driver_fps, 30);
    }

    #[test]
    fn spoofed_stats_updates_nothing() {
        let state = fresh_state();
        let mut last_ack = None;
        handle_bridge_datagram(
            b"BRIDGE_STATS fps=30 bitrate=8000 frames=100 drops=0",
            spoofed_src(),
            &mut last_ack,
            &state,
        );
        assert!(last_ack.is_none());
        let s = state.lock().unwrap();
        assert_eq!(s.preview_driver_fps, 0);
        assert!(!s.driver_connected);
    }

    #[test]
    fn malformed_stats_updates_nothing() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"BRIDGE_STATS", driver_src(), &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert_eq!(state.lock().unwrap().preview_driver_fps, 0);
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
            driver_src(),
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
        let result = handle_bridge_datagram(b"random garbage", driver_src(), &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert!(last_ack.is_none());
        let s = state.lock().unwrap();
        assert!(!s.driver_connected);
    }

    #[test]
    fn empty_datagram_does_not_change_state() {
        let state = fresh_state();
        let mut last_ack = None;
        let result = handle_bridge_datagram(b"", driver_src(), &mut last_ack, &state);
        assert!(matches!(result, DatagramHandled::Continue));
        assert!(last_ack.is_none());
    }

    #[test]
    fn multiple_acks_in_a_row_keep_driver_connected() {
        let state = fresh_state();
        let mut last_ack = None;
        for _ in 0..5 {
            handle_bridge_datagram(b"BRIDGE_ACK v1", driver_src(), &mut last_ack, &state);
        }
        let s = state.lock().unwrap();
        assert!(s.driver_connected);
        assert!(s.encoder_active);
    }

    #[test]
    fn ack_then_stats_drain_keeps_draining() {
        let state = fresh_state();
        let mut last_ack = None;
        let r1 = handle_bridge_datagram(b"BRIDGE_ACK v1", driver_src(), &mut last_ack, &state);
        assert!(matches!(r1, DatagramHandled::Continue));
        let r2 = handle_bridge_datagram(
            b"BRIDGE_STATS fps=0 bitrate=0 frames=0 drops=0",
            driver_src(),
            &mut last_ack,
            &state,
        );
        assert!(matches!(r2, DatagramHandled::Continue));
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