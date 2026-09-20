#pragma once
#include <cstddef>
// UDP wire contract for Cardboard++ (ports 42069-42074).
// Source of truth for all discovery/control strings and ports; the bridge
// (net/mod.rs) and Android app (AppConstants) mirror these values byte-for-byte.
namespace wire {
// Video (driver -> phone + localhost bridge preview) and discovery/control.
    static constexpr int kDataPort = 42069;
    static constexpr int kDiscoveryPort = 42070;
// Phone -> driver prober: "CARDBOARD_DISCOVERY" poll and "CARDBOARD_CAP W H"
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
}