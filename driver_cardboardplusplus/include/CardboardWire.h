#pragma once
// Wire-protocol and network constants shared by the UDP transport and phone
// discovery subsystems.
//
// LOCKED CONTRACT -- these values are part of the driver's external behavior
// and must never change (the agentize harness + the bridge mock expect them):
//   * "CARDBOARD_CAP" : phone -> driver hardware decoder-cap message
//   * "ACK"           : driver -> phone discovery acknowledgement
//   * "wake"          : loopback packet that unblocks a blocked discovery recv
//   * 42069 / 42070   : UDP data + discovery ports
#include <cstddef>

namespace wire {
    // Driver -> bridge video stream target port.
    static constexpr int kDataPort = 42069;

    // Phone -> driver broadcast discovery / acknowledgement port.
    static constexpr int kDiscoveryPort = 42070;

    // Phone reports its hardware decoder cap; no ACK is sent for this message.
    // Payload format: "CARDBOARD_CAP <width> <height>" (see Discovery.cpp).
    static constexpr char kCardboardCap[] = "CARDBOARD_CAP";
    static constexpr std::size_t kCardboardCapLen = sizeof(kCardboardCap) - 1; // 13

    // Driver acknowledges a discovery packet (3 bytes, no newline).
    static constexpr char kDiscoveryAck[] = "ACK";
    static constexpr std::size_t kDiscoveryAckLen = sizeof(kDiscoveryAck) - 1; // 3

    // Loopback packet sent during shutdown to unblock a blocked recvfrom.
    static constexpr char kDiscoveryWakeup[] = "wake";
    static constexpr std::size_t kDiscoveryWakeupLen = sizeof(kDiscoveryWakeup) - 1; // 4

    // Bridge (cardboard-bridge.exe) liveness probe: bridge -> driver on the
    // discovery port. The driver replies with kBridgeAck so the bridge marks
    // the driver alive; these packets must never switch the video data target.
    static constexpr char kBridgeHeartbeat[] = "BRIDGE_HELLO";
    static constexpr std::size_t kBridgeHeartbeatLen = sizeof(kBridgeHeartbeat) - 1; // 11

    // Driver -> bridge acknowledgement (the bridge checks starts_with("BRIDGE_ACK")).
    static constexpr char kBridgeAck[] = "BRIDGE_ACK v1";
    static constexpr std::size_t kBridgeAckLen = sizeof(kBridgeAck) - 1; // 13

    // Bridge -> driver stream-settings push (BRIDGE_CFG <fps> <bitrate_kbps> <codec>).
    static constexpr char kBridgeCfg[] = "BRIDGE_CFG";
    static constexpr std::size_t kBridgeCfgLen = sizeof(kBridgeCfg) - 1; // 10

    // Bridge -> driver local-preview switch. "BRIDGE_PREVIEW 1" keeps the
    // localhost stream on (so ffplay/bridge can watch without a phone);
    // "BRIDGE_PREVIEW 0" stops it. The driver defaults to ON so the ffplay
    // workflow works even if the bridge never sends this.
    static constexpr char kBridgePreview[] = "BRIDGE_PREVIEW";
    static constexpr std::size_t kBridgePreviewLen = sizeof(kBridgePreview) - 1; // 13

    // Driver -> bridge periodic streaming stats, sent on the discovery socket
    // right after each BRIDGE_ACK (the bridge's heartbeat drives the cadence).
    // Format: "BRIDGE_STATS fps=<n> bitrate=<kbps> frames=<n> drops=<n>".
    static constexpr char kBridgeStats[] = "BRIDGE_STATS";
    static constexpr std::size_t kBridgeStatsLen = sizeof(kBridgeStats) - 1; // 12
}