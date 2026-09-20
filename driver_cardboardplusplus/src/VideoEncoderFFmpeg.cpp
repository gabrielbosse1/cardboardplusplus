#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include "H264Utils.h"
#include <cstring>
// Opens the H.264 codec (hardware candidates when useGpu, libx264 otherwise), the Annex-B filter, SPS/PPS cache, staging texture, and scale/frame state.
bool VideoEncoder::InitializeFFmpeg()
{
    ENCODER_LOG("Initializing FFmpeg H264 encoder...");
    const char* hwCandidates[] = { "h264_amf", "h264_nvenc", "h264_qsv", "libx264" };
    const char* swCandidates[] = { "libx264" };
    const char** codecNames = m_useGpuEncoding ? hwCandidates : swCandidates;
    const int codecNameCount = m_useGpuEncoding ? 4 : 1;
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
        if (isAmf) {
            AVBufferRef* hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (hwdev) {
                AVD3D11VADeviceContext* d3d11va =
                    (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hwdev->data)->hwctx;
                d3d11va->device = m_pDevice;
                m_pDevice->AddRef();
                if (av_hwdevice_ctx_init(hwdev) == 0) {
                    m_pHwDeviceCtx = hwdev;
                    m_pCodecContext->hw_device_ctx = av_buffer_ref(hwdev);
                    ENCODER_LOG("Wrapped driver ID3D11Device for AMF");
                } else {
                    ENCODER_ERROR("Failed to init D3D11VA hw device for AMF");
                    av_buffer_unref(&hwdev);
                }
            } else {
                ENCODER_ERROR("Failed to allocate D3D11VA hw device for AMF");
            }
            av_opt_set(m_pCodecContext->priv_data, "usage", "ultralowlatency", 0);
            av_opt_set(m_pCodecContext->priv_data, "rc", "cbr", 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        } else if (isLibx264) {
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
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = m_width;
    stagingDesc.Height = m_height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = m_pDevice->CreateTexture2D(&stagingDesc, nullptr, &m_pStagingTexture);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create staging texture! HRESULT: 0x%x", hr);
        return false;
    }
    ENCODER_LOG("Staging texture created successfully");
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
// Converts the BGRA readback buffer to the NV12 software frame; runs on the encoding thread before FinishEncode.
bool VideoEncoder::SwsConvert()
{
    uint8_t* srcData = m_pReadbackBuffer;
    int srcPitch = m_width * 4;
    if (!srcData) {
        ENCODER_ERROR("SwsConvert called without readback data!");
        return false;
    }
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
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (m_lastCallUs != 0) {
        int64_t intervalUs = ((t0.QuadPart - m_lastCallUs) * 1000000) / m_perfFreq.QuadPart;
        m_intervalSumUs += intervalUs;
        if (intervalUs > m_intervalMaxUs) m_intervalMaxUs = intervalUs;
        m_intervalCount++;
    }
    m_lastCallUs = t0.QuadPart;
    uint32_t hash = ComputeFrameHash();
    if (m_lastFrameHash != 0 && hash == m_lastFrameHash) m_dupCount++;
    m_lastFrameHash = hash;
    m_summaryFrames++;
    m_pFrame->pts = pts;
    m_hasValidFrame = true;
    if (!SendFrameToEncoder()) {
        ENCODER_ERROR("Failed to send SBS frame to encoder!");
        return false;
    }
    if (!ReceiveEncodedPackets()) {
        ENCODER_ERROR("Failed to receive SBS encoded packets!");
        return false;
    }
    QueryPerformanceCounter(&t1);
    int64_t elapsedUs = ((t1.QuadPart - t0.QuadPart) * 1000000) / m_perfFreq.QuadPart;
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
uint32_t VideoEncoder::ComputeFrameHash()
{
    if (!m_pSoftwareFrameBuffer) return 0;
    const uint8_t* p = m_pSoftwareFrameBuffer;
    int total = m_width * m_height * 3 / 2;
    uint32_t h = 2166136261u;
    for (int i = 0; i < total; i += 1024) {
        h ^= p[i];
        h *= 16777619u;
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