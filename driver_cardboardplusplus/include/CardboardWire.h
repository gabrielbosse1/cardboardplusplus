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
}