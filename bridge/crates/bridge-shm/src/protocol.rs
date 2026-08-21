//! Wire protocol shared with the C++ driver.
//!
//! # Layout (authoritative copy: `docs/TRANSPORT.md`)
//!
//! Region layout:
//!
//! ```text
//! +----------------------+  0
//! | RegionHeader (128B)  |
//! +----------------------+  128
//! | Slot 0  (slot_size)  |
//! +----------------------+  ...
//! | Slot 1               |
//! | ...                  |
//! ```
//!
//! Every numeric field is little-endian. Producers may only bump `write_seq`
//! after the whole slot is written (release). Consumers may only reuse a slot
//! after they have advanced their own cursor (acquire happens on the same
//! memory, no locking needed because a slot is either "in-flight" or "owned" by
//! exactly one side between the seq bumps).
//!
//! ## Command region (settings push, Bridge → driver)
//!
//! Because the status region is single-producer (driver), settings are pushed
//! over a **second named region** `Local\cardboard_pp_bridge_cmd` that reuses the
//! exact same header + slot layout but **flips the roles**: the Rust bridge is
//! the producer (its `CmdProducer` bumps `write_seq`) and the driver is the
//! consumer (it polls from its own cursor, latest-wins, same rules). The bridge
//! never reads from this region and the driver never writes to it, so the
//! verified status path stays byte-for-byte untouched.
//!
//! - Message types on the command region: only `SETTINGS` (7) is used today;
//!   the slot/msg_type rules are identical to the status ring.
//! - Producer semantics (match the Rust `CmdProducer`): on a fresh region the
//!   producer initializes the header (magic/version/layout, `write_seq = 0`) and
//!   wipes the slots; on an existing region it keeps `write_seq` monotonic and
//!   does not wipe anything.
//!
//! ## RegionHeader (offset 0, 128 bytes)
//!
//! | offset | type | name |
//! |--------|------|------|
//! | 0x00   | [u8;4] | magic `"CBPP"` |
//! | 0x04   | u32  | version `1` |
//! | 0x08   | u32  | header_size `128` |
//! | 0x0C   | u32  | slot_size (bytes, >= 32) |
//! | 0x10   | u32  | slot_count |
//! | 0x14   | u32  | flags (reserved, 0) |
//! | 0x18   | u64  | write_seq (producer publishes) |
//! | 0x20   | u64  | read_seq (consumer consumes) |
//! | 0x28   | u64  | dropped (consumer increments when skipped) |
//! | 0x30..0x80 | padding, zeroed |
//!
//! Slot at `slot_index`: base = 128 + index * slot_size.
//!
//! | offset | type | name |
//! |--------|------|------|
//! | 0x00   | u64  | slot_seq (== index of the slot) |
//! | 0x08   | u32  | msg_type |
//! | 0x0C   | u32  | payload_len |
//! | 0x10   | u8[] | payload (slot_size - 16) |
//!
//! A `slot_seq != write_seq % slot_count` means the slot belongs to an older
//! turn; the next slot matching the current turn is the newest "one message per
//! turn" — this is a **latest-wins** ring: the producer overwrites the oldest
//! in-flight message when the consumer is slow. `msg_type == 0` means empty.

pub const MAGIC: [u8; 4] = *b"CBPP";
pub const PROTOCOL_VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 128;
pub const MIN_SLOT_SIZE: usize = 32;
pub const NAME_PREFIX: &str = "cardboard_pp_bridge";
/// Command (Bridge → driver) region name.
pub const CMD_NAME_PREFIX: &str = "cardboard_pp_bridge_cmd";
/// Command region uses the same header/slot layout with roles flipped.
pub const CMD_SLOT_SIZE: usize = 256;
pub const CMD_SLOT_COUNT: usize = 8;
pub const CMD_REGION_SIZE: usize = HEADER_SIZE + CMD_SLOT_SIZE * CMD_SLOT_COUNT;

/// Message types. Must match `BridgeProtocol.h` in the driver.
#[allow(non_snake_case)]
pub mod MsgType {
    pub const EMPTY: u32 = 0;
    /// `TextureSetCreated`: a swap texture set was created for a process.
    pub const TEXTURE_SET_CREATED: u32 = 1;
    /// `FrameSubmitted`: a Present happened with per-eye textures.
    pub const FRAME_SUBMITTED: u32 = 2;
    /// `CapReported`: phone reported its hardware decoder cap (forwarded).
    pub const CAP_REPORTED: u32 = 3;
    /// `Pose`: HMD pose sample forwarded from the driver.
    pub const POSE: u32 = 4;
    /// `ControllerInput`: controller axis/button sample forwarded.
    pub const CONTROLLER_INPUT: u32 = 5;
    /// `Telemetry`: driver telemetry summary.
    pub const TELEMETRY: u32 = 6;
    /// `Settings`: Bridge → driver settings change (command region only).
    pub const SETTINGS: u32 = 7;
}

/// Payloads. All `repr(C)`, little-endian, fixed size; extra bytes zero.
pub mod payload {
    /// Msg `TEXTURE_SET_CREATED`. 40 bytes.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct TextureSetCreated {
        pub pid: u32,
        pub width: u32,
        pub height: u32,
        pub format: u32, // DXGI_FORMAT
        pub flags: u32,
        pub pad1: u32,
        pub shared_handle: u64,
    }

    /// Msg `FRAME_SUBMITTED`. 40 bytes.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct FrameSubmitted {
        pub left_handle: u64,
        pub right_handle: u64,
        pub pts: i64,
        pub frame_index: u64,
        pub format: u32,
        pub pad1: u32,
    }

    /// Msg `CAP_REPORTED`. 16 bytes.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct CapReported {
        pub width: u32,
        pub height: u32,
        pub pad1: u32,
        pub pad2: u32,
    }

    /// Msg `POSE`. 72 bytes (DriverPose_t essentials).
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct Pose {
        pub pos: [f32; 3],
        pub vel: [f32; 3],
        pub accel: [f32; 3],
        pub rot: [f32; 4], // w x y z
        pub ang_vel: [f32; 3],
        pub ang_accel: [f32; 3],
        pub timestamp_ns: i64,
    }

    /// Msg `CONTROLLER_INPUT`. 32 bytes.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct ControllerInput {
        pub device: u32,
        pub axis: [f32; 4],
        pub buttons: u64,
        pub timestamp_ns: i64,
    }

    /// Msg `TELEMETRY`. 64 bytes.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct Telemetry {
        pub frames: u64,
        pub avg_encode_us: u64,
        pub max_encode_us: u64,
        pub avg_interval_us: u64,
        pub max_interval_us: u64,
        pub dup_count: u64,
        pub summary_frames: u64,
        pub pad: [u64; 1],
    }

    /// Msg `SETTINGS` (command region, Bridge → driver). 32 bytes.
    ///
    /// `encoder`: 0 = software (libx264), 1 = GPU (NVENC/AMF/QSV).
    /// `stream_enabled`: 0/1 — the Bridge is the single on/off switch.
    /// `seq`: monotonically increasing change id (for driver bookkeeping).
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct SettingsChange {
        pub width: u32,
        pub height: u32,
        pub fps: u32,
        pub bitrate_kbps: u32,
        pub encoder: u32,
        pub stream_enabled: u32,
        pub seq: u64,
    }
}

impl payload::TextureSetCreated {
    pub const SIZE: usize = std::mem::size_of::<payload::TextureSetCreated>();
}
impl payload::FrameSubmitted {
    pub const SIZE: usize = std::mem::size_of::<payload::FrameSubmitted>();
}
impl payload::CapReported {
    pub const SIZE: usize = std::mem::size_of::<payload::CapReported>();
}
impl payload::Pose {
    pub const SIZE: usize = std::mem::size_of::<payload::Pose>();
}
impl payload::ControllerInput {
    pub const SIZE: usize = std::mem::size_of::<payload::ControllerInput>();
}
impl payload::Telemetry {
    pub const SIZE: usize = std::mem::size_of::<payload::Telemetry>();
}
impl payload::SettingsChange {
    pub const SIZE: usize = std::mem::size_of::<payload::SettingsChange>();
}
const _: () = assert!(std::mem::size_of::<payload::SettingsChange>() == 32);

/// The maximum payload a slot can carry. We aim for slots that fit the largest
/// fixed-size message (Telemetry = 64 bytes) plus the 16-byte slot header.
pub const MAX_PAYLOAD: usize = 240;

/// Size of a single slot (matches driver default).
pub const DEFAULT_SLOT_SIZE: usize = 256;
/// Number of slots in a default region.
pub const DEFAULT_SLOT_COUNT: usize = 64;
/// Total region size for the default config.
pub const DEFAULT_REGION_SIZE: usize = HEADER_SIZE + DEFAULT_SLOT_SIZE * DEFAULT_SLOT_COUNT;

/// A parsed message produced by the consumer.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BridgeMessage {
    TextureSetCreated(payload::TextureSetCreated),
    FrameSubmitted(payload::FrameSubmitted),
    CapReported(payload::CapReported),
    Pose(payload::Pose),
    ControllerInput(payload::ControllerInput),
    Telemetry(payload::Telemetry),
    /// An unknown or empty slot (slots with msg_type 0 are skipped before this).
    Unknown { msg_type: u32, len: u32 },
}

/// Cursor to the slot that holds message index `msg_index`.
pub fn slot_offset(write_seq: u64, slot_count: u32, slot_size: u32) -> usize {
    let idx = (write_seq % slot_count as u64) as usize;
    HEADER_SIZE + idx * slot_size as usize
}