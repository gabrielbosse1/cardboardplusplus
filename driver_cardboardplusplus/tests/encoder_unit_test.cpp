// Encoder unit tests: H.264 helper matrix (Phase P, pure — no FFmpeg/Vulkan)
// plus, under CBPP_ENCODER_E2E_TEST, the real VideoEncoder internals running
// headless like the vulkan_pipeline_smoke phase C (lavapipe compose + libx264).
//
// Phase P pins the two inline helpers shared by the encoder and the UDP
// transport: the IDR-quirk insertion point scan (a past bug inserted an empty
// IDR NAL mid-access-unit and corrupted every keyframe) and the 4-byte
// big-endian length header the phone reassembler depends on.
//
// Build (pure): g++ -std=c++17 -O2 -Wall -Wextra -I driver_cardboardplusplus/include
//               -o encoder_unit_tests driver_cardboardplusplus/tests/encoder_unit_test.cpp
// Build (e2e):  same + -DCBPP_ENCODER_E2E_TEST and the real encoder sources
//               (wired in CI like the phase C compile).
#include "H264Utils.h"
#include "EncoderSettings.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "== PASS" : "== FAIL", what);
    if (!ok) g_failures++;
}

// --- byte-string builders ----------------------------------------------------
struct NalBuilder {
    std::vector<uint8_t> bytes;
    void sc4() { bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x01}); }
    void sc3() { bytes.insert(bytes.end(), {0x00, 0x00, 0x01}); }
    void nal(uint8_t type, uint8_t refIdc, std::initializer_list<uint8_t> payload) {
        bytes.push_back((uint8_t)((refIdc << 5) | type));
        bytes.insert(bytes.end(), payload);
    }
};

// Phase P1: FindIdrInsertionPoint matrix --------------------------------------
// The historical quirk: some encoder builds emit the IDR slice header directly
// after the PPS NAL with no start code between them. OnEncodedPacket inserts a
// "00 00 00 01 65" marker there — but ONLY there. Any start code after the PPS
// means the access unit is already structured (SEI/AUD/filler/IDR all present)
// and inserting would split it; a type-5 byte with nal_ref_idc==0 is not an
// IDR slice header.
static void TestFindIdrInsertionPoint() {
    printf("--- A1: FindIdrInsertionPoint matrix ---\n");

    // 4-byte SC + PPS(0x68, ref 0) + IDR slice header (0x65 = ref 3, type 5)
    // directly after PPS payload — the quirk shape. Must insert before 0x65.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A, 0x99, 0x49});          // PPS
        size_t sliceOff = b.bytes.size();
        b.nal(5, 3, {0x91, 0xE0, 0x11, 0x22, 0x33, 0x44}); // IDR slice header, no SC
        b.bytes.insert(b.bytes.end(), {0xAA, 0xBB});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == (int)sliceOff, "A1 quirk shape: 4B-SC PPS + bare IDR slice -> inserts at slice");
    }
    // 3-byte SC PPS: the outer scan matches only 4-byte start codes, so a
    // 3-byte-SC PPS is not found and nothing is inserted (the phone still
    // detects the keyframe by scanning payload NAL types — pinned as current
    // behavior, not an endorsement).
    {
        NalBuilder b;
        b.sc3(); b.nal(8, 0, {0x2A, 0x99, 0x49});
        b.nal(5, 3, {0x91, 0xE0});
        b.bytes.insert(b.bytes.end(), {0xAA, 0xBB, 0xCC});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 3-byte SC PPS is not matched (no insert, pinned)");
    }
    // PPS then SEI (type 6) with a start code between: structured AU -> never.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        b.sc4(); b.nal(6, 0, {0x01, 0x02, 0x03});          // SEI
        b.nal(5, 3, {0x91, 0xE0});                          // IDR slice w/o SC
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 SEI between PPS and IDR -> no insert");
    }
    // PPS then IDR WITH its own start code: structured -> never.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        b.sc4(); b.nal(5, 3, {0x91, 0xE0});
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 IDR after PPS with start code -> no insert");
    }
    // PPS then filler (type 12) then IDR: any start code after PPS -> never.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        b.sc4(); b.nal(12, 0, {0xFF, 0xFF});
        b.nal(5, 3, {0x91});
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 filler NAL after PPS -> no insert");
    }
    // AUD (type 9) right after PPS: same rule.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        b.sc4(); b.nal(9, 0, {0x10});
        b.nal(5, 3, {0x91});
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 AUD after PPS -> no insert");
    }
    // Non-keyframe: never, even with a bare type-5-looking byte.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        b.nal(5, 3, {0x91});
        b.bytes.insert(b.bytes.end(), {0x55, 0x66});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), false);
        Check(at == -1, "A1 non-keyframe -> never insert");
    }
    // Tiny packet (size <= 10): never.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 size<=10 -> never insert");
    }
    // Type-5 byte with nal_ref_idc == 0 (0x05): not an IDR slice header.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A, 0x99});
        b.nal(5, 0, {0x91});
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 type-5 with nal_ref_idc=0 -> no insert");
    }
    // Multi-slice IDR: both slices bare after PPS -> insert before the FIRST.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        size_t first = b.bytes.size();
        b.nal(5, 3, {0x91});
        b.nal(5, 3, {0x92});
        b.bytes.insert(b.bytes.end(), {0x77});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == (int)first, "A1 multi-slice IDR -> inserts before the first slice");
    }
    // Type-5 byte beyond the 32-byte search window after PPS -> no insert.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        for (int i = 0; i < 40; i++) b.bytes.push_back(0x7F);  // filler bytes, no type-5
        b.nal(5, 3, {0x91});
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 bare slice beyond the 32-byte window -> no insert");
    }
    // Type-5 byte preceded by a start code inside the window -> no insert.
    {
        NalBuilder b;
        b.sc4(); b.nal(8, 0, {0x2A});
        b.sc3(); b.nal(5, 3, {0x91});
        b.bytes.insert(b.bytes.end(), {0x55});
        int at = h264::FindIdrInsertionPoint(b.bytes.data(), (int)b.bytes.size(), true);
        Check(at == -1, "A1 slice preceded by start code -> no insert");
    }
}

// Phase P2: BuildLengthPrefixedPacket big-endian framing ----------------------
// The phone reassembler reads frame_len = BE32(header) and then frame_len
// bytes; any non-BE framing desyncs the whole stream.
static void TestBuildLengthPrefixedPacket() {
    printf("--- A2: BuildLengthPrefixedPacket BE framing ---\n");
    struct Case { int size; uint8_t b0, b1, b2, b3; const char* what; };
    Case cases[] = {
        { 0,        0x00, 0x00, 0x00, 0x00, "A2 size 0 -> header 00 00 00 00" },
        { 1,        0x00, 0x00, 0x00, 0x01, "A2 size 1 -> header 00 00 00 01" },
        { 255,      0x00, 0x00, 0x00, 0xFF, "A2 size 255 -> header 00 00 00 FF" },
        { 256,      0x00, 0x00, 0x01, 0x00, "A2 size 256 -> header 00 00 01 00" },
        { 65535,    0x00, 0x00, 0xFF, 0xFF, "A2 size 65535 -> header 00 00 FF FF" },
        { 16777216, 0x01, 0x00, 0x00, 0x00, "A2 size 2^24 -> header 01 00 00 00" },
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> payload((size_t)c.size);
        for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 13 + 5);
        int framedSize = 0;
        uint8_t* framed = h264::BuildLengthPrefixedPacket(
            payload.data(), c.size, &framedSize);
        bool ok = framed != nullptr && framedSize == c.size + 4;
        if (ok) ok = framed[0] == c.b0 && framed[1] == c.b1 && framed[2] == c.b2 && framed[3] == c.b3;
        if (ok && c.size > 0) {
            ok = memcmp(framed + 4, payload.data(), (size_t)c.size) == 0;
        }
        Check(ok, c.what);
        free(framed);
    }
}

// Phase P3: SanitizeEncoderDims / ValidateEncoderSettings matrix --------------
// The rules live in EncoderSettings.h (single source of truth with the
// driver). Dimensions are floored to multiples of 16 — in particular the
// 1920x1080 default becomes 1920x1072, and a request of 1081 floors to 1072
// (NOT 1080: 1080 is not 16-aligned). The tests pin what actually ships.
static void TestEncoderSettingsMatrix() {
    printf("--- A7: encoder dims/settings validation matrix ---\n");
    struct DimsCase { int w, h; bool pass; int outW, outH; const char* what; };
    DimsCase dc[] = {
        { 319, 1080, false,   0,    0,   "A7 319 wide -> reject" },
        { 320, 1080, true,  320, 1072,   "A7 320 wide -> pass" },
        { 320,  179, false,   0,    0,   "A7 179 high -> reject (below 180)" },
        { 320,  180, true,  320,  176,   "A7 180 high floors to 176 (still >= 160)" },
        {1081, 1080, true, 1072, 1072,   "A7 1081 floors to 1072 (16-aligned; not 1080)" },
        {1920, 1080, true, 1920, 1072,   "A7 production default 1920x1080 -> 1920x1072" },
        {7680, 7680, true, 7680, 7680,   "A7 7680 upper bound passes unchanged" },
        {7681, 1080, false,   0,    0,   "A7 7681 wide -> reject" },
        {1080, 7681, false,   0,    0,   "A7 7681 high -> reject" },
        {   0, 1080, false,   0,    0,   "A7 zero width -> reject" },
        {  -1,  540, false,   0,    0,   "A7 negative width -> reject" },
        {  335, 2160, true,  320, 2160,  "A7 335 floors to 320 (2160 stays, 16-aligned)" },
    };
    for (const DimsCase& c : dc) {
        int w = c.w, h = c.h;
        bool ok = cbpp::SanitizeEncoderDims(w, h);
        Check(ok == c.pass && (!c.pass || (w == c.outW && h == c.outH)), c.what);
    }
    // Settings validation: malformed values keep the live encoder values
    // (ApplyEncoderSettings logs and stores the kept ones; the running encoder
    // is only rebuilt for meaningful CHANGES, so a garbage datagram is inert).
    {
        const int curW = 1920, curH = 1072, curFps = 60, curBps = 20000000;
        auto v = cbpp::ValidateEncoderSettings(0, 0, 0, 0, curW, curH, curFps, curBps);
        Check(!v.dimsValid && v.width == curW && v.height == curH &&
              v.fps == curFps && v.bitrateBps == curBps,
              "A7 all-invalid request keeps every live value");
        v = cbpp::ValidateEncoderSettings(2880, 1632, 90, 30000000, curW, curH, curFps, curBps);
        Check(v.dimsValid && v.width == 2880 && v.height == 1632 &&
              v.fps == 90 && v.bitrateBps == 30000000,
              "A7 valid request passes through untouched");
        v = cbpp::ValidateEncoderSettings(curW, curH, 121, 500, curW, curH, curFps, curBps);
        Check(v.fps == curFps && v.bitrateBps == curBps && v.fpsAdjusted && v.bitrateAdjusted,
              "A7 fps 121 and bitrate 500 bps fall back to live values");
        v = cbpp::ValidateEncoderSettings(curW, curH, 1, 1000, curW, curH, curFps, curBps);
        Check(v.fps == 1 && v.bitrateBps == 1000 && !v.fpsAdjusted && !v.bitrateAdjusted,
              "A7 fps 1 and bitrate 1 kbps are the accepted lower bounds");
        v = cbpp::ValidateEncoderSettings(curW, curH, 120, 100 * 1000 * 1000, curW, curH, curFps, curBps);
        Check(v.fps == 120 && v.bitrateBps == 100 * 1000 * 1000,
              "A7 fps 120 and bitrate 100 Mbps are the accepted upper bounds");
        v = cbpp::ValidateEncoderSettings(curW, curH, curFps, -1, curW, curH, curFps, curBps);
        Check(v.bitrateBps == curBps && !v.bitrateAdjusted,
              "A7 negative bitrate means 'keep' (BRIDGE_CFG omission), not an error");
    }
}

int main() {
    printf("=== Encoder unit tests (phase P: pure helpers) ===\n");
    TestFindIdrInsertionPoint();
    TestBuildLengthPrefixedPacket();
    TestEncoderSettingsMatrix();
#ifdef CBPP_ENCODER_E2E_TEST
    RunEncoderE2EPhase();
#endif
    printf("=== Encoder unit tests: %s (%d failures) ===\n",
           g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
