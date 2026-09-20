use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};
use crate::app::SharedState;
use crate::net::mediapipe::MediapipeClient;
use crate::net::{CAMERA_PORT, MEDIAPIPE_PORT};
/// Length of the big-endian u16 sequence prefix on each camera datagram.
pub const SEQ_HEADER_LEN: usize = 2;
const POLL_INTERVAL: Duration = Duration::from_millis(1);
const DETECT_QUEUE: usize = 2;
const STALE_RESYNC_AFTER: u32 = 30;
/// Work item for the detect thread: sequence id, decoded size, RGBA pixels
/// (empty when only the JPEG is needed), and the shared JPEG for MediaPipe.
type DetectJob = (u16, u32, u32, Vec<u8>, Arc<Vec<u8>>);
/// Starts the camera pipeline (UDP 42072): a non-blocking receive thread plus
/// a detect thread connected by a 2-deep latest-wins channel. `client` is the
/// optional pre-connected MediaPipe sidecar; when None the detect thread
/// lazy-connects on first use. Logs-and-returns when the socket cannot bind.
pub fn spawn(state: SharedState, client: Option<MediapipeClient>) {
    let sock = match UdpSocket::bind(format!("0.0.0.0:{CAMERA_PORT}")) {
        Ok(sock) => sock,
        Err(err) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("camera bind on {CAMERA_PORT} failed: {err}"));
            }
            return;
        }
    };
    if sock.set_nonblocking(true).is_err() {
        if let Ok(mut s) = state.lock() {
            s.push_log("camera socket non-blocking failed".into());
        }
        return;
    }
    if let Ok(mut s) = state.lock() {
        s.push_log(format!("camera listener on udp {CAMERA_PORT}"));
    }
    let (tx, rx) = mpsc::sync_channel::<DetectJob>(DETECT_QUEUE);
    let sidecar_ok = Arc::new(AtomicBool::new(client.is_some()));
    {
        let detect_state = state.clone();
        let ok = sidecar_ok.clone();
        std::thread::spawn(move || detect_loop(rx, detect_state, client, ok));
    }
    std::thread::spawn(move || camera_loop(sock, state, tx, sidecar_ok));
}
/// Reorders the u16 phone sequence numbers: accepts in-order frames, counts
/// gaps (dropped UDP), drops duplicates and stale retransmits, and resyncs
/// after STALE_RESYNC_AFTER stale frames in a row (phone app restarted and
/// the counter wrapped back to 0).
#[derive(Default)]
struct SeqTracker {
    last: Option<u16>,
    stale_run: u32,
    gaps: u64,
    stale_dropped: u64,
}
impl SeqTracker {
    /// Returns true when `seq` is newer than the last accepted frame.
    /// Wrapping subtraction handles the u16 rollover; a delta in the top
    /// half of the range means the frame is older (stale/duplicate).
    fn accept(&mut self, seq: u16) -> bool {
        match self.last {
            None => {
                self.last = Some(seq);
                true
            }
            Some(last) => {
                let delta = seq.wrapping_sub(last);
                if delta == 0 {
                    return false;
                }
                if delta > 0x8000 {
                    self.stale_dropped += 1;
                    self.stale_run += 1;
                    if self.stale_run > STALE_RESYNC_AFTER {
                        self.last = Some(seq);
                        self.stale_run = 0;
                        return true;
                    }
                    return false;
                }
                if delta > 1 {
                    self.gaps += (delta - 1) as u64;
                }
                self.last = Some(seq);
                self.stale_run = 0;
                true
            }
        }
    }
}
/// Non-blocking receive loop. Drains every queued datagram per tick and keeps
/// only the newest (latest-wins: no point decoding a stale frame when a newer
/// one is already here). After 10s of silence logs a one-time firewall hint.
fn camera_loop(
    sock: UdpSocket,
    state: SharedState,
    tx: mpsc::SyncSender<DetectJob>,
    sidecar_ok: Arc<AtomicBool>,
) {
    let mut buf = [0u8; 65535];
    let mut frame_count: u64 = 0;
    let mut latest: Vec<u8> = Vec::new();
    let mut tracker = SeqTracker::default();
    let started = Instant::now();
    let mut silence_hinted = false;
    let mut got_one_ever = false;
    loop {
        let mut got_one = false;
        loop {
            match sock.recv(&mut buf) {
                Ok(n) if n > SEQ_HEADER_LEN => {
                    latest.clear();
                    latest.extend_from_slice(&buf[..n]);
                    got_one = true;
                }
                Ok(_) => {}
                _ => break,
            }
        }
        if !got_one {
            if !silence_hinted && !got_one_ever && started.elapsed() > Duration::from_secs(10) {
                silence_hinted = true;
                if let Ok(mut s) = state.lock() {
                    s.push_log(format!(
                        "camera bound but silent after 10s — check Windows firewall inbound UDP {CAMERA_PORT} and the phone's Wi-Fi"
                    ));
                }
            }
            std::thread::sleep(POLL_INTERVAL);
            continue;
        }
        got_one_ever = true;
        frame_count += 1;
        process_frame(&latest, &state, frame_count, &tx, &mut tracker, &sidecar_ok);
    }
}
/// Validates one camera datagram and routes it: drops stale/duplicate
/// sequences and non-JPEG payloads, stores the decoded RGBA for the preview,
/// and queues a DetectJob (try_send: the queue holder drops rather than
/// blocks when the detector is busy). Every 60th frame logs to the ring log.
fn process_frame(
    datagram: &[u8],
    state: &SharedState,
    frame_count: u64,
    tx: &mpsc::SyncSender<DetectJob>,
    tracker: &mut SeqTracker,
    sidecar_ok: &Arc<AtomicBool>,
) {
    let seq = u16::from_be_bytes([datagram[0], datagram[1]]);
    if !tracker.accept(seq) {
        crate::debug_log!(state, "[camera] dropped stale/duplicate seq={seq}");
        return;
    }
    let jpeg_data = &datagram[SEQ_HEADER_LEN..];
    if jpeg_data.len() < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8 {
        return;
    }
    crate::debug_log!(state, "[camera] frame #{frame_count} seq={seq}, {} bytes jpeg", jpeg_data.len());
    let jpeg_shared: Arc<Vec<u8>> = Arc::new(jpeg_data.to_vec());
    let (overlay_on, enabled, pending) = state
        .lock()
        .map(|s| (s.hand_overlay, s.hand_enabled, s.camera_frame.is_some()))
        .unwrap_or((true, false, false));
    if !overlay_on {
        if !enabled && !sidecar_ok.load(Ordering::Relaxed) && pending {
            let _ = tx.try_send((seq, 0, 0, Vec::new(), jpeg_shared));
            return;
        }
        let (w, h, rgba) = match decode_jpeg(jpeg_data) {
            Some(v) => v,
            None => return,
        };
        if let Ok(mut s) = state.lock() {
            s.note_camera_frame(w, h, rgba);
            if frame_count % 60 == 1 {
                s.push_log(format!("camera frame #{frame_count} seq={seq}, {w}x{h}"));
            }
        }
        let _ = tx.try_send((seq, w, h, Vec::new(), jpeg_shared));
        return;
    }
    let (w, h, rgba) = match decode_jpeg(jpeg_data) {
        Some(v) => v,
        None => return,
    };
    if frame_count % 60 == 1 {
        crate::debug_log!(state, "[camera] frame #{frame_count} seq={seq}, {w}x{h} (overlay on, skipping raw store)");
    }
    let _ = tx.try_send((seq, w, h, rgba, jpeg_shared));
}
/// Detect thread: takes the newest queued job (skipping backlog), lazy-
/// connects the MediaPipe sidecar when hand tracking is enabled, runs
/// detection on the JPEG, draws the hand overlay into the RGBA when enabled,
/// and publishes the frame plus hand count to shared state.
fn detect_loop(
    rx: mpsc::Receiver<DetectJob>,
    state: SharedState,
    mut client: Option<MediapipeClient>,
    sidecar_ok: Arc<AtomicBool>,
) {
    loop {
        let mut job = match rx.recv() {
            Ok(j) => j,
            Err(_) => break,
        };
        while let Ok(newer) = rx.try_recv() {
            job = newer;
        }
        let (seq, w, h, mut rgba, jpeg) = job;
        let (enabled, overlay) = match state.lock() {
            Ok(s) => (s.hand_enabled, s.hand_overlay),
            Err(_) => (false, true),
        };
        if enabled && client.is_none() {
            let (d, p, t) = match state.lock() {
                Ok(s) => (
                    s.hand_min_detection as f32 / 100.0,
                    s.hand_min_presence as f32 / 100.0,
                    s.hand_min_tracking as f32 / 100.0,
                ),
                Err(_) => (0.5, 0.5, 0.5),
            };
            if let Some(cli) = MediapipeClient::connect_healthy(MEDIAPIPE_PORT, d, p, t) {
                client = Some(cli);
                sidecar_ok.store(true, Ordering::Relaxed);
                if let Ok(mut s) = state.lock() {
                    s.push_log("mediapipe: lazy-connected to sidecar".into());
                }
            }
        }
        if client.is_some() {
            sidecar_ok.store(true, Ordering::Relaxed);
        }
        let hands = match (&client, enabled) {
            (Some(cli), true) => cli.detect(&jpeg),
            _ => vec![],
        };
        let count = hands.len();
        if overlay && count > 0 && !rgba.is_empty() {
            crate::hand_overlay::draw_hands(&mut rgba, w, h, &hands);
        }
        if let Ok(mut s) = state.lock() {
            s.camera_detected_hands = count;
            if !rgba.is_empty() {
                if overlay {
                    s.note_camera_frame(w, h, rgba);
                } else {
                    s.store_camera_frame(w, h, rgba);
                }
            }
        }
        if count > 0 {
            crate::debug_log!(&state, "[camera] seq={seq} {count} hand(s)");
        }
    }
}
/// Decodes a phone JPEG (256x192 q38) straight to RGBA via zune-jpeg.
/// Returns None on corrupt data (dropped UDP tail); callers skip the frame.
fn decode_jpeg(jpeg_data: &[u8]) -> Option<(u32, u32, Vec<u8>)> {
    use zune_core::bytestream::ZCursor;
    use zune_core::colorspace::ColorSpace;
    use zune_core::options::DecoderOptions;
    let options = DecoderOptions::default().jpeg_set_out_colorspace(ColorSpace::RGBA);
    let mut decoder = zune_jpeg::JpegDecoder::new_with_options(ZCursor::new(jpeg_data), options);
    let rgba = decoder.decode().ok()?;
    let info = decoder.info()?;
    Some((info.width as u32, info.height as u32, rgba))
}
#[cfg(test)]
mod tests {
    // SeqTracker contract tests: in-order accept with gap counting, u16
    // wraparound, duplicate/stale rejection, and resync after a phone-side
    // sequence restart. Test names read as the spec.
    use super::*;
    #[test]
    fn seq_tracker_accepts_in_order_and_counts_gaps() {
        let mut t = SeqTracker::default();
        assert!(t.accept(0));
        assert!(t.accept(1));
        assert!(t.accept(5));
        assert_eq!(t.gaps, 3);
    }
    #[test]
    fn seq_tracker_wraps_around_u16() {
        let mut t = SeqTracker::default();
        assert!(t.accept(0xFFFF));
        assert!(t.accept(0));
        assert!(t.accept(1));
        assert_eq!(t.gaps, 0);
    }
    #[test]
    fn seq_tracker_rejects_duplicates_and_stale() {
        let mut t = SeqTracker::default();
        assert!(t.accept(100));
        assert!(!t.accept(100));
        assert!(!t.accept(50));
        assert_eq!(t.stale_dropped, 1);
        assert!(t.accept(101));
    }
    #[test]
    fn seq_tracker_resyncs_after_phone_restart() {
        let mut t = SeqTracker::default();
        assert!(t.accept(5000));
        for seq in 0..STALE_RESYNC_AFTER {
            assert!(!t.accept(seq as u16), "seq={seq} should still look stale");
        }
        assert!(t.accept(STALE_RESYNC_AFTER as u16));
        assert!(t.accept(STALE_RESYNC_AFTER as u16 + 1));
    }
}
