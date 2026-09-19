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

    // Default to the compositor render size (SBS 1920x1080), not an
    // upscaled 2880x1620 that only encodes interpolated pixels (M3).
    // Sanitized like every other write: even + 16-aligned.
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

    // Never clamp to a degenerate size: an absurd cap (or align-down of a
    // tiny encoder) must keep the old resolution, not kill the stream (M4).
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

// Range-check + align encoder dims in place (H5/M3/M4). Returns false when the
// proposed size is outside 320x180..7680 or aligns down to nothing — the
// caller must then reject-and-keep-old. 16-align implies even (H.264 4:2:0).
bool HmdDriver::SanitizeEncoderDims(int& w, int& h)
{
    if (w < 320 || h < 180 || w > 7680 || h > 7680)
        return false;
    w = (w / 16) * 16;
    h = (h / 16) * 16;
    // Align-down costs at most 15px, so a sane input can only land just under
    // the floor; anything smaller was degenerate to begin with.
    return w >= 320 && h >= 160;
}

void HmdDriver::RegisterEncoderCallbacks(VideoEncoder* enc)
{
    enc->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });
    // Without this, BridgeServer::PublishTelemetry is never called and the SHM
    // status ring stays empty (H3). Every (re-)init must go through here (H4).
    enc->SetTelemetryCallback([this](const cbpp::PayloadTelemetry& t) {
        if (m_bridgeInitialized.load(std::memory_order_relaxed) && m_bridgeServer.running()) {
            m_bridgeServer.PublishTelemetry(t);
        }
    });
}

// Single validated applier behind the SHM (ApplyStreamSettings) and UDP
// (ApplyBridgeCfg / ApplyHardwareCap) control planes (M5). Out-of-range
// fields keep their old values; a fully-noop change skips the re-init.
bool HmdDriver::ApplyEncoderSettings(int w, int h, int fps, int bitrateBps, bool useGpu, const char* reason)
{
    // Never tear down encoder resources while the background thread may still
    // be reading them (queued compose / in-flight readback).
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
    // bitrateBps <= 0 is the caller's "keep old" sentinel; >100Mbps is insane.
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

bool HmdDriver::ApplyBridgeCfg(int fps, int bitrateKbps, const char* codec)
{
    // Named codecs pin the backend; "auto" (or unknown) keeps the current one
    // so a bitrate-only push never flips HW<->SW underneath the stream.
    // "gpu"/"cpu" are the bridge's coarse switch: GPU lets VideoEncoder probe
    // the hardware backends for the installed card, CPU forces libx264.
    // (m_encoderUseGpu is read lock-free here; ApplyEncoderSettings re-checks
    // the final value under the mutex — worst case a concurrent change wins.)
    bool newGpu = m_encoderUseGpu;
    if (codec) {
        if (strcmp(codec, "libx264") == 0 || strcmp(codec, "cpu") == 0) newGpu = false;
        else if (strcmp(codec, "h264_amf") == 0 || strcmp(codec, "h264_nvenc") == 0
                 || strcmp(codec, "h264_qsv") == 0 || strcmp(codec, "gpu") == 0) newGpu = true;
    }
    // W/H untouched by BRIDGE_CFG: propose current (re-validated inside).
    // kbps clamp before *1000 so the multiply can't overflow (H5).
    int bps = (bitrateKbps >= 1 && bitrateKbps <= 100000) ? bitrateKbps * 1000 : -1;
    return ApplyEncoderSettings(m_encoderW, m_encoderH, fps, bps, newGpu, "BRIDGE_CFG");
}

bool HmdDriver::ApplyHardwareCap(int capW, int capH)
{
    // Validate BEFORE storing: a poisoned cap used to stick forever (M4).
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
        clampedW = m_encoderW;
        clampedH = m_encoderH;
        fps = m_encoderFps;
        bitrate = m_encoderBitrate;
        gpu = m_encoderUseGpu;
    }
    // Same validated re-init path as every other settings source (M5). The
    // dims were just clamped above; re-validation inside is a no-op.
    return ApplyEncoderSettings(clampedW, clampedH, fps, bitrate, gpu, "hardware-cap");
}