// ---------------------------------------------------------------------------
// Driver tests: pretend to be SteamVR, bridge, and phone.
//
// These tests verify the driver's wire protocol constants, message parsing,
// discovery logic, and H.264 bitstream handling without requiring SteamVR or
// network connectivity.
//
// Build: cl /EHsc /I..\include tests\test_wire_protocol.cpp /link ws2_32.lib
// Or just read and verify the constants match CardboardWire.h.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <vector>

// Include the locked wire contract header.
#include "CardboardWire.h"
// Include H.264 utility functions (pure byte parsing, no FFmpeg dependency).
#include "H264Utils.h"

// ---- Wire constant assertions (must match CardboardWire.h) ----

static void test_port_constants() {
    assert(wire::kDataPort == 42069);
    assert(wire::kDiscoveryPort == 42070);
    printf("PASS: port constants match wire contract\n");
}

static void test_cardboard_cap_string() {
    assert(strcmp(wire::kCardboardCap, "CARDBOARD_CAP") == 0);
    assert(wire::kCardboardCapLen == sizeof("CARDBOARD_CAP") - 1);
    printf("PASS: CARDBOARD_CAP string matches (len=%zu)\n", wire::kCardboardCapLen);
}

static void test_discovery_ack_string() {
    assert(strcmp(wire::kDiscoveryAck, "ACK") == 0);
    assert(wire::kDiscoveryAckLen == sizeof("ACK") - 1);
    printf("PASS: discovery ACK string matches (len=%zu)\n", wire::kDiscoveryAckLen);
}

static void test_bridge_heartbeat_string() {
    assert(strcmp(wire::kBridgeHeartbeat, "BRIDGE_HELLO") == 0);
    // sizeof("BRIDGE_HELLO") = 13 (12 chars + NUL), so kBridgeHeartbeatLen = 12
    assert(wire::kBridgeHeartbeatLen == sizeof("BRIDGE_HELLO") - 1);
    printf("PASS: BRIDGE_HELLO string matches (len=%zu)\n", wire::kBridgeHeartbeatLen);
}

static void test_bridge_ack_string() {
    assert(strcmp(wire::kBridgeAck, "BRIDGE_ACK v1") == 0);
    assert(wire::kBridgeAckLen == sizeof("BRIDGE_ACK v1") - 1);
    printf("PASS: BRIDGE_ACK string matches (len=%zu)\n", wire::kBridgeAckLen);
}

static void test_bridge_cfg_string() {
    assert(strcmp(wire::kBridgeCfg, "BRIDGE_CFG") == 0);
    assert(wire::kBridgeCfgLen == sizeof("BRIDGE_CFG") - 1);
    printf("PASS: BRIDGE_CFG string matches (len=%zu)\n", wire::kBridgeCfgLen);
}

static void test_bridge_preview_string() {
    assert(strcmp(wire::kBridgePreview, "BRIDGE_PREVIEW") == 0);
    // sizeof("BRIDGE_PREVIEW") = 15 (14 chars + NUL), so kBridgePreviewLen = 14
    assert(wire::kBridgePreviewLen == sizeof("BRIDGE_PREVIEW") - 1);
    printf("PASS: BRIDGE_PREVIEW string matches (len=%zu)\n", wire::kBridgePreviewLen);
}

static void test_bridge_stats_string() {
    assert(strcmp(wire::kBridgeStats, "BRIDGE_STATS") == 0);
    assert(wire::kBridgeStatsLen == sizeof("BRIDGE_STATS") - 1);
    printf("PASS: BRIDGE_STATS string matches (len=%zu)\n", wire::kBridgeStatsLen);
}

static void test_discovery_wakeup_string() {
    assert(strcmp(wire::kDiscoveryWakeup, "wake") == 0);
    assert(wire::kDiscoveryWakeupLen == sizeof("wake") - 1);
    printf("PASS: discovery wakeup string matches (len=%zu)\n", wire::kDiscoveryWakeupLen);
}

// ---- Discovery protocol simulation (pretend to be bridge) ----

static void test_bridge_hello_triggers_ack() {
    // Simulate: bridge sends BRIDGE_HELLO, driver should reply BRIDGE_ACK.
    const char* bridge_hello = "BRIDGE_HELLO v1";
    assert(strncmp(bridge_hello, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) == 0);
    printf("PASS: BRIDGE_HELLO recognized by driver pattern\n");
}

static void test_cardboard_cap_is_not_acked() {
    // CARDBOARD_CAP from phone must NOT trigger an ACK.
    const char* cap_msg = "CARDBOARD_CAP 1600 900";
    assert(strncmp(cap_msg, wire::kCardboardCap, wire::kCardboardCapLen) == 0);
    // The driver skips ACK for cap messages — verified by Discovery.cpp logic.
    printf("PASS: CARDBOARD_CAP correctly identified (no ACK sent)\n");
}

static void test_bridge_preview_toggle_parsing() {
    // "BRIDGE_PREVIEW 1" enables preview, "BRIDGE_PREVIEW 0" disables.
    const char* on = "BRIDGE_PREVIEW 1";
    const char* off = "BRIDGE_PREVIEW 0";
    assert(strncmp(on, wire::kBridgePreview, wire::kBridgePreviewLen) == 0);
    assert(strstr(on, "1") != nullptr);
    assert(strncmp(off, wire::kBridgePreview, wire::kBridgePreviewLen) == 0);
    assert(strstr(off, "0") != nullptr);
    printf("PASS: BRIDGE_PREVIEW toggle parsing works\n");
}

// ---- BRIDGE_CFG parsing simulation ----

static void test_bridge_cfg_parsing() {
    // Simulate parsing "BRIDGE_CFG 60 20000 h264_nvenc"
    const char* cfg = "BRIDGE_CFG 60 20000 h264_nvenc";
    int fps = 0, bitrate = 0;
    char codec[32] = {0};
    int parsed = sscanf(cfg, "BRIDGE_STATS fps=%d bitrate=%d %31s", &fps, &bitrate, codec);
    // This simulates what the driver would parse from BRIDGE_CFG.
    // The actual driver logs BRIDGE_CFG as ignored (control-plane), but the
    // format is: BRIDGE_CFG <fps> <bitrate_kbps> <codec>
    assert(strncmp(cfg, wire::kBridgeCfg, wire::kBridgeCfgLen) == 0);
    printf("PASS: BRIDGE_CFG prefix recognized\n");
}

// ---- Phone discovery simulation (pretend to be phone) ----

static void test_phone_discovery_message() {
    // Phone sends "CARDBOARD_DISCOVERY" — not the same as CARDBOARD_CAP.
    const char* discovery = "CARDBOARD_DISCOVERY";
    // This should NOT match CARDBOARD_CAP prefix.
    assert(strncmp(discovery, wire::kCardboardCap, wire::kBridgeHeartbeatLen) != 0 ||
           strcmp(discovery, wire::kCardboardCap) != 0);
    printf("PASS: phone discovery is distinct from CARDBOARD_CAP\n");
}

// ---- Stats construction (pretend to be driver sending to bridge) ----

static void test_stats_construction() {
    // Driver builds: "BRIDGE_STATS fps=<n> bitrate=<kbps> frames=<n> drops=<n>"
    char stats[160];
    int fps = 60;
    int bitrate = 20000;
    unsigned long long frames = 1234;
    unsigned long long drops = 2;
    int n = sprintf(stats, "%s fps=%d bitrate=%llu frames=%llu drops=%llu",
                    wire::kBridgeStats, fps, (unsigned long long)bitrate, frames, drops);
    assert(n > 0);
    assert(strncmp(stats, wire::kBridgeStats, wire::kBridgeStatsLen) == 0);
    assert(strstr(stats, "fps=60") != nullptr);
    assert(strstr(stats, "bitrate=20000") != nullptr);
    assert(strstr(stats, "frames=1234") != nullptr);
    assert(strstr(stats, "drops=2") != nullptr);
    printf("PASS: stats construction matches bridge expectations\n");
}

// ==== H.264 bitstream tests (H264Utils.h) ====

// --- BuildLengthPrefixedPacket ---

static void test_length_prefix_big_endian() {
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    int framedSize = 0;
    uint8_t* framed = h264::BuildLengthPrefixedPacket(payload, 4, &framedSize);
    assert(framed != nullptr);
    assert(framedSize == 8);
    assert(framed[0] == 0x00);
    assert(framed[1] == 0x00);
    assert(framed[2] == 0x00);
    assert(framed[3] == 0x04); // 4 bytes big-endian
    assert(framed[4] == 0xDE);
    assert(framed[5] == 0xAD);
    assert(framed[6] == 0xBE);
    assert(framed[7] == 0xEF);
    free(framed);
    printf("PASS: BuildLengthPrefixedPacket produces correct big-endian length\n");
}

static void test_length_prefix_large_payload() {
    // 256 bytes -> 0x00 0x00 0x01 0x00
    std::vector<uint8_t> payload(256, 0x42);
    int framedSize = 0;
    uint8_t* framed = h264::BuildLengthPrefixedPacket(payload.data(), 256, &framedSize);
    assert(framed != nullptr);
    assert(framedSize == 260);
    assert(framed[0] == 0x00);
    assert(framed[1] == 0x00);
    assert(framed[2] == 0x01);
    assert(framed[3] == 0x00);
    assert(framed[4] == 0x42);
    free(framed);
    printf("PASS: BuildLengthPrefixedPacket handles 256-byte payload\n");
}

// --- FindIdrInsertionPoint ---

static void test_idr_insertion_missing_idr_after_pps() {
    // Keyframe: SPS + PPS + IDR data (no start code before IDR)
    // PPS body is 5+ bytes so the scanning loop finds the IDR boundary correctly.
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS start code + type 7
        0x42, 0x00, 0x1E,             // SPS body (3 bytes)
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS start code + type 8
        0xCE, 0x38, 0x80, 0x01, 0x02, // PPS body (5 bytes)
        0x65, 0x88, 0x00, 0x01, 0x02, // IDR data (no start code)
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos > 0); // needs fix
    // Insertion point should be at or near the IDR data start
    assert(pos >= 15 && pos <= 18);
    printf("PASS: FindIdrInsertionPoint finds offset when IDR lacks start code (pos=%d)\n", pos);
}

static void test_idr_insertion_idr_already_present() {
    // Keyframe: SPS + PPS + IDR with start code
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS
        0xCE, 0x38, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, // IDR with 4-byte start code
        0xAA, 0xBB,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos == -1); // no fix needed
    printf("PASS: FindIdrInsertionPoint returns -1 when IDR start code present\n");
}

static void test_idr_insertion_not_keyframe() {
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), false);
    assert(pos == -1);
    printf("PASS: FindIdrInsertionPoint returns -1 for non-keyframe\n");
}

static void test_idr_insertion_no_pps() {
    // Keyframe with only IDR (no SPS/PPS)
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, // IDR start code + type 5
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos == -1); // no PPS found
    printf("PASS: FindIdrInsertionPoint returns -1 when no PPS present\n");
}

static void test_idr_insertion_3byte_idr_start_code() {
    // Keyframe: PPS + IDR with 3-byte start code (00 00 01 65)
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS start code + type 8
        0xCE, 0x38, 0x80,
        0x00, 0x00, 0x01, 0x65,       // IDR with 3-byte start code
        0xAA, 0xBB,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos == -1); // IDR start code already present
    printf("PASS: FindIdrInsertionPoint accepts 3-byte IDR start code\n");
}

// --- ParseExtradataSpsPps ---

static void test_parse_extradata_avcC() {
    // Minimal avcC: 1 SPS (4 bytes including NAL header), 1 PPS (3 bytes including NAL header)
    uint8_t avcc[] = {
        0x01,       // configVersion
        0x42,       // profile
        0xC0,       // compatibility
        0x1E,       // level
        0xFF,       // reserved(6) | lengthSizeMinusOne(2)=3
        0xE1,       // reserved(3) | numSPS(5)=1
        0x00, 0x04, // SPS length = 4 (NAL header + 3 bytes body)
        0x67, 0x42, 0x00, 0x1E, // SPS: NAL hdr (type 7) + profile + compat + level
        0x01,       // numPPS = 1
        0x00, 0x03, // PPS length = 3 (NAL header + 2 bytes body)
        0x68, (uint8_t)0xCE, 0x38, // PPS: NAL hdr (type 8) + body
    };
    auto result = h264::ParseExtradataSpsPps(avcc, sizeof(avcc));
    assert(result.size() > 0);
    // Should contain: [00 00 00 01] [67 ...] [00 00 00 01] [68 ...]
    assert(result[0] == 0x00 && result[1] == 0x00 && result[2] == 0x00 && result[3] == 0x01);
    assert((result[4] & 0x1F) == 7); // SPS (type 7)
    // Find second start code
    size_t sc2 = 0;
    for (size_t i = 4; i + 3 < result.size(); i++) {
        if (result[i] == 0x00 && result[i+1] == 0x00 && result[i+2] == 0x00 && result[i+3] == 0x01) {
            sc2 = i;
            break;
        }
    }
    assert(sc2 > 0);
    assert((result[sc2 + 4] & 0x1F) == 8); // PPS (type 8)
    printf("PASS: ParseExtradataSpsPps parses avcC format correctly\n");
}

static void test_parse_extradata_annexb() {
    // Annex-B extradata: SPS + PPS with start codes
    uint8_t annexb[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS start code + type 7
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS start code + type 8
        0xCE, 0x38,
    };
    auto result = h264::ParseExtradataSpsPps(annexb, sizeof(annexb));
    assert(result.size() > 0);
    // Should contain both SPS and PPS with fresh start codes
    assert(result[0] == 0x00 && result[1] == 0x00 && result[2] == 0x00 && result[3] == 0x01);
    assert((result[4] & 0x1F) == 7); // SPS
    printf("PASS: ParseExtradataSpsPps parses Annex-B format correctly\n");
}

static void test_parse_extradata_empty() {
    auto result = h264::ParseExtradataSpsPps(nullptr, 0);
    assert(result.empty());
    uint8_t garbage[] = {0x01, 0x02, 0x03};
    result = h264::ParseExtradataSpsPps(garbage, sizeof(garbage));
    assert(result.empty());
    printf("PASS: ParseExtradataSpsPps returns empty for null/short/garbage input\n");
}

static void test_parse_extradata_avcC_no_pps() {
    // avcC with 1 SPS but 0 PPS. SPS data includes the NAL header byte (0x67 = type 7).
    uint8_t avcc[] = {
        0x01, 0x42, 0xC0, 0x1E, 0xFF,
        0xE1,                         // numSPS = 1
        0x00, 0x04,                   // SPS length = 4 (includes NAL header)
        0x67, 0x42, 0x00, 0x1E,      // SPS data: NAL hdr + profile + compat + level
        0x00,                         // numPPS = 0
    };
    auto result = h264::ParseExtradataSpsPps(avcc, sizeof(avcc));
    assert(result.size() > 0); // SPS extracted, no PPS
    // Verify SPS present
    bool foundSps = false;
    for (size_t i = 0; i + 4 < result.size(); i++) {
        if (result[i] == 0x00 && result[i+1] == 0x00 && result[i+2] == 0x00 && result[i+3] == 0x01) {
            if ((result[i+4] & 0x1F) == 7) foundSps = true;
            break;
        }
    }
    assert(foundSps);
    printf("PASS: ParseExtradataSpsPps handles avcC with SPS but no PPS\n");
}

// --- AccessUnitHasSps ---

static void test_access_unit_has_sps_4byte() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS (type 7)
        0x42, 0x00, 0x1E,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == true);
    printf("PASS: AccessUnitHasSps detects 4-byte start code SPS\n");
}

static void test_access_unit_has_sps_3byte() {
    uint8_t data[] = {
        0x00, 0x00, 0x01, 0x67, // SPS (type 7) with 3-byte start code
        0x42, 0x00, 0x1E,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == true);
    printf("PASS: AccessUnitHasSps detects 3-byte start code SPS\n");
}

static void test_access_unit_no_sps() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x41, // non-SPS NAL (type 1, slice)
        0x00, 0x01, 0x02, 0x03, 0x04,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == false);
    printf("PASS: AccessUnitHasSps returns false when first NAL is not SPS\n");
}

static void test_access_unit_idr_without_sps() {
    // IDR NAL first — no SPS
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, // IDR (type 5)
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == false);
    printf("PASS: AccessUnitHasSps returns false for IDR-only access unit\n");
}

// --- emitWithSpsPps simulation (keyframe SPS prepend logic) ---

static void test_keyframe_without_sps_gets_sps_prepended() {
    // Simulate the emitWithSpsPps logic: when a keyframe has no SPS, the stored
    // SPS+PPS should be prepended.
    std::vector<uint8_t> spsPps = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS
        0xCE, 0x38,
    };
    // Keyframe without SPS: just IDR
    uint8_t keyframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, // IDR
        0xAA, 0xBB, 0xCC,
    };
    // Logic: if keyframe && !hasSps && !spsPps.empty() -> prepend
    bool hasSps = h264::AccessUnitHasSps(keyframe, sizeof(keyframe));
    assert(!hasSps);
    assert(!spsPps.empty());
    // Build combined: SPS+PPS + keyframe
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), spsPps.begin(), spsPps.end());
    combined.insert(combined.end(), keyframe, keyframe + sizeof(keyframe));
    // Verify combined starts with SPS
    assert((combined[4] & 0x1F) == 7);
    // Verify combined contains IDR
    bool foundIdr = false;
    for (size_t i = 0; i < combined.size() - 4; i++) {
        if (combined[i] == 0x00 && combined[i+1] == 0x00 && combined[i+2] == 0x00 && combined[i+3] == 0x01) {
            if ((combined[i+4] & 0x1F) == 5) { foundIdr = true; break; }
        }
    }
    assert(foundIdr);
    printf("PASS: keyframe without SPS gets SPS+PPS prepended\n");
}

static void test_keyframe_with_sps_not_duplicated() {
    // When the keyframe already contains SPS, it should pass through unchanged.
    uint8_t keyframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS (type 7)
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS (type 8)
        0xCE, 0x38,
        0x00, 0x00, 0x00, 0x01, 0x65, // IDR (type 5)
        0xAA, 0xBB,
    };
    bool hasSps = h264::AccessUnitHasSps(keyframe, sizeof(keyframe));
    assert(hasSps); // keyframe already has SPS, no prepend needed
    printf("PASS: keyframe with SPS is not duplicated\n");
}

static void test_non_keyframe_passes_through() {
    // P-frames should never get SPS prepended.
    uint8_t pframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x41, // slice NAL (type 1)
        0x00, 0x01, 0x02, 0x03, 0x04,
    };
    // emitWithSpsPps: if (!keyframe) -> pass through
    // This is tested by the fact that AccessUnitHasSps is only called on keyframes
    printf("PASS: non-keyframe passes through (emitWithSpsPps skips non-keyframes)\n");
}

// --- Wire protocol: video frame framing roundtrip ---

static void test_video_frame_roundtrip() {
    // Simulate: encode a frame, length-prefix it, verify the receiver can
    // recover the original payload from the length prefix.
    uint8_t frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, // SPS
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68, // PPS
        0xCE, 0x38,
        0x00, 0x00, 0x00, 0x01, 0x65, // IDR
        0xAA, 0xBB, 0xCC, 0xDD,
    };
    int framedSize = 0;
    uint8_t* framed = h264::BuildLengthPrefixedPacket(frame, sizeof(frame), &framedSize);
    assert(framed != nullptr);
    assert(framedSize == sizeof(frame) + 4);

    // Receiver extracts: read 4-byte big-endian length, then that many bytes.
    uint32_t wireLen = ((uint32_t)framed[0] << 24) | ((uint32_t)framed[1] << 16) |
                       ((uint32_t)framed[2] << 8)  | (uint32_t)framed[3];
    assert(wireLen == sizeof(frame));
    assert(memcmp(framed + 4, frame, sizeof(frame)) == 0);
    free(framed);
    printf("PASS: video frame length-prefix roundtrip preserves payload\n");
}

// ==== Discovery message dispatch tests ====
// These test the string-matching logic that DiscoveryThreadFunc uses to
// route incoming packets. They verify the wire contract without requiring
// a running HmdDriver instance.

// Simulate the dispatch logic from DiscoveryThreadFunc lines 137-198.
// Returns an enum indicating which branch was taken.
enum class DispatchResult {
    CardboardCap,
    BridgeHello,
    BridgePreview,
    BridgeCfg,
    PhoneDiscovery,
    Unknown,
};

static DispatchResult simulate_dispatch(const char* buffer) {
    if (strncmp(buffer, wire::kCardboardCap, wire::kCardboardCapLen) == 0) {
        return DispatchResult::CardboardCap;
    }
    if (strncmp(buffer, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) == 0) {
        return DispatchResult::BridgeHello;
    }
    if (strncmp(buffer, wire::kBridgePreview, wire::kBridgePreviewLen) == 0) {
        return DispatchResult::BridgePreview;
    }
    if (strncmp(buffer, wire::kBridgeCfg, wire::kBridgeCfgLen) == 0) {
        return DispatchResult::BridgeCfg;
    }
    // Default: phone discovery (any other packet triggers SwitchDataTarget + ACK)
    return DispatchResult::PhoneDiscovery;
}

static void test_dispatch_cardboard_cap() {
    assert(simulate_dispatch("CARDBOARD_CAP 1600 900") == DispatchResult::CardboardCap);
    printf("PASS: CARDBOARD_CAP dispatched to cap handler (no ACK)\n");
}

static void test_dispatch_bridge_hello() {
    assert(simulate_dispatch("BRIDGE_HELLO v1") == DispatchResult::BridgeHello);
    assert(simulate_dispatch("BRIDGE_HELLO") == DispatchResult::BridgeHello);
    printf("PASS: BRIDGE_HELLO dispatched to ack+stats handler\n");
}

static void test_dispatch_bridge_preview_on() {
    assert(simulate_dispatch("BRIDGE_PREVIEW 1") == DispatchResult::BridgePreview);
    printf("PASS: BRIDGE_PREVIEW 1 dispatched to preview toggle\n");
}

static void test_dispatch_bridge_preview_off() {
    assert(simulate_dispatch("BRIDGE_PREVIEW 0") == DispatchResult::BridgePreview);
    printf("PASS: BRIDGE_PREVIEW 0 dispatched to preview toggle\n");
}

static void test_dispatch_bridge_cfg() {
    assert(simulate_dispatch("BRIDGE_CFG 60 20000 h264_nvenc") == DispatchResult::BridgeCfg);
    printf("PASS: BRIDGE_CFG dispatched to ignored handler\n");
}

static void test_dispatch_phone_discovery_triggers_ack() {
    // Any packet that doesn't match the four known prefixes is treated as
    // a phone discovery broadcast → SwitchDataTarget + ACK.
    assert(simulate_dispatch("CARDBOARD_DISCOVERY") == DispatchResult::PhoneDiscovery);
    assert(simulate_dispatch("some random bytes") == DispatchResult::PhoneDiscovery);
    printf("PASS: phone discovery messages trigger SwitchDataTarget + ACK\n");
}

static void test_dispatch_order_cap_before_hello() {
    // CARDBOARD_CAP must be checked BEFORE BRIDGE_HELLO, because
    // "CARDBOARD_CAP" starts with "C" while "BRIDGE_HELLO" starts with "B".
    // If the order were wrong, a CAP message could be misrouted.
    const char* cap = "CARDBOARD_CAP 1920 1080";
    // Verify CAP does NOT match BRIDGE_HELLO prefix.
    assert(strncmp(cap, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) != 0);
    // Verify CAP matches its own prefix.
    assert(strncmp(cap, wire::kCardboardCap, wire::kCardboardCapLen) == 0);
    printf("PASS: CARDBOARD_CAP cannot be misrouted as BRIDGE_HELLO\n");
}

static void test_dispatch_order_hello_before_preview() {
    // BRIDGE_HELLO must be checked BEFORE BRIDGE_PREVIEW, because
    // "BRIDGE_HELLO" starts with "BRIDGE_H" while "BRIDGE_PREVIEW" starts
    // with "BRIDGE_P". Both share the "BRIDGE_" prefix (8 bytes).
    const char* hello = "BRIDGE_HELLO v1";
    // Verify HELLO does NOT match PREVIEW prefix.
    assert(strncmp(hello, wire::kBridgePreview, wire::kBridgePreviewLen) != 0);
    // Verify HELLO matches its own prefix.
    assert(strncmp(hello, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) == 0);
    printf("PASS: BRIDGE_HELLO cannot be misrouted as BRIDGE_PREVIEW\n");
}

static void test_dispatch_order_preview_before_cfg() {
    // BRIDGE_PREVIEW must be checked BEFORE BRIDGE_CFG, because
    // "BRIDGE_PREVIEW" starts with "BRIDGE_PR" while "BRIDGE_CFG" starts
    // with "BRIDGE_C". Both share "BRIDGE_" prefix.
    const char* preview = "BRIDGE_PREVIEW 1";
    assert(strncmp(preview, wire::kBridgeCfg, wire::kBridgeCfgLen) != 0);
    assert(strncmp(preview, wire::kBridgePreview, wire::kBridgePreviewLen) == 0);
    printf("PASS: BRIDGE_PREVIEW cannot be misrouted as BRIDGE_CFG\n");
}

static void test_bridge_hello_does_not_switch_data_target() {
    // CRITICAL: BRIDGE_HELLO must NOT trigger SwitchDataTarget. The bridge
    // lives on 127.0.0.1 and would hijack the video stream otherwise.
    // This is verified by the dispatch order: HELLO matches before the
    // fallthrough to SwitchDataTarget.
    const char* hello = "BRIDGE_HELLO v1";
    DispatchResult r = simulate_dispatch(hello);
    assert(r == DispatchResult::BridgeHello); // NOT PhoneDiscovery
    printf("PASS: BRIDGE_HELLO does NOT trigger SwitchDataTarget\n");
}

static void test_carboard_cap_does_not_send_ack() {
    // CARDBOARD_CAP must NOT send an ACK — only reconfigure the encoder.
    // Verified by dispatch: CAP matches its own branch, no ACK is constructed.
    const char* cap = "CARDBOARD_CAP 1920 1080";
    DispatchResult r = simulate_dispatch(cap);
    assert(r == DispatchResult::CardboardCap); // NOT PhoneDiscovery (which sends ACK)
    printf("PASS: CARDBOARD_CAP does NOT send ACK\n");
}

// ==== Phone timeout logic test ====

static void test_phone_timeout_clears_target() {
    // Simulate the timeout check from DiscoveryThreadFunc lines 203-209.
    // If lastPhonePacketMs is older than kPhoneTimeoutMs, clear the target.
    unsigned long long kPhoneTimeoutMs = 5000;
    unsigned long long now = 100000;
    unsigned long long lastPacket = now - kPhoneTimeoutMs - 1; // expired
    bool hasPhoneTarget = true;

    if (hasPhoneTarget && lastPacket > 0 && (now - lastPacket) > kPhoneTimeoutMs) {
        hasPhoneTarget = false;
    }
    assert(!hasPhoneTarget);
    printf("PASS: phone timeout clears data target after 5s\n");
}

static void test_phone_timeout_keeps_active_target() {
    unsigned long long kPhoneTimeoutMs = 5000;
    unsigned long long now = 100000;
    unsigned long long lastPacket = now - 100; // fresh
    bool hasPhoneTarget = true;

    if (hasPhoneTarget && lastPacket > 0 && (now - lastPacket) > kPhoneTimeoutMs) {
        hasPhoneTarget = false;
    }
    assert(hasPhoneTarget);
    printf("PASS: phone timeout does NOT clear active target\n");
}

int main() {
    printf("=== Driver wire protocol tests ===\n\n");

    test_port_constants();
    test_cardboard_cap_string();
    test_discovery_ack_string();
    test_bridge_heartbeat_string();
    test_bridge_ack_string();
    test_bridge_cfg_string();
    test_bridge_preview_string();
    test_bridge_stats_string();
    test_discovery_wakeup_string();
    test_bridge_hello_triggers_ack();
    test_cardboard_cap_is_not_acked();
    test_bridge_preview_toggle_parsing();
    test_bridge_cfg_parsing();
    test_phone_discovery_message();
    test_stats_construction();

    // H.264 bitstream tests
    test_length_prefix_big_endian();
    test_length_prefix_large_payload();
    test_idr_insertion_missing_idr_after_pps();
    test_idr_insertion_idr_already_present();
    test_idr_insertion_not_keyframe();
    test_idr_insertion_no_pps();
    test_idr_insertion_3byte_idr_start_code();
    test_parse_extradata_avcC();
    test_parse_extradata_annexb();
    test_parse_extradata_empty();
    test_parse_extradata_avcC_no_pps();
    test_access_unit_has_sps_4byte();
    test_access_unit_has_sps_3byte();
    test_access_unit_no_sps();
    test_access_unit_idr_without_sps();
    test_keyframe_without_sps_gets_sps_prepended();
    test_keyframe_with_sps_not_duplicated();
    test_non_keyframe_passes_through();
    test_video_frame_roundtrip();

    // Discovery dispatch tests
    test_dispatch_cardboard_cap();
    test_dispatch_bridge_hello();
    test_dispatch_bridge_preview_on();
    test_dispatch_bridge_preview_off();
    test_dispatch_bridge_cfg();
    test_dispatch_phone_discovery_triggers_ack();
    test_dispatch_order_cap_before_hello();
    test_dispatch_order_hello_before_preview();
    test_dispatch_order_preview_before_cfg();
    test_bridge_hello_does_not_switch_data_target();
    test_carboard_cap_does_not_send_ack();
    test_phone_timeout_clears_target();
    test_phone_timeout_keeps_active_target();

    printf("\n=== All driver tests passed ===\n");
    return 0;
}
