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

/// One ACK poll. On the first ACK this announces the handshake in the log and
/// flips the driver/encoder flags to connected; later ACKs merely refresh the
/// timestamp keeping the link marked alive.
fn poll_for_ack(
    conn: &Mutex<DriverConn>,
    buf: &mut [u8; 512],
    last_ack: &mut Option<Instant>,
    state: &SharedState,
) {
    let Ok((n, _src)) = conn.lock().expect("driver conn lock").sock.recv_from(buf) else {
        return; // poll timed out — no driver on the wire yet
    };
    let msg = String::from_utf8_lossy(&buf[..n]);
    if !msg.trim_start().starts_with("BRIDGE_ACK") {
        return;
    }
    *last_ack = Some(Instant::now());
    if let Ok(mut s) = state.lock() {
        if !s.driver_connected {
            s.push_log("driver handshake established".into());
        }
        s.driver_connected = true;
        // A live driver implies an active encoder until told otherwise.
        s.encoder_active = true;
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
}