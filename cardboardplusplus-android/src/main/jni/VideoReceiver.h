#ifndef CARDBOARD_PLUS_PLUS_VIDEO_RECEIVER_H
#define CARDBOARD_PLUS_PLUS_VIDEO_RECEIVER_H

#include <stdint.h>
#include <vector>
#include <queue>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>

namespace ndk_cardboardplusplus {

class VideoReceiver {
 public:
  VideoReceiver();
  ~VideoReceiver();

  bool Start(int port);
  void Stop();

  bool HasFrame();
  bool GetFrame(uint8_t** data, int* size);

 private:
  int socket_fd_;
  std::thread receive_thread_;
  std::atomic<bool> running_;

  // Queue of complete, decoder-ready frames. A single buffer was previously
  // used, but the receive thread runs far ahead of the render thread and
  // overwrote it. We keep a queue but bound its depth.
  //
  // Frames are dropped only in whole GOPs: a P-frame decoded without its
  // reference (because an earlier frame was dropped) corrupts the picture
  // until the next keyframe, so we must never break a keyframe->P-frame
  // chain. frame_is_key_ parallels frame_queue_ and records whether each
  // queued frame is a keyframe (detected from its first NAL type: SPS/IDR).
  std::deque<std::vector<uint8_t>> frame_queue_;
  std::deque<bool> frame_is_key_;
  std::vector<uint8_t> current_frame_;
  std::mutex buffer_mutex_;

  static const int kMaxPacketSize = 65536;
  // Upper bound for a single encoded frame. Should comfortably exceed the
  // largest keyframe (IDR) the encoder can emit, or a valid large frame would
  // be mistaken for stream desync and dropped. Tied loosely to a 4K-class SBS
  // stream; raise if you increase resolution/bitrate.
  static const int kMaxFrameSize = 16 * 1024 * 1024;

  // Max queued decoded-ready frames. The receiver runs far ahead of the
  // decoder/render thread; once the backlog exceeds this depth we drop the
  // oldest *complete GOPs* (keeping the newest intact GOP) so latency stays
  // bounded without corrupting the picture. Must be >= the encoder GOP size
  // (see VideoEncoder gop_size) or we would be forced to drop inside a GOP.
  static const size_t kMaxQueueDepth = 24;

  void ReceiveLoop();
};

}  // namespace ndk_cardboardplusplus

#endif  // CARDBOARD_PLUS_PLUS_VIDEO_RECEIVER_H