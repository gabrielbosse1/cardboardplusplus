#include "HmdDriver.h"
#include "DriverLog.h"
#include <mutex>
#include <utility>
#include <vector>

using namespace vr;

// ---------------------------------------------------------------------------
// Background encoder thread.
//
// All slow GPU+CPU work for a frame happens here: open the per-layer shared
// eye copies on the encoder's own D3D11 device, composite them SBS, read
// them back, convert BGRA->NV12, H264-encode, and hand packets to
// UdpTransport via OnEncodedPacket. Present() only queues; it never blocks
// on GPU encode work.
// ---------------------------------------------------------------------------

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

void HmdDriver::EncodePendingFrame(const PendingFrame& frame)
{
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    if (!m_encoderInitialized || !m_pVideoEncoder) return;
    if (frame.layers.empty()) return;

    // Open each shared private eye copy on the encoder's own D3D11 device.
    // This device is separate from the compositor's device, so there is
    // zero contention with the compositor thread.
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

    // Phase 1: GPU work on the encoding device (composite all layers + readback).
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

    // Release shared textures before CPU work.
    m_pVideoEncoder->ReleaseEyeTextures();

    // Phase 2: CPU work — BGRA→NV12 conversion + H264 encode + UDP send.
    if (!m_pVideoEncoder->SwsConvert()) {
        DriverLog("SwsConvert failed for pts=%lld", (long long)frame.pts);
        return;
    }
    if (!m_pVideoEncoder->FinishEncode(frame.pts)) {
        DriverLog("FinishEncode failed for pts=%lld", (long long)frame.pts);
    }
}