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

// ===== Phase E: real VideoEncoder internals (CBPP_ENCODER_E2E_TEST) =========
// Links the production encoder sources on top of lavapipe (VulkanContext +
// VulkanCompose compose the frame; libx264 encodes it), exactly like the
// vulkan_pipeline_smoke phase C, and pins the encoder's internal contracts:
// keyframe policy, GOP cadence + heartbeat pts, heartbeat edge cases, GPU/CPU
// source switching, duplicate-frame telemetry arithmetic, shutdown drain and
// the multi-hardware fallback order.
//
// Outside SteamVR the OpenVR inline accessors dereference a null driver
// context; a stub context is installed instead. It serves a FAKE IVRDriverLog
// so every ENCODER_LOG line lands in a capture sink — the fallback-order and
// lifecycle tests assert against the real log lines the driver would emit.
#ifdef CBPP_ENCODER_E2E_TEST

// vulkan.h FIRST (like the smoke test): it declares the vk* prototypes unless
// VK_NO_PROTOTYPES was defined first, and VulkanContext.h defines that macro
// for its own dynamic-loader path.
#include <vulkan/vulkan.h>
#include "VulkanContext.h"
#include "VulkanCompose.h"
#include "VideoEncoder.h"
#include "openvr_driver.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace {

struct CapturedLog {
    std::mutex m;
    std::vector<std::string> lines;
    void add(const char* msg) {
        std::lock_guard<std::mutex> l(m);
        lines.emplace_back(msg ? msg : "");
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> l(m);
        return lines;
    }
};
CapturedLog g_capturedLog;

struct FakeLog final : vr::IVRDriverLog {
    void Log(const char* pchLogMessage) override { g_capturedLog.add(pchLogMessage); }
};
FakeLog g_fakeLog;

struct StubDriverContext final : vr::IVRDriverContext {
    void* GetGenericInterface(const char* name, vr::EVRInitError* e) override {
        if (e) *e = vr::VRInitError_None;
        if (name && strncmp(name, "IVRDriverLog", 12) == 0) return &g_fakeLog;
        return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override { return 0; }
};
StubDriverContext g_stubContext;

}  // namespace

// Direct DriverLog calls (ComputeFrameHash's NV-MEAN line) also land in the
// capture; ENCODER_LOG lines arrive through the fake log interface above.
void DriverLog(const char* pFormat, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, pFormat);
    vsnprintf(buf, sizeof(buf), pFormat, ap);
    va_end(ap);
    g_capturedLog.add(buf);
}

namespace {

struct PacketSink {
    std::mutex m;
    std::vector<std::vector<uint8_t>> data;
    std::vector<int64_t> pts;
    std::vector<bool> key;
    void attach(VideoEncoder& enc) {
        enc.SetEncodedPacketCallback([this](uint8_t* d, int s, int64_t p, bool kf) {
            std::lock_guard<std::mutex> l(m);
            data.emplace_back(d, d + s);
            pts.push_back(p);
            key.push_back(kf);
        });
    }
    size_t size() {
        std::lock_guard<std::mutex> l(m);
        return data.size();
    }
    std::vector<int64_t> ptsSnapshot() {
        std::lock_guard<std::mutex> l(m);
        return pts;
    }
    std::vector<bool> keySnapshot() {
        std::lock_guard<std::mutex> l(m);
        return key;
    }
};

bool HasNalType(const std::vector<uint8_t>& p, uint8_t type) {
    for (size_t i = 0; i + 4 < p.size(); i++) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1))) {
            size_t nal = i + (p[i + 2] == 1 ? 3 : 4);
            if (nal < p.size() && (p[nal] & 0x1F) == type) return true;
        }
    }
    return false;
}

bool IsAnnexB(const std::vector<uint8_t>& p) {
    for (size_t i = 0; i + 4 < p.size() && i < 64; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1)))
            return true;
    }
    return false;
}

// Full production chain for one frame: compose -> readback -> convert -> encode.
bool EncodeOneFrame(VideoEncoder& enc, const GpuImage& gl, const GpuImage& gr, int64_t pts) {
    if (!enc.ComposeSBSGPU({ gl }, { gr },
                           { VideoEncoder::LayerBounds{ 0, 0, 1, 1 } },
                           { VideoEncoder::LayerBounds{ 0, 0, 1, 1 } })) return false;
    if (!enc.ReadbackToBuffer()) return false;
    if (!enc.ConvertFrame()) return false;
    return enc.FinishEncode(pts);
}

}  // namespace

static void RunEncoderE2EPhase() {
    printf("--- encoder e2e phase (real VideoEncoder on lavapipe) ---\n");
    vr::VRDriverContext() = &g_stubContext;

    VulkanContext ctx;
    bool ctxOk = ctx.Initialize(nullptr);
    Check(ctxOk, "encoder e2e: VulkanContext on lavapipe");
    if (!ctxOk) return;

    // Exportable eye images (the SteamVR IPC shape: BGRA8, dedicated
    // allocation, opaque-fd export), red left / green right.
    VkDevice dev = ctx.Device();
    VkPhysicalDevice phys = ctx.Physical();
    auto getMemFd = (PFN_vkGetMemoryFdKHR)ctx.GetDeviceProc("vkGetMemoryFdKHR");
    if (!getMemFd) { Check(false, "vkGetMemoryFdKHR available"); return; }

    struct Eye { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; int fd = -1; };
    auto makeEye = [&](uint8_t r, uint8_t g, uint8_t b) -> Eye {
        Eye e;
        VkExternalMemoryImageCreateInfo ext = {};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &ext;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = { 128, 128, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (vkCreateImage(dev, &ici, nullptr, &e.img) != VK_SUCCESS) { e.img = VK_NULL_HANDLE; return e; }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(dev, e.img, &req);
        VkExportMemoryAllocateInfo exp = {};
        exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryDedicatedAllocateInfo ded = {};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.image = e.img;
        ded.pNext = &exp;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &ded;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = 0xFFFFFFFFu;
        VkPhysicalDeviceMemoryProperties mp = {};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if ((req.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                ai.memoryTypeIndex = t; break;
            }
        }
        if (ai.memoryTypeIndex == 0xFFFFFFFFu) ai.memoryTypeIndex = 0;
        if (vkAllocateMemory(dev, &ai, nullptr, &e.mem) != VK_SUCCESS) { e.img = VK_NULL_HANDLE; return e; }
        if (vkBindImageMemory(dev, e.img, e.mem, 0) != VK_SUCCESS) { e.img = VK_NULL_HANDLE; return e; }
        // Solid fill through a staging buffer + queue copy.
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = (VkDeviceSize)128 * 128 * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer stage = VK_NULL_HANDLE;
        vkCreateBuffer(dev, &bci, nullptr, &stage);
        VkMemoryRequirements sreq = {};
        vkGetBufferMemoryRequirements(dev, stage, &sreq);
        VkMemoryAllocateInfo sai = {};
        sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        sai.allocationSize = sreq.size;
        sai.memoryTypeIndex = 0xFFFFFFFFu;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if ((sreq.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                sai.memoryTypeIndex = t; break;
            }
        }
        if (sai.memoryTypeIndex == 0xFFFFFFFFu) sai.memoryTypeIndex = 0;
        VkDeviceMemory smem = VK_NULL_HANDLE;
        void* smap = nullptr;
        bool staged = vkAllocateMemory(dev, &sai, nullptr, &smem) == VK_SUCCESS &&
                      vkBindBufferMemory(dev, stage, smem, 0) == VK_SUCCESS &&
                      vkMapMemory(dev, smem, 0, bci.size, 0, &smap) == VK_SUCCESS;
        if (staged) {
            for (int i = 0; i < 128 * 128; i++) {
                uint8_t* px = (uint8_t*)smap + (size_t)i * 4;
                px[0] = b; px[1] = g; px[2] = r; px[3] = 255;  // BGRA
            }
            VkCommandPoolCreateInfo pci = {};
            pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.queueFamilyIndex = ctx.GetQueueFamily();
            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(dev, &pci, nullptr, &pool);
            VkCommandBufferAllocateInfo cai = {};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(dev, &cai, &cmd);
            VkCommandBufferBeginInfo bi = {};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cmd, &bi);
            VkImageMemoryBarrier imb = {};
            imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imb.image = e.img;
            imb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &imb);
            VkBufferImageCopy region = {};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { 128, 128, 1 };
            vkCmdCopyBufferToImage(cmd, stage, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si = {};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(ctx.Queue(), 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(ctx.Queue());
            vkDestroyCommandPool(dev, pool, nullptr);
        }
        vkDestroyBuffer(dev, stage, nullptr);
        vkFreeMemory(dev, smem, nullptr);
        VkMemoryGetFdInfoKHR fdInfo = {};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = e.mem;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (getMemFd(dev, &fdInfo, &e.fd) != VK_SUCCESS) e.fd = -1;
        return e;
    };

    Eye eyeL = makeEye(255, 0, 0);
    Eye eyeR = makeEye(0, 255, 0);
    Check(eyeL.fd >= 0 && eyeR.fd >= 0, "encoder e2e: exportable eyes created");
    if (eyeL.fd < 0 || eyeR.fd < 0) return;
    GpuImage gl = {}; gl.handle = (uint64_t)eyeL.fd; gl.width = 128; gl.height = 128;
    gl.format = (uint32_t)VK_FORMAT_B8G8R8A8_UNORM;
    GpuImage gr = gl; gr.handle = (uint64_t)eyeR.fd;

    // ---- A5a: heartbeat + RequestKeyframe on a never-initialized encoder ----
    {
        VideoEncoder encFresh;
        Check(!encFresh.ReencodeLastFrame(0), "A5 heartbeat before any frame returns false (no crash)");
        encFresh.RequestKeyframe();  // must be a safe no-op
        encFresh.RequestKeyframe();
        Check(true, "A3 RequestKeyframe while uninitialized is safe");
        encFresh.Shutdown();         // double-safe: never initialized
        Check(true, "A5 Shutdown on a never-initialized encoder is safe");
    }

    // ---- A4: GOP cadence + heartbeat pts (unforced cadence) ----
    {
        PacketSink sink1;
        VideoEncoder enc;
        bool ok = enc.Initialize(512, 256, 30, 4000000, false, &ctx, "libx264");
        Check(ok, "A4 encoder initialized (lavapipe + libx264)");
        if (ok) {
            sink1.attach(enc);
            for (int64_t p = 0; p < 25 && ok; p++) ok = EncodeOneFrame(enc, gl, gr, p);
            Check(ok, "A4 frames 0..24 encoded through the full chain");
            // Idle heartbeats: the Windows heartbeat re-sends the last frame at
            // lastPts+1; with gop 10 the cadence carries on through them, so a
            // late joiner during idle gets an IDR within 10 re-sends (~1 s at
            // the 100 ms heartbeat).
            for (int64_t p = 25; p < 35 && ok; p++) {
                ok = enc.ReencodeLastFrame(p);
                if (!ok) break;
            }
            Check(ok, "A4 ten idle heartbeats encoded (pts 25..34)");
            enc.RequestKeyframe();
            ok = enc.ReencodeLastFrame(35) && enc.ReencodeLastFrame(36);
            Check(ok, "A4 forced-keyframe + follow-up heartbeats encoded");

            auto pts = sink1.ptsSnapshot();
            auto key = sink1.keySnapshot();
            std::lock_guard<std::mutex> l(sink1.m);
            auto& data = sink1.data;
            // pts: exactly 0..36 in order — heartbeats continue monotonically
            // past the last present frame and never collide or regress.
            bool ptsOk = pts.size() == 37;
            for (size_t i = 0; ptsOk && i < pts.size(); i++) ptsOk = pts[i] == (int64_t)i;
            Check(ptsOk, "A4 pts monotonic 0..36, one packet per frame/heartbeat, no regression");
            bool annexb = !data.empty();
            for (const auto& p : data) { if (!IsAnnexB(p)) annexb = false; }
            Check(annexb, "A4 every packet is Annex-B");
            bool idr0 = HasNalType(data[0], 5) && key[0];
            Check(idr0, "A4 first encode emits an IDR (flag + NAL type 5)");
            Check(key[10] && HasNalType(data[10], 5), "A4 GOP cadence: IDR at frame 10 (gop 10)");
            Check(key[20] && HasNalType(data[20], 5), "A4 GOP cadence: IDR at frame 20");
            bool midOk = true;
            for (int i : { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 24 }) {
                if (key[(size_t)i]) midOk = false;
            }
            Check(midOk, "A4 non-IDR between IDR frames (flags coherent with GOP)");
            Check(key[30] && HasNalType(data[30], 5),
                  "A4 heartbeat cadence: the 10th idle re-send is an IDR (late-joiner guarantee)");
            bool hbMid = true;
            for (int i : { 25, 26, 27, 28, 29, 31, 32, 33, 34 }) {
                if (key[(size_t)i]) hbMid = false;
            }
            Check(hbMid, "A4 heartbeat re-sends between IDR boundaries are non-IDR");
            Check(key[35] && HasNalType(data[35], 5),
                  "A3 KEYFRAME_REQ during idle: forced IDR on the heartbeat");
            Check(!key[36] && !HasNalType(data[36], 5),
                  "A3 RequestKeyframe arms EXACTLY one frame (consumed, second is non-IDR)");
            // A9: shutdown drain — every pts 0..36 delivered exactly once.
            std::set<int64_t> uniq(pts.begin(), pts.end());
            Check(uniq.size() == pts.size(), "A9 no duplicate pts within the session (no lost/re-sent frames)");
        }
        // The encoder scope above ended (destructor -> Shutdown); assert the
        // packet count is final: no trailing packets appear after Shutdown
        // (per-frame drain with delay 0).
        Check(sink1.size() == 37, "A9 no trailing packets after Shutdown (per-frame drain, delay 0)");
    }

    // ---- A3: keyframe policy on a present-frame stream ----
    {
        PacketSink sink2;
        VideoEncoder enc;
        bool ok = enc.Initialize(512, 256, 30, 4000000, false, &ctx, "libx264");
        if (ok) {
            sink2.attach(enc);
            for (int64_t p = 0; p <= 4 && ok; p++) ok = EncodeOneFrame(enc, gl, gr, p);
            enc.RequestKeyframe();
            ok = EncodeOneFrame(enc, gl, gr, 5);
            Check(ok, "A3 frame after RequestKeyframe encoded");
            ok = EncodeOneFrame(enc, gl, gr, 6);
            Check(ok, "A3 follow-up frame encoded");
            auto key = sink2.keySnapshot();
            std::lock_guard<std::mutex> l(sink2.m);
            Check(key[5] && HasNalType(sink2.data[5], 5),
                  "A3 RequestKeyframe makes the next present frame an IDR");
            Check(!key[6] && !HasNalType(sink2.data[6], 5),
                  "A3 ...and only that one (flag consumed, frame 6 back to non-IDR)");
        }
        if (ok) enc.Shutdown();
    }

    // ---- A5b/A6: CPU-fallback encoder (odd width refuses GPU NV12) + interleaving ----
    {
        VideoEncoder cpuEnc;
        bool ok = cpuEnc.Initialize(514, 256, 30, 4000000, false, &ctx, "libx264");
        Check(ok, "A6 CPU-fallback encoder initialized (514 wide refuses GPU NV12)");
        if (ok) {
            PacketSink sink2;
            sink2.attach(cpuEnc);
            ok = EncodeOneFrame(cpuEnc, gl, gr, 0);
            Check(ok, "A6 CPU-fallback frame encoded (swscale source)");
            Check(cpuEnc.ReencodeLastFrame(1), "A5 heartbeat after a CPU-fallback frame succeeds");
            ok = EncodeOneFrame(cpuEnc, gl, gr, 2);
            Check(ok, "A6 CPU-fallback frame 2 encoded");
            // Interleave GPU and CPU encoders — ConvertFrame must pick the
            // right source for whichever encoder runs.
            PacketSink sinkG, sinkC;
            VideoEncoder gpuEnc;
            bool gok = gpuEnc.Initialize(512, 256, 30, 4000000, false, &ctx, "libx264");
            Check(gok, "A6 GPU-NV12 encoder initialized (512 wide)");
            if (gok) {
                sinkG.attach(gpuEnc);
                sinkC.attach(cpuEnc);
                bool alt = true;
                for (int i = 0; i < 4 && alt; i++) {
                    alt = EncodeOneFrame(gpuEnc, gl, gr, 10 + 2 * i) &&
                          EncodeOneFrame(cpuEnc, gl, gr, 11 + 2 * i);
                }
                Check(alt, "A6 alternating GPU/CPU source switching produces a frame every time");
                // The captured log must be free of conversion failures.
                auto lines = g_capturedLog.snapshot();
                bool noConvFail = true;
                for (const auto& s : lines) {
                    if (s.find("swscale conversion failed") != std::string::npos ||
                        s.find("avcodec_send_frame failed") != std::string::npos) noConvFail = false;
                }
                Check(noConvFail, "A6 no conversion/encode errors logged during switching");
                Check(sinkG.size() >= 4 && sinkC.size() >= 4,
                      "A6 both encoders delivered packets every round");
                gpuEnc.Shutdown();
            }
            cpuEnc.Shutdown();
        }
    }

    // ---- A8: duplicate-frame hash + telemetry summary arithmetic ----
    {
        VideoEncoder enc;
        bool ok = enc.Initialize(512, 256, 30, 4000000, false, &ctx, "libx264");
        Check(ok, "A8 telemetry encoder initialized");
        if (ok) {
            std::mutex tm;
            std::vector<cbpp::PayloadTelemetry> summaries;
            enc.SetTelemetryCallback([&](const cbpp::PayloadTelemetry& t) {
                std::lock_guard<std::mutex> l(tm);
                summaries.push_back(t);
            });
            // 120 identical frames -> exactly one summary at the 120th encode.
            bool ok120 = true;
            for (int64_t p = 0; p < 120 && ok120; p++) ok120 = EncodeOneFrame(enc, gl, gr, p);
            Check(ok120, "A8 120 identical frames encoded (summary fired at the 120th)");
            std::vector<cbpp::PayloadTelemetry> snap;
            {
                std::lock_guard<std::mutex> l(tm);
                snap = summaries;
            }
            bool oneSummary = snap.size() == 1;
            Check(oneSummary, "A8 summary fires at summaryInterval (120 encodes)");
            if (oneSummary) {
                const auto& t = snap[0];
                bool arith = t.frames == 120 && t.summary_frames == 120 &&
                             t.dup_count == 119;  // identical content: every frame after the first is a dup
                Check(arith, "A8 identical frames: dup_count == frames-1 (hash-identical)");
                bool bounds = t.avg_encode_us <= t.max_encode_us &&
                              t.avg_interval_us <= t.max_interval_us &&
                              t.max_encode_us > 0 && t.max_interval_us > 0;
                Check(bounds, "A8 telemetry arithmetic invariants (avg <= max, all sampled)");
            }
            // Alternating content: swap each frame -> no dups. The dup hash
            // samples the NV12 buffer every 1024 bytes; at this test's
            // 256-wide SBS that is always column 0 (left eye), so the LEFT
            // eye must alternate for the sampled hash to see the change.
            Eye eyeL2 = makeEye(0, 0, 255);
            GpuImage gl2 = gl; gl2.handle = (uint64_t)eyeL2.fd;
            if (eyeL2.fd >= 0) {
                // Exactly 120 more encodes (121..240): the second summary
                // fires at the 240th, and no extra encode leaks into its
                // dup window.
                for (int64_t p = 121; p <= 240; p++) {
                    const GpuImage& left = (p & 1) ? gl2 : gl;
                    EncodeOneFrame(enc, left, gr, p);
                }
                std::vector<cbpp::PayloadTelemetry> snap2;
                {
                    std::lock_guard<std::mutex> l(tm);
                    snap2 = summaries;
                }
                bool two = snap2.size() == 2;
                Check(two, "A8 second summary fires at the next 120 encodes");
                if (two) {
                    Check(snap2[1].dup_count == 0,
                          "A8 alternating content resets the dup streak (hash changes every frame)");
                    Check(snap2[1].frames == 120 && snap2[1].summary_frames == 120,
                          "A8 per-summary counters reset after each summary");
                }
            } else {
                Check(false, "A8 blue left eye created for the alternating-content test");
            }
            enc.Shutdown();
        }
        // Heartbeat after a re-initialized encoder: a fresh encoder has no
        // frame source, so the heartbeat must refuse (stale GPU NV12 pointer
        // from the previous instance must never be reachable here).
        VideoEncoder encNew;
        bool okNew = encNew.Initialize(512, 256, 30, 4000000, false, &ctx, "libx264");
        if (okNew) {
            Check(!encNew.ReencodeLastFrame(0),
                  "A5 fresh (re-initialized) encoder refuses the heartbeat (no stale source)");
            encNew.Shutdown();
        }
    }

    // ---- A10: hardware fallback order (no hw encoders on the runner) ----
    {
        size_t logMark = g_capturedLog.snapshot().size();
        VideoEncoder enc;
        bool ok = enc.Initialize(512, 256, 30, 4000000, true, &ctx, nullptr);  // "gpu": full hw candidate chain
        Check(ok, "A10 gpu-mode encoder falls back to a working codec");
        auto lines = g_capturedLog.snapshot();
        int openIdx = -1, lastSkipIdx = -1, vaapiIdx = -1, nvencIdx = -1, qsvIdx = -1, failIdx = -1;
        for (size_t i = logMark; i < lines.size(); i++) {
            const std::string& s = lines[i];
            if (s.find("Opened H264 encoder: libx264") != std::string::npos) openIdx = (int)i;
            if (s.find("Encoder h264_vaapi") != std::string::npos && s.find("not available") != std::string::npos) vaapiIdx = (int)i;
            if (s.find("VAAPI frame pool unavailable") != std::string::npos) vaapiIdx = (int)i;
            if (s.find("Encoder h264_nvenc") != std::string::npos && s.find("not available") != std::string::npos) nvencIdx = (int)i;
            if (s.find("Encoder h264_qsv") != std::string::npos && s.find("not available") != std::string::npos) qsvIdx = (int)i;
            if (s.find("trying next encoder") != std::string::npos) lastSkipIdx = (int)i;
            if (s.find("No usable H264 encoder found") != std::string::npos) failIdx = (int)i;
        }
        Check(ok && openIdx >= 0, "A10 libx264 opened after the hardware candidates were skipped");
        Check(failIdx < 0, "A10 fallback ended in a working encoder (no total failure)");
        // Documented order vaapi -> nvenc -> qsv -> libx264: each skip that
        // fired appears before the open, and vaapi's failure precedes nvenc/qsv
        // skips when those codecs exist in this FFmpeg build.
        Check(vaapiIdx >= 0, "A10 vaapi candidate attempted and skipped/f failed first (runner has no DRM device)");
        if (nvencIdx >= 0) Check(vaapiIdx < nvencIdx, "A10 vaapi precedes nvenc in the fallback order");
        if (qsvIdx >= 0) {
            if (nvencIdx >= 0) Check(nvencIdx < qsvIdx, "A10 nvenc precedes qsv in the fallback order");
            Check(qsvIdx < openIdx, "A10 qsv precedes the libx264 open");
        }
        if (lastSkipIdx >= 0) Check(lastSkipIdx < openIdx, "A10 skips precede the software open");
        if (ok) enc.Shutdown();
    }
}

#endif  // CBPP_ENCODER_E2E_TEST

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
