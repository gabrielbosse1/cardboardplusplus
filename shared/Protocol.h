#pragma once
#ifndef CBPP_PROTOCOL_H
#define CBPP_PROTOCOL_H

#include <cstdint>

// ============================================================
// CardboardPlusPlus Network Protocol
// ============================================================
// All messages are UDP. Big-endian (network byte order) unless
// stated otherwise. Each packet starts with a CBPP header.
// ============================================================

namespace cbpp {

// ---- Port Assignments ----
static constexpr uint16_t PORT_BROADCAST = 42070;  // Discovery (PC -> LAN)
static constexpr uint16_t PORT_VIDEO     = 42069;  // H264 stream (PC -> Phone)
static constexpr uint16_t PORT_CAMERA    = 42071;  // Camera frames (Phone -> PC)
static constexpr uint16_t PORT_TRACKING  = 42072;  // IMU/pose (Phone -> PC)

// ---- Magic bytes ----
static constexpr uint8_t MAGIC[2] = { 'C', 'B' };

// ---- Packet Types ----
enum PacketType : uint8_t {
    // Discovery: PC broadcasts this periodically so phones can find it.
    PT_DISCOVERY_ANNOUNCE = 0x01,

    // Phone tells PC "I'm here, send video to me". Sent after hearing announce.
    PT_DISCOVERY_RESPONSE = 0x02,

    // Video frame chunk. Sent from PC to phone on PORT_VIDEO.
    PT_VIDEO_CHUNK = 0x10,

    // Camera frame chunk. Sent from phone to PC on PORT_CAMERA.
    PT_CAMERA_CHUNK = 0x20,

    // Tracking data. Sent from phone to PC on PORT_TRACKING.
    PT_TRACKING = 0x30,
};

// ---- Common header: every packet starts with this ----
#pragma pack(push, 1)

struct PacketHeader {
    uint8_t  magic[2];   // 'C', 'B'
    uint8_t  version;    // protocol version (1)
    uint8_t  type;       // PacketType
    uint32_t payloadSize; // size of payload following this header
};
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

// ---- DISCOVERY_ANNOUNCE (PC -> broadcast, PORT_BROADCAST) ----
// Header + AnnouncePayload
struct AnnouncePayload {
    uint32_t videoPort;    // port phone should receive video on (42069)
    uint32_t cameraPort;   // port PC will listen for camera on (42071)
    uint32_t trackingPort; // port PC will listen for tracking on (42072)
    uint32_t serverIp;     // PC's IPv4 address (network byte order)
    char     name[32];     // null-terminated friendly name
};

// ---- DISCOVERY_RESPONSE (phone -> PC unicast, PORT_BROADCAST) ----
// Header + ResponsePayload
struct ResponsePayload {
    uint32_t clientIp;     // phone's IPv4 address (network byte order)
    uint32_t clientPort;   // phone's preferred port for video (usually 42069)
};

// ---- VIDEO_CHUNK (PC -> phone, PORT_VIDEO) ----
// Header + VideoChunkHeader + [data bytes]
struct VideoChunkHeader {
    uint32_t frameId;      // monotonically increasing frame counter
    uint32_t chunkIndex;   // which chunk of this frame (0-based)
    uint32_t totalChunks;  // total chunks for this frame
    uint32_t frameSize;    // total size of the entire encoded frame
    uint8_t  keyframe;     // 1 if this frame is a keyframe
};

// ---- CAMERA_CHUNK (phone -> PC, PORT_CAMERA) ----
// Header + CameraChunkHeader + [data bytes]
struct CameraChunkHeader {
    uint32_t frameId;
    uint32_t chunkIndex;
    uint32_t totalChunks;
    uint32_t frameSize;
    uint32_t width;
    uint32_t height;
};

// ---- TRACKING (phone -> PC, PORT_TRACKING) ----
// Header + TrackingPayload
// Data is in float32, device coordinate system.
// For now: orientation as quaternion, position as fixed placeholder.
struct TrackingPayload {
    // Orientation (quaternion, Cardboard SDK convention)
    float orientationW;
    float orientationX;
    float orientationY;
    float orientationZ;

    // Position placeholder (meters, SteamVR convention: Y-up)
    float positionX;
    float positionY;   // fixed height placeholder
    float positionZ;

    // Timestamp in nanoseconds (monotonic, phone clock)
    int64_t timestampNanos;
};

#pragma pack(pop)

// ---- Constants ----
static constexpr uint8_t  PROTOCOL_VERSION  = 1;
static constexpr uint32_t VIDEO_CHUNK_SIZE  = 60000;
static constexpr uint32_t CAMERA_CHUNK_SIZE = 60000;
static constexpr int      BROADCAST_INTERVAL_MS = 100;  // 10 Hz
static constexpr float    DEFAULT_HEAD_HEIGHT = 1.7f;  // meters

}  // namespace cbpp

#endif  // CBPP_PROTOCOL_H
