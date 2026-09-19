//! Fast camera frame receiver from the phone.
//!
//! Wire (UDP 42072): `[u16 seq BE][JPEG 256x192]` per datagram, one frame per
//! datagram, 30 fps. Latest-wins: the recv thread keeps only the newest
//! datagram; a slow MediaPipe worker can never stall the display path.
//!
//! Threads (stdlib only):
//! - recv: non-blocking drain-to-latest, JPEG decode, immediate
//!   `note_camera_frame` for the UI, forward JPEG to the detect channel
//!   (dropped when the worker is busy).
//! - detect: blocking TCP `MediapipeClient::detect` on the latest JPEG only,
//!   draws the skeleton overlay, stores the annotated frame.

use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};

use crate::app::SharedState;
use crate::net::mediapipe::MediapipeClient;
use crate::net::{CAMERA_PORT, MEDIAPIPE_PORT};

/// u16 seq header in front of every JPEG datagram (big-endian).
pub const SEQ_HEADER_LEN: usize = 2;

/// Idle sleep when no datagrams are available.
const POLL_INTERVAL: Duration = Duration::from_millis(1);

/// Depth of the detect channel. 2 slots: one in flight, one newest-waiting.
const DETECT_QUEUE: usize = 2;

/// After this many consecutive stale frames the phone presumably restarted
/// (seq reset to 0) and the tracker re-anchors instead of dropping forever.
const STALE_RESYNC_AFTER: u32 = 30;

type DetectJob = (u16, u32, u32, Vec<u8>, Arc<Vec<u8>>); // seq, w, h, rgba, jpeg (shared)

/// Bind the camera socket and start the receive + detect threads.
/// Called by `AppCore::new`.
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
    // Non-blocking so the drain loop below actually drains.
    if sock.set_nonblocking(true).is_err() {
        if let Ok(mut s) = state.lock() {
            s.push_log("camera socket non-blocking failed".into());
        }
        return;
    }

    if let Ok(mut s) = state.lock() {
        s.push_log(format!("camera listener on udp {CAMERA_PORT}"));
    }

    // Latest-wins channel to the detect worker.
    let (tx, rx) = mpsc::sync_channel::<DetectJob>(DETECT_QUEUE);
    // True once the sidecar has answered (startup or lazy-connect): lets the
    // recv thread skip decodes nobody will use (see `process_frame`).
    let sidecar_ok = Arc::new(AtomicBool::new(client.is_some()));

    // Always run the detect worker, even when the sidecar wasn't reachable
    // at startup: it lazy-connects (and retries) so a slow Python import
    // or a manually-started server still heals without a bridge restart.
    {
        let detect_state = state.clone();
        let ok = sidecar_ok.clone();
        std::thread::spawn(move || detect_loop(rx, detect_state, client, ok));
    }

    std::thread::spawn(move || camera_loop(sock, state, tx, sidecar_ok));
}

/// Wrapping-aware frame ordering for the u16 BE seq header (endianness
/// untouched). Accepts only frames newer than anything seen: duplicates and
/// reordered (stale) frames are rejected, forward jumps count the skipped
/// frames as gaps.
#[derive(Default)]
struct SeqTracker {
    last: Option<u16>,
    stale_run: u32,
    gaps: u64,
    stale_dropped: u64,
}

impl SeqTracker {
    fn accept(&mut self, seq: u16) -> bool {
        match self.last {
            None => {
                self.last = Some(seq);
                true
            }
            Some(last) => {
                let delta = seq.wrapping_sub(last);
                if delta == 0 {
                    return false; // duplicate
                }
                if delta > 0x8000 {
                    // Older than `last`: reorder. But a phone restart resets
                    // seq to 0, which also looks stale — re-anchor after a run.
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

/// Recv thread: drain to the newest datagram, decode, display immediately,
/// forward the JPEG to the detect worker when it has room.
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
        // Drain every available datagram; only the newest survives.
        let mut got_one = false;
        loop {
            match sock.recv(&mut buf) {
                Ok(n) if n > SEQ_HEADER_LEN => {
                    latest.clear();
                    latest.extend_from_slice(&buf[..n]);
                    got_one = true;
                }
                Ok(_) => {} // too short for seq + JPEG — ignore
                _ => break, // Would-block — done for this tick
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

/// Decode one datagram: strip seq, JPEG decode, store for UI, offer to detect.
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
    // Quick SOI sanity check before paying for a full decode.
    if jpeg_data.len() < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8 {
        return;
    }
    crate::debug_log!(state, "[camera] frame #{frame_count} seq={seq}, {} bytes jpeg", jpeg_data.len());
    // Shared payload: the detect job clones the Arc, not the bytes.
    let jpeg_shared: Arc<Vec<u8>> = Arc::new(jpeg_data.to_vec());

    // When the overlay is on, only the detect thread should store display
    // frames — it annotates them with the skeleton. Storing raw frames here
    // would cause a visible raw↔overlay flicker as the two threads race to
    // overwrite the same slot.
    let (overlay_on, enabled, pending) = state
        .lock()
        .map(|s| (s.hand_overlay, s.hand_enabled, s.camera_frame.is_some()))
        .unwrap_or((true, false, false));
    if !overlay_on {
        if !enabled && !sidecar_ok.load(Ordering::Relaxed) && pending {
            // Nobody watches (last frame unconsumed), nothing to detect
            // with, overlay off: skip the decode, but still offer the JPEG
            // to the worker so a late sidecar lazy-connects and the hand
            // count stays live.
            let _ = tx.try_send((seq, 0, 0, Vec::new(), jpeg_shared));
            return;
        }
        let (w, h, rgba) = match decode_jpeg(jpeg_data) {
            Some(v) => v,
            None => return,
        };
        // Display path: never waits for MediaPipe. `rgba` moves in (no
        // clone); the worker gets an empty frame — the raw display store
        // above is already the annotated-free output it would have stored.
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
        // Still log even when skipping the display write, for diagnostics.
        crate::debug_log!(state, "[camera] frame #{frame_count} seq={seq}, {w}x{h} (overlay on, skipping raw store)");
    }

    // Detect path: latest-wins. Channel full = worker busy → drop this one
    // (the display path above already stored the frame).
    let _ = tx.try_send((seq, w, h, rgba, jpeg_shared));
}

/// Detect thread: blocking TCP detect on the latest job only. Holds an
/// optional client: `None` (sidecar unreachable at startup) lazy-connects
/// on the first enabled frame and retries, so the pipeline self-heals.
fn detect_loop(
    rx: mpsc::Receiver<DetectJob>,
    state: SharedState,
    mut client: Option<MediapipeClient>,
    sidecar_ok: Arc<AtomicBool>,
) {
    loop {
        // Drain to the newest job; intermediate frames are superseded.
        let mut job = match rx.recv() {
            Ok(j) => j,
            Err(_) => break, // sender gone
        };
        while let Ok(newer) = rx.try_recv() {
            job = newer;
        }
        let (seq, w, h, mut rgba, jpeg) = job;
        // Master switch lives in shared state so the UI toggle takes effect
        // on the very next frame without touching the thread layout.
        let (enabled, overlay) = match state.lock() {
            Ok(s) => (s.hand_enabled, s.hand_overlay),
            Err(_) => (false, true),
        };
        // Lazy (re)connect: first enabled frame after a failed startup, or
        // a manually-started sidecar, picks up the server without a restart.
        // Healthy probe (not a bare accept): a wedged squatter must not be
        // adopted — startup reclaims those; here we just retry next frame.
        // Thresholds come from state so the probe never clobbers tuning.
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
                    // When overlay is on, this is the sole display writer.
                    // Count the frame for fps so the pill doesn't read zero.
                    s.note_camera_frame(w, h, rgba);
                } else {
                    s.store_camera_frame(w, h, rgba);
                }
            }
        }
        // debug_log! re-locks the state — emit after the guard above drops.
        if count > 0 {
            crate::debug_log!(&state, "[camera] seq={seq} {count} hand(s)");
        }
    }
}

/// JPEG → (w, h, RGBA). Pure-Rust decode of a ~256x192 frame (~50 kpx).
/// Decodes straight to RGBA (no RGB→RGBA repack pass).
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
    use super::*;

    #[test]
    fn seq_tracker_accepts_in_order_and_counts_gaps() {
        let mut t = SeqTracker::default();
        assert!(t.accept(0));
        assert!(t.accept(1));
        assert!(t.accept(5)); // skipped 2..4
        assert_eq!(t.gaps, 3);
    }

    #[test]
    fn seq_tracker_wraps_around_u16() {
        let mut t = SeqTracker::default();
        assert!(t.accept(0xFFFF));
        assert!(t.accept(0)); // wrapping +1, not stale
        assert!(t.accept(1));
        assert_eq!(t.gaps, 0);
    }

    #[test]
    fn seq_tracker_rejects_duplicates_and_stale() {
        let mut t = SeqTracker::default();
        assert!(t.accept(100));
        assert!(!t.accept(100)); // duplicate
        assert!(!t.accept(50)); // reordered
        assert_eq!(t.stale_dropped, 1);
        assert!(t.accept(101)); // stream continues
    }

    #[test]
    fn seq_tracker_resyncs_after_phone_restart() {
        let mut t = SeqTracker::default();
        assert!(t.accept(5000));
        // Phone restarted: seq back at 0. First frames look stale...
        for seq in 0..STALE_RESYNC_AFTER {
            assert!(!t.accept(seq as u16), "seq={seq} should still look stale");
        }
        // ...then the tracker re-anchors and the stream resumes.
        assert!(t.accept(STALE_RESYNC_AFTER as u16));
        assert!(t.accept(STALE_RESYNC_AFTER as u16 + 1));
    }
}
