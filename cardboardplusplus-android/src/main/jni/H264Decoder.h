#ifndef CARDBOARD_PLUS_PLUS_H264_DECODER_H
#define CARDBOARD_PLUS_PLUS_H264_DECODER_H

#include <stdint.h>
#include <memory>

namespace ndk_cardboardplusplus {

class H264Decoder {
 public:
  H264Decoder();
  ~H264Decoder();

  bool Initialize(int width, int height);
  void Shutdown();

  bool DecodePacket(const uint8_t* data, int size);
  bool HasDecodedFrame();
  // Returns the latest decoded frame converted to tightly-packed RGBA
  // (stride = width * 4). *out_size is width * height * 4.
  bool GetRGBAFrame(uint8_t** out_rgba, int* out_size,
                    int* out_width, int* out_height);

  int GetWidth() const { return width_; }
  int GetHeight() const { return height_; }

 private:
  // Convert a decoded AVFrame (void* to avoid leaking FFmpeg types into the
  // header) to the RGBA buffer via libswscale.
  void ConvertFrame(void* frame);

  int width_;
  int height_;
  bool initialized_;
  bool has_new_frame_;

  void* codec_context_;
  void* decoded_frame_;
  uint8_t* rgb_buffer_;
  int rgb_buffer_size_;

  // libswscale conversion context (created lazily, recreated on format/size
  // change). Stored as void* to keep FFmpeg types out of this header.
  void* sws_ctx_;
  int sws_src_fmt_;
  int sws_w_;
  int sws_h_;
};

}  // namespace ndk_cardboardplusplus

#endif  // CARDBOARD_PLUS_PLUS_H264_DECODER_H