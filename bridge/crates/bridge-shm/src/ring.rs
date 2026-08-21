//! Ring-buffer consumer.
//!
//! Single-producer / single-consumer **latest-wins** ring over the shared
//! region:
//!
//! - The producer owns `header.write_seq` and bumps it (release) after the slot
//!   at `write_seq % slot_count` has been fully written.
//! - The consumer tracks its own `next` cursor. Normal path: FIFO, one message
//!   per `try_consume()`.
//! - If the producer overwrote a slot the consumer has not yet read (a whole
//!   ring's worth of messages behind), the consumer drops everything up to the
//!   newest write and counts the drop. We prefer newest data over replaying
//!   stale frames — a slow consumer never blocks the driver's Present path.
//!
//! `read_seq` / `dropped` in the header are informational (written back on
//! drop); correctness only requires the producer's `write_seq` + the `slot_seq`
//! tag in front of each slot.

use crate::mem::{MemResult, SharedMemory, cmd_region_name, region_name};
use crate::protocol::{
    BridgeMessage, MsgType, slot_offset, CMD_SLOT_COUNT, CMD_SLOT_SIZE, HEADER_SIZE, MAGIC,
    MIN_SLOT_SIZE, PROTOCOL_VERSION,
};

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct RegionHeader {
    magic: [u8; 4],
    version: u32,
    header_size: u32,
    slot_size: u32,
    slot_count: u32,
    flags: u32,
    write_seq: u64,
    read_seq: u64,
    dropped: u64,
    _pad: [u8; 80],
}
const _: () = assert!(std::mem::size_of::<RegionHeader>() == HEADER_SIZE);

/// The consumer of the shared ring.
pub struct BridgeConsumer {
    _mem: SharedMemory,
    base: *mut u8,
    slot_size: u32,
    slot_count: u32,
    next: u64,
    dropped: u64,
}

// SAFETY: BridgeConsumer is used from exactly one thread (its own loop thread).
unsafe impl Send for BridgeConsumer {}

impl Drop for BridgeConsumer {
    fn drop(&mut self) {
        // Publish read_seq + dropped back to the header (informational).
        unsafe {
            let header = &mut *(self.base as *mut RegionHeader);
            std::ptr::write_volatile(&mut header.read_seq, self.next);
            std::ptr::write_volatile(&mut header.dropped, self.dropped);
        }
    }
}

impl BridgeConsumer {
    /// Open the named region a producer already created.
    pub fn open(expected_size: usize) -> MemResult<Self> {
        let mem = SharedMemory::open(&region_name(), expected_size)?;
        let base = mem.base();
        let header = unsafe { &*(base as *const RegionHeader) };
        if &header.magic[..] != &MAGIC[..] {
            return Err(crate::mem::MemError::InvalidState("bad magic"));
        }
        if header.version != PROTOCOL_VERSION {
            return Err(crate::mem::MemError::InvalidState("version mismatch"));
        }
        if header.slot_size < MIN_SLOT_SIZE as u32 || header.slot_count == 0 {
            return Err(crate::mem::MemError::InvalidState("bad layout"));
        }
        let expected_region = HEADER_SIZE + header.slot_size as usize * header.slot_count as usize;
        if expected_size < expected_region {
            return Err(crate::mem::MemError::InvalidState("region smaller than layout"));
        }
        let write_seq = unsafe { std::ptr::read_volatile(&header.write_seq) };
        Ok(Self {
            _mem: mem,
            base,
            slot_size: header.slot_size,
            slot_count: header.slot_count,
            next: write_seq,
            dropped: 0,
        })
    }

    /// Slots skipped because the producer overwrote unread data.
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    /// Current producer write sequence (informational).
    pub fn write_seq(&self) -> u64 {
        unsafe { std::ptr::read_volatile(&(*(self.base as *const RegionHeader)).write_seq) }
    }

    /// How many messages are ready to be consumed right now.
    pub fn pending(&self) -> u64 {
        let ws = self.write_seq();
        ws.saturating_sub(self.next)
    }

    /// Try to consume the next message without blocking.
    pub fn try_consume(&mut self) -> Option<BridgeMessage> {
        let ws = self.write_seq();
        if ws == self.next {
            return None;
        }
        if !self.validate_slot(self.next) {
            // Producer overwrote the slot we expected. Latest-wins: skip to the
            // newest sequence and wait for the next tick.
            self.dropped += ws.saturating_sub(self.next);
            self.next = ws;
            return None;
        }
        let msg = self.decode(self.next);
        self.next += 1;
        msg
    }

    fn slot_ptr(&self, msg_index: u64) -> *const u8 {
        let off = slot_offset(msg_index, self.slot_count, self.slot_size);
        unsafe { (self.base as *const u8).add(off) }
    }

    /// True if the slot still holds the message we expect for `msg_index`.
    fn validate_slot(&self, msg_index: u64) -> bool {
        let slot = unsafe { &*(self.slot_ptr(msg_index) as *const SlotHeader) };
        slot.slot_seq == msg_index && slot.msg_type != MsgType::EMPTY
    }

    fn decode(&self, msg_index: u64) -> Option<BridgeMessage> {
        let slot = unsafe { &*(self.slot_ptr(msg_index) as *const SlotHeader) };
        let payload_len = (self.slot_size as usize).saturating_sub(16).min(slot.payload_len as usize);
        let payload: &[u8] = unsafe {
            std::slice::from_raw_parts(self.slot_ptr(msg_index).add(16), payload_len)
        };
        decode_msg(slot.msg_type, payload)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct SlotHeader {
    slot_seq: u64,
    msg_type: u32,
    payload_len: u32,
}
const _: () = assert!(std::mem::size_of::<SlotHeader>() == 16);

fn decode_msg(msg_type: u32, payload: &[u8]) -> Option<BridgeMessage> {
    use crate::protocol::MsgType as T;
    let m = match msg_type {
        T::TEXTURE_SET_CREATED => {
            BridgeMessage::TextureSetCreated(*bytes_to::<crate::protocol::payload::TextureSetCreated>(payload)?)
        }
        T::FRAME_SUBMITTED => BridgeMessage::FrameSubmitted(*bytes_to::<crate::protocol::payload::FrameSubmitted>(payload)?),
        T::CAP_REPORTED => BridgeMessage::CapReported(*bytes_to::<crate::protocol::payload::CapReported>(payload)?),
        T::POSE => BridgeMessage::Pose(*bytes_to::<crate::protocol::payload::Pose>(payload)?),
        T::CONTROLLER_INPUT => BridgeMessage::ControllerInput(*bytes_to::<crate::protocol::payload::ControllerInput>(payload)?),
        T::TELEMETRY => BridgeMessage::Telemetry(*bytes_to::<crate::protocol::payload::Telemetry>(payload)?),
        _ => BridgeMessage::Unknown {
            msg_type,
            len: payload.len() as u32,
        },
    };
    Some(m)
}

/// Producer side of the **command** region (Bridge → driver settings).
///
/// The driver is the only consumer of this region and opens it read-only from
/// its own cursor (latest-wins, exactly like the Rust `BridgeConsumer`, but with
/// the roles flipped). See `protocol.rs` for the layout rationale.
pub struct CmdProducer {
    _mem: SharedMemory,
    base: *mut u8,
    slot_size: u32,
    slot_count: u32,
    write_seq: u64,
}

// SAFETY: CmdProducer is used from a single thread (the UI worker loop).
unsafe impl Send for CmdProducer {}

impl CmdProducer {
    /// Create or open the command region.
    ///
    /// If an existing region with a matching header is found it is kept as-is
    /// (`write_seq` continues monotonically, exactly like the driver's status
    /// region survives a consumer restart). Otherwise the region is
    /// initialized from scratch.
    pub fn open(expected_size: usize) -> MemResult<Self> {
        let mem = SharedMemory::create(&cmd_region_name(), expected_size, 0)?;
        let base = mem.base();
        let need_init;

        // Read the current header (may be a fresh mapping or a live one).
        let header = unsafe { &*(base as *const RegionHeader) };
        let valid = &header.magic[..] == &MAGIC[..]
            && header.version == PROTOCOL_VERSION
            && header.slot_size >= MIN_SLOT_SIZE as u32
            && header.slot_count != 0
            && header.header_size == HEADER_SIZE as u32;
        let existing = if valid {
            let expected_region = header.header_size as usize
                + header.slot_size as usize * header.slot_count as usize;
            Some(expected_region)
        } else {
            None
        };

        match existing {
            Some(expected_region) if expected_region <= expected_size => {
                // Reuse live region; keep write_seq monotonic.
                need_init = false;
            }
            _ => {
                need_init = true;
            }
        }

        if need_init {
            unsafe {
                // Wipe header + slots so a consumer never sees stale seq/tags.
                std::ptr::write_bytes(base, 0u8, expected_size);
            }
            let h = unsafe { &mut *(base as *mut RegionHeader) };
            h.magic = MAGIC;
            h.version = PROTOCOL_VERSION;
            h.header_size = HEADER_SIZE as u32;
            h.slot_size = CMD_SLOT_SIZE as u32;
            h.slot_count = CMD_SLOT_COUNT as u32;
            h.flags = 0;
            h.write_seq = 0;
        }

        // Release-store the header so consumers see a consistent layout.
        unsafe {
            std::ptr::write_volatile(
                &mut (*(base as *mut RegionHeader)).write_seq,
                if need_init { 0 } else { (*(base as *const RegionHeader)).write_seq },
            );
        }

        let slot_size = CMD_SLOT_SIZE as u32;
        let slot_count = CMD_SLOT_COUNT as u32;
        // Cached cursor; continue from the existing counter on restart.
        let write_seq = unsafe { std::ptr::read_volatile(&(*(base as *const RegionHeader)).write_seq) };

        Ok(Self {
            _mem: mem,
            base,
            slot_size,
            slot_count,
            write_seq,
        })
    }

    /// Latest write sequence published so far (informational).
    pub fn write_seq(&self) -> u64 {
        self.write_seq
    }

    /// Publish one settings change (latest-wins). Returns the new write_seq.
    pub fn publish_settings(&mut self, settings: &crate::protocol::payload::SettingsChange) -> u64 {
        let seq = self.write_seq;
        let slot = unsafe { &mut *(self.slot_ptr(seq) as *mut SlotHeader) };
        unsafe {
            std::ptr::write_bytes(slot as *mut SlotHeader as *mut u8, 0u8, self.slot_size as usize);
        }
        slot.slot_seq = seq;
        slot.msg_type = MsgType::SETTINGS;
        slot.payload_len = crate::protocol::payload::SettingsChange::SIZE as u32;
        unsafe {
            let dst = (self.slot_ptr(seq) as *mut u8).add(std::mem::size_of::<SlotHeader>());
            std::ptr::copy_nonoverlapping(
                settings as *const crate::protocol::payload::SettingsChange as *const u8,
                dst,
                crate::protocol::payload::SettingsChange::SIZE,
            );
        }

        let next = seq + 1;
        // Publish: header write_seq becomes visible only after the slot write.
        unsafe {
            std::ptr::write_volatile(&mut (*(self.base as *mut RegionHeader)).write_seq, next);
        }
        self.write_seq = next;
        next
    }

    fn slot_ptr(&self, msg_index: u64) -> *mut u8 {
        let off = slot_offset(msg_index, self.slot_count, self.slot_size);
        unsafe { (self.base as *mut u8).add(off) }
    }
}

fn bytes_to<T: Copy>(b: &[u8]) -> Option<&T> {
    if b.len() < std::mem::size_of::<T>() {
        return None;
    }
    Some(unsafe { &*(b.as_ptr() as *const T) })
}
