#include "HmdDriver.h"
#include "DriverLog.h"
#include <mutex>
#include <utility>
#include <vector>
using namespace vr;
// Encoding thread loop started from Activate; waits for Present to queue a frame, encodes it, then signals idle.
void HmdDriver::EncodingThreadFunc()
{
    while (m_encodingRunning) {
        std::unique_lock<std::mutex> lock(m_encodeMutex);
        m_encodeCv.wait(lock, [this] { return m_frameQueued || !m_encodingRunning; });
        if (!m_encodingRunning) break;
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
// Runs the full GPU compose + readback + NV12 convert + FFmpeg encode chain for one queued frame.
void HmdDriver::EncodePendingFrame(const PendingFrame& frame)
{
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    if (!m_encoderInitialized || !m_pVideoEncoder) return;
    if (frame.layers.empty()) return;
    std::vector<std::pair<HANDLE, HANDLE>> handles;
    handles.reserve(frame.layers.size());
    for (const auto& l : frame.layers)
        handles.push_back({ l.hLeft, l.hRight });
    std::vector<ID3D11Texture2D*> pLefts, pRights;
    if (!m_pVideoEncoder->OpenSharedEyeTextures(handles, pLefts, pRights)) {
        DriverLog("OpenSharedEyeTextures failed for pts=%lld", (long long)frame.pts);
        return;
    }
    if (pLefts.empty()) {
        m_pVideoEncoder->ReleaseEyeTextures();
        return;
    }
    std::vector<VideoEncoder::LayerBounds> lbL, lbR;
    lbL.reserve(frame.layers.size());
    lbR.reserve(frame.layers.size());
    for (const auto& l : frame.layers) {
        lbL.push_back({ l.boundsLeft.uMin, l.boundsLeft.vMin, l.boundsLeft.uMax, l.boundsLeft.vMax });
        lbR.push_back({ l.boundsRight.uMin, l.boundsRight.vMin, l.boundsRight.uMax, l.boundsRight.vMax });
    }
    if (!m_pVideoEncoder->ComposeSBSGPU(pLefts, pRights, lbL, lbR)) {
        DriverLog("ComposeSBSGPU failed for pts=%lld", (long long)frame.pts);
        m_pVideoEncoder->ReleaseEyeTextures();
        return;
    }
    if (!m_pVideoEncoder->ReadbackToBuffer()) {
        DriverLog("ReadbackToBuffer failed for pts=%lld", (long long)frame.pts);
        m_pVideoEncoder->ReleaseEyeTextures();
        return;
    }
    if (!m_pVideoEncoder->SwsConvert()) {
        DriverLog("SwsConvert failed for pts=%lld", (long long)frame.pts);
        return;
    }
    if (!m_pVideoEncoder->FinishEncode(frame.pts)) {
        DriverLog("FinishEncode failed for pts=%lld", (long long)frame.pts);
    }
}