#include "H264Decoder.h"
#include <android/log.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#define LOG_TAG "H264Decoder"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ndk_cardboardplusplus {

// FFmpeg is loaded at runtime from the ffmpeg-kit AAR via dlopen(); we only
// resolve the symbols we use. The struct/enum layouts come from the real
// FFmpeg headers (driver_cardboardplusplus/lib/ffmpeg/include) so we read
// AVFrame/AVPacket fields correctly instead of guessing offsets.
struct FFmpegFunctions {
    void* avcodec_handle;
    void* avutil_handle;
    void* swscale_handle;

    const AVCodec* (*avcodec_find_decoder)(enum AVCodecID id);
    const AVCodec* (*avcodec_find_decoder_by_name)(const char* name);
    AVCodecContext* (*avcodec_alloc_context3)(const AVCodec* codec);
    int (*avcodec_open2)(AVCodecContext* avctx, const AVCodec* codec, AVDictionary** options);
    AVFrame* (*av_frame_alloc)(void);
    void (*av_frame_free)(AVFrame** frame);
    void* (*av_malloc)(size_t size);
    void (*av_free)(void* ptr);
    int (*avcodec_send_packet)(AVCodecContext* avctx, const AVPacket* avpkt);
    int (*avcodec_receive_frame)(AVCodecContext* avctx, AVFrame* frame);
    void (*avcodec_free_context)(AVCodecContext** avctx);
    void (*avcodec_flush_buffers)(AVCodecContext* avctx);
    int (*av_dict_set)(AVDictionary** dict, const char* key, const char* value, int flags);
    void (*av_dict_free)(AVDictionary** dict);
    struct SwsContext* (*sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                         int dstW, int dstH, enum AVPixelFormat dstFormat,
                                         int flags, SwsFilter* srcFilter, SwsFilter* dstFilter, const double* param);
    int (*sws_scale)(struct SwsContext* c, const uint8_t* const srcSlice[], const int srcStride[],
                    int srcSliceY, int srcSliceH, uint8_t* const dst[], const int dstStride[]);
    void (*sws_freeContext)(struct SwsContext* swsContext);
};

static FFmpegFunctions g_ffmpeg;

H264Decoder::H264Decoder()
    : width_(0), height_(0), initialized_(false), has_new_frame_(false),
      codec_context_(nullptr), decoded_frame_(nullptr),
      rgb_buffer_(nullptr), rgb_buffer_size_(0),
      sws_ctx_(nullptr), sws_src_fmt_(-1), sws_w_(0), sws_h_(0) {
}

H264Decoder::~H264Decoder() {
  Shutdown();
}

bool H264Decoder::Initialize(int width, int height) {
  if (initialized_) {
    LOGD("Already initialized");
    return true;
  }

  width_ = width;
  height_ = height;

  LOGD("Initializing decoder with requested size: %dx%d", width, height);

  memset(&g_ffmpeg, 0, sizeof(g_ffmpeg));

  g_ffmpeg.avcodec_handle = dlopen("libavcodec.so", RTLD_NOW);
  if (!g_ffmpeg.avcodec_handle) {
    LOGE("Failed to load libavcodec.so: %s", dlerror());
    return false;
  }
  LOGD("Loaded libavcodec.so");

  g_ffmpeg.avutil_handle = dlopen("libavutil.so", RTLD_NOW);
  if (!g_ffmpeg.avutil_handle) {
    LOGE("Failed to load libavutil.so: %s", dlerror());
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }
  LOGD("Loaded libavutil.so");

  g_ffmpeg.swscale_handle = dlopen("libswscale.so", RTLD_NOW);
  if (!g_ffmpeg.swscale_handle) {
    LOGE("Failed to load libswscale.so: %s", dlerror());
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }
  LOGD("Loaded libswscale.so");

  g_ffmpeg.avcodec_find_decoder = (decltype(g_ffmpeg.avcodec_find_decoder))dlsym(g_ffmpeg.avcodec_handle, "avcodec_find_decoder");
  g_ffmpeg.avcodec_find_decoder_by_name = (decltype(g_ffmpeg.avcodec_find_decoder_by_name))dlsym(g_ffmpeg.avcodec_handle, "avcodec_find_decoder_by_name");
  g_ffmpeg.avcodec_alloc_context3 = (decltype(g_ffmpeg.avcodec_alloc_context3))dlsym(g_ffmpeg.avcodec_handle, "avcodec_alloc_context3");
  g_ffmpeg.avcodec_open2 = (decltype(g_ffmpeg.avcodec_open2))dlsym(g_ffmpeg.avcodec_handle, "avcodec_open2");
  g_ffmpeg.av_frame_alloc = (decltype(g_ffmpeg.av_frame_alloc))dlsym(g_ffmpeg.avcodec_handle, "av_frame_alloc");
  g_ffmpeg.av_frame_free = (decltype(g_ffmpeg.av_frame_free))dlsym(g_ffmpeg.avcodec_handle, "av_frame_free");
  g_ffmpeg.av_malloc = (decltype(g_ffmpeg.av_malloc))dlsym(g_ffmpeg.avutil_handle, "av_malloc");
  g_ffmpeg.av_free = (decltype(g_ffmpeg.av_free))dlsym(g_ffmpeg.avutil_handle, "av_free");
  g_ffmpeg.avcodec_send_packet = (decltype(g_ffmpeg.avcodec_send_packet))dlsym(g_ffmpeg.avcodec_handle, "avcodec_send_packet");
  g_ffmpeg.avcodec_receive_frame = (decltype(g_ffmpeg.avcodec_receive_frame))dlsym(g_ffmpeg.avcodec_handle, "avcodec_receive_frame");
  g_ffmpeg.avcodec_free_context = (decltype(g_ffmpeg.avcodec_free_context))dlsym(g_ffmpeg.avcodec_handle, "avcodec_free_context");
  g_ffmpeg.avcodec_flush_buffers = (decltype(g_ffmpeg.avcodec_flush_buffers))dlsym(g_ffmpeg.avcodec_handle, "avcodec_flush_buffers");
  g_ffmpeg.av_dict_set = (decltype(g_ffmpeg.av_dict_set))dlsym(g_ffmpeg.avutil_handle, "av_dict_set");
  g_ffmpeg.av_dict_free = (decltype(g_ffmpeg.av_dict_free))dlsym(g_ffmpeg.avutil_handle, "av_dict_free");
  g_ffmpeg.sws_getContext = (decltype(g_ffmpeg.sws_getContext))dlsym(g_ffmpeg.swscale_handle, "sws_getContext");
  g_ffmpeg.sws_scale = (decltype(g_ffmpeg.sws_scale))dlsym(g_ffmpeg.swscale_handle, "sws_scale");
  g_ffmpeg.sws_freeContext = (decltype(g_ffmpeg.sws_freeContext))dlsym(g_ffmpeg.swscale_handle, "sws_freeContext");

  LOGD("Function pointers loaded: avcodec_find_decoder=%p, avcodec_alloc_context3=%p, avcodec_open2=%p, av_frame_alloc=%p",
       (void*)g_ffmpeg.avcodec_find_decoder, (void*)g_ffmpeg.avcodec_alloc_context3,
       (void*)g_ffmpeg.avcodec_open2, (void*)g_ffmpeg.av_frame_alloc);

  if (!g_ffmpeg.avcodec_find_decoder || !g_ffmpeg.avcodec_alloc_context3 || !g_ffmpeg.avcodec_open2 ||
      !g_ffmpeg.av_frame_alloc || !g_ffmpeg.avcodec_send_packet || !g_ffmpeg.avcodec_receive_frame ||
      !g_ffmpeg.sws_getContext || !g_ffmpeg.sws_scale || !g_ffmpeg.sws_freeContext) {
    LOGE("Failed to get FFmpeg function pointers");
    dlclose(g_ffmpeg.swscale_handle);
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }

  if (!g_ffmpeg.av_dict_set || !g_ffmpeg.av_dict_free) {
    LOGW("av_dict_set or av_dict_free not available, using nullptr for options");
  }

  const AVCodec* codec = nullptr;

  if (g_ffmpeg.avcodec_find_decoder_by_name) {
    // Prefer the software "h264" decoder: it outputs YUV420P/NV12 we can
    // convert with swscale. Hardware wrappers (mediacodec/v4l2m2m) may emit
    // opaque surfaces we cannot read back here.
    const char* codec_names[] = {"h264", "libopenh264", "h264_v4l2m2m", nullptr};
    for (int i = 0; codec_names[i] != nullptr; i++) {
      codec = g_ffmpeg.avcodec_find_decoder_by_name(codec_names[i]);
      if (codec) {
        LOGD("Found H264 decoder: %s", codec_names[i]);
        break;
      }
    }
  }

  if (!codec) {
    codec = g_ffmpeg.avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec) {
      LOGD("Found H264 decoder by ID");
    }
  }

  if (!codec) {
    LOGE("H264 decoder not found");
    dlclose(g_ffmpeg.swscale_handle);
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }

  codec_context_ = g_ffmpeg.avcodec_alloc_context3(codec);
  if (!codec_context_) {
    LOGE("Failed to allocate codec context");
    dlclose(g_ffmpeg.swscale_handle);
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }

  LOGD("Calling avcodec_open2 - let FFmpeg use defaults");

  int ret = g_ffmpeg.avcodec_open2((AVCodecContext*)codec_context_, codec, nullptr);

  if (ret < 0) {
    LOGE("Failed to open codec: %d", ret);
    g_ffmpeg.avcodec_free_context((AVCodecContext**)&codec_context_);
    dlclose(g_ffmpeg.swscale_handle);
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }

  LOGD("Codec opened successfully, using requested dimensions %dx%d", width_, height_);

  decoded_frame_ = g_ffmpeg.av_frame_alloc();
  if (!decoded_frame_) {
    LOGE("Failed to allocate decoded frame");
    g_ffmpeg.avcodec_free_context((AVCodecContext**)&codec_context_);
    dlclose(g_ffmpeg.swscale_handle);
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }
  LOGD("Allocated decoded_frame_ at %p", decoded_frame_);

  rgb_buffer_size_ = width_ * height_ * 4;
  rgb_buffer_ = (uint8_t*)g_ffmpeg.av_malloc(rgb_buffer_size_);
  if (!rgb_buffer_) {
    LOGE("Failed to allocate RGB buffer");
    g_ffmpeg.av_frame_free((AVFrame**)&decoded_frame_);
    g_ffmpeg.avcodec_free_context((AVCodecContext**)&codec_context_);
    dlclose(g_ffmpeg.swscale_handle);
    dlclose(g_ffmpeg.avutil_handle);
    dlclose(g_ffmpeg.avcodec_handle);
    return false;
  }

  memset(rgb_buffer_, 0, rgb_buffer_size_);

  initialized_ = true;
  LOGD("H264 decoder initialized: %dx%d", width, height);

  return true;
}

void H264Decoder::Shutdown() {
  if (!initialized_) return;

  if (sws_ctx_ && g_ffmpeg.sws_freeContext) {
    g_ffmpeg.sws_freeContext((SwsContext*)sws_ctx_);
    sws_ctx_ = nullptr;
    sws_src_fmt_ = -1;
    sws_w_ = 0;
    sws_h_ = 0;
  }

  if (rgb_buffer_ && g_ffmpeg.av_free) {
    g_ffmpeg.av_free(rgb_buffer_);
    rgb_buffer_ = nullptr;
  }

  if (decoded_frame_ && g_ffmpeg.av_frame_free) {
    g_ffmpeg.av_frame_free((AVFrame**)&decoded_frame_);
    decoded_frame_ = nullptr;
  }

  if (codec_context_ && g_ffmpeg.avcodec_free_context) {
    g_ffmpeg.avcodec_free_context((AVCodecContext**)&codec_context_);
    codec_context_ = nullptr;
  }

  if (g_ffmpeg.swscale_handle) dlclose(g_ffmpeg.swscale_handle);
  if (g_ffmpeg.avutil_handle) dlclose(g_ffmpeg.avutil_handle);
  if (g_ffmpeg.avcodec_handle) dlclose(g_ffmpeg.avcodec_handle);

  memset(&g_ffmpeg, 0, sizeof(g_ffmpeg));

  initialized_ = false;
  has_new_frame_ = false;
  LOGD("H264 decoder shutdown complete");
}

// Convert a decoded AVFrame to tightly-packed RGBA (stride = width * 4) using
// libswscale. The source pixel format is inferred from the reliably-readable
// data[]/linesize[] planes: 3 planes => YUV420P, 2 planes => NV12. swscale
// handles all color-range/matrix details and is SIMD-accelerated.
void H264Decoder::ConvertFrame(void* frame_ptr) {
  AVFrame* frame = (AVFrame*)frame_ptr;
  if (!frame->data[0] || frame->linesize[0] <= 0) {
    return;
  }

  if (rgb_buffer_size_ < width_ * height_ * 4) {
    LOGE("RGBA buffer too small: %d < %d", rgb_buffer_size_, width_ * height_ * 4);
    return;
  }

  enum AVPixelFormat src_fmt;
  if (frame->data[2]) {
    src_fmt = AV_PIX_FMT_YUV420P;
  } else if (frame->data[1]) {
    src_fmt = AV_PIX_FMT_NV12;
  } else {
    LOGE("Decoded frame has no chroma plane");
    return;
  }

  if (!sws_ctx_ || sws_src_fmt_ != (int)src_fmt || sws_w_ != width_ || sws_h_ != height_) {
    if (sws_ctx_) {
      g_ffmpeg.sws_freeContext((SwsContext*)sws_ctx_);
      sws_ctx_ = nullptr;
    }
    sws_ctx_ = g_ffmpeg.sws_getContext(width_, height_, src_fmt, width_, height_,
                                       AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
      LOGE("sws_getContext failed");
      return;
    }
    sws_src_fmt_ = (int)src_fmt;
    sws_w_ = width_;
    sws_h_ = height_;
    LOGD("Created sws context: %dx%d %d -> RGBA", width_, height_, (int)src_fmt);
  }

  int dst_linesize = width_ * 4;
  int rows = g_ffmpeg.sws_scale((SwsContext*)sws_ctx_,
                                frame->data, frame->linesize,
                                0, height_,
                                &rgb_buffer_, &dst_linesize);
  if (rows > 0) {
    has_new_frame_ = true;
    LOGD("Converted frame to RGBA: %dx%d", width_, height_);
  } else {
    LOGE("sws_scale failed: %d", rows);
  }
}

bool H264Decoder::DecodePacket(const uint8_t* data, int size) {
  if (!initialized_ || !data || size <= 0) {
    return false;
  }

  AVPacket pkt;
  memset(&pkt, 0, sizeof(AVPacket));
  pkt.data = const_cast<uint8_t*>(data);
  pkt.size = size;
  pkt.stream_index = 0;
  pkt.pts = AV_NOPTS_VALUE;
  pkt.dts = AV_NOPTS_VALUE;
  pkt.flags = 0;
  pkt.pos = -1;

  // Drain every frame the decoder can currently produce.
  auto ReceiveAll = [&]() {
    while (true) {
      int ret = g_ffmpeg.avcodec_receive_frame((AVCodecContext*)codec_context_,
                                               (AVFrame*)decoded_frame_);
      if (ret == AVERROR(EAGAIN)) {
        // Decoder needs more input; normal, not an error.
        break;
      }
      if (ret == AVERROR_EOF) {
        LOGD("Decoder EOF");
        break;
      }
      if (ret < 0) {
        LOGE("Error decoding: %d", ret);
        if (ret == AVERROR_INVALIDDATA && g_ffmpeg.avcodec_flush_buffers) {
          LOGE("Invalid data - flushing decoder");
          g_ffmpeg.avcodec_flush_buffers((AVCodecContext*)codec_context_);
        }
        break;
      }
      ConvertFrame(decoded_frame_);
    }
  };

  int ret = g_ffmpeg.avcodec_send_packet((AVCodecContext*)codec_context_, &pkt);
  if (ret == AVERROR(EAGAIN)) {
    // Input buffer full: drain output, then retry the same packet. Do NOT
    // flush here - flushing discards reference frames and corrupts the
    // following P-frames.
    ReceiveAll();
    ret = g_ffmpeg.avcodec_send_packet((AVCodecContext*)codec_context_, &pkt);
  }

  if (ret < 0) {
    LOGE("avcodec_send_packet failed: %d", ret);
    if (data && size > 4) {
      LOGE("Packet size=%d, first 20 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
           size, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
           data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15],
           data[16], data[17], data[18], data[19]);
      if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        LOGE("Has 4-byte start code, NAL type=%d", data[4] & 0x1F);
      } else if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        LOGE("Has 3-byte start code, NAL type=%d", data[3] & 0x1F);
      } else {
        LOGE("No start code found at start!");
      }
    }
    if ((ret == AVERROR(EINVAL) || ret == AVERROR_INVALIDDATA) && g_ffmpeg.avcodec_flush_buffers) {
      LOGE("Invalid data - flushing decoder");
      g_ffmpeg.avcodec_flush_buffers((AVCodecContext*)codec_context_);
    }
    return false;
  }

  ReceiveAll();
  return true;
}

bool H264Decoder::HasDecodedFrame() {
  return initialized_ && has_new_frame_;
}

bool H264Decoder::GetRGBAFrame(uint8_t** out_rgba, int* out_size,
                               int* out_width, int* out_height) {
  if (!initialized_ || !has_new_frame_) {
    LOGE("GetRGBAFrame: not initialized or no frame");
    return false;
  }

  if (!rgb_buffer_) {
    LOGE("GetRGBAFrame: no RGBA buffer");
    has_new_frame_ = false;
    return false;
  }

  LOGD("GetRGBAFrame: returning RGBA frame, size=%d, %dx%d",
       rgb_buffer_size_, width_, height_);

  *out_rgba = rgb_buffer_;
  *out_size = rgb_buffer_size_;
  *out_width = width_;
  *out_height = height_;

  has_new_frame_ = false;
  return true;
}

}  // namespace ndk_cardboardplusplus
