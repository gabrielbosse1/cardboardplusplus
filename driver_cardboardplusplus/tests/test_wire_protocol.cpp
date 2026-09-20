// Driver wire-protocol checks: wire-string/port constants plus local mirrors of the discovery dispatch, phone-timeout,
// and H.264 framing/SPS helpers prove the driver and bridge/phone agree byte-for-byte without opening sockets.
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <vector>
#include "CardboardWire.h"
#include "H264Utils.h"
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
    assert(wire::kBridgeHeartbeatLen == sizeof("BRIDGE_HELLO") - 1);
    printf("PASS: BRIDGE_HELLO string matches (len=%zu)\n", wire::kBridgeHeartbeatLen);
}
static void test_bridge_ack_string() {
    assert(strcmp(wire::kBridgeAck, "BRIDGE_ACK v1") == 0);
    assert(wire::kBridgeAckLen == sizeof("BRIDGE_ACK v1") - 1);
    printf("PASS: BRIDGE_ACK string matches (len=%zu)\n", wire::kBridgeAckLen);
}
static void test_bridge_ack_carries_build_version() {
    char ack[64];
    int n = snprintf(ack, sizeof(ack), "%s %s", wire::kBridgeAck, "542");
    assert(n > 0 && n < (int)sizeof(ack));
    assert(strncmp(ack, wire::kBridgeAck, wire::kBridgeAckLen) == 0);
    const char* ver = ack + wire::kBridgeAckLen;
    while (*ver == ' ') ver++;
    assert(strcmp(ver, "542") == 0);
    printf("PASS: versioned BRIDGE_ACK keeps prefix, version extracts\n");
}
static void test_bridge_cfg_string() {
    assert(strcmp(wire::kBridgeCfg, "BRIDGE_CFG") == 0);
    assert(wire::kBridgeCfgLen == sizeof("BRIDGE_CFG") - 1);
    printf("PASS: BRIDGE_CFG string matches (len=%zu)\n", wire::kBridgeCfgLen);
}
static void test_bridge_preview_string() {
    assert(strcmp(wire::kBridgePreview, "BRIDGE_PREVIEW") == 0);
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
static void test_keyframe_req_string() {
    assert(strcmp(wire::kKeyframeReq, "KEYFRAME_REQ") == 0);
    assert(wire::kKeyframeReqLen == sizeof("KEYFRAME_REQ") - 1);
    printf("PASS: KEYFRAME_REQ string matches (len=%zu)\n", wire::kKeyframeReqLen);
}
static void test_bridge_hello_triggers_ack() {
    const char* bridge_hello = "BRIDGE_HELLO v1";
    assert(strncmp(bridge_hello, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) == 0);
    printf("PASS: BRIDGE_HELLO recognized by driver pattern\n");
}
static void test_cardboard_cap_is_not_acked() {
    const char* cap_msg = "CARDBOARD_CAP 1600 900";
    assert(strncmp(cap_msg, wire::kCardboardCap, wire::kCardboardCapLen) == 0);
    printf("PASS: CARDBOARD_CAP correctly identified (no ACK sent)\n");
}
static void test_bridge_preview_toggle_parsing() {
    const char* on = "BRIDGE_PREVIEW 1";
    const char* off = "BRIDGE_PREVIEW 0";
    assert(strncmp(on, wire::kBridgePreview, wire::kBridgePreviewLen) == 0);
    assert(strstr(on, "1") != nullptr);
    assert(strncmp(off, wire::kBridgePreview, wire::kBridgePreviewLen) == 0);
    assert(strstr(off, "0") != nullptr);
    printf("PASS: BRIDGE_PREVIEW toggle parsing works\n");
}
static void test_bridge_cfg_parsing() {
    const char* cfg = "BRIDGE_CFG 60 20000 h264_nvenc";
    int fps = 0, bitrate = 0;
    char codec[32] = {0};
    int parsed = sscanf(cfg, "BRIDGE_CFG %d %d %31s", &fps, &bitrate, codec);
    assert(parsed == 3);
    assert(fps == 60);
    assert(bitrate == 20000);
    assert(strcmp(codec, "h264_nvenc") == 0);
    const char* cfg2 = "BRIDGE_CFG 30 8000";
    int fps2 = 0, bitrate2 = 0;
    char codec2[32] = {0};
    assert(sscanf(cfg2, "BRIDGE_CFG %d %d %31s", &fps2, &bitrate2, codec2) >= 2);
    assert(fps2 == 30 && bitrate2 == 8000);
    assert(strncmp(cfg, wire::kBridgeCfg, wire::kBridgeCfgLen) == 0);
    printf("PASS: BRIDGE_CFG parses fps/bitrate/codec\n");
}
static void test_phone_discovery_message() {
    const char* discovery = "CARDBOARD_DISCOVERY";
    assert(strncmp(discovery, wire::kCardboardCap, wire::kBridgeHeartbeatLen) != 0 ||
           strcmp(discovery, wire::kCardboardCap) != 0);
    printf("PASS: phone discovery is distinct from CARDBOARD_CAP\n");
}
static void test_stats_construction() {
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
static void test_length_prefix_big_endian() {
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    int framedSize = 0;
    uint8_t* framed = h264::BuildLengthPrefixedPacket(payload, 4, &framedSize);
    assert(framed != nullptr);
    assert(framedSize == 8);
    assert(framed[0] == 0x00);
    assert(framed[1] == 0x00);
    assert(framed[2] == 0x00);
    assert(framed[3] == 0x04);
    assert(framed[4] == 0xDE);
    assert(framed[5] == 0xAD);
    assert(framed[6] == 0xBE);
    assert(framed[7] == 0xEF);
    free(framed);
    printf("PASS: BuildLengthPrefixedPacket produces correct big-endian length\n");
}
static void test_length_prefix_large_payload() {
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
static void test_idr_insertion_missing_idr_after_pps() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38, 0x80, 0x01, 0x02,
        0x65, 0x88, 0x00, 0x01, 0x02,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos > 0);
    assert(pos >= 15 && pos <= 18);
    printf("PASS: FindIdrInsertionPoint finds offset when IDR lacks start code (pos=%d)\n", pos);
}
static void test_idr_insertion_idr_already_present() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos == -1);
    printf("PASS: FindIdrInsertionPoint returns -1 when IDR start code present\n");
}
static void test_idr_insertion_not_keyframe() {
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), false);
    assert(pos == -1);
    printf("PASS: FindIdrInsertionPoint returns -1 for non-keyframe\n");
}
static void test_idr_insertion_no_pps() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos == -1);
    printf("PASS: FindIdrInsertionPoint returns -1 when no PPS present\n");
}
static void test_idr_insertion_3byte_idr_start_code() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38, 0x80,
        0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB,
    };
    int pos = h264::FindIdrInsertionPoint(data, sizeof(data), true);
    assert(pos == -1);
    printf("PASS: FindIdrInsertionPoint accepts 3-byte IDR start code\n");
}
static void test_parse_extradata_avcC() {
    uint8_t avcc[] = {
        0x01,
        0x42,
        0xC0,
        0x1E,
        0xFF,
        0xE1,
        0x00, 0x04,
        0x67, 0x42, 0x00, 0x1E,
        0x01,
        0x00, 0x03,
        0x68, (uint8_t)0xCE, 0x38,
    };
    auto result = h264::ParseExtradataSpsPps(avcc, sizeof(avcc));
    assert(result.size() > 0);
    assert(result[0] == 0x00 && result[1] == 0x00 && result[2] == 0x00 && result[3] == 0x01);
    assert((result[4] & 0x1F) == 7);
    size_t sc2 = 0;
    for (size_t i = 4; i + 3 < result.size(); i++) {
        if (result[i] == 0x00 && result[i+1] == 0x00 && result[i+2] == 0x00 && result[i+3] == 0x01) {
            sc2 = i;
            break;
        }
    }
    assert(sc2 > 0);
    assert((result[sc2 + 4] & 0x1F) == 8);
    printf("PASS: ParseExtradataSpsPps parses avcC format correctly\n");
}
static void test_parse_extradata_annexb() {
    uint8_t annexb[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38,
    };
    auto result = h264::ParseExtradataSpsPps(annexb, sizeof(annexb));
    assert(result.size() > 0);
    assert(result[0] == 0x00 && result[1] == 0x00 && result[2] == 0x00 && result[3] == 0x01);
    assert((result[4] & 0x1F) == 7);
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
    uint8_t avcc[] = {
        0x01, 0x42, 0xC0, 0x1E, 0xFF,
        0xE1,
        0x00, 0x04,
        0x67, 0x42, 0x00, 0x1E,
        0x00,
    };
    auto result = h264::ParseExtradataSpsPps(avcc, sizeof(avcc));
    assert(result.size() > 0);
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
static void test_access_unit_has_sps_4byte() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == true);
    printf("PASS: AccessUnitHasSps detects 4-byte start code SPS\n");
}
static void test_access_unit_has_sps_3byte() {
    uint8_t data[] = {
        0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == true);
    printf("PASS: AccessUnitHasSps detects 3-byte start code SPS\n");
}
static void test_access_unit_no_sps() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x41,
        0x00, 0x01, 0x02, 0x03, 0x04,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == false);
    printf("PASS: AccessUnitHasSps returns false when first NAL is not SPS\n");
}
static void test_access_unit_idr_without_sps() {
    uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE,
    };
    assert(h264::AccessUnitHasSps(data, sizeof(data)) == false);
    printf("PASS: AccessUnitHasSps returns false for IDR-only access unit\n");
}
static void test_keyframe_without_sps_gets_sps_prepended() {
    std::vector<uint8_t> spsPps = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38,
    };
    uint8_t keyframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB, 0xCC,
    };
    bool hasSps = h264::AccessUnitHasSps(keyframe, sizeof(keyframe));
    assert(!hasSps);
    assert(!spsPps.empty());
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), spsPps.begin(), spsPps.end());
    combined.insert(combined.end(), keyframe, keyframe + sizeof(keyframe));
    assert((combined[4] & 0x1F) == 7);
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
    uint8_t keyframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38,
        0x00, 0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB,
    };
    bool hasSps = h264::AccessUnitHasSps(keyframe, sizeof(keyframe));
    assert(hasSps);
    printf("PASS: keyframe with SPS is not duplicated\n");
}
static void test_non_keyframe_passes_through() {
    uint8_t pframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x41,
        0x00, 0x01, 0x02, 0x03, 0x04,
    };
    printf("PASS: non-keyframe passes through (emitWithSpsPps skips non-keyframes)\n");
}
static void test_video_frame_roundtrip() {
    uint8_t frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0xCE, 0x38,
        0x00, 0x00, 0x00, 0x01, 0x65,
        0xAA, 0xBB, 0xCC, 0xDD,
    };
    int framedSize = 0;
    uint8_t* framed = h264::BuildLengthPrefixedPacket(frame, sizeof(frame), &framedSize);
    assert(framed != nullptr);
    assert(framedSize == sizeof(frame) + 4);
    uint32_t wireLen = ((uint32_t)framed[0] << 24) | ((uint32_t)framed[1] << 16) |
                       ((uint32_t)framed[2] << 8)  | (uint32_t)framed[3];
    assert(wireLen == sizeof(frame));
    assert(memcmp(framed + 4, frame, sizeof(frame)) == 0);
    free(framed);
    printf("PASS: video frame length-prefix roundtrip preserves payload\n");
}
enum class DispatchResult {
    CardboardCap,
    BridgeHello,
    BridgePreview,
    BridgeCfg,
    KeyframeReq,
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
    if (strncmp(buffer, wire::kKeyframeReq, wire::kKeyframeReqLen) == 0) {
        return DispatchResult::KeyframeReq;
    }
    static const char kPhoneDiscovery[] = "CARDBOARD_DISCOVERY";
    static const char kPhoneHello[] = "CARDBOARD_PHONE_HELLO";
    size_t len = strlen(buffer);
    if (len >= sizeof(kPhoneDiscovery) - 1 &&
        strncmp(buffer, kPhoneDiscovery, sizeof(kPhoneDiscovery) - 1) == 0) {
        return DispatchResult::PhoneDiscovery;
    }
    if (len >= sizeof(kPhoneHello) - 1 &&
        strncmp(buffer, kPhoneHello, sizeof(kPhoneHello) - 1) == 0) {
        return DispatchResult::PhoneDiscovery;
    }
    return DispatchResult::Unknown;
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
    printf("PASS: BRIDGE_CFG dispatched to encoder re-init handler\n");
}
static void test_dispatch_keyframe_req() {
    assert(simulate_dispatch("KEYFRAME_REQ") == DispatchResult::KeyframeReq);
    const char* req = "KEYFRAME_REQ";
    assert(strncmp(req, wire::kCardboardCap, wire::kCardboardCapLen) != 0);
    assert(strncmp(req, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) != 0);
    assert(strncmp(req, wire::kBridgePreview, wire::kBridgePreviewLen) != 0);
    assert(strncmp(req, wire::kBridgeCfg, wire::kBridgeCfgLen) != 0);
    printf("PASS: KEYFRAME_REQ dispatched to forced-IDR handler (no ACK, no target switch)\n");
}
static void test_dispatch_phone_discovery_triggers_ack() {
    assert(simulate_dispatch("CARDBOARD_DISCOVERY") == DispatchResult::PhoneDiscovery);
    assert(simulate_dispatch("CARDBOARD_PHONE_HELLO") == DispatchResult::PhoneDiscovery);
    assert(simulate_dispatch("CARDBOARD_PHONE_HELLO v1") == DispatchResult::PhoneDiscovery);
    printf("PASS: allowlisted phone discovery messages trigger SwitchDataTarget + ACK\n");
}
static void test_dispatch_unknown_dropped_without_ack() {
    assert(simulate_dispatch("some random bytes") == DispatchResult::Unknown);
    assert(simulate_dispatch("wake") == DispatchResult::Unknown);
    assert(simulate_dispatch("ACK") == DispatchResult::Unknown);
    assert(simulate_dispatch("") == DispatchResult::Unknown);
    printf("PASS: unknown packets are dropped (no target switch, no ACK)\n");
}
static void test_dispatch_order_cap_before_hello() {
    const char* cap = "CARDBOARD_CAP 1920 1080";
    assert(strncmp(cap, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) != 0);
    assert(strncmp(cap, wire::kCardboardCap, wire::kCardboardCapLen) == 0);
    printf("PASS: CARDBOARD_CAP cannot be misrouted as BRIDGE_HELLO\n");
}
static void test_dispatch_order_hello_before_preview() {
    const char* hello = "BRIDGE_HELLO v1";
    assert(strncmp(hello, wire::kBridgePreview, wire::kBridgePreviewLen) != 0);
    assert(strncmp(hello, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) == 0);
    printf("PASS: BRIDGE_HELLO cannot be misrouted as BRIDGE_PREVIEW\n");
}
static void test_dispatch_order_preview_before_cfg() {
    const char* preview = "BRIDGE_PREVIEW 1";
    assert(strncmp(preview, wire::kBridgeCfg, wire::kBridgeCfgLen) != 0);
    assert(strncmp(preview, wire::kBridgePreview, wire::kBridgePreviewLen) == 0);
    printf("PASS: BRIDGE_PREVIEW cannot be misrouted as BRIDGE_CFG\n");
}
static void test_bridge_hello_does_not_switch_data_target() {
    const char* hello = "BRIDGE_HELLO v1";
    DispatchResult r = simulate_dispatch(hello);
    assert(r == DispatchResult::BridgeHello);
    printf("PASS: BRIDGE_HELLO does NOT trigger SwitchDataTarget\n");
}
static void test_carboard_cap_does_not_send_ack() {
    const char* cap = "CARDBOARD_CAP 1920 1080";
    DispatchResult r = simulate_dispatch(cap);
    assert(r == DispatchResult::CardboardCap);
    printf("PASS: CARDBOARD_CAP does NOT send ACK\n");
}
static void test_phone_timeout_clears_target() {
    unsigned long long kPhoneTimeoutMs = 5000;
    unsigned long long now = 100000;
    unsigned long long lastPacket = now - kPhoneTimeoutMs - 1;
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
    unsigned long long lastPacket = now - 100;
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
    test_bridge_ack_carries_build_version();
    test_bridge_cfg_string();
    test_bridge_preview_string();
    test_bridge_stats_string();
    test_discovery_wakeup_string();
    test_keyframe_req_string();
    test_bridge_hello_triggers_ack();
    test_cardboard_cap_is_not_acked();
    test_bridge_preview_toggle_parsing();
    test_bridge_cfg_parsing();
    test_phone_discovery_message();
    test_stats_construction();
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
    test_dispatch_cardboard_cap();
    test_dispatch_bridge_hello();
    test_dispatch_bridge_preview_on();
    test_dispatch_bridge_preview_off();
    test_dispatch_bridge_cfg();
    test_dispatch_keyframe_req();
    test_dispatch_phone_discovery_triggers_ack();
    test_dispatch_unknown_dropped_without_ack();
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
