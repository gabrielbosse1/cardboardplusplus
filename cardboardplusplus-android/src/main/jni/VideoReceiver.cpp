#include "VideoReceiver.h"
#include <android/log.h>
#include <sys/socket.h>
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

namespace ndk_cardboardplusplus {

VideoReceiver::VideoReceiver()
    : socket_fd_(-1), running_(false) {
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

  running_ = true;
  receive_thread_ = std::thread(&VideoReceiver::ReceiveLoop, this);

  return true;
}

void VideoReceiver::Stop() {
  if (!running_) return;

  running_ = false;

  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RDWR);
    close(socket_fd_);
    socket_fd_ = -1;
  }

  // Wake the forwarding thread out of WaitAndGetFrame so join() can't hang.
  frame_cv_.notify_all();

  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }

  LOGD("VideoReceiver stopped");
}

bool VideoReceiver::HasFrame() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return !frame_queue_.empty();
}

bool VideoReceiver::GetFrame(uint8_t** data, int* size, bool* is_key) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  if (frame_queue_.empty()) {
    return false;
  }

  // Move the oldest queued frame into current_frame_ so the returned pointer
  // stays valid until the next GetFrame call (the caller forwards it
  // synchronously within that window, same contract as before).
  current_frame_ = std::move(frame_queue_.front());
  frame_queue_.pop_front();
  current_is_key_ = frame_is_key_.front();
  frame_is_key_.pop_front();

  if (current_frame_.empty()) {
    return false;
  }

  *data = current_frame_.data();
  *size = static_cast<int>(current_frame_.size());
  if (is_key) *is_key = current_is_key_;

  return true;
}

bool VideoReceiver::WaitAndGetFrame(uint8_t** data, int* size, bool* is_key) {
  std::unique_lock<std::mutex> lock(buffer_mutex_);
  frame_cv_.wait(lock, [this] { return !frame_queue_.empty() || !running_; });
  if (frame_queue_.empty()) {
    return false;  // Stopped with nothing queued.
  }

  current_frame_ = std::move(frame_queue_.front());
  frame_queue_.pop_front();
  current_is_key_ = frame_is_key_.front();
  frame_is_key_.pop_front();

  if (current_frame_.empty()) {
    return false;
  }

  *data = current_frame_.data();
  *size = static_cast<int>(current_frame_.size());
  if (is_key) *is_key = current_is_key_;

  return true;
}

void VideoReceiver::MaybeSendKeyframeNack() {
  if (!has_sender_) return;
  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_nack_).count();
  if (elapsed_ms < kNackIntervalMs) return;
  last_nack_ = now;

  // Wire string shared with the PC driver (see CardboardWire.h) — the driver
  // forces the next frame to IDR and replies with nothing.
  static const char kKeyframeReq[] = "KEYFRAME_REQ";
  sockaddr_in target = last_sender_;
  target.sin_port = htons(kDiscoveryPort);
  ssize_t sent = sendto(socket_fd_, kKeyframeReq, sizeof(kKeyframeReq) - 1, 0,
                        (sockaddr*)&target, sizeof(target));
  if (sent < 0) {
    LOGW("KEYFRAME_REQ send failed");
  } else {
    LOGW("Video desync: sent KEYFRAME_REQ, awaiting forced IDR");
  }
}

void VideoReceiver::ReceiveLoop() {
  LOGD("Receive loop started, waiting for data...");

  // Reassembly buffer for length-prefixed frames. buf_head_ marks consumed
  // bytes so fully-arrived frames advance an offset instead of memmove-ing
  // the whole remainder (compaction only when the head grows large).
  std::vector<uint8_t> buffer;
  size_t buf_head_ = 0;
  std::vector<uint8_t> packet_buffer(kMaxPacketSize);

  while (running_) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;

    int select_result = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
    if (select_result <= 0) {
      continue;
    }

    sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    ssize_t bytes = recvfrom(socket_fd_, packet_buffer.data(), kMaxPacketSize, 0,
                             (sockaddr*)&sender, &sender_len);
    if (bytes <= 0) {
      continue;
    }

    // The only sender on the video port is the driver; remember it so loss
    // recovery (KEYFRAME_REQ) knows where to send.
    last_sender_ = sender;
    has_sender_ = true;

    buffer.insert(buffer.end(), packet_buffer.begin(), packet_buffer.begin() + bytes);

    // Parse as many complete length-prefixed frames as we currently have.
    // Wire format per frame: a 4-byte big-endian length N, followed by N
    // payload bytes. Each payload is exactly one libx264 AVPacket, i.e. one
    // full encoded frame (a keyframe packet carries SPS+PPS+all IDR slices; a
    // P-frame packet carries all its slices). The whole packet is queued for
    // the Java MediaCodec instead of splitting NALs into access units (so
    // only the last slice of an IDR keyframe would survive).
    while (buffer.size() - buf_head_ >= 4) {
      const uint8_t* base = buffer.data() + buf_head_;
      size_t frame_len = ((size_t)base[0] << 24) |
                         ((size_t)base[1] << 16) |
                         ((size_t)base[2] << 8) |
                         ((size_t)base[3]);

      if (frame_len == 0 || frame_len > (size_t)kMaxFrameSize) {
        // Invalid length: stream desynced (e.g. a UDP datagram was lost).
        // Drop the buffer and resync from the next datagram.
        LOGE("Invalid frame length %zu, resetting reassembly buffer", frame_len);
        buffer.clear();
        buf_head_ = 0;
        MaybeSendKeyframeNack();
        break;
      }

      if (buffer.size() - buf_head_ < 4 + frame_len) {
        // Frame not fully arrived yet; if the backlog already exceeds the
        // largest plausible frame, we missed its length prefix long ago —
        // desync now instead of buffering megabytes. Otherwise wait for more
        // datagrams.
        if (buffer.size() - buf_head_ > (size_t)kMaxFrameSize) {
          LOGE("Reassembly backlog %zu exceeds max frame, resyncing",
               buffer.size() - buf_head_);
          buffer.clear();
          buf_head_ = 0;
          MaybeSendKeyframeNack();
          break;
        }
        break;
      }

      std::vector<uint8_t> frame(base + 4, base + 4 + frame_len);
      buf_head_ += 4 + frame_len;
      // Compact only when the consumed head is large, keeping the common
      // case a pointer bump with no memmove.
      if (buf_head_ == buffer.size()) {
        buffer.clear();
        buf_head_ = 0;
      } else if (buf_head_ > 1024 * 1024) {
        buffer.erase(buffer.begin(), buffer.begin() + buf_head_);
        buf_head_ = 0;
      }

      // Detect keyframes by scanning the whole payload for any SPS(7) or
      // IDR(5) NAL. libx264 emits AUD(9) before a keyframe, so checking only
      // the first NAL would miss real keyframes and mark them as P-frames.
      bool is_key = false;
      for (size_t i = 0; i + 4 < frame.size(); ++i) {
        if (frame[i] == 0x00 && frame[i+1] == 0x00 &&
            frame[i+2] == 0x00 && frame[i+3] == 0x01) {
          // 4-byte start code: NAL header at i+4
          int nal_type = frame[i+4] & 0x1F;
          if (nal_type == 5 || nal_type == 7) { is_key = true; break; }
        } else if (frame[i] == 0x00 && frame[i+1] == 0x00 && frame[i+2] == 0x01) {
          // 3-byte start code: NAL header at i+3
          int nal_type = frame[i+3] & 0x1F;
          if (nal_type == 5 || nal_type == 7) { is_key = true; break; }
        }
      }

      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        // Bound latency without corrupting the picture: only ever drop
        // complete GOPs. We keep the newest intact GOP and discard everything
        // before its keyframe, so no P-frame ever loses its reference.
        while (frame_queue_.size() > kMaxQueueDepth) {
          size_t last_key = 0;
          bool found_key = false;
          for (size_t i = 0; i < frame_queue_.size(); ++i) {
            if (frame_is_key_[i]) { last_key = i; found_key = true; }
          }
          if (!found_key) {
            // No keyframe buffered (all P-frames after a dropped GOP): drop
            // the oldest to avoid unbounded growth. Recovers at next keyframe.
            frame_queue_.pop_front();
            frame_is_key_.pop_front();
            continue;
          }
          if (last_key == 0) {
            // The whole backlog is a single GOP (keyframe at the front) that
            // already exceeds the depth: keep the keyframe and shed the
            // oldest P-frames (from the back). Never drop the keyframe here,
            // or every following P-frame would decode against a missing
            // reference and corrupt the picture.
            frame_queue_.pop_back();
            frame_is_key_.pop_back();
            continue;
          }
          while (last_key > 0) {
            frame_queue_.pop_front();
            frame_is_key_.pop_front();
            --last_key;
          }
          break;
        }
        frame_queue_.push_back(std::move(frame));
        frame_is_key_.push_back(is_key);
        frame_cv_.notify_one();
        LOGD("Queued frame, payload=%zu, key=%d, queue_size=%zu", frame_len, is_key, frame_queue_.size());
      }
    }
  }

  LOGD("Receive loop ended");
}

}  // namespace ndk_cardboardplusplus
