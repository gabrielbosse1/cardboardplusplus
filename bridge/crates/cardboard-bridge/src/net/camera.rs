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
use std::time::Duration;

use crate::app::SharedState;
use crate::net::mediapipe::MediapipeClient;
use crate::net::{CAMERA_PORT, MEDIAPIPE_PORT};

/// u16 seq header in front of every JPEG datagram (big-endian).
pub const SEQ_HEADER_LEN: usize = 2;

/// Idle sleep when no datagrams are available.
const POLL_INTERVAL: Duration = Duration::from_millis(1);

/// Depth of the detect channel. 2 slots: one in flight, one newest-waiting.
const DETECT_QUEUE: usize = 2;

type DetectJob = (u16, u32, u32, Vec<u8>, Vec<u8>); // seq, w, h, rgba, jpeg

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
    let detect_running = Arc::new(AtomicBool::new(true));

    // Always run the detect worker, even when the sidecar wasn't reachable
    // at startup: it lazy-connects (and retries) so a slow Python import
    // or a manually-started server still heals without a bridge restart.
    {
        let detect_state = state.clone();
        let running = detect_running.clone();
        std::thread::spawn(move || detect_loop(rx, detect_state, client, running));
    }

    std::thread::spawn(move || camera_loop(sock, state, tx));
}

/// Recv thread: drain to the newest datagram, decode, display immediately,
/// forward the JPEG to the detect worker when it has room.
fn camera_loop(sock: UdpSocket, state: SharedState, tx: mpsc::SyncSender<DetectJob>) {
    let mut buf = [0u8; 65535];
    let mut frame_count: u64 = 0;
    let mut latest: Vec<u8> = Vec::new();

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
            std::thread::sleep(POLL_INTERVAL);
            continue;
        }
        frame_count += 1;
        process_frame(&latest, &state, frame_count, &tx);
    }
}

/// Decode one datagram: strip seq, JPEG decode, store for UI, offer to detect.
fn process_frame(
    datagram: &[u8],
    state: &SharedState,
    frame_count: u64,
    tx: &mpsc::SyncSender<DetectJob>,
) {
    let seq = u16::from_be_bytes([datagram[0], datagram[1]]);
    let jpeg_data = &datagram[SEQ_HEADER_LEN..];
    // Quick SOI sanity check before paying for a full decode.
    if jpeg_data.len() < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8 {
        return;
    }
    crate::debug_log!(state, "[camera] frame #{frame_count} seq={seq}, {} bytes jpeg", jpeg_data.len());

    let (w, h, rgba) = match decode_jpeg(jpeg_data) {
        Some(v) => v,
        None => return,
    };

    // When the overlay is on, only the detect thread should store display
    // frames — it annotates them with the skeleton. Storing raw frames here
    // would cause a visible raw↔overlay flicker as the two threads race to
    // overwrite the same slot.
    let overlay_on = state.lock().map(|s| s.hand_overlay).unwrap_or(true);
    if !overlay_on {
        // Display path: never waits for MediaPipe.
        if let Ok(mut s) = state.lock() {
            s.note_camera_frame(w, h, rgba.clone());
            if frame_count % 60 == 1 {
                s.push_log(format!("camera frame #{frame_count} seq={seq}, {w}x{h}"));
            }
        }
    } else if frame_count % 60 == 1 {
        // Still log even when skipping the display write, for diagnostics.
        crate::debug_log!(state, "[camera] frame #{frame_count} seq={seq}, {w}x{h} (overlay on, skipping raw store)");
    }

    // Detect path: latest-wins. Channel full = worker busy → drop this one
    // (the display path above already stored the frame).
    let _ = tx.try_send((seq, w, h, rgba, jpeg_data.to_vec()));
}

/// Detect thread: blocking TCP detect on the latest job only. Holds an
/// optional client: `None` (sidecar unreachable at startup) lazy-connects
/// on the first enabled frame and retries, so the pipeline self-heals.
fn detect_loop(
    rx: mpsc::Receiver<DetectJob>,
    state: SharedState,
    mut client: Option<MediapipeClient>,
    running: Arc<AtomicBool>,
) {
    while running.load(Ordering::Relaxed) {
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
                if let Ok(mut s) = state.lock() {
                    s.push_log("mediapipe: lazy-connected to sidecar".into());
                }
            }
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
            if count > 0 {
                crate::debug_log!(&state, "[camera] seq={seq} {count} hand(s)");
            }
        }
    }
}

/// JPEG → (w, h, RGBA). Pure-Rust decode of a ~256x192 frame (~50 kpx).
fn decode_jpeg(jpeg_data: &[u8]) -> Option<(u32, u32, Vec<u8>)> {
    let mut decoder = jpeg_decoder::Decoder::new(jpeg_data);
    let pixels = decoder.decode().ok()?;
    let info = decoder.info()?;
    let w = info.width as u32;
    let h = info.height as u32;
    let mut rgba = Vec::with_capacity((w * h) as usize * 4);
    for chunk in pixels.chunks(3) {
        if chunk.len() < 3 {
            break;
        }
        rgba.push(chunk[0]);
        rgba.push(chunk[1]);
        rgba.push(chunk[2]);
        rgba.push(255);
    }
    Some((w, h, rgba))
}
