#pragma once
#include <cstdint>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <atomic>
#endif
namespace cbpp {
// 128-byte region prologue; write_seq is the publish cursor the bridge polls.
struct RegionHeader {
    uint8_t  magic[4];
    uint32_t version;
    uint32_t header_size;
    uint32_t slot_size;
    uint32_t slot_count;
    uint32_t flags;
    uint64_t write_seq;
    uint64_t read_seq;
    uint64_t dropped;
    uint8_t  pad[80];
};
static_assert(sizeof(RegionHeader) == 128, "RegionHeader must be 128 bytes");
// 16-byte per-slot prologue; slot_seq orders slots, msg_type selects the payload struct.
struct SlotHeader {
    uint64_t slot_seq;
    uint32_t msg_type;
    uint32_t payload_len;
};
static_assert(sizeof(SlotHeader) == 16, "SlotHeader must be 16 bytes");
// Layout constants: magic/version, region geometry, and command-region sizes shared with the bridge.
constexpr uint8_t  MAGIC[4]    = { 'C', 'B', 'P', 'P' };
constexpr uint32_t VERSION     = 1;
constexpr uint32_t HEADER_SIZE = 128;
constexpr uint32_t MIN_SLOT_SIZE = 32;
constexpr uint32_t SLOT_SIZE   = 256;
constexpr uint32_t SLOT_COUNT  = 64;
constexpr uint32_t DEFAULT_REGION_SIZE = HEADER_SIZE + SLOT_SIZE * SLOT_COUNT;
constexpr uint32_t CMD_SLOT_SIZE  = 256;
constexpr uint32_t CMD_SLOT_COUNT = 8;
constexpr uint32_t CMD_REGION_SIZE = HEADER_SIZE + CMD_SLOT_SIZE * CMD_SLOT_COUNT;
// Message ids carried in SlotHeader.msg_type; telemetry/settings are the paths in active use.
enum MsgType : uint32_t {
    MT_EMPTY              = 0,
    MT_TEXTURE_SET_CREATED = 1,
    MT_FRAME_SUBMITTED   = 2,
    MT_CAP_REPORTED      = 3,
    MT_POSE              = 4,
    MT_CONTROLLER_INPUT  = 5,
    MT_TELEMETRY         = 6,
    MT_SETTINGS          = 7,
};
// Payload structs exchanged in slots; telemetry flows driver->bridge, settings flows bridge->driver.
// Relaxed/acquire-release helpers for the write_seq cursor across the two processes.
struct PayloadTextureSetCreated {
    uint32_t pid;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;
    uint32_t pad1;
    uint64_t shared_handle;
};
static_assert(sizeof(PayloadTextureSetCreated) == 32, "size");
struct PayloadFrameSubmitted {
    uint64_t left_handle;
    uint64_t right_handle;
    int64_t  pts;
    uint64_t frame_index;
    uint32_t format;
    uint32_t pad1;
};
static_assert(sizeof(PayloadFrameSubmitted) == 40, "size");
struct PayloadCapReported {
    uint32_t width;
    uint32_t height;
    uint32_t pad1;
    uint32_t pad2;
};
static_assert(sizeof(PayloadCapReported) == 16, "size");
struct PayloadPose {
    float pos[3];
    float vel[3];
    float accel[3];
    float rot[4];
    float ang_vel[3];
    float ang_accel[3];
    int64_t timestamp_ns;
};
static_assert(sizeof(PayloadPose) == 88, "size");
struct PayloadControllerInput {
    uint32_t device;
    float    axis[4];
    uint64_t buttons;
    int64_t  timestamp_ns;
};
static_assert(sizeof(PayloadControllerInput) == 40, "size");
struct PayloadTelemetry {
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
struct PayloadSettingsChange {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t encoder;
    uint32_t stream_enabled;
    uint64_t seq;
};
static_assert(sizeof(PayloadSettingsChange) == 32, "size");
inline uint64_t LoadRelaxedU64(const volatile uint64_t& p) {
#ifdef _MSC_VER
    return _ReadWriteBarrier(), p;
#else
    return std::atomic_ref<const uint64_t>(const_cast<uint64_t&>(p)).load(std::memory_order_relaxed);
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
}