//! JPEG camera frame receiver from the phone.
//!
//! Listens on UDP 42072 for JPEG datagrams sent by `CameraStreamer` on the
//! phone. Each complete datagram is one JPEG frame.  The raw JPEG is forwarded
//! to the Python MediaPipe server via TCP for hand detection; the RGBA copy
//! with hand skeleton overlay goes to the UI preview.

use std::net::UdpSocket;
use std::time::Duration;

use crate::app::SharedState;
use crate::net::mediapipe::MediapipeClient;
use crate::net::CAMERA_PORT;

/// Poll cadence for the non-blocking recv loop.
const POLL_INTERVAL: Duration = Duration::from_millis(10);

/// Bind the camera socket and start the receive loop.  Called by `AppCore::new`.
pub fn spawn(state: SharedState, client: Option<MediapipeClient>) {
    let sock = match UdpSocket::bind(format!("0.0.0.0:{CAMERA_PORT}")) {
        Ok(sock) => sock,
        Err(err) => {
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("camera bind on {CAMERA_PORT} failed: {err}"));
            }
            return;
        }
    }
    .into();

    if let Ok(mut s) = state.lock() {
        s.push_log(format!("camera listener on udp {CAMERA_PORT}"));
    }

    std::thread::spawn(move || camera_loop(sock, state, client));
}

/// Receive JPEG datagrams forever: decode each to RGBA, forward raw JPEG to
/// the MediaPipe server for hand detection, draw skeleton overlay, store for
/// the UI preview.
fn camera_loop(sock: UdpSocket, state: SharedState, client: Option<MediapipeClient>) {
    let mut buf = [0u8; 65535];
    let mut frame_count: u64 = 0;

    loop {
        match sock.recv(&mut buf) {
            Ok(n) if n > 0 => {
                frame_count += 1;
                process_frame(&buf[..n], &state, frame_count, client.as_ref());
            }
            _ => {}
        }
        std::thread::sleep(POLL_INTERVAL);
    }
}

/// Decode a JPEG datagram to RGBA, forward raw JPEG to MediaPipe server,
/// draw hand skeleton overlay, store in `SharedState`.
fn process_frame(
    jpeg_data: &[u8],
    state: &SharedState,
    frame_count: u64,
    client: Option<&MediapipeClient>,
) {
    // Decode JPEG -> RGBA for the UI preview.
    let mut decoder = jpeg_decoder::Decoder::new(jpeg_data);
    let pixels: Vec<u8> = match decoder.decode() {
        Ok(p) => p,
        Err(_) => return,
    };
    let info = match decoder.info() {
        Some(i) => i,
        None => return,
    };
    let w = info.width as u32;
    let h = info.height as u32;

    let pixel_count = (w * h) as usize;
    let mut rgba = Vec::with_capacity(pixel_count * 4);
    for chunk in pixels.chunks(3) {
        rgba.push(chunk[0]);
        rgba.push(chunk[1]);
        rgba.push(chunk[2]);
        rgba.push(255);
    }

    // Forward raw JPEG to MediaPipe server for hand detection, then draw
    // the skeleton overlay directly onto the RGBA pixels.
    if let Some(cli) = client {
        let hands = cli.detect(jpeg_data);
        let count = hands.len();
        if count > 0 {
            crate::hand_overlay::draw_hands(&mut rgba, w, h, &hands);
        }
        if let Ok(mut s) = state.lock() {
            s.camera_detected_hands = count;
            if frame_count % 30 == 1 && count > 0 {
                s.push_log(format!(
                    "hand detection: frame #{frame_count}, {count} hand(s), 21 landmarks each",
                ));
            }
        }
    }

    if let Ok(mut s) = state.lock() {
        s.note_camera_frame(w, h, rgba);
        if frame_count % 60 == 1 {
            s.push_log(format!("camera frame #{frame_count}, {w}x{h}"));
        }
    }
}
