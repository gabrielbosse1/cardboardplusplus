#pragma once
#include <cstddef>
#include <cstdint>
// UDP wire contract for Cardboard++ (ports 42069-42074).
// Source of truth for all discovery/control strings and ports; the bridge
// (net/mod.rs) and Android app (AppConstants) mirror these values byte-for-byte.
namespace wire {
// Video (driver -> phone + localhost bridge preview) and discovery/control.
    static constexpr int kDataPort = 42069;
    static constexpr int kDiscoveryPort = 42070;
// Phone -> driver host copyr: "CARDBOARD_DISCOVERY" poll and "CARDBOARD_CAP W H"
// resolution clamp. Driver answers discovery with kDiscoveryAck.
    static constexpr char kCardboardCap[] = "CARDBOARD_CAP";
    static constexpr std::size_t kCardboardCapLen = sizeof(kCardboardCap) - 1;
    static constexpr char kDiscoveryAck[] = "ACK";
    static constexpr std::size_t kDiscoveryAckLen = sizeof(kDiscoveryAck) - 1;
// Wake ping the phone listens for while the app is backgrounded.
    static constexpr char kDiscoveryWakeup[] = "wake";
    static constexpr std::size_t kDiscoveryWakeupLen = sizeof(kDiscoveryWakeup) - 1;
// Bridge <-> driver control (UDP 42070): heartbeat, ack, config push,
// preview toggle, and periodic stats line. Prefix-matched via kXxxLen.
    static constexpr char kBridgeHeartbeat[] = "BRIDGE_HELLO";
    static constexpr std::size_t kBridgeHeartbeatLen = sizeof(kBridgeHeartbeat) - 1;
    static constexpr char kBridgeAck[] = "BRIDGE_ACK v1";
    static constexpr std::size_t kBridgeAckLen = sizeof(kBridgeAck) - 1;
    static constexpr char kBridgeCfg[] = "BRIDGE_CFG";
    static constexpr std::size_t kBridgeCfgLen = sizeof(kBridgeCfg) - 1;
    static constexpr char kBridgePreview[] = "BRIDGE_PREVIEW";
    static constexpr std::size_t kBridgePreviewLen = sizeof(kBridgePreview) - 1;
    static constexpr char kBridgeStats[] = "BRIDGE_STATS";
    static constexpr std::size_t kBridgeStatsLen = sizeof(kBridgeStats) - 1;
// Phone -> driver request for an immediate encoder keyframe (after stalls).
    static constexpr char kKeyframeReq[] = "KEYFRAME_REQ";
    static constexpr std::size_t kKeyframeReqLen = sizeof(kKeyframeReq) - 1;
// Bridge -> driver sensor forward (rotation quats originating on the phone).
    static constexpr int kSensorPort = 42074;

// --- Bridge preview framing (driver -> 127.0.0.1:42069 only) -----------------
// The phone copy keeps its 4-byte length prefix and is NOT affected. The local
// preview copy used to be raw Annex-B split at arbitrary 1400-byte offsets, so a
// lost datagram was invisible to the bridge and ffmpeg decoded the bytes around
// the hole as one slice. Every preview datagram now carries this header followed
// by one fragment of exactly one encoded access unit. All fields big-endian.
//
//   off size field
//    0   4   magic       'C' 'B' 'P' 'V'
//    4   1   version     kPreviewVersion
//    5   1   flags       kPreviewFlag*
//    6   2   frag_index  0 .. frag_count-1
//    8   2   frag_count  fragments in this access unit (0 for control datagrams)
//   10   2   payload_len bytes after the header in this datagram
//   12   4   session     changes on every encoder (re)init; frame ids restart
//   16   4   frame_id    +1 per access unit within a session
//   20   4   frame_size  total access-unit bytes (sum of all payload_len)
//
// Control datagram: frag_count 0, payload_len 0, frame_id 0, frame_size 0.
// kPreviewFlagEnd on a control datagram is STREAM_END: the driver is stopping
// this session on purpose (SteamVR unload, preview turned off), so the bridge
// can go idle at once instead of waiting for its receive timeout.
//
// Mirrored by bridge/crates/cardboard-bridge/src/net/preview.rs — change both.
    static constexpr uint32_t kPreviewMagic = 0x43425056u;  // "CBPV"
    static constexpr uint8_t kPreviewVersion = 1;
    static constexpr std::size_t kPreviewHeaderSize = 24;
    static constexpr std::size_t kPreviewMaxDatagram = 1400;
    static constexpr std::size_t kPreviewMaxPayload = kPreviewMaxDatagram - kPreviewHeaderSize;
    static constexpr uint8_t kPreviewFlagKeyframe = 0x01;
    static constexpr uint8_t kPreviewFlagEnd = 0x02;
// Bridge -> driver: the preview lost a frame and needs an IDR. Distinct from the
// phone's KEYFRAME_REQ so it never refreshes the phone-liveness timer.
    static constexpr char kBridgeKeyframeReq[] = "BRIDGE_KEYFRAME_REQ";
    static constexpr std::size_t kBridgeKeyframeReqLen = sizeof(kBridgeKeyframeReq) - 1;
}
