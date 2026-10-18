#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include "H264Utils.h"
#include "DriverLog.h"
#include <cstring>
// Opens the H.264 codec (hardware candidates when useGpu, libx264 otherwise), the Annex-B filter, SPS/PPS cache, staging texture, and scale/frame state.
bool VideoEncoder::InitializeFFmpeg()
{
    ENCODER_LOG("Initializing FFmpeg H264 encoder...");
#ifdef _WIN32
    const char* hwCandidates[] = { "h264_amf", "h264_nvenc", "h264_qsv", "libx264" };
    const int hwCandidateCount = 4;
#else
    // VAAPI covers Intel/AMD; NVENC and QSV cover the NVIDIA/Intel discrete paths.
    const char* hwCandidates[] = { "h264_vaapi", "h264_nvenc", "h264_qsv", "libx264" };
    const int hwCandidateCount = 4;
#endif
    const char* swCandidates[] = { "libx264" };
    const char** codecNames = m_useGpuEncoding ? hwCandidates : swCandidates;
    const int codecNameCount = m_useGpuEncoding ? hwCandidateCount : 1;
    const AVCodec* codec = nullptr;
    for (int n = 0; n < codecNameCount; n++) {
        const char* name = codecNames[n];
        const AVCodec* c = avcodec_find_encoder_by_name(name);
        if (!c) {
            ENCODER_LOG("Encoder %s not available, skipping", name);
            continue;
        }
        m_pCodecContext = avcodec_alloc_context3(c);
        if (!m_pCodecContext) {
            ENCODER_ERROR("Failed to allocate codec context for %s!", name);
            continue;
        }
        m_pCodecContext->width = m_width;
        m_pCodecContext->height = m_height;
        m_pCodecContext->time_base = { 1, m_fps };
        m_pCodecContext->framerate = { m_fps, 1 };
        m_pCodecContext->gop_size = 10;
        m_pCodecContext->max_b_frames = 0;
        m_pCodecContext->delay = 0;
        m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;
        m_pCodecContext->bit_rate = m_bitrate;
        m_pCodecContext->rc_min_rate = m_bitrate;
        m_pCodecContext->rc_max_rate = m_bitrate;
        m_pCodecContext->rc_buffer_size = m_bitrate / 2;
        m_pCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        bool isLibx264 = (strcmp(name, "libx264") == 0);
        bool isAmf     = (strcmp(name, "h264_amf") == 0);
        bool isVaapi   = (strcmp(name, "h264_vaapi") == 0);
#ifdef _WIN32
        if (isAmf) {
            // AMF is the one encoder that still needs a host graphics device of its
            // own; the encoder owns none, so it creates a private one here.
            ID3D11Device* hostDevice = nullptr;
            D3D_FEATURE_LEVEL level;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                          nullptr, 0, D3D11_SDK_VERSION, &hostDevice,
                                          &level, nullptr);
            AVBufferRef* hwdev = (SUCCEEDED(hr)) ? av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA) : nullptr;
            if (hwdev && hostDevice) {
                AVD3D11VADeviceContext* d3d11va =
                    (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hwdev->data)->hwctx;
                d3d11va->device = hostDevice;
                hostDevice->AddRef();
                if (av_hwdevice_ctx_init(hwdev) == 0) {
                    m_pHwDeviceCtx = hwdev;
                    m_pCodecContext->hw_device_ctx = av_buffer_ref(hwdev);
                    ENCODER_LOG("AMF running on a private host device");
                } else {
                    ENCODER_ERROR("Failed to init the AMF hardware device context");
                    av_buffer_unref(&hwdev);
                    hostDevice->Release();
                }
            } else {
                ENCODER_ERROR("Failed to create the AMF hardware device context");
                if (hwdev) av_buffer_unref(&hwdev);
                if (hostDevice) hostDevice->Release();
            }
            av_opt_set(m_pCodecContext->priv_data, "usage", "ultralowlatency", 0);
            av_opt_set(m_pCodecContext->priv_data, "rc", "cbr", 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        } else
#else
        if (isVaapi) {
            // VAAPI takes the DRM render node and uploads the host-visible NV12 the
            // Vulkan compute pass already produced.
            AVBufferRef* hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
            if (hwdev) {
                av_hwdevice_ctx_set_properties(hwdev, "device", "/dev/dri/renderD128", 0, nullptr, 0);
                if (av_hwdevice_ctx_init(hwdev) == 0) {
                    m_pHwDeviceCtx = hwdev;
                    m_pCodecContext->hw_device_ctx = av_buffer_ref(hwdev);
                    m_pCodecContext->pix_fmt = AV_PIX_FMT_VAAPI;
                    ENCODER_LOG("VAAPI device /dev/dri/renderD128 ready");
                } else {
                    ENCODER_ERROR("Failed to init VAAPI hw device");
                    av_buffer_unref(&hwdev);
                }
            } else {
                ENCODER_ERROR("Failed to allocate VAAPI hw device");
            }
            av_opt_set(m_pCodecContext->priv_data, "rc_mode", "CBR", 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "constrained_baseline", 0);
        } else
#endif
        if (isLibx264) {
            av_opt_set(m_pCodecContext->priv_data, "preset", "ultrafast", 0);
            av_opt_set(m_pCodecContext->priv_data, "tune", "zerolatency", 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        } else {
            const char* hwPreset = (strcmp(name, "h264_nvenc") == 0) ? "fast" : "veryfast";
            av_opt_set(m_pCodecContext->priv_data, "preset", hwPreset, 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        }
        int ret = avcodec_open2(m_pCodecContext, c, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            ENCODER_ERROR("Failed to open codec %s: %s", name, errbuf);
            if (m_pCodecContext->hw_device_ctx) {
                av_buffer_unref(&m_pCodecContext->hw_device_ctx);
            }
            if (m_pHwDeviceCtx) {
                av_buffer_unref(&m_pHwDeviceCtx);
            }
            avcodec_free_context(&m_pCodecContext);
            m_pCodecContext = nullptr;
            continue;
        }
        codec = c;
        m_useGpuEncoding = !isLibx264;
        ENCODER_LOG("Opened H264 encoder: %s (%s)", name,
                    m_useGpuEncoding ? "hardware/GPU" : "software");
        break;
    }
    if (!codec || !m_pCodecContext) {
        ENCODER_ERROR("No usable H264 encoder found!");
        return false;
    }
    m_pBsfCtx = nullptr;
    if (m_pCodecContext->extradata && m_pCodecContext->extradata_size > 0) {
        const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if (bsf) {
            int bret = av_bsf_alloc(bsf, &m_pBsfCtx);
            if (bret >= 0) {
                AVCodecParameters* par = m_pBsfCtx->par_in;
                par->codec_id = AV_CODEC_ID_H264;
                par->extradata = (uint8_t*)av_memdup(m_pCodecContext->extradata,
                                                     m_pCodecContext->extradata_size);
                par->extradata_size = m_pCodecContext->extradata_size;
                bret = av_bsf_init(m_pBsfCtx);
                if (bret >= 0) {
                    ENCODER_LOG("h264_mp4toannexb BSF enabled (Annex B on the wire)");
                } else {
                    av_bsf_free(&m_pBsfCtx);
                    m_pBsfCtx = nullptr;
                    ENCODER_ERROR("Failed to init h264_mp4toannexb BSF!");
                }
            } else {
                ENCODER_ERROR("Failed to allocate h264_mp4toannexb BSF!");
            }
        }
    } else {
        ENCODER_LOG("No encoder extradata available; BSF skipped");
    }
    m_spsPpsAnnexB.clear();
    if (m_pCodecContext->extradata && m_pCodecContext->extradata_size > 0) {
        const uint8_t* e = m_pCodecContext->extradata;
        int esz = m_pCodecContext->extradata_size;
        if (esz >= 16) {
            ENCODER_LOG("Extradata [%d bytes]: %02X %02X %02X %02X %02X %02X %02X %02X "
                        "%02X %02X %02X %02X %02X %02X %02X %02X",
                        esz, e[0],e[1],e[2],e[3],e[4],e[5],e[6],e[7],
                        e[8],e[9],e[10],e[11],e[12],e[13],e[14],e[15]);
        } else {
            char hexBuf[64] = {};
            int pos = 0;
            for (int i = 0; i < esz && i < 16 && pos < 60; i++) {
                pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, " %02X", e[i]);
            }
            ENCODER_LOG("Extradata [%d bytes]:%s", esz, hexBuf);
        }
        m_spsPpsAnnexB = h264::ParseExtradataSpsPps(e, esz);
        if (!m_spsPpsAnnexB.empty()) {
            ENCODER_LOG("Extracted Annex-B SPS+PPS from extradata (%zu bytes)", m_spsPpsAnnexB.size());
        } else {
            ENCODER_LOG("WARNING: No SPS/PPS extracted from extradata (%d bytes) — keyframes may lack them", esz);
        }
    } else {
        ENCODER_LOG("No extradata for SPS/PPS extraction (size=%d)", m_pCodecContext ? m_pCodecContext->extradata_size : -1);
    }
    ENCODER_LOG("Encoder name: %s, long name: %s",
                codec->name, codec->long_name ? codec->long_name : "N/A");
m_pConvertContext = sws_getContext(
        m_width, m_height, AV_PIX_FMT_BGRA,
        m_width, m_height, AV_PIX_FMT_NV12,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!m_pConvertContext) {
        ENCODER_ERROR("Failed to create SWScale context!");
        return false;
    }
    ENCODER_LOG("SWScale context created for BGRA -> NV12 conversion");
    m_pSoftwareFrameBuffer = (uint8_t*)av_malloc(m_width * m_height * 4);
    if (!m_pSoftwareFrameBuffer) {
        ENCODER_ERROR("Failed to allocate software frame buffer!");
        return false;
    }
    ENCODER_LOG("Codec opened successfully!");
    ENCODER_LOG("  Selected pix_fmt: %d (%s)",
                m_pCodecContext->pix_fmt,
                av_get_pix_fmt_name(m_pCodecContext->pix_fmt));
    m_pFrame = av_frame_alloc();
    if (!m_pFrame) {
        ENCODER_ERROR("Failed to allocate frame!");
        return false;
    }
    m_pFrame->format = AV_PIX_FMT_NV12;
    m_pFrame->data[0] = m_pSoftwareFrameBuffer;
    m_pFrame->linesize[0] = m_width;
    m_pFrame->data[1] = m_pSoftwareFrameBuffer + m_width * m_height;
    m_pFrame->linesize[1] = m_width;
    m_pFrame->width = m_width;
    m_pFrame->height = m_height;
    m_pPacket = av_packet_alloc();
    if (!m_pPacket) {
        ENCODER_ERROR("Failed to allocate packet!");
        return false;
    }
    ENCODER_LOG("FFmpeg H264 encoder initialized successfully!");
    ENCODER_LOG("  Codec: %s", codec->name);
    ENCODER_LOG("  Resolution: %dx%d", m_width, m_height);
    ENCODER_LOG("  FPS: %d", m_fps);
    ENCODER_LOG("  Bitrate: %d bps", m_bitrate);
    return true;
}
// Frees the bitstream filter, packet, frame, scale context, codec, and hardware device opened above.
void VideoEncoder::CleanupFFmpeg()
{
    ENCODER_LOG("Cleaning up FFmpeg resources...");
    if (m_pBsfCtx) {
        av_bsf_free(&m_pBsfCtx);
        m_pBsfCtx = nullptr;
    }
    if (m_pPacket) {
        av_packet_free(&m_pPacket);
        m_pPacket = nullptr;
    }
    if (m_pFrame) {
        av_frame_free(&m_pFrame);
        m_pFrame = nullptr;
    }
    if (m_pConvertContext) {
        sws_freeContext(m_pConvertContext);
        m_pConvertContext = nullptr;
    }
    if (m_pCodecContext) {
        avcodec_free_context(&m_pCodecContext);
        m_pCodecContext = nullptr;
    }
    if (m_pHwDeviceCtx) {
        av_buffer_unref(&m_pHwDeviceCtx);
        m_pHwDeviceCtx = nullptr;
    }
    ENCODER_LOG("FFmpeg resources cleaned up.");
}
// Points the encoder frame at the GPU NV12 buffer produced by the Vulkan
// compute pass; falls back to the CPU swscale conversion when that path did
// not run for this frame.
bool VideoEncoder::ConvertFrame()
{
    if (!m_pGpuNv12) return SwsConvert();
    m_pFrame->data[0] = (uint8_t*)m_pGpuNv12;
    m_pFrame->data[1] = (uint8_t*)m_pGpuNv12 + (size_t)m_width * m_height;
    return true;
}
// Converts the BGRA frame source (Vulkan host copy map or D3D staging readback) to
// the NV12 software frame; runs on the encoding thread before FinishEncode.
bool VideoEncoder::SwsConvert()
{
    uint8_t* srcData = m_pSwsSrc;
    int srcPitch = m_width * 4;
    if (!srcData) {
        ENCODER_ERROR("SwsConvert called without readback data!");
        return false;
    }
    m_pFrame->data[0] = m_pSoftwareFrameBuffer;
    m_pFrame->data[1] = m_pSoftwareFrameBuffer + m_width * m_height;
#ifdef DRIVER_DIAG
    {
        // Rate-limited source brightness: 0 = black frame, 200-400 = real content.
        uint64_t sum = 0;
        int n = 0;
        for (int y = 0; y < m_height; y += 8) {
            const uint8_t* row = srcData + (size_t)y * srcPitch;
            for (int x = 0; x < m_width; x += 8) {
                const uint8_t* px = row + x * 4;
                sum += (uint64_t)px[0] + px[1] + px[2];
                n++;
            }
        }
        static int s_diagSrc = 0;
        static int s_blackStreak = 0;
        uint64_t mean = sum / (uint64_t)(n > 0 ? n : 1);
        bool periodic = ++s_diagSrc % 300 == 0;
        if (mean < 4) s_blackStreak++; else s_blackStreak = 0;
        if (periodic || (mean < 4 && s_blackStreak == 1)) {
            ENCODER_LOG("[DIAG swssrc] meanRGB=%llu", (unsigned long long)mean);
        }
    }
#endif
    uint8_t* srcSlice[1] = { srcData };
    int srcStride[1] = { srcPitch };
    uint8_t* dstSlice[2] = { m_pSoftwareFrameBuffer, m_pSoftwareFrameBuffer + m_width * m_height };
    int dstStride[2] = { m_width, m_width };
    int ret = sws_scale(m_pConvertContext, srcSlice, srcStride, 0, m_height,
                        dstSlice, dstStride);
    if (ret != m_height) {
        ENCODER_ERROR("swscale conversion failed! Returned %d, expected %d", ret, m_height);
        return false;
    }
    return true;
}
// Stamps pts, sends the frame, drains packets, and updates encode timing/duplicate counters; pts orders the stream.
bool VideoEncoder::FinishEncode(int64_t pts)
{
    if (!m_initialized) {
        ENCODER_ERROR("FinishEncode called without valid encoder!");
        return false;
    }
    uint64_t t0 = cbpp::PlatformNowUs();
    if (m_lastCallUs != 0) {
        int64_t intervalUs = (int64_t)(t0 - m_lastCallUs);
        m_intervalSumUs += intervalUs;
        if (intervalUs > m_intervalMaxUs) m_intervalMaxUs = intervalUs;
        m_intervalCount++;
    }
    m_lastCallUs = t0;
    uint32_t hash = ComputeFrameHash();
    if (m_lastFrameHash != 0 && hash == m_lastFrameHash) m_dupCount++;
    m_lastFrameHash = hash;
    m_summaryFrames++;
    m_hasValidFrame = true;
    m_pFrame->pts = pts;
    if (!SendFrameToEncoder()) {
        ENCODER_ERROR("Failed to send SBS frame to encoder!");
        return false;
    }
    if (!ReceiveEncodedPackets()) {
        ENCODER_ERROR("Failed to receive SBS encoded packets!");
        return false;
    }
    uint64_t t1 = cbpp::PlatformNowUs();
    int64_t elapsedUs = (int64_t)(t1 - t0);
    m_encSumUs += elapsedUs;
    if (elapsedUs > m_encMaxUs) m_encMaxUs = elapsedUs;
    m_encCount++;
    if (elapsedUs > 40000) {
        ENCODER_LOG("[TELEMETRY] SLOW frame: %lldus encode (exceeds frame budget)", (long long)elapsedUs);
    }
    if (m_encCount > 0 && (m_encCount % m_summaryInterval == 0)) {
        LogTelemetrySummary();
    }
    return true;
}
// Arms a one-shot IDR for the next SendFrameToEncoder; set by the KEYFRAME_REQ discovery path.
// Pushes the NV12 frame into the codec, applying an armed IDR when present.
void VideoEncoder::RequestKeyframe()
{
    m_forceKeyframe.store(true, std::memory_order_relaxed);
}
// Pushes the NV12 frame into the codec, applying an armed IDR when present.
bool VideoEncoder::SendFrameToEncoder()
{
    if (m_forceKeyframe.exchange(false, std::memory_order_relaxed) && m_pFrame) {
        m_pFrame->pict_type = AV_PICTURE_TYPE_I;
    }
    int ret = avcodec_send_frame(m_pCodecContext, m_pFrame);
    if (ret < 0) {
        ENCODER_ERROR("avcodec_send_frame failed! Error code: %d", ret);
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        ENCODER_ERROR("Error: %s", errbuf);
        return false;
    }
    m_hasValidFrame = false;
    return true;
}
// Drains encoded packets through the Annex-B filter, prepending cached SPS/PPS to keyframes that lack them, then fires the packet callback.
// Logs a human-readable FFmpeg error string; context names the failing call, errorCode is the AVERROR value.
bool VideoEncoder::ReceiveEncodedPackets()
{
    auto emitWithSpsPps = [this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        if (!keyframe || m_spsPpsAnnexB.empty() || size < 5) {
            if (m_encodedPacketCallback) {
                m_encodedPacketCallback(data, size, pts, keyframe);
            }
            return;
        }
        if (h264::AccessUnitHasSps(data, size)) {
            if (m_encodedPacketCallback) {
                m_encodedPacketCallback(data, size, pts, keyframe);
            }
            return;
        }
        int totalSize = (int)m_spsPpsAnnexB.size() + size;
        uint8_t* combined = (uint8_t*)malloc(totalSize);
        if (combined) {
            memcpy(combined, m_spsPpsAnnexB.data(), m_spsPpsAnnexB.size());
            memcpy(combined + m_spsPpsAnnexB.size(), data, size);
            if (m_encodedPacketCallback) {
                m_encodedPacketCallback(combined, totalSize, pts, keyframe);
            }
            free(combined);
        } else {
            if (m_encodedPacketCallback) {
                m_encodedPacketCallback(data, size, pts, keyframe);
            }
        }
    };
    int ret;
    while ((ret = avcodec_receive_packet(m_pCodecContext, m_pPacket)) >= 0) {
        bool keyframe = (m_pPacket->flags & AV_PKT_FLAG_KEY) != 0;
        if (m_pBsfCtx) {
            if (av_bsf_send_packet(m_pBsfCtx, m_pPacket) == 0) {
                AVPacket* filtered = av_packet_alloc();
                while (av_bsf_receive_packet(m_pBsfCtx, filtered) >= 0) {
                    emitWithSpsPps(filtered->data, filtered->size,
                                   filtered->pts, keyframe);
                    av_packet_unref(filtered);
                }
                av_packet_free(&filtered);
            }
            av_packet_unref(m_pPacket);
            continue;
        }
        emitWithSpsPps(m_pPacket->data, m_pPacket->size,
                        m_pPacket->pts, keyframe);
        av_packet_unref(m_pPacket);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        LogFFmpegError("avcodec_receive_packet", ret);
        return false;
    }
    return true;
}
// Logs a human-readable FFmpeg error string; context names the failing call, errorCode is the AVERROR value.
void VideoEncoder::LogFFmpegError(const char* context, int errorCode)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    ENCODER_ERROR("%s failed with error %d: %s", context, errorCode, errbuf);
}
// Logs linked libavcodec/format/util/scale versions; called once during Initialize.
// Samples the NV12 buffer for the duplicate-frame detector in FinishEncode.
void VideoEncoder::LogFFmpegVersion()
{
    ENCODER_LOG("FFmpeg version info:");
    ENCODER_LOG("  libavcodec version: %d.%d.%d",
                AV_VERSION_MAJOR(avcodec_version()),
                AV_VERSION_MINOR(avcodec_version()),
                AV_VERSION_MICRO(avcodec_version()));
    ENCODER_LOG("  libavformat version: %d.%d.%d",
                AV_VERSION_MAJOR(avformat_version()),
                AV_VERSION_MINOR(avformat_version()),
                AV_VERSION_MICRO(avformat_version()));
    ENCODER_LOG("  libavutil version: %d.%d.%d",
                AV_VERSION_MAJOR(avutil_version()),
                AV_VERSION_MINOR(avutil_version()),
                AV_VERSION_MICRO(avutil_version()));
    ENCODER_LOG("  libswscale version: %d.%d.%d",
                AV_VERSION_MAJOR(swscale_version()),
                AV_VERSION_MINOR(swscale_version()),
                AV_VERSION_MICRO(swscale_version()));
}
// Samples the NV12 buffer for the duplicate-frame detector in FinishEncode.
// Every 300th frame also logs the sampled mean brightness (0-255) so a black
// frame is distinguishable from real content in the driver log.
uint32_t VideoEncoder::ComputeFrameHash()
{
    if (!m_pSoftwareFrameBuffer) return 0;
    const uint8_t* p = m_pSoftwareFrameBuffer;
    int total = m_width * m_height * 3 / 2;
    uint32_t h = 2166136261u;
    uint64_t sum = 0;
    int count = 0;
    static unsigned s_nvFrame = 0;
    for (int i = 0; i < total; i += 1024) {
        h ^= p[i];
        h *= 16777619u;
        sum += p[i];
        count++;
    }
    if (++s_nvFrame % 300 == 0) {
        DriverLog("NV-MEAN: frame=%u mean=%d", s_nvFrame, (int)(sum / (count > 0 ? count : 1)));
    }
    return h;
}
// Emits the encode-time summary to the log and the telemetry callback, then resets the interval counters.
void VideoEncoder::LogTelemetrySummary()
{
    int64_t frames = m_encCount > 0 ? m_encCount : 1;
    int64_t avgEnc = m_encSumUs / frames;
    int64_t avgInt = m_intervalCount > 0 ? m_intervalSumUs / m_intervalCount : 0;
    double dupRate = m_summaryFrames > 0 ? (100.0 * m_dupCount / m_summaryFrames) : 0.0;
    ENCODER_LOG("[TELEMETRY] frames=%lld avgEncode=%lldus maxEncode=%lldus avgInterval=%lldus maxInterval=%lldus dups=%d dupRate=%.1f%%",
                (long long)m_encCount, (long long)avgEnc, (long long)m_encMaxUs,
                (long long)avgInt, (long long)m_intervalMaxUs,
                m_dupCount, dupRate);
    if (m_telemetryCallback) {
        cbpp::PayloadTelemetry t{};
        t.frames = (uint64_t)m_encCount;
        t.avg_encode_us = (uint64_t)(avgEnc >= 0 ? avgEnc : 0);
        t.max_encode_us = (uint64_t)(m_encMaxUs >= 0 ? m_encMaxUs : 0);
        t.avg_interval_us = (uint64_t)(avgInt >= 0 ? avgInt : 0);
        t.max_interval_us = (uint64_t)(m_intervalMaxUs >= 0 ? m_intervalMaxUs : 0);
        t.dup_count = (uint64_t)(m_dupCount >= 0 ? m_dupCount : 0);
        t.summary_frames = (uint64_t)m_summaryFrames;
        t.pad = 0;
        m_telemetryCallback(t);
    }
    m_encSumUs = 0; m_encMaxUs = 0; m_encCount = 0;
    m_intervalSumUs = 0; m_intervalMaxUs = 0; m_intervalCount = 0;
    m_dupCount = 0; m_summaryFrames = 0;
}