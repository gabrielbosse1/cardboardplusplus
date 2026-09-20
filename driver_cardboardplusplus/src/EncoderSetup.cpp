#include "HmdDriver.h"
#include "DriverLog.h"
#include <cstring>
using namespace vr;
// Creates VideoEncoder at the default 1920x1080@60 with stored caps applied; called from HmdDriver::Activate.
bool HmdDriver::InitializeVideoEncoder()
{
    DriverLog("========================================");
    DriverLog("Initializing Video Encoder...");
    DriverLog("========================================");
    if (!m_pD3D11Device || !m_pD3D11DeviceContext) {
        DriverLog("Cannot initialize encoder: D3D device not available!");
        return false;
    }
    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder!");
        return false;
    }
    m_encoderW = 1920;
    m_encoderH = 1080;
    m_encoderFps = 60;
    m_encoderBitrate = 20000000;
    m_encoderUseGpu = false;
    SanitizeEncoderDims(m_encoderW, m_encoderH);
    ClampEncoderToCap();
    int width = m_encoderW;
    int height = m_encoderH;
    int fps = m_encoderFps;
    int bitrate = m_encoderBitrate;
    bool useGpuEncoding = m_encoderUseGpu;
    DriverLog("Encoder configuration:");
    DriverLog("  Resolution: %dx%d (SBS: %dx%d per eye)", width, height, width / 2, height);
    DriverLog("  FPS: %d", fps);
    DriverLog("  Bitrate: %d bps (%d kbps)", bitrate, bitrate / 1000);
    DriverLog("  GPU Encoding: %s", useGpuEncoding ? "YES" : "NO");
    RegisterEncoderCallbacks(m_pVideoEncoder);
    if (!m_pVideoEncoder->Initialize(m_pD3D11Device, m_pD3D11DeviceContext, width, height, fps, bitrate, useGpuEncoding)) {
        DriverLog("VideoEncoder::Initialize failed!");
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return false;
    }
    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Video Encoder initialized successfully!");
    return true;
}
// Tears down and deletes the encoder; called from Deactivate and before every re-init.
void HmdDriver::ShutdownVideoEncoder()
{
    DriverLog("Shutting down Video Encoder...");
    if (m_pVideoEncoder) {
        m_pVideoEncoder->Shutdown();
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
    }
    m_encoderInitialized = false;
    DriverLog("Video Encoder shutdown complete.");
}
// Scales m_encoderW/H down to fit the phone decoder cap while keeping 16-pixel alignment; no-op without a stored cap.
void HmdDriver::ClampEncoderToCap()
{
    if (m_pendingCapW <= 0 || m_pendingCapH <= 0) {
        return;
    }
    double scale = 1.0;
    double sx = (double)m_pendingCapW / (double)m_encoderW;
    double sy = (double)m_pendingCapH / (double)m_encoderH;
    if (sx < scale) scale = sx;
    if (sy < scale) scale = sy;
    if (scale >= 0.999) {
        return;
    }
    int newW = (int)(m_encoderW * scale);
    int newH = (int)(m_encoderH * scale);
    newW = (newW / 16) * 16;
    newH = (newH / 16) * 16;
    if (newW < 320 || newH < 160) {
        DriverLog("Hardware cap %dx%d would clamp to degenerate %dx%d, keeping %dx%d",
                  m_pendingCapW, m_pendingCapH, newW, newH, m_encoderW, m_encoderH);
        return;
    }
    {
        DriverLog("Clamping encoder to hardware cap %dx%d: %dx%d -> %dx%d",
                  m_pendingCapW, m_pendingCapH, m_encoderW, m_encoderH, newW, newH);
        m_encoderW = newW;
        m_encoderH = newH;
    }
}
// Validates w/h against the 320x180..7680 range and rounds down to multiples of 16 for the H.264 encoder.
bool HmdDriver::SanitizeEncoderDims(int& w, int& h)
{
    if (w < 320 || h < 180 || w > 7680 || h > 7680)
        return false;
    w = (w / 16) * 16;
    h = (h / 16) * 16;
    return w >= 320 && h >= 160;
}
// Routes encoded packets to OnEncodedPacket (UDP fan-out) and telemetry to the bridge SHM publisher.
void HmdDriver::RegisterEncoderCallbacks(VideoEncoder* enc)
{
    enc->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });
    enc->SetTelemetryCallback([this](const cbpp::PayloadTelemetry& t) {
        if (m_bridgeInitialized.load(std::memory_order_relaxed) && m_bridgeServer.running()) {
            m_bridgeServer.PublishTelemetry(t);
        }
    });
}
// Validates and stores w/h/fps/bitrate/gpu choice labeled by reason, then rebuilds the encoder when values change meaningfully.
bool HmdDriver::ApplyEncoderSettings(int w, int h, int fps, int bitrateBps, bool useGpu, const char* reason)
{
    WaitEncoderIdle();
    std::lock_guard<std::mutex> lock(m_encoderMutex);
    int newW = w, newH = h;
    if (!SanitizeEncoderDims(newW, newH)) {
        DriverLog("%s: rejecting bad dims %dx%d, keeping %dx%d",
                  reason, w, h, m_encoderW, m_encoderH);
        newW = m_encoderW;
        newH = m_encoderH;
    }
    int newFps = (fps >= 1 && fps <= 120) ? fps : m_encoderFps;
    int newBitrate = (bitrateBps >= 1000 && bitrateBps <= 100 * 1000 * 1000) ? bitrateBps : m_encoderBitrate;
    bool newGpu = useGpu;
    int oldW = m_encoderW;
    int oldH = m_encoderH;
    int oldFps = m_encoderFps;
    int oldBitrate = m_encoderBitrate;
    bool oldGpu = m_encoderUseGpu;
    m_encoderW = newW;
    m_encoderH = newH;
    m_encoderFps = newFps;
    m_encoderBitrate = newBitrate;
    m_encoderUseGpu = newGpu;
    ClampEncoderToCap();
    if (!m_encoderInitialized || !m_pVideoEncoder) {
        DriverLog("%s stored (encoder not ready, applied on init)", reason);
        return true;
    }
    bool meaningful = (m_encoderW != oldW || m_encoderH != oldH || m_encoderFps != oldFps ||
                       m_encoderBitrate != oldBitrate || m_encoderUseGpu != oldGpu);
    if (!meaningful)
        return false;
    DriverLog("Re-initializing encoder from %s: %dx%d @%d fps, %d kbps, gpu=%d",
              reason, m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate / 1000, m_encoderUseGpu ? 1 : 0);
    m_pVideoEncoder->Shutdown();
    delete m_pVideoEncoder;
    m_pVideoEncoder = nullptr;
    m_encoderInitialized = false;
    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder during %s re-init!", reason);
        return false;
    }
    RegisterEncoderCallbacks(m_pVideoEncoder);
    if (!m_pVideoEncoder->Initialize(m_pD3D11Device, m_pD3D11DeviceContext,
                                     m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate, m_encoderUseGpu)) {
        DriverLog("VideoEncoder re-init failed under %s!", reason);
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return false;
    }
    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Encoder re-initialized at %dx%d @%d fps from %s", m_encoderW, m_encoderH, m_encoderFps, reason);
    return true;
}
// Maps BRIDGE_CFG fps/bitrate/codec onto the encoder settings path; codec names select the GPU or CPU encoder.
bool HmdDriver::ApplyBridgeCfg(int fps, int bitrateKbps, const char* codec)
{
    bool newGpu = m_encoderUseGpu;
    if (codec) {
        if (strcmp(codec, "libx264") == 0 || strcmp(codec, "cpu") == 0) newGpu = false;
        else if (strcmp(codec, "h264_amf") == 0 || strcmp(codec, "h264_nvenc") == 0
                 || strcmp(codec, "h264_qsv") == 0 || strcmp(codec, "gpu") == 0) newGpu = true;
    }
    int bps = (bitrateKbps >= 1 && bitrateKbps <= 100000) ? bitrateKbps * 1000 : -1;
    return ApplyEncoderSettings(m_encoderW, m_encoderH, fps, bps, newGpu, "BRIDGE_CFG");
}
// Stores the phone CARDBOARD_CAP size and clamps the running encoder to it; held for init when the encoder is absent.
bool HmdDriver::ApplyHardwareCap(int capW, int capH)
{
    if (capW < 320 || capH < 180 || capW > 7680 || capH > 7680) {
        DriverLog("Hardware cap %dx%d out of range (320x180-7680), ignored", capW, capH);
        return false;
    }
    capW &= ~1;
    capH &= ~1;
    int clampedW, clampedH, fps, bitrate;
    bool gpu;
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        m_pendingCapW = capW;
        m_pendingCapH = capH;
        if (!m_encoderInitialized || !m_pVideoEncoder) {
            DriverLog("Hardware cap %dx%d received (encoder not ready, applied on init)", capW, capH);
            return true;
        }
        int oldW = m_encoderW;
        int oldH = m_encoderH;
        ClampEncoderToCap();
        if (m_encoderW == oldW && m_encoderH == oldH) {
            DriverLog("Hardware cap %dx%d >= current encoder %dx%d, no change", capW, capH, oldW, oldH);
            return false;
        }
        DriverLog("Re-initializing encoder under hardware cap %dx%d: %dx%d -> %dx%d",
                  capW, capH, oldW, oldH, m_encoderW, m_encoderH);
        clampedW = m_encoderW;
        clampedH = m_encoderH;
        fps = m_encoderFps;
        bitrate = m_encoderBitrate;
        gpu = m_encoderUseGpu;
    }
    return ApplyEncoderSettings(clampedW, clampedH, fps, bitrate, gpu, "hardware-cap");
}