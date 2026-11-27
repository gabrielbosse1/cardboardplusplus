#ifndef CARDBOARD_PLUS_PLUS_VIDEO_RECEIVER_H
#define CARDBOARD_PLUS_PLUS_VIDEO_RECEIVER_H

#include <stdint.h>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <netinet/in.h>

#include "VideoFraming.h"

namespace ndk_cardboardplusplus {

// Receives the driver's framed video (CBPV header per datagram, see
// VideoFraming.h / CardboardWire.h) on UDP 42069 and hands complete access
// units, plus stream-end events, to the decode thread.
//
// Guarantees to the decoder:
//  - only whole access units, never one with a fragment missing;
//  - after a start, a new session or any loss, nothing but a keyframe until a
//    keyframe arrives (and KEYFRAME_REQ goes to the driver at once, repeated
//    every kNackIntervalMs while still waiting, so recovery takes one round
//    trip instead of the rest of the GOP);
//  - an End item when the stream stops: STREAM_END from the driver, or no
//    datagram for kStreamTimeoutMs. The decode thread tears the decoder down
//    and the renderer goes back to black.
class VideoReceiver {
 public:
  // Why a stream ended (QueueItem::end_reason). Mirrored in VideoDecoder.java.
  enum EndReason { kEndGoodbye = 1, kEndTimeout = 2 };

  using Item = video_framing::QueueItem;

  VideoReceiver();
  ~VideoReceiver();

  bool Start(int port);
  void Stop();

  // Blocks (condition variable, no polling) until a frame or End item is queued
  // or Stop() is called. Returns false on stop with an empty queue.
  bool WaitAndGetItem(Item* out);

 private:
  int socket_fd_;
  std::thread receive_thread_;
  std::atomic<bool> running_;

  // Receive -> decode queue. Bounded by video_framing::TrimQueue, which drops
  // whole GOPs only and never leaves a P-frame without its reference.
  std::deque<Item> queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;

  static const int kMaxPacketSize = 65536;
  // Max queued items before whole GOPs are shed. Above the encoder GOP (10) so a
  // normal backlog never forces a flush back to the next keyframe.
  static const size_t kMaxQueueDepth = 16;
  // No datagram for this long while streaming means the driver is gone (SteamVR
  // killed, PC asleep, Wi-Fi lost). Same value as the Bridge preview.
  static const int kStreamTimeoutMs = 2000;

  // Loss recovery: the driver's discovery port. Mirrors wire::kDiscoveryPort /
  // kKeyframeReq in the driver's CardboardWire.h — change together.
  static const int kDiscoveryPort = 42070;
  // Gap between repeated keyframe requests while the gate is still waiting.
  static const int kNackIntervalMs = 250;

  void ReceiveLoop();
  // Handles one datagram on the receive thread.
  void OnDatagram(const uint8_t* data, size_t len, int64_t now_ms);
  // Queues an item and wakes the decode thread. Returns true when the queue had
  // to be flushed back to the next keyframe.
  bool Push(Item item);
  void PushEnd(int reason);
  // Sends KEYFRAME_REQ to the last-seen video sender (the driver) if the
  // throttle allows. Receive thread only.
  void MaybeSendKeyframeNack(int64_t now_ms, const char* why);

  // Receive-thread state.
  video_framing::Reassembler reassembler_;
  video_framing::KeyframeGate gate_;
  video_framing::RequestThrottle nack_throttle_{kNackIntervalMs};
  bool streaming_ = false;
  int64_t last_datagram_ms_ = 0;
  uint64_t gated_drops_ = 0;
  sockaddr_in last_sender_;
  bool has_sender_ = false;
};

}  // namespace ndk_cardboardplusplus

#endif  // CARDBOARD_PLUS_PLUS_VIDEO_RECEIVER_H
