use crate::mem::{MemResult, SharedMemory, cmd_region_name, region_name};
use crate::protocol::{
    BridgeMessage, MsgType, slot_offset, CMD_SLOT_COUNT, CMD_SLOT_SIZE, HEADER_SIZE, MAGIC,
    MIN_SLOT_SIZE, PROTOCOL_VERSION,
};
#[repr(C)]
#[derive(Clone, Copy, Debug)]
/// Binary header at offset 0 of each region. Mirrors `protocol.rs` layout; `write_seq`/`read_seq` form the producer→consumer cursor.
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
/// Consumer over the driver→bridge status ring. Opened by `bridge-core/src/shm.rs`; `next` is the next sequence this side expects.
pub struct BridgeConsumer {
    _mem: SharedMemory,
    base: *mut u8,
    slot_size: u32,
    slot_count: u32,
    next: u64,
    dropped: u64,
}
unsafe impl Send for BridgeConsumer {}
impl Drop for BridgeConsumer {
    fn drop(&mut self) {
        unsafe {
            let header = &mut *(self.base as *mut RegionHeader);
            std::ptr::write_volatile(&mut header.read_seq, self.next);
            std::ptr::write_volatile(&mut header.dropped, self.dropped);
        }
    }
}
impl BridgeConsumer {
    /// Opens the driver region and validates magic/version/layout. Starts `next` at the driver's `write_seq` so stale slots are skipped.
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
    /// Returns messages skipped because the consumer fell behind. Read by the diagnostics UI.
    pub fn dropped(&self) -> u64 {
        self.dropped
    }
    /// Reads the driver's live write cursor with a volatile load. Called by `pending`/`try_consume` and the UI poller.
    pub fn write_seq(&self) -> u64 {
        unsafe { std::ptr::read_volatile(&(*(self.base as *const RegionHeader)).write_seq) }
    }
    /// Returns unread message count (`write_seq - next`). Drives the queued-depth readout.
    pub fn pending(&self) -> u64 {
        let ws = self.write_seq();
        ws.saturating_sub(self.next)
    }
    /// Consumes the next message, or `None` when caught up. On overwrite it counts the gap as dropped and resyncs to `write_seq`.
    pub fn try_consume(&mut self) -> Option<BridgeMessage> {
        let ws = self.write_seq();
        if ws == self.next {
            return None;
        }
        if !self.validate_slot(self.next) {
            self.dropped += ws.saturating_sub(self.next);
            self.next = ws;
            return None;
        }
        let msg = self.decode(self.next);
        self.next += 1;
        msg
    }
    /// Maps a sequence number to its slot address via `slot_offset`. Used by `validate_slot` and `decode`.
    fn slot_ptr(&self, msg_index: u64) -> *const u8 {
        let off = slot_offset(msg_index, self.slot_count, self.slot_size);
        unsafe { (self.base as *const u8).add(off) }
    }
    /// Checks that the slot still holds `msg_index` and a real message type. Guards against reading an overwritten slot.
    fn validate_slot(&self, msg_index: u64) -> bool {
        let slot = unsafe { &*(self.slot_ptr(msg_index) as *const SlotHeader) };
        slot.slot_seq == msg_index && slot.msg_type != MsgType::EMPTY
    }
    /// Copies the slot payload into a `BridgeMessage`. Called by `try_consume` after validation.
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
pub struct CmdProducer {
    _mem: SharedMemory,
    base: *mut u8,
    slot_size: u32,
    slot_count: u32,
    write_seq: u64,
}
unsafe impl Send for CmdProducer {}
impl CmdProducer {
    pub fn open(expected_size: usize) -> MemResult<Self> {
        let mem = SharedMemory::create(&cmd_region_name(), expected_size, 0)?;
        let base = mem.base();
        let need_init;
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
                need_init = false;
            }
            _ => {
                need_init = true;
            }
        }
        if need_init {
            unsafe {
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
        unsafe {
            std::ptr::write_volatile(
                &mut (*(base as *mut RegionHeader)).write_seq,
                if need_init { 0 } else { (*(base as *const RegionHeader)).write_seq },
            );
        }
        let slot_size = CMD_SLOT_SIZE as u32;
        let slot_count = CMD_SLOT_COUNT as u32;
        let write_seq = unsafe { std::ptr::read_volatile(&(*(base as *const RegionHeader)).write_seq) };
        Ok(Self {
            _mem: mem,
            base,
            slot_size,
            slot_count,
            write_seq,
        })
    }
    pub fn write_seq(&self) -> u64 {
        self.write_seq
    }
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
