// BridgeProtocol.h
// Shared-memory wire layout between the SteamVR driver (producer of the status
// region) and the Rust bridge (consumer), plus the command region (bridge ->
// driver settings).
//
// MUST stay byte-for-byte identical with bridge/crates/bridge-shm/src/protocol.rs.
// The Rust side is the authoritative copy; change both together.
//
// Status region (named file mapping Local\cardboard_pp_bridge; driver -> bridge):
//
//   +---------------------------+   0
//   | RegionHeader (128 bytes)  |
//   +---------------------------+   128
//   | Slot 0  (slot_size bytes) |
//   +---------------------------+
//   | Slot 1                    |
//   | ...                       |
//
// Command region (Local\cardboard_pp_bridge_cmd; bridge -> driver settings):
// identical header + slot layout, but the producer is the BRIDGE (CmdProducer)
// and the consumer is the driver (it polls from its own cursor). The only
// message type on this region is SETTINGS. See protocol.rs for the rationale.
//
// Producer owns header.write_seq and bumps it (release) after fully writing the
// slot at write_seq % slot_count. Consumer owns its own cursor. Latest-wins
// ring: producer overwrites the oldest in-flight slot when the consumer is slow;
// a slow consumer drops ahead to the newest message rather than blocking.

#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <atomic>
#endif

namespace cbpp {

// ---- RegionHeader (offset 0, 128 bytes) ----
struct RegionHeader {
    uint8_t  magic[4];          // 0x00 "CBPP"
    uint32_t version;           // 0x04 = 1
    uint32_t header_size;       // 0x08 = 128
    uint32_t slot_size;         // 0x0C bytes, >= 32
    uint32_t slot_count;        // 0x10
    uint32_t flags;             // 0x14 reserved, 0
    uint64_t write_seq;         // 0x18 producer publishes
    uint64_t read_seq;          // 0x20 consumer consumes (info)
    uint64_t dropped;           // 0x28 consumer skips (info)
    uint8_t  pad[80];           // 0x30..0x80
};
static_assert(sizeof(RegionHeader) == 128, "RegionHeader must be 128 bytes");

// ---- SlotHeader (16 bytes) at slot base ----
struct SlotHeader {
    uint64_t slot_seq;          // 0x00 == index of the slot
    uint32_t msg_type;          // 0x08
    uint32_t payload_len;       // 0x0C
};
static_assert(sizeof(SlotHeader) == 16, "SlotHeader must be 16 bytes");

// Region constants (must match protocol.rs).
constexpr uint8_t  MAGIC[4]    = { 'C', 'B', 'P', 'P' };
constexpr uint32_t VERSION     = 1;
constexpr uint32_t HEADER_SIZE = 128;
constexpr uint32_t MIN_SLOT_SIZE = 32;
constexpr uint32_t SLOT_SIZE   = 256;
constexpr uint32_t SLOT_COUNT  = 64;
constexpr uint32_t DEFAULT_REGION_SIZE = HEADER_SIZE + SLOT_SIZE * SLOT_COUNT;

// Command region (bridge -> driver) constants (must match protocol.rs).
constexpr uint32_t CMD_SLOT_SIZE  = 256;
constexpr uint32_t CMD_SLOT_COUNT = 8;
constexpr uint32_t CMD_REGION_SIZE = HEADER_SIZE + CMD_SLOT_SIZE * CMD_SLOT_COUNT;

// Message types (must match protocol.rs MsgType).
enum MsgType : uint32_t {
    MT_EMPTY              = 0,
    MT_TEXTURE_SET_CREATED = 1,
    MT_FRAME_SUBMITTED   = 2,
    MT_CAP_REPORTED      = 3,
    MT_POSE              = 4,
    MT_CONTROLLER_INPUT  = 5,
    MT_TELEMETRY         = 6,
    MT_SETTINGS          = 7,   // command region only (bridge -> driver)
};

// ---- Fixed-size payloads (repr(C), NATURAL alignment, little-endian) ----

struct PayloadTextureSetCreated {   // MT_TEXTURE_SET_CREATED, 32 bytes
    uint32_t pid;
    uint32_t width;
    uint32_t height;
    uint32_t format;                // DXGI_FORMAT
    uint32_t flags;
    uint32_t pad1;
    uint64_t shared_handle;
};
static_assert(sizeof(PayloadTextureSetCreated) == 32, "size");

struct PayloadFrameSubmitted {      // MT_FRAME_SUBMITTED, 40 bytes
    uint64_t left_handle;
    uint64_t right_handle;
    int64_t  pts;
    uint64_t frame_index;
    uint32_t format;
    uint32_t pad1;
};
static_assert(sizeof(PayloadFrameSubmitted) == 40, "size");

struct PayloadCapReported {         // MT_CAP_REPORTED, 16 bytes
    uint32_t width;
    uint32_t height;
    uint32_t pad1;
    uint32_t pad2;
};
static_assert(sizeof(PayloadCapReported) == 16, "size");

struct PayloadPose {                // MT_POSE, 88 bytes
    float pos[3];
    float vel[3];
    float accel[3];
    float rot[4];   // w x y z
    float ang_vel[3];
    float ang_accel[3];
    int64_t timestamp_ns;
};
static_assert(sizeof(PayloadPose) == 88, "size");

struct PayloadControllerInput {     // MT_CONTROLLER_INPUT, 40 bytes
    uint32_t device;
    float    axis[4];
    uint64_t buttons;
    int64_t  timestamp_ns;
};
static_assert(sizeof(PayloadControllerInput) == 40, "size");

struct PayloadTelemetry {           // MT_TELEMETRY, 64 bytes
    uint64_t frames;
    uint64_t avg_encode_us;
    uint64_t max_encode_us;
    uint64_t avg_interval_us;
    uint64_t max_interval_us;
    uint64_t dup_count;
    uint64_t summary_frames;
    uint64_t pad;
};
static_assert(sizeof(PayloadTelemetry) == 64, "size");

struct PayloadSettingsChange {      // MT_SETTINGS, 32 bytes (bridge -> driver)
    uint32_t width;                 // 0x00 SBS encoder width
    uint32_t height;                // 0x04 encoder height
    uint32_t fps;                   // 0x08 target frame rate
    uint32_t bitrate_kbps;          // 0x0C bitrate in kbps
    uint32_t encoder;               // 0x10 0 = software (libx264), 1 = GPU
    uint32_t stream_enabled;        // 0x14 0/1 (bridge is the on/off switch)
    uint64_t seq;                   // 0x18 monotonically increasing change id
};
static_assert(sizeof(PayloadSettingsChange) == 32, "size");

inline uint64_t LoadRelaxedU64(const volatile uint64_t& p) {
#ifdef _MSC_VER
    return _ReadWriteBarrier(), p;
#else
    return std::atomic_ref<const uint64_t>(const_cast<uint64_t&>(p)).load(std::memory_order_relaxed);
#endif
}

inline uint64_t LoadAcquireU64(const volatile uint64_t& p) {
#ifdef _MSC_VER
    return _ReadWriteBarrier(), p;
#else
    return std::atomic_ref<const uint64_t>(const_cast<uint64_t&>(p)).load(std::memory_order_acquire);
#endif
}

inline void StoreReleaseU64(volatile uint64_t& p, uint64_t v) {
#ifdef _MSC_VER
    _ReadWriteBarrier();
    p = v;
    _ReadWriteBarrier();
#else
    std::atomic_ref<uint64_t>(reinterpret_cast<uint64_t&>(p)).store(v, std::memory_order_release);
#endif
}

} // namespace cbpp