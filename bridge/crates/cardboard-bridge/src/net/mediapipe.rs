//! TCP client for the Python MediaPipe hand-landmark server.
//!
//! The bridge spawns `mediapipe_server.py` as a child process.  The server
//! listens on TCP 127.0.0.1:42073.  For each JPEG frame the bridge sends
//! (length-prefixed), the server replies with a compact binary packet
//! containing up to 2 detected hands (21 landmarks each).

use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};
use std::time::Duration;

/// A single 3-D landmark in normalised image coordinates.
#[derive(Debug, Clone, Copy, Default)]
pub struct Landmark {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// One detected hand: 21 landmarks + handedness + confidence score.
#[derive(Debug, Clone)]
pub struct DetectedHand {
    pub landmarks: [Landmark; 21],
    pub handedness: String,
    pub score: f32,
}

/// Length prefix that selects a config frame instead of a JPEG. A JPEG can
/// never be 4 GiB, so this can't collide with a real frame.
const CONFIG_SENTINEL_U32: u32 = 0xFFFFFFFF;
/// Config payload kind: recreate the landmarker with 3× f32 LE confidences.
const CONFIG_KIND_MODEL: u8 = 0x01;

/// Shared handle to the TCP connection to the Python MediaPipe server.
#[derive(Clone)]
pub struct MediapipeClient {
    stream: Arc<Mutex<TcpStream>>,
    port: u16,
}

impl MediapipeClient {
    /// Quick single-shot connect (no retries). Used to check if a server
    /// is already running before attempting to spawn one.
    pub fn try_once(port: u16) -> anyhow::Result<Self> {
        let addr = format!("127.0.0.1:{port}");
        let (tx, rx) = std::sync::mpsc::channel();
        let addr_clone = addr.clone();
        std::thread::spawn(move || {
            let _ = tx.send(TcpStream::connect(&addr_clone));
        });
        let stream = rx.recv_timeout(Duration::from_secs(2))
            .map_err(|_| anyhow::anyhow!("connect timed out"))?
            .map_err(|e| anyhow::anyhow!(e))?;
        // Bound every detect() round-trip: a wedged Python server must not
        // stall the camera thread forever.
        let _ = stream.set_read_timeout(Some(Duration::from_secs(2)));
        let _ = stream.set_write_timeout(Some(Duration::from_secs(2)));
        eprintln!("[mediapipe] connected to {addr}");
        Ok(Self {
            stream: Arc::new(Mutex::new(stream)),
            port,
        })
    }

    /// TCP accept + protocol health probe. An accept alone proves nothing:
    /// a stale sidecar (e.g. from a crashed older bridge) accepts and never
    /// answers. The probe doubles as a no-op config push of the current
    /// thresholds, so a healthy server keeps its tuning.
    pub fn connect_healthy(port: u16, det: f32, pres: f32, track: f32) -> Option<Self> {
        let client = Self::try_once(port).ok()?;
        client.set_config(det, pres, track).then_some(client)
    }

    /// Take back `port` from a stale sidecar that failed the health probe.
    /// Only the PID currently LISTENING on `port` is a candidate, only when
    /// its image is a Python interpreter — anything else (self, system,
    /// non-Python binaries) is refused with a reason, never killed.
    pub fn reclaim_port(port: u16) -> Reclaim {
        let Some(pid) = listen_pid(port) else {
            return Reclaim::AlreadyFree;
        };
        if pid == std::process::id() || pid < 10 {
            return Reclaim::Refused(format!("PID {pid} looks like self/system"));
        }
        match process_image_name(pid) {
            Some(name) if is_python_image(&name) => {
                if kill_pid(pid) && wait_port_free(port) {
                    Reclaim::Freed(pid)
                } else {
                    Reclaim::Refused(format!("PID {pid} ({name}) would not die"))
                }
            }
            Some(name) => Reclaim::Refused(format!("PID {pid} ({name}) is not the sidecar")),
            None => Reclaim::Refused(format!("PID {pid} (unreadable image)")),
        }
    }
    /// Connect to the Python server. Retries, in case the
    /// child process is still starting up (MediaPipe/TF import takes
    /// several seconds on first run, so allow up to ~15 s).
    pub fn connect(port: u16) -> anyhow::Result<Self> {
        let addr = format!("127.0.0.1:{port}");
        let stream = Self::try_connect(&addr, 30, std::time::Duration::from_millis(500))?;
        eprintln!("[mediapipe] connected to {addr}");
        Ok(Self {
            stream: Arc::new(Mutex::new(stream)),
            port,
        })
    }

    fn try_connect(addr: &str, retries: u32, delay: Duration) -> anyhow::Result<TcpStream> {
        let connect_timeout = Duration::from_secs(2);
        for attempt in 0..retries {
            // Use a channel-based timeout: spawn connect in a thread, recv
            // with timeout.  This avoids Windows issues where
            // TcpStream::connect_timeout hangs on filtered localhost ports.
            let (tx, rx) = std::sync::mpsc::channel();
            let addr_str = addr.to_owned();
            std::thread::spawn(move || {
                let _ = tx.send(TcpStream::connect(&addr_str));
            });
            match rx.recv_timeout(connect_timeout) {
                Ok(Ok(s)) => {
                    let _ = s.set_read_timeout(Some(Duration::from_secs(2)));
                    let _ = s.set_write_timeout(Some(Duration::from_secs(2)));
                    return Ok(s);
                }
                Ok(Err(e)) if attempt + 1 < retries => {
                    eprintln!("[mediapipe] connect attempt {} failed: {e}, retrying...", attempt + 1);
                    std::thread::sleep(delay);
                }
                Ok(Err(e)) => return Err(e.into()),
                Err(_) if attempt + 1 < retries => {
                    eprintln!("[mediapipe] connect attempt {} timed out, retrying...", attempt + 1);
                    std::thread::sleep(delay);
                }
                Err(_) => return Err(anyhow::anyhow!("connect timed out")),
            }
        }
        unreachable!()
    }

    /// Send a JPEG frame and receive hand landmarks. On an IO error the
    /// connection is re-established with backoff and the request retried
    /// once, so a restarted Python server recovers without a bridge restart.
    pub fn detect(&self, jpeg: &[u8]) -> Vec<DetectedHand> {
        let mut stream = match self.stream.lock() {
            Ok(s) => s,
            Err(_) => return vec![],
        };

        if let Some(hands) = Self::detect_once(&mut stream, jpeg) {
            return hands;
        }

        // IO error: reconnect with backoff, then retry once.
        let addr = format!("127.0.0.1:{}", self.port);
        for attempt in 0..4 {
            match Self::try_connect(&addr, 1, Duration::from_millis(100)) {
                Ok(fresh) => {
                    *stream = fresh;
                    return Self::detect_once(&mut stream, jpeg).unwrap_or_default();
                }
                Err(_) => std::thread::sleep(Duration::from_millis(100 << attempt.min(4))),
            }
        }
        vec![]
    }

    /// Push new model confidences (0.0-1.0) to the running server: the
    /// sidecar recreates its landmarker in place, no restart involved.
    /// Uses its own short-lived connection so a wedged 2 s detect never
    /// head-of-line-blocks tuning. The sidecar serves one connection at a
    /// time, so the shared detect stream is parked first (it reconnects
    /// lazily on the next `detect()` via the backoff path).
    /// Returns false on any IO error (the caller logs).
    pub fn set_config(&self, det: f32, pres: f32, track: f32) -> bool {
        if let Ok(s) = self.stream.lock() {
            let _ = s.shutdown(std::net::Shutdown::Both);
        }
        let addr = format!("127.0.0.1:{}", self.port);
        let mut stream = match Self::try_connect(&addr, 2, Duration::from_millis(100)) {
            Ok(s) => s,
            Err(_) => return false,
        };
        let mut buf = [0u8; 4 + 1 + 3 * 4];
        buf[0..4].copy_from_slice(&CONFIG_SENTINEL_U32.to_le_bytes());
        buf[4] = CONFIG_KIND_MODEL;
        buf[5..9].copy_from_slice(&det.to_le_bytes());
        buf[9..13].copy_from_slice(&pres.to_le_bytes());
        buf[13..17].copy_from_slice(&track.to_le_bytes());
        if stream.write_all(&buf).is_err() {
            return false;
        }
        let _ = stream.flush();
        let mut ack = [0u8; 1];
        stream.read_exact(&mut ack).is_ok()
        // Fresh connection closes on drop here.
    }

    /// One request/response round-trip. Returns `None` on any IO error.
    fn detect_once(stream: &mut TcpStream, jpeg: &[u8]) -> Option<Vec<DetectedHand>> {
        // Send: [4 bytes LE length] [JPEG data]
        let len_bytes = (jpeg.len() as u32).to_le_bytes();
        if stream.write_all(&len_bytes).is_err() || stream.write_all(jpeg).is_err() {
            return None;
        }
        let _ = stream.flush();

        // Read: [1 byte num_hands] then per hand: [1 byte handedness] [4 bytes score] [252 bytes landmarks]
        let mut hdr = [0u8; 1];
        if stream.read_exact(&mut hdr).is_err() {
            return None;
        }
        let n = hdr[0] as usize;
        if n == 0 {
            return Some(vec![]);
        }

        let mut hands = Vec::with_capacity(n);
        for _ in 0..n {
            let mut hand_buf = [0u8; 1 + 4 + 21 * 3 * 4]; // handedness + score + landmarks
            if stream.read_exact(&mut hand_buf).is_err() {
                // Torn TCP mid-reply: partial hands are worse than none.
                return None;
            }
            let h_code = hand_buf[0];
            let score = f32::from_le_bytes([hand_buf[1], hand_buf[2], hand_buf[3], hand_buf[4]]);
            let handedness = if h_code == 0 { "Left" } else { "Right" };

            let mut landmarks = [Landmark::default(); 21];
            for i in 0..21 {
                let off = 5 + i * 12;
                landmarks[i] = Landmark {
                    x: f32::from_le_bytes(hand_buf[off..off + 4].try_into().unwrap()),
                    y: f32::from_le_bytes(hand_buf[off + 4..off + 8].try_into().unwrap()),
                    z: f32::from_le_bytes(hand_buf[off + 8..off + 12].try_into().unwrap()),
                };
            }

            hands.push(DetectedHand {
                landmarks,
                handedness: handedness.to_string(),
                score,
            });
        }

        Some(hands)
    }
}

/// Outcome of trying to take back the sidecar port from a stale occupant.
#[derive(Debug, PartialEq)]
pub enum Reclaim {
    /// Killed the stale sidecar (PID) and the port is free; spawn fresh.
    Freed(u32),
    /// Nobody is listening anymore; spawn fresh.
    AlreadyFree,
    /// Someone holds the port and we won't touch it; human-readable reason.
    Refused(String),
}

/// PID currently LISTENING on `port`, via `netstat`. The listener row is
/// identified by its `0.0.0.0:0` foreign endpoint, not the state word (which
/// is localized on some Windows installs) — see `parse_listen_pid`.
/// `pub(crate)` so the preview bind can surface who holds UDP 42069.
#[cfg(windows)]
pub(crate) fn listen_pid(port: u16) -> Option<u32> {
    let out = std::process::Command::new("netstat")
        .args(["-ano", "-p", "TCP"])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    parse_listen_pid(&String::from_utf8_lossy(&out.stdout), port)
}

#[cfg(not(windows))]
pub(crate) fn listen_pid(_port: u16) -> Option<u32> {
    None
}

/// Port part of a netstat endpoint (`127.0.0.1:42073`, `[::]:42073`).
fn endpoint_port(ep: &str) -> Option<&str> {
    ep.rsplit(':').next().map(|p| p.trim_end_matches(']'))
}

/// Parse `netstat -ano -p TCP` output: PID of the listener on `port`.
/// Pure (no process touched) so unit tests pin it with canned output.
fn parse_listen_pid(netstat: &str, port: u16) -> Option<u32> {
    let want = port.to_string();
    for line in netstat.lines() {
        // `TCP  <local>  <foreign>  <state>  <pid>` — 5 columns.
        let cols: Vec<&str> = line.split_whitespace().collect();
        if cols.len() != 5 || !cols[0].eq_ignore_ascii_case("TCP") {
            continue;
        }
        if endpoint_port(cols[1]) != Some(want.as_str()) {
            continue;
        }
        // Listener rows point at 0.0.0.0:0 / [::]:0; ESTABLISHED rows point
        // at a real peer (possibly our own bridge) and must not match.
        if cols[2] != "0.0.0.0:0" && cols[2] != "[::]:0" {
            continue;
        }
        if let Ok(pid) = cols[4].parse::<u32>() {
            return Some(pid);
        }
    }
    None
}

/// Only a Python interpreter may be killed as a stale sidecar.
fn is_python_image(image_path: &str) -> bool {
    let name = image_path
        .rsplit(['\\', '/'])
        .next()
        .unwrap_or(image_path);
    name.eq_ignore_ascii_case("python.exe") || name.eq_ignore_ascii_case("pythonw.exe")
}

/// Full image path of `pid` via kernel32 (no new crates for one call).
#[cfg(windows)]
fn process_image_name(pid: u32) -> Option<String> {
    use std::os::windows::ffi::OsStringExt;
    const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
    #[link(name = "kernel32")]
    extern "system" {
        fn OpenProcess(
            dwDesiredAccess: u32,
            bInheritHandle: i32,
            dwProcessId: u32,
        ) -> *mut std::ffi::c_void;
        fn QueryFullProcessImageNameW(
            hProcess: *mut std::ffi::c_void,
            dwFlags: u32,
            lpExeName: *mut u16,
            lpdwSize: *mut u32,
        ) -> i32;
        fn CloseHandle(hObject: *mut std::ffi::c_void) -> i32;
    }
    unsafe {
        let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if handle.is_null() {
            return None;
        }
        let mut buf = [0u16; 512];
        let mut len = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(handle, 0, buf.as_mut_ptr(), &mut len);
        CloseHandle(handle);
        if ok == 0 {
            return None;
        }
        Some(
            std::ffi::OsString::from_wide(&buf[..len as usize])
                .to_string_lossy()
                .into_owned(),
        )
    }
}

#[cfg(not(windows))]
fn process_image_name(_pid: u32) -> Option<String> {
    None
}

#[cfg(windows)]
fn kill_pid(pid: u32) -> bool {
    std::process::Command::new("taskkill")
        .args(["/F", "/PID", &pid.to_string()])
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

#[cfg(not(windows))]
fn kill_pid(_pid: u32) -> bool {
    false
}

/// Poll until `port` refuses connections (listener gone), ~5 s max.
fn wait_port_free(port: u16) -> bool {
    for _ in 0..10 {
        match TcpStream::connect(format!("127.0.0.1:{port}")) {
            Err(_) => return true,
            Ok(_) => std::thread::sleep(Duration::from_millis(500)),
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    /// Start a mock MediaPipe server that reads one request and replies with
    /// a canned response. Returns the port to connect to.
    fn start_mock_server(response: Vec<u8>) -> u16 {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                // Read the 4-byte length prefix + JPEG data.
                let mut hdr = [0u8; 4];
                let _ = stream.read_exact(&mut hdr);
                let len = u32::from_le_bytes(hdr) as usize;
                let mut body = vec![0u8; len];
                let _ = stream.read_exact(&mut body);
                // Send canned response.
                let _ = stream.write_all(&response);
                let _ = stream.flush();
            }
        });
        port
    }

    #[test]
    fn detect_returns_empty_when_no_hands() {
        // Response: 0 hands
        let port = start_mock_server(vec![0x00]);

        let client = MediapipeClient::connect(port).unwrap();
        let hands = client.detect(b"\xFF\xD8\xFF\xE0");
        assert!(hands.is_empty());
    }

    #[test]
    fn detect_parses_one_left_hand() {
        // Build a response with 1 left hand.
        let mut resp = vec![0x01]; // 1 hand
        resp.push(0x00); // handedness: Left
        resp.extend_from_slice(&0.95f32.to_le_bytes()); // score
        for _ in 0..21 {
            resp.extend_from_slice(&0.1f32.to_le_bytes()); // x
            resp.extend_from_slice(&0.2f32.to_le_bytes()); // y
            resp.extend_from_slice(&0.3f32.to_le_bytes()); // z
        }
        let port = start_mock_server(resp);

        let client = MediapipeClient::connect(port).unwrap();
        let hands = client.detect(b"\xFF\xD8");
        assert_eq!(hands.len(), 1);
        assert_eq!(hands[0].handedness, "Left");
        assert_eq!(hands[0].score, 0.95);
        assert_eq!(hands[0].landmarks[0].x, 0.1);
        assert_eq!(hands[0].landmarks[0].y, 0.2);
        assert_eq!(hands[0].landmarks[0].z, 0.3);
    }

    #[test]
    fn detect_parses_two_hands() {
        let mut resp = vec![0x02]; // 2 hands
        for handedness in [0x00u8, 0x01u8] {
            resp.push(handedness);
            resp.extend_from_slice(&0.8f32.to_le_bytes());
            for _ in 0..21 {
                resp.extend_from_slice(&0.0f32.to_le_bytes());
                resp.extend_from_slice(&0.0f32.to_le_bytes());
                resp.extend_from_slice(&0.0f32.to_le_bytes());
            }
        }
        let port = start_mock_server(resp);

        let client = MediapipeClient::connect(port).unwrap();
        let hands = client.detect(b"\xFF\xD8");
        assert_eq!(hands.len(), 2);
        assert_eq!(hands[0].handedness, "Left");
        assert_eq!(hands[1].handedness, "Right");
    }

    #[test]
    fn detect_sends_length_prefixed_jpeg() {
        // Mock server that echoes back the received JPEG length as the response.
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut hdr = [0u8; 4];
                let _ = stream.read_exact(&mut hdr);
                let len = u32::from_le_bytes(hdr) as usize;
                let mut body = vec![0u8; len];
                let _ = stream.read_exact(&mut body);
                // Reply: 1 hand with score = received length as f32 (for assertion).
                let mut resp = vec![0x01];
                resp.push(0x00);
                resp.extend_from_slice(&(len as f32).to_le_bytes());
                for _ in 0..21 {
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                    resp.extend_from_slice(&0.00f32.to_le_bytes());
                    resp.extend_from_slice(&0.00f32.to_le_bytes());
                }
                let _ = stream.write_all(&resp);
                let _ = stream.flush();
            }
        });

        let client = MediapipeClient::connect(port).unwrap();
        let jpeg = vec![0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10];
        let hands = client.detect(&jpeg);
        assert_eq!(hands.len(), 1);
        // Score encodes the JPEG length we sent.
        assert_eq!(hands[0].score, jpeg.len() as f32);
    }

    #[test]
    fn detect_returns_empty_on_connection_failure() {
        // Connect to a port that nothing is listening on.
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener); // close the port

        let stream = std::net::TcpStream::connect(format!("127.0.0.1:{port}"));
        assert!(stream.is_err());
    }

    #[test]
    fn detect_reconnects_after_server_restart() {
        use std::sync::mpsc::channel;

        // Server A: accepts one connection, then on signal closes it and
        // drops the listener so the port can be rebound (server "death").
        let listener_a = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener_a.local_addr().unwrap().port();
        let (accepted_tx, accepted_rx) = channel();
        let (kill_tx, kill_rx) = channel();
        std::thread::spawn(move || {
            if let Ok((conn, _)) = listener_a.accept() {
                let _ = accepted_tx.send(());
                let _ = kill_rx.recv();
                drop(conn);
            }
            drop(listener_a);
        });

        let client = MediapipeClient::connect(port).unwrap();
        accepted_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        kill_tx.send(()).unwrap();

        // Server B: rebind the same port; the client's next detect must
        // reconnect here and retry the request.
        let mut listener_b = None;
        for _ in 0..50 {
            match TcpListener::bind(format!("127.0.0.1:{port}")) {
                Ok(l) => { listener_b = Some(l); break; }
                Err(_) => std::thread::sleep(Duration::from_millis(50)),
            }
        }
        let listener_b = listener_b.unwrap();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener_b.accept() {
                let mut hdr = [0u8; 4];
                let _ = stream.read_exact(&mut hdr);
                let len = u32::from_le_bytes(hdr) as usize;
                let mut body = vec![0u8; len];
                let _ = stream.read_exact(&mut body);
                let mut resp = vec![0x01, 0x00]; // 1 left hand
                resp.extend_from_slice(&1.0f32.to_le_bytes());
                for _ in 0..21 {
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                    resp.extend_from_slice(&0.0f32.to_le_bytes());
                }
                let _ = stream.write_all(&resp);
                let _ = stream.flush();
            }
        });

        // Only the post-reconnect retry can produce a hand: the first attempt
        // runs on the dead connection.
        let hands = client.detect(b"\xFF\xD8");
        assert_eq!(hands.len(), 1);
    }

    #[test]
    fn set_config_sends_sentinel_frame_and_reads_ack() {
        use std::sync::mpsc::channel;
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let (tx, rx) = channel();
        std::thread::spawn(move || {
            // Two sequential connections: the client's idle detect stream,
            // then the short-lived config connection (`set_config` parks
            // the first, exactly like production).
            for _ in 0..2 {
                let Ok((mut stream, _)) = listener.accept() else { return };
                let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
                let mut buf = [0u8; 17];
                match stream.read_exact(&mut buf) {
                    Ok(()) if &buf[0..4] == &0xFFFFFFFFu32.to_le_bytes() => {
                        let _ = tx.send(buf);
                        let _ = stream.write_all(&[0x00]);
                        let _ = stream.flush();
                        return;
                    }
                    _ => {} // idle detect stream (or timeout) — wait for the next one
                }
            }
        });

        let client = MediapipeClient::connect(port).unwrap();
        assert!(client.set_config(0.8, 0.7, 0.6));
        let got = rx.recv_timeout(Duration::from_secs(10)).unwrap();
        assert_eq!(&got[0..4], &0xFFFFFFFFu32.to_le_bytes());
        assert_eq!(got[4], 0x01);
        assert_eq!(f32::from_le_bytes(got[5..9].try_into().unwrap()), 0.8);
        assert_eq!(f32::from_le_bytes(got[9..13].try_into().unwrap()), 0.7);
        assert_eq!(f32::from_le_bytes(got[13..17].try_into().unwrap()), 0.6);
    }

    #[test]
    fn mediapipe_port_matches_contract() {
        assert_eq!(crate::net::MEDIAPIPE_PORT, 42073);
    }

    const SAMPLE_NETSTAT: &str = "Active Connections\r\n\r\n  Proto  Local Address          Foreign Address        State           PID\r\n  TCP    127.0.0.1:42073        0.0.0.0:0              LISTENING       1804\r\n  TCP    127.0.0.1:42073        127.0.0.1:51234        ESTABLISHED     1804\r\n  TCP    127.0.0.1:8567         0.0.0.0:0              LISTENING       9999\r\n";

    #[test]
    fn netstat_output_maps_listener_to_pid() {
        // The LISTENING row wins; the ESTABLISHED row (a live client, maybe
        // our own bridge) must not shadow it.
        assert_eq!(parse_listen_pid(SAMPLE_NETSTAT, 42073), Some(1804));
        assert_eq!(parse_listen_pid(SAMPLE_NETSTAT, 8567), Some(9999));
    }

    #[test]
    fn netstat_parser_ignores_localized_state_words() {
        // German Windows reports ABHÖREN instead of LISTENING — the foreign
        // endpoint (0.0.0.0:0), not the state word, identifies listeners.
        let german = "  Proto  Lokale Adresse         Remoteadresse          Status            PID\r\n  TCP    127.0.0.1:42073        0.0.0.0:0              ABH\u{d6}REN         4321\r\n";
        assert_eq!(parse_listen_pid(german, 42073), Some(4321));
    }

    #[test]
    fn netstat_parser_returns_none_when_port_absent() {
        assert_eq!(parse_listen_pid(SAMPLE_NETSTAT, 12345), None);
        assert_eq!(parse_listen_pid("", 42073), None);
        // Port as a suffix of a longer port must not match.
        let tricky = "  TCP    127.0.0.1:142073       0.0.0.0:0              LISTENING       7\r\n";
        assert_eq!(parse_listen_pid(tricky, 42073), None);
    }

    #[test]
    fn python_image_check_matches_interpreters_only() {
        assert!(is_python_image(r"C:\Python312\python.exe"));
        assert!(is_python_image(r"C:\a\b\pythonw.EXE"));
        assert!(is_python_image("/usr/bin/python.exe"));
        assert!(!is_python_image(r"C:\bridge\cardboard-bridge.exe"));
        assert!(!is_python_image("python")); // bare name, no .exe
        assert!(!is_python_image(r"C:\tools\python_helper.exe"));
    }

    /// A non-Python process squatting the sidecar port must survive reclaim:
    /// the bridge reports it instead of killing it. Binds the real contract
    /// port briefly; skips if a sidecar is already running.
    #[test]
    #[cfg(windows)]
    fn reclaim_refuses_non_python_port_holder() {
        let listener = match std::net::TcpListener::bind("127.0.0.1:42073") {
            Ok(l) => l,
            Err(_) => return, // sidecar running — nothing to prove here
        };
        match MediapipeClient::reclaim_port(42073) {
            // Our own listener trips either the self-guard or the
            // non-python guard — both must refuse, never kill.
            Reclaim::Refused(_) => {}
            other => panic!("must refuse a non-python squatter, got {other:?}"),
        }
        // And the squatter is still alive to take a connection.
        assert!(std::net::TcpStream::connect("127.0.0.1:42073").is_ok());
        drop(listener);
    }
}
