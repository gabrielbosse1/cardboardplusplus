#include "VideoReceiver.h"
#include <android/log.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>

#define LOG_TAG "VideoReceiver"
// Debug logs compile out in release (NDEBUG). Warnings and errors always fire.
#ifdef NDEBUG
#define LOGD(...) ((void)0)
#else
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace ndk_cardboardplusplus {

namespace {
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

VideoReceiver::VideoReceiver()
    : socket_fd_(-1), running_(false) {
  memset(&last_sender_, 0, sizeof(last_sender_));
}

VideoReceiver::~VideoReceiver() {
  Stop();
}

bool VideoReceiver::Start(int port) {
  if (running_) {
    LOGD("Already running");
    return false;
  }

  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    LOGE("Failed to create socket");
    return false;
  }

  int reuse = 1;
  setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  int recv_buf = 4 * 1024 * 1024;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOGE("Failed to bind to port %d", port);
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  LOGD("UDP receiver bound to port %d", port);

  // A restart begins from Idle: no session, gate waiting for a keyframe.
  reassembler_.Clear();
  gate_ = video_framing::KeyframeGate();
  nack_throttle_.Reset();
  streaming_ = false;
  gated_drops_ = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
  }

  running_ = true;
  receive_thread_ = std::thread(&VideoReceiver::ReceiveLoop, this);

  return true;
}

void VideoReceiver::Stop() {
  if (!running_) return;

  running_ = false;

  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RDWR);
  }

  // Wake the decode thread out of WaitAndGetItem so join() can't hang.
  queue_cv_.notify_all();

  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
  // Closed only after the receive thread is gone: it still sends KEYFRAME_REQ
  // on this descriptor and must never write to a reused fd number.
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }

  LOGD("VideoReceiver stopped");
}

bool VideoReceiver::WaitAndGetItem(Item* out) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  queue_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
  if (queue_.empty()) {
    return false;  // Stopped with nothing queued.
  }
  *out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

bool VideoReceiver::Push(Item item) {
  bool flushed;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(item));
    flushed = video_framing::TrimQueue(&queue_, kMaxQueueDepth);
  }
  queue_cv_.notify_one();
  return flushed;
}

void VideoReceiver::PushEnd(int reason) {
  Item end;
  end.kind = Item::kEnd;
  end.end_reason = reason;
  {
    // Frames still queued belong to the stream that just ended; decoding them
    // would only delay the return to idle.
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::deque<Item> kept;
    for (auto& it : queue_) {
      if (it.kind == Item::kEnd) kept.push_back(std::move(it));
    }
    queue_.swap(kept);
    queue_.push_back(std::move(end));
  }
  queue_cv_.notify_one();
  streaming_ = false;
  gate_.Invalidate();
  nack_throttle_.Reset();
}

void VideoReceiver::MaybeSendKeyframeNack(int64_t now_ms, const char* why) {
  if (!has_sender_ || socket_fd_ < 0) return;
  if (!nack_throttle_.Allow(now_ms)) return;

  // Wire string shared with the PC driver (see CardboardWire.h) — the driver
  // forces the next frame to IDR and replies with nothing.
  static const char kKeyframeReq[] = "KEYFRAME_REQ";
  sockaddr_in target = last_sender_;
  target.sin_port = htons(kDiscoveryPort);
  ssize_t sent = sendto(socket_fd_, kKeyframeReq, sizeof(kKeyframeReq) - 1, 0,
                        (sockaddr*)&target, sizeof(target));
  if (sent < 0) {
    LOGW("KEYFRAME_REQ send failed (%s)", why);
  } else {
    LOGW("KEYFRAME_REQ sent (%s), holding last picture until the IDR", why);
  }
}

void VideoReceiver::OnDatagram(const uint8_t* data, size_t len, int64_t now_ms) {
  video_framing::Output out = reassembler_.Push(data, len);
  if (out.kind == video_framing::Output::kEnd) {
    LOGI("STREAM_END from driver (session %08X): back to idle", out.session);
    PushEnd(kEndGoodbye);
    return;
  }
  if (out.kind != video_framing::Output::kFrame) return;

  if (out.new_session) {
    LOGI("New driver session %08X: waiting for its keyframe", out.session);
    gate_.Invalidate();
  }
  if (out.loss) {
    const video_framing::Stats& st = reassembler_.stats();
    LOGW("Video loss before frame %u (incomplete=%llu skipped=%llu total)", out.frame_id,
         (unsigned long long)st.incomplete, (unsigned long long)st.skipped);
    gate_.Invalidate();
    // A fresh loss gets its own request immediately.
    nack_throttle_.Reset();
  }
  if (!gate_.Admit(out.keyframe)) {
    gated_drops_++;
    MaybeSendKeyframeNack(now_ms, out.loss ? "loss" : "waiting for keyframe");
    LOGD("Gated frame %u (not a keyframe), drops=%llu", out.frame_id,
         (unsigned long long)gated_drops_);
    return;
  }
  if (out.keyframe) nack_throttle_.Reset();
  if (!streaming_) {
    LOGI("Streaming: first frame %u (session %08X, %zu bytes)", out.frame_id, out.session,
         out.data.size());
    streaming_ = true;
  }

  Item item;
  item.kind = Item::kFrame;
  item.keyframe = out.keyframe;
  item.frame_id = out.frame_id;
  size_t size = out.data.size();
  item.data.swap(out.data);
  if (Push(std::move(item))) {
    // The backlog was one oversized GOP: everything queued was dropped, so the
    // next decodable frame is a keyframe.
    LOGW("Decode backlog flushed at frame %u: waiting for a keyframe", out.frame_id);
    gate_.Invalidate();
    nack_throttle_.Reset();
    MaybeSendKeyframeNack(now_ms, "backlog flush");
  }
  LOGD("Queued frame %u, size=%zu, key=%d", out.frame_id, size, out.keyframe ? 1 : 0);
  (void)size;
}

void VideoReceiver::ReceiveLoop() {
  LOGD("Receive loop started, waiting for data...");
  std::vector<uint8_t> packet(kMaxPacketSize);

  while (running_) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;

    int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
    int64_t now = NowMs();
    if (ready <= 0) {
      if (streaming_ && now - last_datagram_ms_ > kStreamTimeoutMs) {
        LOGW("No video for %d ms: stream timed out, back to idle", kStreamTimeoutMs);
        PushEnd(kEndTimeout);
      }
      continue;
    }

    // Drain everything the kernel holds before going back to select, so a
    // keyframe burst is pulled out of the socket buffer as fast as it lands.
    for (;;) {
      sockaddr_in sender;
      socklen_t sender_len = sizeof(sender);
      ssize_t bytes = recvfrom(socket_fd_, packet.data(), kMaxPacketSize, MSG_DONTWAIT,
                               (sockaddr*)&sender, &sender_len);
      if (bytes <= 0) break;
      // The only sender on the video port is the driver; remember it so loss
      // recovery (KEYFRAME_REQ) knows where to send.
      last_sender_ = sender;
      has_sender_ = true;
      last_datagram_ms_ = now;
      OnDatagram(packet.data(), (size_t)bytes, now);
      if (!running_) break;
    }
  }

  LOGD("Receive loop ended");
}

}  // namespace ndk_cardboardplusplus
