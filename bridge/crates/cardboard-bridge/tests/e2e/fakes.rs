//! Fake collaborators for the e2e run: a fake SteamVR driver (UDP 42070 +
//! 42074, replies like the real Discovery.cpp), a fake phone (telemetry +
//! camera sender on 42071/42072), a fake MediaPipe sidecar (TCP 42073 that
//! acks the config handshake and returns fake hands), and a fake driver-side
//! SHM status region. No mock framework: plain sockets + threads, per repo
//! test policy.
#![allow(dead_code)]
use std::collections::VecDeque;
use std::io::{ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU16, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use crate::harness::{wait_until, CAMERA_PORT, DRIVER_PORT, MEDIAPIPE_PORT, SENSOR_PORT, TELEMETRY_PORT, VIDEO_PORT};
/// A 16x12 JPEG (490 bytes) the fake phone streams as camera frames; the
/// bridge decodes it for the camera pill without needing ffmpeg.
const TEST_JPEG_HEX: &str = "ffd8ffe000104a46494600010200000100010000fffe00104c61766336322e32382e31303000ffdb0043000814141714171b1b1b1b1b1b201e20212121202020202121212424242a2a2a2424242121242428282a2a2e2f2e2b2b2a2b2f2f3232323c3c3939464648565667ffc40072000003010000000000000000000000000002040607010101010100000000000000000000000006050307100002010303030501000000000000000001020311041200213105518122f0b18261711100020103030403010100000000000000010203110421120031054122135161142432ffc0001108000c001003011200021200031200ffda000c03010002110311003f0086540bd363615abe55fac941eff74e45784d900628084e061b6eddabe7fba6dd2e240e5e9e4d8af7a6703eb19d98e9b6a659e4fe8b950ba68164a0c863c537b453bcd1de46c174dbac0b1e0547b5f5bd4f392171c788c6ebdd5a24b66d70a5a167a6b10908ada5f48d4295341c54e37a7dc5c3dbdfc1853d72246c0f0439453e403b1efa1366b37500ad24bc160721552b89056a0d287e06b9a74c620501c3723e6953b4b6179ef8d97f3dac745aea8e2d2dfea9cd4ee0410c723dfc8ca0bc46d8a3775d4aca69f441c8e383db785acad6af3200241ed8118cbe45c18657ab645482834fc0aeff00ffd9";
/// Byte-decodes the embedded JPEG fixture.
pub fn test_jpeg() -> Vec<u8> {
    (0..TEST_JPEG_HEX.len() / 2)
        .map(|i| u8::from_str_radix(&TEST_JPEG_HEX[i * 2..i * 2 + 2], 16).unwrap())
        .collect()
}
/// Builds a tag-0x10 gyro packet (45B), mirroring TelemetrySender.java.
pub fn gyro_packet(ts_ms: u64, av: [f32; 3], ac: [f32; 3], mg: [f32; 3]) -> Vec<u8> {
    let mut p = vec![0x10];
    p.extend_from_slice(&ts_ms.to_le_bytes());
    for v in av.iter().chain(ac.iter()).chain(mg.iter()) {
        p.extend_from_slice(&v.to_le_bytes());
    }
    p
}
/// Builds a tag-0x11 hand-hint packet (15B).
pub fn hand_packet(ts_ms: u64, hands: u8, landmarks: u8, confidence: f32) -> Vec<u8> {
    let mut p = vec![0x11];
    p.extend_from_slice(&ts_ms.to_le_bytes());
    p.push(hands);
    p.push(landmarks);
    p.extend_from_slice(&confidence.to_le_bytes());
    p
}
/// Builds a tag-0x12 rotation packet (25B), quat [w,x,y,z].
pub fn rotation_packet(ts_ms: u64, quat: [f32; 4]) -> Vec<u8> {
    let mut p = vec![0x12];
    p.extend_from_slice(&ts_ms.to_le_bytes());
    for v in quat {
        p.extend_from_slice(&v.to_le_bytes());
    }
    p
}
/// Builds a tag-0x13 net-stats packet (21B).
pub fn netstats_packet(ts_ms: u64, frames: u32, stalls: u32, fps: f32) -> Vec<u8> {
    let mut p = vec![0x13];
    p.extend_from_slice(&ts_ms.to_le_bytes());
    p.extend_from_slice(&frames.to_le_bytes());
    p.extend_from_slice(&stalls.to_le_bytes());
    p.extend_from_slice(&fps.to_le_bytes());
    p
}
/// Builds the phone hello text with the version token the bridge records.
pub fn hello_packet(version: &str) -> Vec<u8> {
    format!("CARDBOARD_PHONE_HELLO v1 {version}").into_bytes()
}
/// Builds one camera datagram: u16 BE sequence + one whole JPEG.
pub fn camera_datagram(seq: u16, jpeg: &[u8]) -> Vec<u8> {
    let mut d = vec![(seq >> 8) as u8, (seq & 0xff) as u8];
    d.extend_from_slice(jpeg);
    d
}
/// Stats the fake driver reports in every BRIDGE_STATS reply.
#[derive(Clone, Copy)]
pub struct Stats {
    pub fps: i32,
    pub kbps: i32,
    pub frames: u64,
    pub drops: u64,
}
/// Fake SteamVR driver: owns UDP 42070 + 42074, mirrors Discovery.cpp
/// replies (BRIDGE_ACK + STATS for hellos, ACK + CARDBOARD_CAP echo for
/// phone beacons) and records everything the bridge sends so tests can
/// confirm it landed "on the driver".
pub struct FakeDriver {
    /// Held to keep UDP 42070 bound; the reader thread has its own clone.
    _ctrl: UdpSocket,
    /// Held to keep UDP 42074 bound; the reader thread has its own clone.
    _sensor_sock: UdpSocket,
    video: UdpSocket,
    stop: Arc<AtomicBool>,
    captured: Arc<Mutex<VecDeque<String>>>,
    sensor: Arc<Mutex<Vec<Vec<u8>>>>,
    stats: Arc<Mutex<Stats>>,
    muted: Arc<AtomicBool>,
    phone_target: Arc<Mutex<Option<SocketAddr>>>,
    phone_port: Arc<AtomicU16>,
}
impl FakeDriver {
    pub fn new() -> FakeDriver {
        let ctrl = UdpSocket::bind(("127.0.0.1", DRIVER_PORT)).expect("fake driver bind 42070 (stop SteamVR/driver)");
        let sensor = UdpSocket::bind(("127.0.0.1", SENSOR_PORT)).expect("fake driver bind 42074");
        let reader = ctrl.try_clone().unwrap();
        let sensor_reader = sensor.try_clone().unwrap();
        reader.set_read_timeout(Some(Duration::from_millis(150))).unwrap();
        sensor_reader.set_read_timeout(Some(Duration::from_millis(150))).unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let captured = Arc::new(Mutex::new(VecDeque::new()));
        let sensor_store = Arc::new(Mutex::new(Vec::new()));
        let stats = Arc::new(Mutex::new(Stats { fps: 60, kbps: 20000, frames: 0, drops: 0 }));
        let muted = Arc::new(AtomicBool::new(false));
        let phone_target = Arc::new(Mutex::new(None));
        let phone_port = Arc::new(AtomicU16::new(VIDEO_PORT));
        {
            let (stop, captured, stats, muted, phone_target, phone_port) =
                (stop.clone(), captured.clone(), stats.clone(), muted.clone(), phone_target.clone(), phone_port.clone());
            std::thread::spawn(move || {
                let mut buf = [0u8; 2048];
                loop {
                    match reader.recv_from(&mut buf) {
                        Ok((0, _)) => {}
                        Ok((n, src)) => {
                            let msg = String::from_utf8_lossy(&buf[..n]).trim().to_string();
                            if let Ok(mut c) = captured.lock() {
                                if c.len() >= 500 {
                                    c.pop_front();
                                }
                                c.push_back(msg.clone());
                            }
                            if msg.starts_with("BRIDGE_HELLO") {
                                if !muted.load(Ordering::Relaxed) {
                                    let st = *stats.lock().unwrap();
                                    let _ = reader.send_to(b"BRIDGE_ACK v1 4242", src);
                                    let _ = reader.send_to(
                                        format!("BRIDGE_STATS fps={} bitrate={} frames={} drops={}", st.fps, st.kbps, st.frames, st.drops).as_bytes(),
                                        src,
                                    );
                                }
                            } else if msg.starts_with("CARDBOARD_DISCOVERY") || msg.starts_with("CARDBOARD_PHONE_HELLO") {
                                let cap = captured
                                    .lock()
                                    .unwrap()
                                    .iter()
                                    .rev()
                                    .find(|m| m.starts_with("CARDBOARD_CAP"))
                                    .cloned()
                                    .unwrap_or_else(|| "CARDBOARD_CAP 1920 1080".into());
                                let _ = reader.send_to(b"ACK", src);
                                let _ = reader.send_to(cap.as_bytes(), src);
                                if let Ok(mut t) = phone_target.lock() {
                                    *t = Some(SocketAddr::new(src.ip(), phone_port.load(Ordering::Relaxed)));
                                }
                            }
                        }
                        Err(ref e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                        Err(_) => return,
                    }
                    if stop.load(Ordering::Relaxed) {
                        return;
                    }
                }
            });
        }
        {
            let (stop, sensor_store) = (stop.clone(), sensor_store.clone());
            std::thread::spawn(move || {
                let mut buf = [0u8; 2048];
                loop {
                    match sensor_reader.recv_from(&mut buf) {
                        Ok((n, _)) if n > 0 => {
                            let mut store = sensor_store.lock().unwrap();
                            if store.len() >= 256 {
                                store.remove(0);
                            }
                            store.push(buf[..n].to_vec());
                        }
                        Err(ref e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                        Err(_) => return,
                        Ok(_) => {}
                    }
                    if stop.load(Ordering::Relaxed) {
                        return;
                    }
                }
            });
        }
        FakeDriver {
            _ctrl: ctrl,
            _sensor_sock: sensor,
            video: UdpSocket::bind("0.0.0.0:0").unwrap(),
            stop,
            captured,
            sensor: sensor_store,
            stats,
            muted,
            phone_target,
            phone_port,
        }
    }
    fn snapshot(&self) -> Vec<String> {
        self.captured.lock().unwrap().iter().cloned().collect()
    }
    /// Waits until a captured datagram equals `exact`; panics naming the
    /// recent captures on timeout so the failure reads as a wire diff.
    pub fn wait_capture(&self, exact: &str, timeout: Duration) {
        let want = exact.to_string();
        wait_until(&format!("driver capture {exact:?}"), timeout, || {
            self.snapshot().iter().any(|m| m == &want)
        });
    }
    pub fn wait_capture_prefix(&self, prefix: &str, timeout: Duration) -> String {
        let p = prefix.to_string();
        let hit = Arc::new(Mutex::new(None::<String>));
        let found = hit.clone();
        wait_until(&format!("driver capture {prefix:?} ..."), timeout, || {
            if let Some(m) = self.snapshot().into_iter().find(|m| m.starts_with(&p)) {
                *found.lock().unwrap() = Some(m);
                return true;
            }
            false
        });
        let got = hit.lock().unwrap().take();
        got.unwrap()
    }
    pub fn count_prefix(&self, prefix: &str) -> usize {
        self.snapshot().iter().filter(|m| m.starts_with(prefix)).count()
    }
    pub fn last_prefix(&self, prefix: &str) -> Option<String> {
        self.snapshot().into_iter().rev().find(|m| m.starts_with(prefix))
    }
    pub fn set_stats(&self, fps: i32, kbps: i32, frames: u64, drops: u64) {
        *self.stats.lock().unwrap() = Stats { fps, kbps, frames, drops };
    }
    /// When true the fake stops answering hellos: the bridge must expire the
    /// driver after its 5s ack timeout.
    pub fn set_muted(&self, muted: bool) {
        self.muted.store(muted, Ordering::Relaxed);
    }
    /// Port the fake streams the phone copy of the video at (the real driver
    /// always uses 42069 on the phone device; loopback needs its own port).
    pub fn set_phone_port(&self, port: u16) {
        self.phone_port.store(port, Ordering::Relaxed);
    }
    pub fn sensor_packets(&self) -> Vec<Vec<u8>> {
        self.sensor.lock().unwrap().clone()
    }
    pub fn wait_sensor(&self, timeout: Duration, pred: impl Fn(&[u8]) -> bool) -> Vec<u8> {
        let hit = Arc::new(Mutex::new(None::<Vec<u8>>));
        let found = hit.clone();
        wait_until("sensor packet match", timeout, || {
            if let Some(p) = self.sensor_packets().into_iter().find(|p| pred(p)) {
                *found.lock().unwrap() = Some(p);
                return true;
            }
            false
        });
        let got = hit.lock().unwrap().take();
        got.unwrap()
    }
    /// Replays access units like the real driver: 4-byte BE length-prefixed,
    /// chunked at 1400B to the phone target, and raw Annex-B chunked to the
    /// bridge preview socket at 127.0.0.1:42069.
    pub fn stream_h264(&self, aus: &[Vec<u8>]) -> usize {
        let phone = self.phone_target.lock().unwrap().clone();
        let preview: SocketAddr = "127.0.0.1:42069".parse().unwrap();
        let mut sent = 0;
        for au in aus {
            if let Some(target) = &phone {
                let mut framed = Vec::with_capacity(4 + au.len());
                framed.extend_from_slice(&(au.len() as u32).to_be_bytes());
                framed.extend_from_slice(au);
                for chunk in framed.chunks(1400) {
                    let _ = self.video.send_to(chunk, target);
                }
            }
            for chunk in au.chunks(1400) {
                let _ = self.video.send_to(chunk, preview);
            }
            sent += 1;
            std::thread::sleep(Duration::from_millis(33));
        }
        sent
    }
}
impl Drop for FakeDriver {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        // The reader threads hold cloned sockets; they exit within their
        // 150ms recv timeout, and the next test binds the same ports.
        std::thread::sleep(Duration::from_millis(400));
    }
}
/// Fake MediaPipe sidecar on TCP 42073: acks the config sentinel frame and
/// answers every detect request with `hands` synthetic hands. Binding this
/// before the bridge also stops the bridge from spawning real Python.
/// One per test process (see `sidecar()`): the accepted sockets sit in
/// TIME_WAIT after a bridge dies, and Windows refuses to rebind 42073 until
/// they expire, so the listener is created once and never dropped.
pub struct FakeSidecar {
    listener: TcpListener,
    stop: Arc<AtomicBool>,
    hands: Arc<AtomicUsize>,
    detects: Arc<AtomicUsize>,
}
/// Process-global sidecar: every fake-mode bridge connects to this one.
pub fn sidecar() -> &'static FakeSidecar {
    static ONCE: std::sync::OnceLock<FakeSidecar> = std::sync::OnceLock::new();
    ONCE.get_or_init(FakeSidecar::new)
}
impl FakeSidecar {
    fn new() -> FakeSidecar {
        let listener = TcpListener::bind(("127.0.0.1", MEDIAPIPE_PORT)).expect("fake sidecar bind 42073 (stop mediapipe python)");
        let accept_side = listener.try_clone().unwrap();
        accept_side.set_nonblocking(true).unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let hands = Arc::new(AtomicUsize::new(0));
        let detects = Arc::new(AtomicUsize::new(0));
        {
            let (stop, hands, detects) = (stop.clone(), hands.clone(), detects.clone());
            std::thread::spawn(move || {
                while !stop.load(Ordering::Relaxed) {
                    match accept_side.accept() {
                        Ok((stream, _)) => {
                            let (hands, detects, stop) = (hands.clone(), detects.clone(), stop.clone());
                            std::thread::spawn(move || serve_conn(stream, hands, detects, stop));
                        }
                        Err(ref e) if e.kind() == ErrorKind::WouldBlock => {
                            std::thread::sleep(Duration::from_millis(20));
                        }
                        Err(_) => return,
                    }
                }
            });
        }
        FakeSidecar { listener, stop, hands, detects }
    }
    /// Hands the sidecar reports per detect (1 also drives a skeleton packet
    /// to the driver sensor port).
    pub fn set_hands(&self, n: usize) {
        self.hands.store(n, Ordering::Relaxed);
    }
    pub fn detects(&self) -> usize {
        self.detects.load(Ordering::Relaxed)
    }
}
/// One sidecar connection: config sentinel -> 0x00 ack; jpeg request ->
/// fake-hand records; runs until EOF, disconnect, or stop. Read timeouts are
/// only retried at frame boundaries so framing can never desync. The socket
/// is never dropped on purpose: an active close after the bridge dies would
/// leave a TIME_WAIT on 42073 and block the next test process from binding.
fn serve_conn(stream: TcpStream, hands: Arc<AtomicUsize>, detects: Arc<AtomicUsize>, stop: Arc<AtomicBool>) {
    let mut stream = stream;
    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    serve_loop(&mut stream, &hands, &detects, &stop);
    std::mem::forget(stream);
}
fn serve_loop(stream: &mut TcpStream, hands: &Arc<AtomicUsize>, detects: &Arc<AtomicUsize>, stop: &Arc<AtomicBool>) {
    while !stop.load(Ordering::Relaxed) {
        let mut head = [0u8; 4];
        match fill(stream, &mut head, &stop) {
            Ok(true) => {}
            Ok(false) => continue,
            Err(()) => return,
        }
        let len = u32::from_le_bytes(head);
        if len == 0xFFFFFFFF {
            let mut cfg = [0u8; 13];
            if fill(stream, &mut cfg, &stop) != Ok(true) {
                return;
            }
            if stream.write_all(&[0x00]).is_err() {
                return;
            }
            let _ = stream.flush();
            continue;
        }
        if len > 10_000_000 {
            return;
        }
        let mut jpeg = vec![0u8; len as usize];
        if fill(stream, &mut jpeg, &stop) != Ok(true) {
            return;
        }
        detects.fetch_add(1, Ordering::Relaxed);
        let n = hands.load(Ordering::Relaxed);
        let mut resp = vec![n as u8];
        for h in 0..n {
            resp.push(h as u8);
            resp.extend_from_slice(&0.9f32.to_le_bytes());
            for i in 0..21 {
                let img = [0.5f32, 0.9 - 0.03 * i as f32, 0.001 * i as f32];
                for v in img {
                    resp.extend_from_slice(&v.to_le_bytes());
                }
            }
            for i in 0..21 {
                let world = [0.01 * i as f32, -0.02 * i as f32, -0.3 + 0.001 * i as f32];
                for v in world {
                    resp.extend_from_slice(&v.to_le_bytes());
                }
            }
        }
        if stream.write_all(&resp).is_err() {
            return;
        }
        let _ = stream.flush();
    }
}
/// Reads buf completely. Ok(true)=full, Ok(false)=timeout with 0 bytes read
/// (retry at the boundary), Err=disconnect/partial frame.
fn fill(stream: &mut TcpStream, buf: &mut [u8], stop: &AtomicBool) -> Result<bool, ()> {
    let mut off = 0;
    while off < buf.len() {
        if stop.load(Ordering::Relaxed) {
            return Err(());
        }
        match stream.read(&mut buf[off..]) {
            Ok(0) => return Err(()),
            Ok(n) => off += n,
            Err(ref e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {
                if off == 0 {
                    return Ok(false);
                }
                return Err(());
            }
            Err(_) => return Err(()),
        }
    }
    Ok(true)
}
/// Fake phone: an ephemeral UDP socket talking the phone side of the wire
/// (hello/gyro/rotation/hand/netstats to 42071, JPEG frames to 42072,
/// beacons to 42070).
pub struct Phone {
    sock: UdpSocket,
}
impl Phone {
    pub fn new() -> Phone {
        Phone { sock: UdpSocket::bind("0.0.0.0:0").unwrap() }
    }
    pub fn local_port(&self) -> u16 {
        self.sock.local_addr().unwrap().port()
    }
    fn send(&self, port: u16, data: &[u8]) {
        self.sock.send_to(data, ("127.0.0.1", port)).unwrap();
    }
    pub fn hello(&self, version: &str) {
        self.send(TELEMETRY_PORT, &hello_packet(version));
    }
    pub fn gyro(&self, ts_ms: u64) {
        self.send(TELEMETRY_PORT, &gyro_packet(ts_ms, [1.5, -2.5, 3.5], [0.1, 9.8, 0.2], [22.0, -45.0, 11.0]));
    }
    pub fn rotation(&self, ts_ms: u64, quat: [f32; 4]) {
        self.send(TELEMETRY_PORT, &rotation_packet(ts_ms, quat));
    }
    pub fn hand_hint(&self, ts_ms: u64, hands: u8) {
        self.send(TELEMETRY_PORT, &hand_packet(ts_ms, hands, 21, 0.9));
    }
    pub fn net_stats(&self, ts_ms: u64, frames: u32, stalls: u32, fps: f32) {
        self.send(TELEMETRY_PORT, &netstats_packet(ts_ms, frames, stalls, fps));
    }
    pub fn garbage(&self, n: usize) {
        self.send(TELEMETRY_PORT, &vec![0xAA; n]);
    }
    pub fn camera(&self, seq: u16, jpeg: &[u8]) {
        self.send(CAMERA_PORT, &camera_datagram(seq, jpeg));
    }
    pub fn beacon(&self) {
        self.send(DRIVER_PORT, b"CARDBOARD_DISCOVERY");
    }
    /// Next datagram to this phone as text (ACK, CARDBOARD_CAP echo, ...).
    pub fn recv_text(&self, timeout: Duration) -> Option<String> {
        let mut buf = [0u8; 512];
        let n = self.recv_into(&mut buf, timeout)?;
        Some(String::from_utf8_lossy(&buf[..n]).trim().to_string())
    }
    /// Next datagram bytes into buf; None on timeout.
    pub fn recv_into(&self, buf: &mut [u8], timeout: Duration) -> Option<usize> {
        self.sock.set_read_timeout(Some(timeout)).ok()?;
        match self.sock.recv(buf) {
            Ok(n) => Some(n),
            Err(_) => None,
        }
    }
}
/// Fake driver-side SHM status region (the real driver owns this side; the
/// bridge only opens it). Creates the region with the protocol layout and
/// publishes MT_TELEMETRY slots on demand.
pub struct FakeShm {
    mem: bridge_shm::mem::SharedMemory,
    next_seq: AtomicU64,
}
impl FakeShm {
    pub fn create() -> FakeShm {
        use bridge_shm::protocol::{DEFAULT_REGION_SIZE, DEFAULT_SLOT_COUNT, DEFAULT_SLOT_SIZE, HEADER_SIZE, MAGIC, PROTOCOL_VERSION};
        let mem = bridge_shm::mem::SharedMemory::create(&bridge_shm::mem::region_name(), DEFAULT_REGION_SIZE, 0)
            .expect("create shm status region (a real driver may hold it)");
        unsafe {
            let b = mem.base();
            std::ptr::write_bytes(b, 0, DEFAULT_REGION_SIZE);
            std::ptr::copy_nonoverlapping(MAGIC.as_ptr(), b, 4);
            (b.add(4) as *mut u32).write(PROTOCOL_VERSION);
            (b.add(8) as *mut u32).write(HEADER_SIZE as u32);
            (b.add(12) as *mut u32).write(DEFAULT_SLOT_SIZE as u32);
            (b.add(16) as *mut u32).write(DEFAULT_SLOT_COUNT as u32);
            (b.add(24) as *mut u64).write(0);
        }
        FakeShm { mem, next_seq: AtomicU64::new(0) }
    }
    /// Writes one telemetry slot and advances write_seq (driver-side Publish
    /// mirror). The bridge's 50ms drain must then log the batch.
    pub fn publish_telemetry(&self, frames: u64) {
        use bridge_shm::protocol::{payload, slot_offset, MsgType, DEFAULT_SLOT_COUNT, DEFAULT_SLOT_SIZE};
        let seq = self.next_seq.fetch_add(1, Ordering::Relaxed);
        let off = slot_offset(seq, DEFAULT_SLOT_COUNT as u32, DEFAULT_SLOT_SIZE as u32);
        unsafe {
            let base = self.mem.base();
            let slot = base.add(off);
            std::ptr::write_bytes(slot, 0, DEFAULT_SLOT_SIZE);
            (slot as *mut u64).write_unaligned(seq);
            (slot.add(8) as *mut u32).write_unaligned(MsgType::TELEMETRY);
            (slot.add(12) as *mut u32).write_unaligned(payload::Telemetry::SIZE as u32);
            let t = payload::Telemetry {
                frames,
                avg_encode_us: 4200,
                max_encode_us: 9000,
                avg_interval_us: 16600,
                max_interval_us: 22000,
                dup_count: 0,
                summary_frames: frames,
                pad: [0; 1],
            };
            std::ptr::copy_nonoverlapping(
                &t as *const _ as *const u8,
                slot.add(std::mem::size_of::<u64>() + 2 * std::mem::size_of::<u32>()),
                payload::Telemetry::SIZE,
            );
            std::ptr::write_volatile(base.add(24) as *mut u64, seq + 1);
        }
    }
}
