//! Bridge preview receive path: driver -> 127.0.0.1:42069 -> ffmpeg -> hub.
//!
//! The driver sends every encoded access unit as header-framed fragments
//! (layout in CardboardWire.h, "Bridge preview framing"; mirrored here — change
//! both). This module turns datagrams back into whole access units and decides
//! which of them the decoder may see:
//!
//! * `Reassembler` only ever emits complete frames. A frame missing a fragment
//!   is discarded, never forwarded truncated.
//! * `DecodeGate` forwards frames in strictly increasing, contiguous frame-id
//!   order starting at a keyframe that carries SPS. After any gap (lost frame,
//!   decoder queue overflow, new session) it drops everything until the next
//!   clean keyframe and asks the driver for one, so a P-frame is never decoded
//!   against a reference the decoder does not have.
//! * The UDP receive thread never touches the ffmpeg pipe. A blocking pipe
//!   write used to stall the socket drain long enough for keyframe bursts to
//!   overflow the receive buffer.
//! * The receive thread lives for the whole app; ffmpeg instances come and go
//!   per stream (see preview_lifecycle.rs). Each decoder has a generation;
//!   only the current one is fed and allowed to publish pictures.
use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::UdpSocket;
use std::process::{Child, ChildStderr, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crate::app::SharedState;

pub const MAGIC: u32 = 0x4342_5056; // "CBPV"
pub const VERSION: u8 = 1;
pub const HEADER_SIZE: usize = 24;
pub const MAX_DATAGRAM: usize = 1400;
pub const MAX_PAYLOAD: usize = MAX_DATAGRAM - HEADER_SIZE;
pub const FLAG_KEYFRAME: u8 = 0x01;
/// On a control datagram (frag_count 0): STREAM_END, the driver ended this
/// session on purpose (SteamVR unload, preview turned off).
pub const FLAG_END: u8 = 0x02;
/// Bridge -> driver (UDP 42070): the preview needs an IDR.
pub const BRIDGE_KEYFRAME_REQ: &[u8] = b"BRIDGE_KEYFRAME_REQ";

/// Largest access unit accepted (an uncompressed 8K NV12 frame is ~50 MB).
const MAX_FRAME_SIZE: u32 = 64 * 1024 * 1024;
/// Incomplete frames kept while newer ones arrive. Loopback does not reorder in
/// practice; anything older than this is lost.
const MAX_PARTIALS: usize = 4;
/// Requested socket receive buffer. A 2880x1620 keyframe is several hundred KB
/// sent back-to-back; the OS default (64 KB on Windows) drops most of it.
const RECV_BUFFER_BYTES: i32 = 8 * 1024 * 1024;
/// Complete frames queued for the ffmpeg writer. Overflow is a gap, not a
/// silent drop: the gate re-acquires on the next keyframe.
const DECODER_QUEUE_DEPTH: usize = 16;
/// Same throttle the phone uses for KEYFRAME_REQ: one request per loss burst.
const KEYFRAME_REQ_INTERVAL: Duration = Duration::from_millis(500);
const RECV_POLL: Duration = Duration::from_millis(100);
const LOSS_LOG_INTERVAL: Duration = Duration::from_secs(1);
const SUMMARY_INTERVAL: Duration = Duration::from_secs(5);

/// One parsed preview datagram header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub flags: u8,
    pub frag_index: u16,
    pub frag_count: u16,
    pub payload_len: u16,
    pub session: u32,
    pub frame_id: u32,
    pub frame_size: u32,
}

fn be16(d: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([d[at], d[at + 1]])
}

fn be32(d: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
}

impl Header {
    /// Parses and validates a datagram header; None for foreign or garbled
    /// datagrams (same rules as preview::ReadHeader in the driver).
    pub fn parse(d: &[u8]) -> Option<Header> {
        if d.len() < HEADER_SIZE || be32(d, 0) != MAGIC || d[4] != VERSION {
            return None;
        }
        let h = Header {
            flags: d[5],
            frag_index: be16(d, 6),
            frag_count: be16(d, 8),
            payload_len: be16(d, 10),
            session: be32(d, 12),
            frame_id: be32(d, 16),
            frame_size: be32(d, 20),
        };
        let payload = d.len() - HEADER_SIZE;
        if h.payload_len as usize != payload || payload > MAX_PAYLOAD {
            return None;
        }
        if h.frag_count == 0 {
            return if payload == 0 { Some(h) } else { None };
        }
        if h.frag_index >= h.frag_count {
            return None;
        }
        Some(h)
    }

    /// Serializes the header (tests and tooling; the bridge never sends video).
    pub fn write(&self, out: &mut [u8]) {
        out[0..4].copy_from_slice(&MAGIC.to_be_bytes());
        out[4] = VERSION;
        out[5] = self.flags;
        out[6..8].copy_from_slice(&self.frag_index.to_be_bytes());
        out[8..10].copy_from_slice(&self.frag_count.to_be_bytes());
        out[10..12].copy_from_slice(&self.payload_len.to_be_bytes());
        out[12..16].copy_from_slice(&self.session.to_be_bytes());
        out[16..20].copy_from_slice(&self.frame_id.to_be_bytes());
        out[20..24].copy_from_slice(&self.frame_size.to_be_bytes());
    }

    pub fn is_keyframe(&self) -> bool {
        self.flags & FLAG_KEYFRAME != 0
    }

    /// STREAM_END control datagram (preview::IsEnd in the driver).
    pub fn is_end(&self) -> bool {
        self.frag_count == 0 && self.flags & FLAG_END != 0
    }
}

/// The STREAM_END datagram the driver sends (tests and tooling).
pub fn stream_end(session: u32) -> Vec<u8> {
    let h = Header { flags: FLAG_END, frag_index: 0, frag_count: 0, payload_len: 0, session, frame_id: 0, frame_size: 0 };
    let mut d = vec![0u8; HEADER_SIZE];
    h.write(&mut d);
    d
}

/// Fragments the driver uses for an access unit of `size` bytes.
pub fn fragment_count(size: u32) -> u32 {
    if size == 0 {
        return 0;
    }
    let n = (size as usize + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    if n > 0xFFFF {
        0
    } else {
        n as u32
    }
}

/// Splits an access unit into datagrams exactly like the driver's
/// preview::SendFrame. Used by the tests and the loss harness.
pub fn packetize(data: &[u8], session: u32, frame_id: u32, keyframe: bool) -> Vec<Vec<u8>> {
    let count = fragment_count(data.len() as u32);
    let mut out = Vec::with_capacity(count as usize);
    for i in 0..count as usize {
        let start = i * MAX_PAYLOAD;
        let end = (start + MAX_PAYLOAD).min(data.len());
        let h = Header {
            flags: if keyframe { FLAG_KEYFRAME } else { 0 },
            frag_index: i as u16,
            frag_count: count as u16,
            payload_len: (end - start) as u16,
            session,
            frame_id,
            frame_size: data.len() as u32,
        };
        let mut d = vec![0u8; HEADER_SIZE + end - start];
        h.write(&mut d);
        d[HEADER_SIZE..].copy_from_slice(&data[start..end]);
        out.push(d);
    }
    out
}

/// True when the access unit carries an SPS NAL (type 7). The decoder cannot
/// start, or restart after a gap, without one.
pub fn has_sps(data: &[u8]) -> bool {
    let mut i = 0;
    while i + 3 < data.len() {
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            if data[i + 3] & 0x1F == 7 {
                return true;
            }
            i += 3;
        } else {
            i += 1;
        }
    }
    false
}

/// A complete access unit.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub session: u32,
    pub id: u32,
    pub keyframe: bool,
    pub data: Vec<u8>,
}

/// What one datagram produced.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// Every fragment arrived; safe to hand to the gate.
    Frame(Frame),
    /// A frame that will never complete (a fragment was lost, or the sender
    /// abandoned it). Its bytes are thrown away.
    Incomplete { session: u32, id: u32, got: u16, needed: u16 },
    /// The driver started a new encoder session (re-init or restart).
    SessionChanged { from: Option<u32>, to: u32 },
    /// The driver said goodbye (STREAM_END). Partials were discarded and the
    /// session forgotten; the next frame starts a fresh session.
    StreamEnd { session: u32 },
    /// Datagram rejected: foreign, garbled, stale, duplicate, or inconsistent.
    Rejected(Reject),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reject {
    BadHeader,
    Stale,
    Inconsistent,
    Control,
}

struct Partial {
    frag_count: u16,
    frame_size: u32,
    keyframe: bool,
    received: Vec<bool>,
    got: u16,
    data: Vec<u8>,
}

/// Datagrams in, complete access units out. Pure logic, no I/O.
#[derive(Default)]
pub struct Reassembler {
    session: Option<u32>,
    partials: BTreeMap<u32, Partial>,
    newest_emitted: Option<u32>,
    /// Session of the last goodbye; the driver sends it twice.
    ended: Option<u32>,
}

impl Reassembler {
    pub fn new() -> Self {
        Self::default()
    }

    /// Current session (None before the first valid datagram).
    pub fn session(&self) -> Option<u32> {
        self.session
    }

    /// Drops every partial frame and forgets the session.
    pub fn reset(&mut self) {
        self.session = None;
        self.partials.clear();
        self.newest_emitted = None;
        self.ended = None;
    }

    /// Feeds one datagram; events are appended to `out` in the order they
    /// must be handled.
    pub fn push(&mut self, datagram: &[u8], out: &mut Vec<Event>) {
        let Some(h) = Header::parse(datagram) else {
            out.push(Event::Rejected(Reject::BadHeader));
            return;
        };
        if h.frag_count == 0 {
            // A goodbye for a session other than the live one is stale; one
            // for the live session (or before any frame) ends the stream.
            let live = self.session.map_or(true, |s| s == h.session);
            if h.is_end() && live && self.ended != Some(h.session) {
                self.discard_all(out);
                self.session = None;
                self.newest_emitted = None;
                self.ended = Some(h.session);
                out.push(Event::StreamEnd { session: h.session });
            } else {
                out.push(Event::Rejected(Reject::Control));
            }
            return;
        }
        self.ended = None;
        // Geometry first: a datagram that cannot belong to any frame must not
        // be allowed to switch the session.
        if h.frame_size == 0
            || h.frame_size > MAX_FRAME_SIZE
            || fragment_count(h.frame_size) != h.frag_count as u32
        {
            out.push(Event::Rejected(Reject::Inconsistent));
            return;
        }
        let offset = h.frag_index as usize * MAX_PAYLOAD;
        let expected_len = (h.frame_size as usize - offset).min(MAX_PAYLOAD);
        if h.payload_len as usize != expected_len {
            out.push(Event::Rejected(Reject::Inconsistent));
            return;
        }
        if self.session != Some(h.session) {
            self.discard_all(out);
            out.push(Event::SessionChanged { from: self.session, to: h.session });
            self.session = Some(h.session);
            self.newest_emitted = None;
        }
        if let Some(newest) = self.newest_emitted {
            if h.frame_id <= newest {
                out.push(Event::Rejected(Reject::Stale));
                return;
            }
        }
        if let Some(p) = self.partials.get(&h.frame_id) {
            if p.frag_count != h.frag_count || p.frame_size != h.frame_size {
                let p = self.partials.remove(&h.frame_id).expect("checked above");
                out.push(Event::Incomplete {
                    session: h.session,
                    id: h.frame_id,
                    got: p.got,
                    needed: p.frag_count,
                });
                out.push(Event::Rejected(Reject::Inconsistent));
                return;
            }
        }
        let partial = self.partials.entry(h.frame_id).or_insert_with(|| Partial {
            frag_count: h.frag_count,
            frame_size: h.frame_size,
            keyframe: h.is_keyframe(),
            received: vec![false; h.frag_count as usize],
            got: 0,
            data: vec![0u8; h.frame_size as usize],
        });
        let idx = h.frag_index as usize;
        if !partial.received[idx] {
            partial.data[offset..offset + expected_len]
                .copy_from_slice(&datagram[HEADER_SIZE..HEADER_SIZE + expected_len]);
            partial.received[idx] = true;
            partial.got += 1;
        }
        if partial.got == partial.frag_count {
            let done = self.partials.remove(&h.frame_id).expect("present above");
            // Anything older that is still incomplete can no longer be used:
            // a newer frame finished first.
            let older: Vec<u32> = self.partials.range(..h.frame_id).map(|(id, _)| *id).collect();
            for id in older {
                if let Some(p) = self.partials.remove(&id) {
                    out.push(Event::Incomplete { session: h.session, id, got: p.got, needed: p.frag_count });
                }
            }
            self.newest_emitted = Some(h.frame_id);
            out.push(Event::Frame(Frame {
                session: h.session,
                id: h.frame_id,
                keyframe: done.keyframe,
                data: done.data,
            }));
            return;
        }
        while self.partials.len() > MAX_PARTIALS {
            let oldest = *self.partials.keys().next().expect("non-empty");
            let p = self.partials.remove(&oldest).expect("present");
            out.push(Event::Incomplete { session: h.session, id: oldest, got: p.got, needed: p.frag_count });
        }
    }

    fn discard_all(&mut self, out: &mut Vec<Event>) {
        let session = self.session.unwrap_or(0);
        let ids: Vec<u32> = self.partials.keys().copied().collect();
        for id in ids {
            if let Some(p) = self.partials.remove(&id) {
                out.push(Event::Incomplete { session, id, got: p.got, needed: p.frag_count });
            }
        }
    }
}

/// Why the gate held a frame back.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Hold {
    /// Waiting for a keyframe with SPS (stream start, after a gap, new session).
    WaitingForKeyframe,
}

/// Gate decision for one complete frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Decision {
    pub deliver: bool,
    pub hold: Option<Hold>,
    /// Set when this frame revealed missing frame ids: (first missing, this id).
    pub gap: Option<(u32, u32)>,
    /// The caller should ask the driver for an IDR (it applies the throttle).
    pub request_keyframe: bool,
}

/// Decides which complete frames the decoder may see. Pure logic, no I/O.
pub struct DecodeGate {
    need_keyframe: bool,
    session: Option<u32>,
    next_id: u32,
}

impl Default for DecodeGate {
    fn default() -> Self {
        Self { need_keyframe: true, session: None, next_id: 0 }
    }
}

impl DecodeGate {
    pub fn new() -> Self {
        Self::default()
    }

    /// Forces re-acquisition: nothing passes until the next clean keyframe.
    /// Called when a delivered frame never reached the decoder, or the
    /// decoder was replaced.
    pub fn resync(&mut self) {
        self.need_keyframe = true;
    }

    pub fn waiting_for_keyframe(&self) -> bool {
        self.need_keyframe
    }

    pub fn offer(&mut self, f: &Frame) -> Decision {
        let mut d = Decision { deliver: false, hold: None, gap: None, request_keyframe: false };
        if self.session != Some(f.session) {
            self.session = Some(f.session);
            self.need_keyframe = true;
        } else if !self.need_keyframe && f.id != self.next_id {
            d.gap = Some((self.next_id, f.id));
            self.need_keyframe = true;
        }
        if self.need_keyframe {
            if f.keyframe && has_sps(&f.data) {
                self.need_keyframe = false;
            } else {
                d.hold = Some(Hold::WaitingForKeyframe);
                d.request_keyframe = true;
                return d;
            }
        }
        self.next_id = f.id.wrapping_add(1);
        d.deliver = true;
        d
    }
}

/// Counters for the preview path, surfaced in GET /preview and the log.
#[derive(Debug, Clone, Default, serde::Serialize)]
pub struct PreviewStats {
    pub recv_buffer_bytes: i64,
    pub datagrams: u64,
    pub rejected_datagrams: u64,
    pub sessions: u64,
    pub session: u32,
    pub last_frame_id: u32,
    pub frames_complete: u64,
    pub keyframes_complete: u64,
    pub frames_incomplete: u64,
    pub frames_delivered: u64,
    pub frames_held_for_keyframe: u64,
    pub gaps: u64,
    pub decoder_queue_overflows: u64,
    pub keyframe_requests: u64,
    pub decoder_errors: u64,
}

pub type SharedPreviewStats = Arc<Mutex<PreviewStats>>;

/// Enlarges the socket receive buffer and returns the size the OS reports
/// back (Linux caps the request at net.core.rmem_max). Platform-guarded raw
/// setsockopt/getsockopt; std links the socket library on every target.
pub fn tune_recv_buffer(sock: &UdpSocket) -> i64 {
    sys::set_recv_buffer(sock, RECV_BUFFER_BYTES);
    sys::recv_buffer(sock).map(|v| v as i64).unwrap_or(-1)
}

#[cfg(unix)]
mod sys {
    use std::net::UdpSocket;
    use std::os::unix::io::AsRawFd;
    // Linux/Android use the generic socket constants; the BSD family (macOS)
    // uses 0xffff/0x1002.
    #[cfg(any(target_os = "linux", target_os = "android"))]
    const SOL_SOCKET: i32 = 1;
    #[cfg(any(target_os = "linux", target_os = "android"))]
    const SO_RCVBUF: i32 = 8;
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    const SOL_SOCKET: i32 = 0xffff;
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    const SO_RCVBUF: i32 = 0x1002;
    extern "C" {
        fn setsockopt(fd: i32, level: i32, name: i32, value: *const std::ffi::c_void, len: u32) -> i32;
        fn getsockopt(fd: i32, level: i32, name: i32, value: *mut std::ffi::c_void, len: *mut u32) -> i32;
    }
    pub fn set_recv_buffer(sock: &UdpSocket, bytes: i32) -> bool {
        let value = bytes;
        // SAFETY: valid fd for the socket's lifetime; value/len describe an i32.
        unsafe {
            setsockopt(
                sock.as_raw_fd(),
                SOL_SOCKET,
                SO_RCVBUF,
                &value as *const i32 as *const std::ffi::c_void,
                std::mem::size_of::<i32>() as u32,
            ) == 0
        }
    }
    pub fn recv_buffer(sock: &UdpSocket) -> Option<i32> {
        let mut value: i32 = 0;
        let mut len = std::mem::size_of::<i32>() as u32;
        // SAFETY: as above; the kernel writes at most `len` bytes into value.
        let rc = unsafe {
            getsockopt(
                sock.as_raw_fd(),
                SOL_SOCKET,
                SO_RCVBUF,
                &mut value as *mut i32 as *mut std::ffi::c_void,
                &mut len,
            )
        };
        if rc == 0 {
            Some(value)
        } else {
            None
        }
    }
}

#[cfg(windows)]
mod sys {
    use std::net::UdpSocket;
    use std::os::windows::io::AsRawSocket;
    const SOL_SOCKET: i32 = 0xffff;
    const SO_RCVBUF: i32 = 0x1002;
    #[link(name = "ws2_32")]
    extern "system" {
        fn setsockopt(s: usize, level: i32, name: i32, value: *const u8, len: i32) -> i32;
        fn getsockopt(s: usize, level: i32, name: i32, value: *mut u8, len: *mut i32) -> i32;
    }
    pub fn set_recv_buffer(sock: &UdpSocket, bytes: i32) -> bool {
        let value = bytes;
        // SAFETY: valid SOCKET for the socket's lifetime; value/len describe an i32.
        unsafe {
            setsockopt(
                sock.as_raw_socket() as usize,
                SOL_SOCKET,
                SO_RCVBUF,
                &value as *const i32 as *const u8,
                std::mem::size_of::<i32>() as i32,
            ) == 0
        }
    }
    pub fn recv_buffer(sock: &UdpSocket) -> Option<i32> {
        let mut value: i32 = 0;
        let mut len = std::mem::size_of::<i32>() as i32;
        // SAFETY: as above; Winsock writes at most `len` bytes into value.
        let rc = unsafe {
            getsockopt(
                sock.as_raw_socket() as usize,
                SOL_SOCKET,
                SO_RCVBUF,
                &mut value as *mut i32 as *mut u8,
                &mut len,
            )
        };
        if rc == 0 {
            Some(value)
        } else {
            None
        }
    }
}

#[cfg(not(any(unix, windows)))]
mod sys {
    use std::net::UdpSocket;
    pub fn set_recv_buffer(_sock: &UdpSocket, _bytes: i32) -> bool {
        false
    }
    pub fn recv_buffer(_sock: &UdpSocket) -> Option<i32> {
        None
    }
}

/// Per-frame trace to stderr when CARDBOARD_PREVIEW_TRACE=1 (frame ids, sizes,
/// keyframe flags, gate decisions) for correlating artifacts with the stream.
fn trace_enabled() -> bool {
    std::env::var("CARDBOARD_PREVIEW_TRACE").map(|v| v == "1").unwrap_or(false)
}

/// Latest decoded picture for the hub, tagged with the decoder generation
/// allowed to publish. Teardown bumps the generation and clears the frame
/// under this lock, so a picture still in flight from a killed decoder can
/// never reappear after the viewer went back to idle.
#[derive(Default)]
pub struct FrameSlot {
    generation: u64,
    frame: Option<(u32, u32, Vec<u8>)>,
    cleared: bool,
}

impl FrameSlot {
    /// Hub side: (viewer must reset to the idle screen, newest picture).
    /// Both can be set when a new stream published right after a clear.
    pub fn take(&mut self) -> (bool, Option<(u32, u32, Vec<u8>)>) {
        (std::mem::take(&mut self.cleared), self.frame.take())
    }

    /// Drops the published picture and stops every decoder from publishing.
    pub fn clear(&mut self) {
        self.generation = 0;
        self.frame = None;
        self.cleared = true;
    }

    /// Lets `generation` publish. A stale decoder's pictures are dropped.
    fn arm(&mut self, generation: u64) {
        self.generation = generation;
    }

    /// Publishes one RGBA picture when `generation` is current. The caller's
    /// buffer is swapped with the slot's previous one to avoid allocating per
    /// frame. Returns false for a stale decoder.
    fn publish(&mut self, generation: u64, w: u32, h: u32, rgba: &mut Vec<u8>) -> bool {
        if generation == 0 || self.generation != generation {
            return false;
        }
        match self.frame.as_mut() {
            Some((fw, fh, old)) => {
                *fw = w;
                *fh = h;
                std::mem::swap(rgba, old);
            }
            None => self.frame = Some((w, h, std::mem::take(rgba))),
        }
        true
    }
}

pub type SharedFrameSlot = Arc<Mutex<FrameSlot>>;

/// Rare events the receive and decoder threads report to the supervisor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinkEvent {
    /// STREAM_END from the driver.
    Goodbye { session: u32 },
    /// The decoder of `generation` stopped producing output (ffmpeg exited).
    DecoderExited { generation: u64 },
}

/// The current decoder's input queue.
struct DecoderInput {
    generation: u64,
    tx: SyncSender<Vec<u8>>,
}

/// State shared by the receive thread, the decoder threads and the
/// supervisor. Per-datagram facts are counters the supervisor polls; only
/// rare events go through the LinkEvent channel.
#[derive(Default)]
pub struct Link {
    /// Valid stream datagrams (frame fragments) received.
    traffic: AtomicU64,
    /// Pictures published by the current decoder.
    pictures: AtomicU64,
    /// Generation of the decoder frames go to; 0 = none.
    generation: AtomicU64,
    decoder: Mutex<Option<DecoderInput>>,
    stop: AtomicBool,
}

impl Link {
    pub fn traffic(&self) -> u64 {
        self.traffic.load(Ordering::Relaxed)
    }

    pub fn pictures(&self) -> u64 {
        self.pictures.load(Ordering::Relaxed)
    }

    /// Ends the receive thread within RECV_POLL (bridge shutdown).
    pub fn stop(&self) {
        self.stop.store(true, Ordering::Relaxed);
    }

    pub fn stop_requested(&self) -> bool {
        self.stop.load(Ordering::Relaxed)
    }

    fn install(&self, generation: u64, tx: SyncSender<Vec<u8>>) {
        if let Ok(mut d) = self.decoder.lock() {
            *d = Some(DecoderInput { generation, tx });
        }
        self.generation.store(generation, Ordering::Relaxed);
    }

    fn detach(&self) {
        self.generation.store(0, Ordering::Relaxed);
        if let Ok(mut d) = self.decoder.lock() {
            d.take();
        }
    }

    /// Queue of `generation`, if it is still the current decoder.
    fn input_for(&self, generation: u64) -> Option<SyncSender<Vec<u8>>> {
        if generation == 0 {
            return None;
        }
        let d = self.decoder.lock().ok()?;
        d.as_ref().filter(|d| d.generation == generation).map(|d| d.tx.clone())
    }
}

/// One running ffmpeg instance. Owned by the supervisor; `stop_decoder`
/// flushes it by killing the process.
pub struct Decoder {
    child: Child,
    generation: u64,
}

impl Decoder {
    pub fn generation(&self) -> u64 {
        self.generation
    }
}

/// Starts the receive thread for the app's lifetime. It owns UDP 42069,
/// reassembles and gates frames, and feeds whichever decoder is installed in
/// `link`; with none installed, frames are dropped and the gate waits for a
/// keyframe.
pub fn spawn_receiver(
    socket: UdpSocket,
    link: Arc<Link>,
    events: Sender<LinkEvent>,
    state: SharedState,
    stats: SharedPreviewStats,
) {
    thread::spawn(move || receive_loop(socket, &link, &events, &state, &stats));
}

/// Spawns ffmpeg for `generation`, its writer/stdout/stderr threads, arms
/// the frame slot and installs the decoder as the receive thread's target.
pub fn start_decoder(
    generation: u64,
    link: &Arc<Link>,
    slot: &SharedFrameSlot,
    events: &Sender<LinkEvent>,
    state: &SharedState,
    stats: &SharedPreviewStats,
    (frame_w, frame_h): (u32, u32),
) -> std::io::Result<Decoder> {
    let vf = format!("scale={frame_w}:{frame_h}");
    let mut child = Command::new("ffmpeg")
        .args([
            // low_delay: emit each picture as soon as it decodes. Not
            // -fflags nobuffer: it discards the packets read while probing,
            // i.e. the first keyframe and its GOP.
            "-flags", "low_delay",
            "-f", "h264",
            "-probesize", "32768",
            "-analyzeduration", "0",
            "-i", "pipe:0",
            "-vf",
        ])
        .arg(&vf)
        .args(["-f", "rawvideo", "-pix_fmt", "rgba", "-v", "error", "pipe:1"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    let (Some(stdin), Some(stdout), Some(stderr)) = (child.stdin.take(), child.stdout.take(), child.stderr.take())
    else {
        let _ = child.kill();
        let _ = child.wait();
        return Err(std::io::Error::new(std::io::ErrorKind::Other, "ffmpeg pipes unavailable"));
    };
    let (tx, rx) = mpsc::sync_channel::<Vec<u8>>(DECODER_QUEUE_DEPTH);
    thread::spawn(move || decoder_writer_loop(rx, stdin));
    {
        let state = state.clone();
        let stats = stats.clone();
        thread::spawn(move || decoder_stderr_loop(stderr, &state, &stats));
    }
    {
        let link = link.clone();
        let slot = slot.clone();
        let events = events.clone();
        let frame_bytes = (frame_w * frame_h * 4) as usize;
        thread::spawn(move || {
            decoder_stdout_loop(stdout, generation, frame_w, frame_h, frame_bytes, &slot, &link);
            let _ = events.send(LinkEvent::DecoderExited { generation });
        });
    }
    if let Ok(mut s) = slot.lock() {
        s.arm(generation);
    }
    link.install(generation, tx);
    Ok(Decoder { child, generation })
}

/// Flushes a decoder: the receive thread stops feeding it, nothing it still
/// outputs is published, the shown picture is dropped (viewer goes idle) and
/// the process is killed and reaped, discarding its references and queue.
pub fn stop_decoder(decoder: Option<Decoder>, link: &Link, slot: &SharedFrameSlot) {
    link.detach();
    if let Ok(mut s) = slot.lock() {
        s.clear();
    }
    if let Some(mut d) = decoder {
        let _ = d.child.kill();
        let _ = d.child.wait();
    }
}

fn decoder_writer_loop(rx: Receiver<Vec<u8>>, mut stdin: ChildStdin) {
    while let Ok(frame) = rx.recv() {
        if stdin.write_all(&frame).is_err() || stdin.flush().is_err() {
            return;
        }
    }
}

fn decoder_stdout_loop(
    mut stdout: ChildStdout,
    generation: u64,
    frame_w: u32,
    frame_h: u32,
    frame_bytes: usize,
    slot: &SharedFrameSlot,
    link: &Link,
) {
    let mut rgba = vec![0u8; frame_bytes];
    let mut off = 0usize;
    loop {
        if rgba.len() != frame_bytes {
            rgba.resize(frame_bytes, 0);
        }
        match stdout.read(&mut rgba[off..]) {
            Ok(0) | Err(_) => return,
            Ok(n) => {
                off += n;
                if off < frame_bytes {
                    continue;
                }
                off = 0;
                let published = slot.lock().map(|mut s| s.publish(generation, frame_w, frame_h, &mut rgba)).unwrap_or(false);
                if published {
                    link.pictures.fetch_add(1, Ordering::Relaxed);
                }
            }
        }
    }
}

fn decoder_stderr_loop(stderr: ChildStderr, state: &SharedState, stats: &SharedPreviewStats) {
    let mut last_log: Option<Instant> = None;
    let mut suppressed = 0u64;
    for line in BufReader::new(stderr).lines() {
        let Ok(line) = line else { return };
        if let Ok(mut s) = stats.lock() {
            s.decoder_errors += 1;
        }
        if trace_enabled() {
            eprintln!("[preview] ffmpeg: {line}");
        }
        let due = last_log.map(|t| t.elapsed() >= LOSS_LOG_INTERVAL).unwrap_or(true);
        if due {
            let extra = if suppressed > 0 { format!(" (+{suppressed} more)") } else { String::new() };
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("preview decoder: {}{extra}", line.trim()));
            }
            last_log = Some(Instant::now());
            suppressed = 0;
        } else {
            suppressed += 1;
        }
    }
}

/// Mutable bookkeeping for the receive loop.
struct Loop {
    reassembler: Reassembler,
    gate: DecodeGate,
    /// Decoder generation `tx` belongs to; 0 = none.
    fed_generation: u64,
    tx: Option<SyncSender<Vec<u8>>>,
    last_keyframe_req: Option<Instant>,
    last_loss_log: Option<Instant>,
    last_summary: Instant,
    trace: bool,
}

impl Loop {
    /// Follows decoder swaps. A new decoder has no SPS and no references, so
    /// the gate re-acquires on a keyframe; dropping the old queue handle lets
    /// the old writer thread end even when no frame arrives.
    fn sync_decoder(&mut self, link: &Link) {
        let current = link.generation.load(Ordering::Relaxed);
        if current != self.fed_generation {
            self.fed_generation = current;
            self.tx = link.input_for(current);
            self.gate.resync();
        }
    }
}

fn receive_loop(
    socket: UdpSocket,
    link: &Link,
    events: &Sender<LinkEvent>,
    state: &SharedState,
    stats: &SharedPreviewStats,
) {
    let _ = socket.set_nonblocking(false);
    let _ = socket.set_read_timeout(Some(RECV_POLL));
    let mut buf = vec![0u8; 65536];
    let mut out = Vec::new();
    let mut lp = Loop {
        reassembler: Reassembler::new(),
        gate: DecodeGate::new(),
        fed_generation: 0,
        tx: None,
        last_keyframe_req: None,
        last_loss_log: None,
        last_summary: Instant::now(),
        trace: trace_enabled(),
    };
    while !link.stop.load(Ordering::Relaxed) {
        lp.sync_decoder(link);
        match socket.recv(&mut buf) {
            Ok(n) => {
                if let Ok(mut s) = stats.lock() {
                    s.datagrams += 1;
                }
                if Header::parse(&buf[..n]).map_or(false, |h| h.frag_count > 0) {
                    link.traffic.fetch_add(1, Ordering::Relaxed);
                }
                out.clear();
                lp.reassembler.push(&buf[..n], &mut out);
                for ev in out.drain(..) {
                    handle_event(ev, &mut lp, link, events, state, stats);
                }
            }
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut => {}
            // Windows reports ICMP port-unreachable on UDP sockets as
            // ConnectionReset; it carries no data and is not fatal.
            Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset => {}
            Err(_) => thread::sleep(RECV_POLL),
        }
        if lp.last_summary.elapsed() >= SUMMARY_INTERVAL {
            lp.last_summary = Instant::now();
            if let Ok(s) = stats.lock() {
                crate::debug_log!(
                    state,
                    "[preview] session {:08X} frame {}: complete {} (key {}), delivered {}, incomplete {}, held {}, gaps {}, queue overflows {}, IDR requests {}, decoder errors {}",
                    s.session,
                    s.last_frame_id,
                    s.frames_complete,
                    s.keyframes_complete,
                    s.frames_delivered,
                    s.frames_incomplete,
                    s.frames_held_for_keyframe,
                    s.gaps,
                    s.decoder_queue_overflows,
                    s.keyframe_requests,
                    s.decoder_errors
                );
            }
        }
    }
}

/// Applies one reassembler event.
fn handle_event(
    ev: Event,
    lp: &mut Loop,
    link: &Link,
    events: &Sender<LinkEvent>,
    state: &SharedState,
    stats: &SharedPreviewStats,
) {
    match ev {
        Event::Rejected(reason) => {
            if let Ok(mut s) = stats.lock() {
                s.rejected_datagrams += 1;
            }
            if reason == Reject::BadHeader {
                // Most likely a driver from before the framed preview, still
                // sending raw Annex-B; nothing it sends can be decoded safely.
                log_loss(lp, state, "preview: datagram without a CBPV header ignored (driver older than the bridge? update both)".into());
            }
        }
        Event::StreamEnd { session } => {
            lp.gate.resync();
            if let Ok(mut s) = state.lock() {
                s.push_log(format!("preview: driver ended session {session:08X}"));
            }
            let _ = events.send(LinkEvent::Goodbye { session });
        }
        Event::SessionChanged { from, to } => {
            if let Ok(mut s) = stats.lock() {
                s.sessions += 1;
                s.session = to;
            }
            if let Ok(mut s) = state.lock() {
                match from {
                    Some(old) => s.push_log(format!("preview: driver session {old:08X} -> {to:08X}")),
                    None => s.push_log(format!("preview: driver session {to:08X}")),
                }
            }
        }
        Event::Incomplete { session, id, got, needed } => {
            if let Ok(mut s) = stats.lock() {
                s.frames_incomplete += 1;
            }
            if lp.trace {
                eprintln!("[preview] {session:08X} frame {id} INCOMPLETE {got}/{needed} fragments, discarded");
            }
            log_loss(lp, state, format!("preview: frame {id} lost {}/{needed} fragments, discarded", needed - got));
        }
        Event::Frame(frame) => {
            lp.sync_decoder(link);
            if lp.tx.is_none() {
                // No decoder (teardown or restart in progress): nothing to
                // feed, and the next decoder starts from a keyframe anyway.
                return;
            }
            let decision = lp.gate.offer(&frame);
            if let Ok(mut s) = stats.lock() {
                s.frames_complete += 1;
                if frame.keyframe {
                    s.keyframes_complete += 1;
                }
                s.last_frame_id = frame.id;
                if decision.gap.is_some() {
                    s.gaps += 1;
                }
                if decision.hold.is_some() {
                    s.frames_held_for_keyframe += 1;
                }
            }
            if lp.trace {
                eprintln!(
                    "[preview] {:08X} frame {} size {} key {} -> {}{}",
                    frame.session,
                    frame.id,
                    frame.data.len(),
                    frame.keyframe as u8,
                    if decision.deliver { "decode" } else { "hold (waiting for keyframe)" },
                    decision.gap.map(|(a, b)| format!(" gap {a}..{b}")).unwrap_or_default()
                );
            }
            if let Some((first, at)) = decision.gap {
                log_loss(lp, state, format!("preview: frames {first}..{} missing, holding until keyframe", at.wrapping_sub(1)));
            }
            if decision.request_keyframe {
                request_keyframe(lp, stats);
            }
            if !decision.deliver {
                return;
            }
            let Some(tx) = lp.tx.as_ref() else { return };
            match tx.try_send(frame.data) {
                Ok(()) => {
                    if let Ok(mut s) = stats.lock() {
                        s.frames_delivered += 1;
                    }
                }
                Err(TrySendError::Full(_)) => {
                    // The decoder fell behind: this frame never reaches it,
                    // so everything after it would decode against a hole.
                    lp.gate.resync();
                    if let Ok(mut s) = stats.lock() {
                        s.decoder_queue_overflows += 1;
                    }
                    log_loss(lp, state, format!("preview: decoder queue full at frame {}, holding until keyframe", frame.id));
                    request_keyframe(lp, stats);
                }
                Err(TrySendError::Disconnected(_)) => {
                    // Writer gone (ffmpeg exited); the supervisor sees the
                    // exit and installs a new decoder.
                    lp.tx = None;
                    lp.gate.resync();
                }
            }
        }
    }
}

fn log_loss(lp: &mut Loop, state: &SharedState, line: String) {
    let due = lp.last_loss_log.map(|t| t.elapsed() >= LOSS_LOG_INTERVAL).unwrap_or(true);
    if due {
        lp.last_loss_log = Some(Instant::now());
        if let Ok(mut s) = state.lock() {
            s.push_log(line);
        }
    }
}

fn request_keyframe(lp: &mut Loop, stats: &SharedPreviewStats) {
    let due = lp.last_keyframe_req.map(|t| t.elapsed() >= KEYFRAME_REQ_INTERVAL).unwrap_or(true);
    if !due {
        return;
    }
    lp.last_keyframe_req = Some(Instant::now());
    if crate::net::driver::request_keyframe() {
        if let Ok(mut s) = stats.lock() {
            s.keyframe_requests += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    // Wire + reassembly + gate tests. The golden bytes are shared with the
    // driver's test_wire_protocol.cpp (test_preview_header_golden_bytes).
    use super::*;

    const GOLDEN: [u8; 24] = [
        0x43, 0x42, 0x50, 0x56, 0x01, 0x01, 0x00, 0x01, 0x00, 0x03, 0x05, 0x60, 0x01, 0x02, 0x03,
        0x04, 0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00, 0x0B, 0xB8,
    ];

    fn key_au(len: usize) -> Vec<u8> {
        let mut v = vec![0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0, 0, 0, 1, 0x65];
        while v.len() < len {
            v.push((v.len() % 251) as u8 | 0x10);
        }
        v
    }

    fn p_au(len: usize) -> Vec<u8> {
        let mut v = vec![0, 0, 0, 1, 0x41];
        while v.len() < len {
            v.push((v.len() % 241) as u8 | 0x10);
        }
        v
    }

    fn feed(r: &mut Reassembler, dgs: &[Vec<u8>]) -> Vec<Event> {
        let mut out = Vec::new();
        for d in dgs {
            r.push(d, &mut out);
        }
        out
    }

    fn frames(evs: &[Event]) -> Vec<u32> {
        evs.iter()
            .filter_map(|e| if let Event::Frame(f) = e { Some(f.id) } else { None })
            .collect()
    }

    #[test]
    fn header_matches_driver_golden_bytes() {
        let h = Header {
            flags: FLAG_KEYFRAME,
            frag_index: 1,
            frag_count: 3,
            payload_len: 1376,
            session: 0x0102_0304,
            frame_id: 0x0A0B_0C0D,
            frame_size: 3000,
        };
        let mut out = [0u8; 24];
        h.write(&mut out);
        assert_eq!(out, GOLDEN);
        assert_eq!(MAX_PAYLOAD, 1376);
    }

    #[test]
    fn complete_frames_reassemble_byte_exact() {
        let mut r = Reassembler::new();
        for (id, size) in [1usize, 1375, 1376, 1377, 235_000].iter().enumerate() {
            let au = key_au(*size.max(&20));
            let evs = feed(&mut r, &packetize(&au, 9, id as u32, true));
            let got: Vec<&Frame> = evs.iter().filter_map(|e| if let Event::Frame(f) = e { Some(f) } else { None }).collect();
            assert_eq!(got.len(), 1);
            assert_eq!(got[0].data, au);
            assert!(got[0].keyframe);
        }
    }

    #[test]
    fn a_frame_missing_one_fragment_is_never_emitted() {
        let mut r = Reassembler::new();
        let mut dgs = packetize(&key_au(10_000), 1, 0, true);
        dgs.remove(3);
        let mut evs = feed(&mut r, &dgs);
        assert!(frames(&evs).is_empty());
        evs = feed(&mut r, &packetize(&p_au(3000), 1, 1, false));
        assert_eq!(frames(&evs), vec![1]);
        assert!(evs.iter().any(|e| matches!(e, Event::Incomplete { id: 0, needed: 8, got: 7, .. })));
    }

    #[test]
    fn duplicates_and_stale_datagrams_are_rejected() {
        let mut r = Reassembler::new();
        let dgs = packetize(&p_au(3000), 1, 5, false);
        assert_eq!(frames(&feed(&mut r, &dgs)), vec![5]);
        let evs = feed(&mut r, &dgs);
        assert!(frames(&evs).is_empty());
        assert!(evs.iter().all(|e| matches!(e, Event::Rejected(Reject::Stale))));
    }

    #[test]
    fn interleaved_fragments_still_reassemble() {
        let mut r = Reassembler::new();
        let a = packetize(&p_au(4000), 1, 0, false);
        let b = packetize(&p_au(4000), 1, 1, false);
        let mut mixed = Vec::new();
        for i in 0..a.len() {
            mixed.push(a[i].clone());
            mixed.push(b[i].clone());
        }
        assert_eq!(frames(&feed(&mut r, &mixed)), vec![0, 1]);
    }

    #[test]
    fn garbage_and_inconsistent_headers_are_rejected() {
        let mut r = Reassembler::new();
        let mut evs = Vec::new();
        r.push(&[0u8; 40], &mut evs);
        let mut d = packetize(&p_au(3000), 1, 0, false)[0].clone();
        d[20..24].copy_from_slice(&999_999u32.to_be_bytes()); // frame_size vs frag_count
        r.push(&d, &mut evs);
        assert!(frames(&evs).is_empty());
        assert_eq!(evs.iter().filter(|e| matches!(e, Event::Rejected(_))).count(), 2);
    }

    #[test]
    fn new_session_discards_partials_and_reports_change() {
        let mut r = Reassembler::new();
        let mut first = packetize(&key_au(5000), 1, 0, true);
        first.pop();
        feed(&mut r, &first);
        let evs = feed(&mut r, &packetize(&key_au(5000), 2, 0, true));
        assert!(evs.iter().any(|e| matches!(e, Event::SessionChanged { from: Some(1), to: 2 })));
        assert!(evs.iter().any(|e| matches!(e, Event::Incomplete { session: 1, id: 0, .. })));
        assert_eq!(frames(&evs), vec![0]);
    }

    #[test]
    fn stream_end_matches_driver_golden_bytes() {
        // Same bytes as test_preview_stream_end_golden_bytes in the driver.
        let expected: [u8; 24] = [
            0x43, 0x42, 0x50, 0x56, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
            0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        ];
        assert_eq!(stream_end(0x0102_0304), expected.to_vec());
        assert!(Header::parse(&expected).unwrap().is_end());
    }

    #[test]
    fn stream_end_discards_partials_once_and_next_stream_starts_clean() {
        let mut r = Reassembler::new();
        assert_eq!(frames(&feed(&mut r, &packetize(&key_au(3000), 7, 0, true))), vec![0]);
        let mut partial = packetize(&p_au(5000), 7, 1, false);
        partial.pop();
        feed(&mut r, &partial);
        // Goodbye from some other session is ignored.
        let evs = feed(&mut r, &[stream_end(99)]);
        assert!(matches!(evs[..], [Event::Rejected(Reject::Control)]));
        // Sent twice by the driver: exactly one StreamEnd.
        let evs = feed(&mut r, &[stream_end(7), stream_end(7)]);
        assert!(evs.iter().any(|e| matches!(e, Event::Incomplete { session: 7, id: 1, .. })));
        assert_eq!(evs.iter().filter(|e| matches!(e, Event::StreamEnd { session: 7 })).count(), 1);
        assert_eq!(r.session(), None);
        // A restarted driver may reuse frame ids from 0; they are not stale.
        let evs = feed(&mut r, &packetize(&key_au(3000), 8, 0, true));
        assert!(evs.iter().any(|e| matches!(e, Event::SessionChanged { from: None, to: 8 })));
        assert_eq!(frames(&evs), vec![0]);
    }

    #[test]
    fn keyframe_flag_bits_on_a_fragment_are_not_a_goodbye() {
        let mut d = packetize(&key_au(100), 1, 0, true).remove(0);
        d[5] |= FLAG_END;
        let h = Header::parse(&d).unwrap();
        assert!(!h.is_end());
    }

    fn frame(session: u32, id: u32, key: bool) -> Frame {
        Frame { session, id, keyframe: key, data: if key { key_au(40) } else { p_au(40) } }
    }

    #[test]
    fn gate_holds_everything_until_a_keyframe_with_sps() {
        let mut g = DecodeGate::new();
        let d = g.offer(&frame(1, 0, false));
        assert!(!d.deliver && d.request_keyframe);
        // A keyframe flag without SPS is not enough to start a decoder.
        let no_sps = Frame { session: 1, id: 1, keyframe: true, data: p_au(40) };
        assert!(!g.offer(&no_sps).deliver);
        assert!(g.offer(&frame(1, 2, true)).deliver);
        assert!(g.offer(&frame(1, 3, false)).deliver);
    }

    #[test]
    fn gate_stops_at_a_gap_and_resumes_at_the_next_keyframe() {
        let mut g = DecodeGate::new();
        assert!(g.offer(&frame(1, 0, true)).deliver);
        assert!(g.offer(&frame(1, 1, false)).deliver);
        let d = g.offer(&frame(1, 3, false)); // frame 2 lost
        assert_eq!(d.gap, Some((2, 3)));
        assert!(!d.deliver && d.request_keyframe);
        assert!(!g.offer(&frame(1, 4, false)).deliver);
        assert!(g.offer(&frame(1, 5, true)).deliver);
        assert!(g.offer(&frame(1, 6, false)).deliver);
    }

    #[test]
    fn gate_resync_and_session_change_require_a_keyframe() {
        let mut g = DecodeGate::new();
        assert!(g.offer(&frame(1, 0, true)).deliver);
        g.resync();
        assert!(!g.offer(&frame(1, 1, false)).deliver);
        assert!(g.offer(&frame(1, 2, true)).deliver);
        assert!(!g.offer(&frame(2, 0, false)).deliver);
        assert!(g.offer(&frame(2, 1, true)).deliver);
    }

    #[test]
    fn lossy_stream_never_delivers_a_frame_without_its_references() {
        // 200 frames, GOP 10, every 7th datagram dropped: every delivered
        // frame must be a keyframe or the direct successor of a delivered one.
        let mut r = Reassembler::new();
        let mut g = DecodeGate::new();
        let mut delivered: Vec<u32> = Vec::new();
        let mut n = 0usize;
        for id in 0..200u32 {
            let key = id % 10 == 0;
            let au = if key { key_au(20_000) } else { p_au(4_000) };
            for d in packetize(&au, 1, id, key) {
                n += 1;
                if n % 7 == 0 {
                    continue;
                }
                let mut evs = Vec::new();
                r.push(&d, &mut evs);
                for e in evs {
                    if let Event::Frame(f) = e {
                        if g.offer(&f).deliver {
                            if !f.keyframe {
                                assert_eq!(delivered.last().copied(), Some(f.id - 1), "frame {} decoded without its reference", f.id);
                            }
                            delivered.push(f.id);
                        }
                    }
                }
            }
        }
        assert!(!delivered.is_empty());
    }
}
