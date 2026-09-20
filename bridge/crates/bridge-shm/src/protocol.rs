// Shared-memory layout for the driver -> bridge channel (Local\cardboard_pp_bridge).
// Source of truth for region/slot geometry and message payloads; the driver's
// BridgeProtocol.h mirrors these structs byte-for-byte (natural C alignment).
pub const MAGIC: [u8; 4] = *b"CBPP";
// Bumped whenever the layout below changes; readers reject mismatched regions.
pub const PROTOCOL_VERSION: u32 = 1;
// Fixed-size region header; slot payloads start right after it.
pub const HEADER_SIZE: usize = 128;
pub const MIN_SLOT_SIZE: usize = 32;
// OS object names for the status ring and the bridge -> driver command ring.
pub const NAME_PREFIX: &str = "cardboard_pp_bridge";
pub const CMD_NAME_PREFIX: &str = "cardboard_pp_bridge_cmd";
pub const CMD_SLOT_SIZE: usize = 256;
pub const CMD_SLOT_COUNT: usize = 8;
pub const CMD_REGION_SIZE: usize = HEADER_SIZE + CMD_SLOT_SIZE * CMD_SLOT_COUNT;
// Slot discriminant carried in each ring slot header (EMPTY = free slot).
#[allow(non_snake_case)]
pub mod MsgType {
    pub const EMPTY: u32 = 0;
    pub const TEXTURE_SET_CREATED: u32 = 1;
    pub const FRAME_SUBMITTED: u32 = 2;
    pub const CAP_REPORTED: u32 = 3;
    pub const POSE: u32 = 4;
    pub const CONTROLLER_INPUT: u32 = 5;
    pub const TELEMETRY: u32 = 6;
    pub const SETTINGS: u32 = 7;
}
pub mod payload {
    /// Posted when the driver creates its shared-texture set; `shared_handle`
    /// is the OS handle the bridge imports to observe frames.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct TextureSetCreated {
        pub pid: u32,
        pub width: u32,
        pub height: u32,
        pub format: u32,
        pub flags: u32,
        pub pad1: u32,
        pub shared_handle: u64,
    }
    /// Posted per presented stereo frame; handles index into the texture set
    /// from TextureSetCreated, `pts`/`frame_index` order the stream.
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
    /// Phone-reported decode capability forwarded by the driver so the bridge
    /// can clamp the encode resolution before streaming starts.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct CapReported {
        pub width: u32,
        pub height: u32,
        pub pad1: u32,
        pub pad2: u32,
    }
    /// Latest head pose: position/rotation plus linear/angular velocity for
    /// prediction. `rot` is a [w, x, y, z] quaternion, timestamp in nanos.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct Pose {
        pub pos: [f32; 3],
        pub vel: [f32; 3],
        pub accel: [f32; 3],
        pub rot: [f32; 4],
        pub ang_vel: [f32; 3],
        pub ang_accel: [f32; 3],
        pub timestamp_ns: i64,
    }
    /// Hand/controller sample for one device: analog axes plus button bitmask.
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq)]
    pub struct ControllerInput {
        pub device: u32,
        pub axis: [f32; 4],
        pub buttons: u64,
        pub timestamp_ns: i64,
    }
    /// Encoder health snapshot powering the bridge diagnostics tab and the
    /// adaptive-bitrate decision (encode latency, pacing, duplicate frames).
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
    /// Bridge -> driver settings push over the command ring; `seq` lets the
    /// driver drop stale writes (latest-wins) and `encoder` selects the codec.
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
pub const MAX_PAYLOAD: usize = 240;
pub const DEFAULT_SLOT_SIZE: usize = 256;
pub const DEFAULT_SLOT_COUNT: usize = 64;
pub const DEFAULT_REGION_SIZE: usize = HEADER_SIZE + DEFAULT_SLOT_SIZE * DEFAULT_SLOT_COUNT;
/// Decoded view of one ring slot; Unknown carries the raw discriminant so new
/// message types fail visibly instead of mis-parsing.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BridgeMessage {
    TextureSetCreated(payload::TextureSetCreated),
    FrameSubmitted(payload::FrameSubmitted),
    CapReported(payload::CapReported),
    Pose(payload::Pose),
    ControllerInput(payload::ControllerInput),
    Telemetry(payload::Telemetry),
    Unknown { msg_type: u32, len: u32 },
}
/// Byte offset of the slot holding `write_seq` in a ring of `slot_count`
/// slots of `slot_size` bytes. Wraps modulo the count (latest-wins overwrite).
pub fn slot_offset(write_seq: u64, slot_count: u32, slot_size: u32) -> usize {
    let idx = (write_seq % slot_count as u64) as usize;
    HEADER_SIZE + idx * slot_size as usize
}
