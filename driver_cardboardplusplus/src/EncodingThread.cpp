#include "HmdDriver.h"
#include "DriverLog.h"
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>
using namespace vr;
namespace {
// Any encode stage slower than this is reported with its name; the compositor
// log can only say the render thread stalled, not which stage held it up.
constexpr long long kSlowStageUs = 200000;
}  // namespace
// Encoding thread loop started from Activate; waits for Present to queue a frame, encodes it, then signals idle.
void HmdDriver::EncodingThreadFunc()
{
    while (m_encodingRunning) {
        std::unique_lock<std::mutex> lock(m_encodeMutex);
#ifdef _WIN32
        // Windows-only idle heartbeat: when SteamVR stops presenting (no app,
        // dashboard idle), no new frames reach this thread and the UDP stream
        // freezes on the last picture — a late-joining bridge or phone then
        // never sees an IDR and shows nothing. Re-encode the last composed
        // NV12 every 100 ms so the stream stays decodable while idle (the
        // encoder's short GOP turns every 10th re-send into an IDR). Linux
        // keeps the original blocking wait (protected baseline).
        bool queued = m_encodeCv.wait_for(lock, std::chrono::milliseconds(100),
                                          [this] { return m_frameQueued || !m_encodingRunning; });
        if (!m_encodingRunning) break;
        if (!queued) {
            lock.unlock();
            HeartbeatEncode();
            continue;
        }
#else
        m_encodeCv.wait(lock, [this] { return m_frameQueued || !m_encodingRunning; });
        if (!m_encodingRunning) break;
#endif
        m_frameQueued = false;
        PendingFrame frame = m_pendingFrame;
        lock.unlock();
        EncodePendingFrame(frame);
        {
            std::lock_guard<std::mutex> lock(m_encodeDoneMutex);
            m_encodeDone = true;
        }
        m_encodeDoneCv.notify_all();
    }
}

// Re-encodes the last produced frame while SteamVR is idle (see
// EncodingThreadFunc). Runs on the encoding thread; the encoder lock is the
// same one EncodePendingFrame holds, so the frame source cannot change under
// us. Done-flag bookkeeping is deliberately skipped: no Present queued a frame.
void HmdDriver::HeartbeatEncode()
{
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    if (!m_encoderInitialized || !m_pVideoEncoder) return;
    if (!m_pVideoEncoder->ReencodeLastFrame(m_lastFramePts + 1)) return;
    m_lastFramePts++;
}
// Runs the full GPU compose + readback + NV12 convert + FFmpeg encode chain for one queued frame.
void HmdDriver::EncodePendingFrame(const PendingFrame& frame)
{
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    if (!m_encoderInitialized || !m_pVideoEncoder) return;
    if (frame.layers.empty()) return;
    static bool loggedFirstEncode = false;
    if (!loggedFirstEncode) {
        loggedFirstEncode = true;
        DriverLog("Encode: first frame, layers=%zu", frame.layers.size());
    }
    auto stageStart = std::chrono::steady_clock::now();
    auto noteStage = [&](const char* name) {
        long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - stageStart).count();
        if (us >= kSlowStageUs) {
            static unsigned s_count = 0;
            if (++s_count <= 5 || s_count % 20 == 0) {
                DriverLog("[STALL] encode %s took %lld ms (stalls=%u)", name, us / 1000, s_count);
            }
        }
        stageStart = std::chrono::steady_clock::now();
    };
    std::vector<GpuImage> lefts, rights;
    lefts.reserve(frame.layers.size());
    rights.reserve(frame.layers.size());
    for (const auto& l : frame.layers) {
        lefts.push_back(l.left);
        rights.push_back(l.right);
    }
    std::vector<VideoEncoder::LayerBounds> lbL, lbR;
    lbL.reserve(frame.layers.size());
    lbR.reserve(frame.layers.size());
    for (const auto& l : frame.layers) {
        lbL.push_back({ l.boundsLeft.uMin, l.boundsLeft.vMin, l.boundsLeft.uMax, l.boundsLeft.vMax });
        lbR.push_back({ l.boundsRight.uMin, l.boundsRight.vMin, l.boundsRight.uMax, l.boundsRight.vMax });
    }
    if (!m_pVideoEncoder->ComposeSBSGPU(lefts, rights, lbL, lbR)) {
        DriverLog("ComposeSBSGPU failed for pts=%lld", (long long)frame.pts);
        return;
    }
    static bool loggedCompose = false;
    if (!loggedCompose) { loggedCompose = true; DriverLog("Encode: compose ok"); }
    noteStage("ComposeSBSGPU");
    if (!m_pVideoEncoder->ReadbackToBuffer()) {
        // Rate-limited: at frame rate this floods the SteamVR log, and the log
        // write takes a SteamVR-internal lock on the encode thread.
        static unsigned s_readbackFailCount = 0;
        if (++s_readbackFailCount <= 5 || s_readbackFailCount % 60 == 0) {
            DriverLog("ReadbackToBuffer failed for pts=%lld (drop=%u)",
                      (long long)frame.pts, s_readbackFailCount);
        }
        noteStage("ReadbackToBuffer");
        return;
    }
    static bool loggedReadback = false;
    if (!loggedReadback) { loggedReadback = true; DriverLog("Encode: readback ok"); }
    noteStage("ReadbackToBuffer");
    if (!m_pVideoEncoder->ConvertFrame()) {
        DriverLog("ConvertFrame failed for pts=%lld", (long long)frame.pts);
        return;
    }
    static bool loggedConvert = false;
    if (!loggedConvert) { loggedConvert = true; DriverLog("Encode: convert ok"); }
    noteStage("ConvertFrame");
    if (!m_pVideoEncoder->FinishEncode(frame.pts)) {
        DriverLog("FinishEncode failed for pts=%lld", (long long)frame.pts);
    } else {
        m_lastFramePts = frame.pts;
    }
    static bool loggedFinish = false;
    if (!loggedFinish) { loggedFinish = true; DriverLog("Encode: finish ok"); }
    noteStage("FinishEncode");
}