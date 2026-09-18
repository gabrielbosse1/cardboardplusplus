#include "HmdDriver.h"
#include "DriverLog.h"
#include <cstring>

using namespace vr;

// ---------------------------------------------------------------------------
// Encoder lifecycle and configuration.
//
// Owns the VideoEncoder instance end-to-end: first-time bring-up, the
// hardware-decoder-cap reconfiguration path (a "CARDBOARD_CAP" discovery
// packet can clamp the encoder to the phone's decode resolution — see
// Discovery.cpp and ClampEncoderToCap below), and teardown.
// ---------------------------------------------------------------------------

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

    // SBS resolution: 1440x1620 per eye -> 2880x1620 (defaults, clamped to decoder cap)
    m_encoderW = 2880;
    m_encoderH = 1620;
    m_encoderFps = 60;
    m_encoderBitrate = 20000000;
    m_encoderUseGpu = false;
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

    m_pVideoEncoder->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });

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
    // Align down to 16 (H.264 macroblock requirement)
    newW = (newW / 16) * 16;
    newH = (newH / 16) * 16;

    if (newW > 0 && newH > 0) {
        DriverLog("Clamping encoder to hardware cap %dx%d: %dx%d -> %dx%d",
                  m_pendingCapW, m_pendingCapH, m_encoderW, m_encoderH, newW, newH);
        m_encoderW = newW;
        m_encoderH = newH;
    }
}

bool HmdDriver::ApplyBridgeCfg(int fps, int bitrateKbps, const char* codec)
{
    // Same re-init discipline as ApplyHardwareCap: never tear down while the
    // background thread may still be reading encoder resources.
    WaitEncoderIdle();

    std::lock_guard<std::mutex> lock(m_encoderMutex);

    int newFps = (fps > 0 && fps <= 120) ? fps : m_encoderFps;
    int newBitrate = (bitrateKbps > 0 && bitrateKbps <= 100000) ? bitrateKbps * 1000 : m_encoderBitrate;
    // Named codecs pin the backend; "auto" (or unknown) keeps the current one
    // so a bitrate-only push never flips HW<->SW underneath the stream.
    // "gpu"/"cpu" are the bridge's coarse switch: GPU lets VideoEncoder probe
    // the hardware backends for the installed card, CPU forces libx264.
    bool newGpu = m_encoderUseGpu;
    if (codec) {
        if (strcmp(codec, "libx264") == 0 || strcmp(codec, "cpu") == 0) newGpu = false;
        else if (strcmp(codec, "h264_amf") == 0 || strcmp(codec, "h264_nvenc") == 0
                 || strcmp(codec, "h264_qsv") == 0 || strcmp(codec, "gpu") == 0) newGpu = true;
    }

    if (!m_encoderInitialized || !m_pVideoEncoder) {
        // Encoder not up yet; values apply at initialization time.
        m_encoderFps = newFps;
        m_encoderBitrate = newBitrate;
        m_encoderUseGpu = newGpu;
        DriverLog("BRIDGE_CFG stored (encoder not ready, applied on init)");
        return true;
    }

    bool meaningful = (newFps != m_encoderFps || newBitrate != m_encoderBitrate || newGpu != m_encoderUseGpu);
    if (!meaningful) {
        return false;
    }

    DriverLog("Re-initializing encoder from BRIDGE_CFG: @%d fps, %d kbps, gpu=%d",
              newFps, newBitrate / 1000, newGpu ? 1 : 0);

    m_encoderFps = newFps;
    m_encoderBitrate = newBitrate;
    m_encoderUseGpu = newGpu;
    ClampEncoderToCap();

    m_pVideoEncoder->Shutdown();
    delete m_pVideoEncoder;
    m_pVideoEncoder = nullptr;
    m_encoderInitialized = false;

    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder during BRIDGE_CFG re-init!");
        return false;
    }

    m_pVideoEncoder->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });

    if (!m_pVideoEncoder->Initialize(m_pD3D11Device, m_pD3D11DeviceContext,
                                     m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate, m_encoderUseGpu)) {
        DriverLog("VideoEncoder re-init failed under BRIDGE_CFG!");
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return false;
    }

    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Encoder re-initialized at %dx%d @%d fps from BRIDGE_CFG", m_encoderW, m_encoderH, m_encoderFps);
    return true;
}

bool HmdDriver::ApplyHardwareCap(int capW, int capH)
{
    // Never tear down encoder resources while the background thread may still
    // be reading them (queued compose / in-flight readback).
    WaitEncoderIdle();

    std::lock_guard<std::mutex> lock(m_encoderMutex);

    m_pendingCapW = capW;
    m_pendingCapH = capH;

    if (!m_encoderInitialized || !m_pVideoEncoder) {
        // Encoder not up yet; cap will be applied at initialization time.
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

    m_pVideoEncoder->Shutdown();
    delete m_pVideoEncoder;
    m_pVideoEncoder = nullptr;
    m_encoderInitialized = false;

    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder during cap re-init!");
        return false;
    }

    m_pVideoEncoder->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });

    if (!m_pVideoEncoder->Initialize(m_pD3D11Device, m_pD3D11DeviceContext,
                                     m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate, m_encoderUseGpu)) {
        DriverLog("VideoEncoder re-init failed under cap!");
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return false;
    }

    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Encoder re-initialized at %dx%d under hardware cap", m_encoderW, m_encoderH);
    return true;
}