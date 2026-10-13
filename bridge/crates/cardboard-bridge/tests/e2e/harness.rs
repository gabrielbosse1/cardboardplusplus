//! End-to-end harness: launches the real bridge binary headless, speaks its
//! REST plane over raw TCP (no HTTP dependency), polls /status and /logs as
//! the assertion surface, and gates every fake-mode test behind one mutex
//! because the UDP wire ports are fixed machine-wide.
#![allow(dead_code)]
use std::io::{Read, Write};
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};
/// Base REST port for test bridges; bumped per bridge so back-to-back runs
/// never race a TIME_WAIT socket.
const REST_BASE: u16 = 18567;
static NEXT_REST: AtomicU16 = AtomicU16::new(REST_BASE);
/// UDP ports the fake fixtures and the real driver compete for.
pub const VIDEO_PORT: u16 = 42069;
pub const DRIVER_PORT: u16 = 42070;
pub const TELEMETRY_PORT: u16 = 42071;
pub const CAMERA_PORT: u16 = 42072;
pub const MEDIAPIPE_PORT: u16 = 42073;
pub const SENSOR_PORT: u16 = 42074;
/// One bridge at a time: all UDP ports in the wire contract are fixed, so
/// fake-mode tests must not overlap (scripts/e2e.ps1 also forces
/// --test-threads=1 as a belt).
pub fn serial() -> MutexGuard<'static, ()> {
    static GATE: Mutex<()> = Mutex::new(());
    GATE.lock().unwrap_or_else(|e| e.into_inner())
}
/// Live mode attaches to a real running bridge + SteamVR driver instead of
/// spawning fixtures (CBPP_E2E_LIVE=1 scripts/e2e.ps1 -Live).
pub fn live_mode() -> bool {
    std::env::var("CBPP_E2E_LIVE").is_ok_and(|v| v == "1")
}
/// Minimal HTTP/1.1 client over std only. Reads until Content-Length is
/// satisfied or the read times out, so keep-alive never wedges a test.
pub struct Http {
    pub port: u16,
}
impl Http {
    pub fn request(&self, method: &str, path: &str, body: Option<&str>) -> (u16, String) {
        let mut sock = TcpStream::connect(("127.0.0.1", self.port))
            .unwrap_or_else(|e| panic!("REST {method} {path}: connect port {}: {e}", self.port));
        sock.set_read_timeout(Some(Duration::from_secs(4))).unwrap();
        let mut req = format!("{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n");
        if let Some(b) = body {
            req.push_str(&format!("Content-Length: {}\r\nContent-Type: application/json\r\n", b.len()));
        }
        req.push_str("\r\n");
        if let Some(b) = body {
            req.push_str(b);
        }
        sock.write_all(req.as_bytes()).expect("REST request write");
        let mut raw = Vec::new();
        let mut buf = [0u8; 8192];
        loop {
            match sock.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    raw.extend_from_slice(&buf[..n]);
                    if let Some(len) = content_length(&raw) {
                        if raw.len() >= header_end(&raw).unwrap_or(0) + len {
                            break;
                        }
                    }
                }
            }
        }
        let text = String::from_utf8_lossy(&raw).into_owned();
        let status = text
            .split_whitespace()
            .nth(1)
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        let body = header_end(&raw)
            .map(|e| String::from_utf8_lossy(&raw[e + 4..]).into_owned())
            .unwrap_or_default();
        (status, body)
    }
    pub fn get(&self, path: &str) -> (u16, String) {
        self.request("GET", path, None)
    }
    /// GET expecting 200 + JSON; panics with the body on anything else.
    pub fn get_json(&self, path: &str) -> serde_json::Value {
        let (code, body) = self.get(path);
        assert_eq!(code, 200, "GET {path} -> {code} {body}");
        serde_json::from_str(&body).unwrap_or_else(|e| panic!("GET {path}: bad json {e}: {body}"))
    }
    pub fn post(&self, path: &str, body: &str) -> (u16, String) {
        self.request("POST", path, Some(body))
    }
    pub fn post_json(&self, path: &str, body: &str) -> (u16, serde_json::Value) {
        let (code, text) = self.post(path, body);
        let json = if text.is_empty() {
            serde_json::Value::Null
        } else {
            serde_json::from_str(&text).unwrap_or_else(|e| panic!("POST {path}: bad json {e}: {text}"))
        };
        (code, json)
    }
    pub fn healthy(&self) -> bool {
        match self.get("/health") {
            (200, body) => body.contains("\"ok\":true"),
            _ => false,
        }
    }
}
fn header_end(raw: &[u8]) -> Option<usize> {
    raw.windows(4).position(|w| w == b"\r\n\r\n")
}
fn content_length(raw: &[u8]) -> Option<usize> {
    let head = header_end(raw)?;
    let text = String::from_utf8_lossy(&raw[..head]).into_owned();
    for line in text.lines() {
        let (k, v) = line.split_once(':')?;
        if k.trim().eq_ignore_ascii_case("content-length") {
            return v.trim().parse().ok();
        }
    }
    None
}
/// The bridge under test, headless with CARDBOARD_DEBUG=1 so every debug_log!
/// line reaches the ring log. Killed on drop.
pub struct Bridge {
    child: Child,
    pub http: Http,
}
impl Bridge {
    /// Starts a fresh bridge; panics (with the port holder named) when a real
    /// bridge or driver still squats one of the fixed UDP ports.
    pub fn start() -> Bridge {
        for port in [VIDEO_PORT, TELEMETRY_PORT, CAMERA_PORT] {
            if std::net::UdpSocket::bind(("0.0.0.0", port)).is_err() {
                panic!("udp {port} busy — a bridge (or SteamVR driver) is already running; stop it before the fake-mode e2e run");
            }
        }
        let port = NEXT_REST.fetch_add(1, Ordering::Relaxed);
        let child = Command::new(env!("CARGO_BIN_EXE_cardboard-bridge-svc"))
            .arg("--headless")
            .env("CARDBOARD_BRIDGE_PORT", port.to_string())
            .env("CARDBOARD_DEBUG", "1")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn cardboard-bridge-svc");
        let bridge = Bridge { child, http: Http { port } };
        let deadline = Instant::now() + Duration::from_secs(15);
        while !bridge.http.healthy() {
            if Instant::now() > deadline {
                panic!("bridge REST never answered on port {port} within 15s");
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        bridge
    }
    pub fn status(&self) -> serde_json::Value {
        self.http.get_json("/status")
    }
    pub fn logs(&self, n: usize) -> Vec<String> {
        self.http
            .get_json(&format!("/logs?n={n}"))
            .get("logs")
            .and_then(|v| v.as_array())
            .map(|a| a.iter().filter_map(|l| l.as_str().map(str::to_string)).collect())
            .unwrap_or_default()
    }
    pub fn log_contains(&self, needle: &str) -> bool {
        self.logs(200).iter().any(|l| l.contains(needle))
    }
    pub fn wait_log(&self, needle: &str, timeout: Duration) {
        wait_until(&format!("log line containing {needle:?}"), timeout, || self.log_contains(needle));
    }
    pub fn status_field(&self, field: &str) -> serde_json::Value {
        self.status().get(field).cloned().unwrap_or(serde_json::Value::Null)
    }
    /// OS pid of the bridge process (memory probe for the soak test).
    pub fn pid(&self) -> u32 {
        self.child.id()
    }
    pub fn wait_status(&self, field: &str, want: serde_json::Value, timeout: Duration) {
        let what = format!("{field} == {want}");
        wait_until(&what, timeout, || self.status_field(field) == want);
    }
}
impl Drop for Bridge {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
/// Live-mode handle: REST only, nothing spawned or bound.
pub struct LiveBridge(pub Http);
impl LiveBridge {
    pub fn connect() -> LiveBridge {
        let port = std::env::var("CARDBOARD_BRIDGE_PORT")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(8567);
        let http = Http { port };
        assert!(
            http.healthy(),
            "CBPP_E2E_LIVE=1 but no bridge REST on port {port} — start the bridge (and SteamVR) first"
        );
        LiveBridge(http)
    }
}
/// Polls every 100ms until `cond` or panics naming what was awaited.
pub fn wait_until(what: &str, timeout: Duration, mut cond: impl FnMut() -> bool) {
    let deadline = Instant::now() + timeout;
    loop {
        if cond() {
            return;
        }
        assert!(Instant::now() < deadline, "timed out after {timeout:?} waiting for {what}");
        std::thread::sleep(Duration::from_millis(100));
    }
}
/// Artifact directory (target/tmp/.../e2e-artifacts) for saved stream frames.
pub fn artifacts() -> std::path::PathBuf {
    let dir = std::path::Path::new(env!("CARGO_TARGET_TMPDIR")).join("e2e-artifacts");
    std::fs::create_dir_all(&dir).expect("create artifacts dir");
    dir
}
/// True when ffmpeg answers on PATH (the bridge preview and the stream test
/// both need it; tests skip softly without it).
pub fn ffmpeg_available() -> bool {
    static OK: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *OK.get_or_init(|| {
        Command::new("ffmpeg")
            .arg("-version")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    })
}
/// Builds a 320x240 3s Annex-B H.264 file (IDR every 15 frames) for the
/// video-stream test. False when the encode failed → caller skips.
pub fn make_test_stream(path: &std::path::Path) -> bool {
    Command::new("ffmpeg")
        .args([
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=320x240:rate=30:duration=3",
            "-c:v",
            "libx264",
            "-profile:v",
            "baseline",
            "-pix_fmt",
            "yuv420p",
            "-g",
            "15",
            "-f",
            "h264",
        ])
        .arg(path)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}
/// Decodes one Annex-B access unit to a PPM; True on a decodable frame.
pub fn decode_frame_ppm(au: &[u8], dir: &std::path::Path, name: &str) -> bool {
    let src = dir.join(format!("{name}.h264"));
    let out = dir.join(format!("{name}.ppm"));
    if std::fs::write(&src, au).is_err() {
        return false;
    }
    Command::new("ffmpeg")
        .args(["-y", "-f", "h264", "-i"])
        .arg(&src)
        .args(["-frames:v", "1"])
        .arg(&out)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success() && out.is_file())
        .unwrap_or(false)
}
/// Splits Annex-B bytes into access units: each new VCL NAL (types 1/5)
/// starts a new AU, with any non-VCL NALs that followed the previous VCL
/// moved to the AU they precede (SPS/PPS → next IDR).
pub fn split_access_units(data: &[u8]) -> Vec<Vec<u8>> {
    let mut nals: Vec<(usize, u8)> = Vec::new();
    let mut i = 0;
    while i + 3 <= data.len() {
        if data[i] == 0 && data[i + 1] == 0 {
            if data[i + 2] == 1 {
                if i + 3 < data.len() {
                    nals.push((i, data[i + 3] & 0x1f));
                }
                i += 3;
                continue;
            }
            if data[i + 2] == 0 && i + 4 <= data.len() && data[i + 3] == 1 {
                if i + 4 < data.len() {
                    nals.push((i, data[i + 4] & 0x1f));
                }
                i += 4;
                continue;
            }
        }
        i += 1;
    }
    let is_vcl = |t: u8| t == 1 || t == 5;
    if nals.is_empty() {
        return Vec::new();
    }
    let mut aus = Vec::new();
    let mut start = nals[0].0;
    let mut boundary: Option<usize> = None;
    let mut has_vcl = false;
    for &(sc, nal_type) in &nals {
        if is_vcl(nal_type) {
            if has_vcl {
                let cut = boundary.unwrap_or(sc);
                aus.push(data[start..cut].to_vec());
                start = cut;
            }
            has_vcl = true;
            boundary = None;
        } else if has_vcl && boundary.is_none() {
            boundary = Some(sc);
        }
    }
    aus.push(data[start..].to_vec());
    aus
}
/// Working set of a process in MB (Windows: tasklist; Linux: /proc VmRSS).
/// Used by the soak test as its memory-behavior probe.
pub fn working_set_mb(pid: u32) -> Option<u64> {
    #[cfg(windows)]
    {
        let out = Command::new("tasklist")
            .args(["/FI", &format!("PID eq {pid}"), "/FO", "CSV", "/NH"])
            .output()
            .ok()?;
        let text = String::from_utf8_lossy(&out.stdout).into_owned();
        let row = text.lines().find(|l| l.contains(&pid.to_string()))?;
        let mem = row.split("\",\"").nth(4)?;
        mem.trim().trim_matches('"').replace(" K", "").replace(',', "").parse::<u64>().ok().map(|kb| kb / 1024)
    }
    #[cfg(not(windows))]
    {
        let status = std::fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
        let line = status.lines().find(|l| l.starts_with("VmRSS:"))?;
        line.split_whitespace().nth(1)?.parse::<u64>().ok().map(|kb| kb / 1024)
    }
}
