#include "VideoReceiver.h"
#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>

#define LOG_TAG "VideoReceiver"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
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

  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }

  LOGD("VideoReceiver stopped");
}

bool VideoReceiver::HasFrame() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return !frame_queue_.empty();
}

bool VideoReceiver::GetFrame(uint8_t** data, int* size) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  if (frame_queue_.empty()) {
    return false;
  }

  // Move the oldest queued frame into current_frame_ so the returned pointer
  // stays valid until the next GetFrame call (the caller decodes it
  // synchronously within that window, same contract as before).
  current_frame_ = std::move(frame_queue_.front());
  frame_queue_.pop_front();
  frame_is_key_.pop_front();

  if (current_frame_.empty()) {
    return false;
  }

  *data = current_frame_.data();
  *size = static_cast<int>(current_frame_.size());

  return true;
}

void VideoReceiver::ReceiveLoop() {
  LOGD("Receive loop started, waiting for data...");

  // Reassembly buffer for length-prefixed frames.
  std::vector<uint8_t> buffer;
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

    ssize_t bytes = recv(socket_fd_, packet_buffer.data(), kMaxPacketSize, 0);
    if (bytes <= 0) {
      continue;
    }

    buffer.insert(buffer.end(), packet_buffer.begin(), packet_buffer.begin() + bytes);

    // Parse as many complete length-prefixed frames as we currently have.
    // Wire format per frame: a 4-byte big-endian length N, followed by N
    // payload bytes. Each payload is exactly one libx264 AVPacket, i.e. one
    // full encoded frame (a keyframe packet carries SPS+PPS+all IDR slices; a
    // P-frame packet carries all its slices). Feeding the whole packet to the
    // decoder at once lets FFmpeg assemble multi-slice frames correctly,
    // instead of the old behaviour that split every NAL into its own access
    // unit (so only the last slice of an IDR keyframe survived).
    while (buffer.size() >= 4) {
      size_t frame_len = ((size_t)buffer[0] << 24) |
                         ((size_t)buffer[1] << 16) |
                         ((size_t)buffer[2] << 8) |
                         ((size_t)buffer[3]);

      if (frame_len == 0 || frame_len > kMaxFrameSize) {
        // Invalid length: stream desynced (e.g. a UDP datagram was lost).
        // Drop the buffer and resync from the next datagram.
        LOGE("Invalid frame length %zu, resetting reassembly buffer", frame_len);
        buffer.clear();
        break;
      }

      if (buffer.size() < 4 + frame_len) {
        // Frame not fully arrived yet; wait for more datagrams.
        break;
      }

      std::vector<uint8_t> frame(buffer.begin() + 4, buffer.begin() + 4 + frame_len);
      buffer.erase(buffer.begin(), buffer.begin() + 4 + frame_len);

      // Detect keyframes from the payload's first NAL unit. libx264 emits
      // SPS(7)/IDR(5) only inside keyframe packets; P-frame packets start with
      // a slice (type 1) or SEI(6). This lets us drop whole GOPs without a
      // keyframe flag on the wire.
      bool is_key = false;
      for (size_t i = 0; i + 3 < frame.size(); ++i) {
        if (frame[i] == 0x00 && frame[i+1] == 0x00 && frame[i+2] == 0x01) {
          int nal_type = frame[i+3] & 0x1F;
          is_key = (nal_type == 5 || nal_type == 7);
          break;
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
        LOGD("Queued frame, payload=%zu, key=%d, queue_size=%zu", frame_len, is_key, frame_queue_.size());
      }
    }
  }

  LOGD("Receive loop ended");
}

}  // namespace ndk_cardboardplusplus
