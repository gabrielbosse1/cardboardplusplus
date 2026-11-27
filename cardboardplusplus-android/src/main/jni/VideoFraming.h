#ifndef CARDBOARD_PLUS_PLUS_VIDEO_FRAMING_H
#define CARDBOARD_PLUS_PLUS_VIDEO_FRAMING_H

// Phone side of the driver -> phone video framing (UDP 42069). The layout is the
// one documented in driver_cardboardplusplus/include/CardboardWire.h (CBPV, the
// same header the Bridge preview uses); the constants below mirror it and the
// driver tests check both sides byte for byte against each other.
//
// Pure C++14 with no Android or socket dependency: VideoReceiver.cpp runs it on
// the receive thread, and driver_cardboardplusplus/tests/test_wire_protocol.cpp
// compiles it on the host to feed it the driver's real packetizer output.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace video_framing {

// --- Wire constants (mirror wire::kPreview* in CardboardWire.h) -------------
static const uint32_t kMagic = 0x43425056u;  // "CBPV"
static const uint8_t kVersion = 1;
static const std::size_t kHeaderSize = 24;
static const std::size_t kMaxDatagram = 1400;
static const std::size_t kMaxPayload = kMaxDatagram - kHeaderSize;
static const uint8_t kFlagKeyframe = 0x01;
static const uint8_t kFlagEnd = 0x02;
// Largest access unit accepted. Far above any keyframe the encoder emits; a
// header claiming more is garbage, not a frame.
static const uint32_t kMaxFrameSize = 16u * 1024u * 1024u;

struct Header {
  uint8_t flags = 0;
  uint16_t frag_index = 0;
  uint16_t frag_count = 0;
  uint16_t payload_len = 0;
  uint32_t session = 0;
  uint32_t frame_id = 0;
  uint32_t frame_size = 0;
};

inline uint16_t GetU16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t GetU32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Parses and validates one datagram header. False for foreign or garbled data
// (same rules as preview::ReadHeader in the driver and Header::parse in the bridge).
inline bool ParseHeader(const uint8_t* d, std::size_t len, Header* h) {
  if (len < kHeaderSize) return false;
  if (GetU32(d) != kMagic || d[4] != kVersion) return false;
  h->flags = d[5];
  h->frag_index = GetU16(d + 6);
  h->frag_count = GetU16(d + 8);
  h->payload_len = GetU16(d + 10);
  h->session = GetU32(d + 12);
  h->frame_id = GetU32(d + 16);
  h->frame_size = GetU32(d + 20);
  if ((std::size_t)h->payload_len != len - kHeaderSize) return false;
  if (h->payload_len > kMaxPayload) return false;
  if (h->frag_count == 0) return h->payload_len == 0;  // control datagram
  if (h->frag_index >= h->frag_count) return false;
  return true;
}

// True when an Annex-B access unit carries an SPS (7) or IDR slice (5). Used in
// addition to the header flag so a keyframe is never mistaken for a P-frame.
inline bool HasKeyframeNal(const uint8_t* p, std::size_t n) {
  for (std::size_t i = 0; i + 3 < n; ++i) {
    if (p[i] != 0 || p[i + 1] != 0) continue;
    std::size_t at = 0;
    if (p[i + 2] == 1) {
      at = i + 3;
    } else if (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1) {
      at = i + 4;
    } else {
      continue;
    }
    int type = p[at] & 0x1F;
    if (type == 5 || type == 7) return true;
  }
  return false;
}

// What one datagram produced.
struct Output {
  enum Kind { kNone, kFrame, kEnd };
  Kind kind = kNone;
  // Frames were lost or arrived incomplete since the last delivered frame. The
  // caller must stop feeding P-frames until a keyframe and ask for one.
  bool loss = false;
  // The driver started a new session (encoder re-init); frame ids restarted.
  bool new_session = false;
  uint32_t session = 0;
  uint32_t frame_id = 0;
  bool keyframe = false;
  std::vector<uint8_t> data;  // complete access unit when kind == kFrame
};

// Counters for logs and tests.
struct Stats {
  uint64_t datagrams = 0;
  uint64_t rejected = 0;   // not a valid header, or inconsistent with its frame
  uint64_t stale = 0;      // fragment of a frame already delivered or abandoned
  uint64_t duplicates = 0;
  uint64_t frames = 0;     // complete frames delivered
  uint64_t incomplete = 0; // frames abandoned with fragments missing
  uint64_t skipped = 0;    // frame ids never seen at all (whole frame lost)
  uint64_t ends = 0;
};

// Rebuilds complete access units from fragments. A frame is delivered only when
// every fragment arrived; a frame that is superseded while incomplete is dropped
// and reported as loss, never handed on partially. The driver sends frames in
// order, so one frame is assembled at a time.
class Reassembler {
 public:
  Output Push(const uint8_t* d, std::size_t len) {
    Output out;
    stats_.datagrams++;
    Header h;
    if (!ParseHeader(d, len, &h)) {
      stats_.rejected++;
      return out;
    }
    if (h.frag_count == 0) {
      if ((h.flags & kFlagEnd) == 0) return out;  // unknown control datagram
      // Goodbye for the live session, or the first one after a restart with
      // nothing received yet. Repeats (the driver sends two) are ignored.
      if (ended_ && h.session == ended_session_) return out;
      if (has_session_ && h.session != session_) return out;
      out.kind = Output::kEnd;
      out.session = h.session;
      stats_.ends++;
      ended_ = true;
      ended_session_ = h.session;
      Reset();
      return out;
    }
    if (h.frame_size == 0 || h.frame_size > kMaxFrameSize ||
        h.frag_count != FragmentCount(h.frame_size)) {
      stats_.rejected++;
      return out;
    }
    std::size_t offset = (std::size_t)h.frag_index * kMaxPayload;
    std::size_t expect = h.frame_size - offset;
    if (expect > kMaxPayload) expect = kMaxPayload;
    if (offset >= h.frame_size || h.payload_len != expect) {
      stats_.rejected++;
      return out;
    }
    if (!has_session_ || h.session != session_) {
      if (ended_ && h.session == ended_session_) {
        stats_.stale++;  // late fragment of the session that just said goodbye
        return out;
      }
      bool had = has_session_;
      Reset();
      has_session_ = true;
      session_ = h.session;
      ended_ = false;
      pending_new_session_ = had;
    }
    // Ordering against what was already delivered or abandoned.
    if (has_done_ && !After(h.frame_id, done_id_)) {
      stats_.stale++;
      return out;
    }
    if (assembling_ && h.frame_id != cur_id_) {
      if (!After(h.frame_id, cur_id_)) {
        stats_.stale++;
        return out;
      }
      // A newer frame started while this one still misses fragments.
      Abandon();
    }
    if (!assembling_) {
      assembling_ = true;
      cur_id_ = h.frame_id;
      cur_size_ = h.frame_size;
      cur_count_ = h.frag_count;
      cur_flags_ = h.flags;
      got_count_ = 0;
      buf_.resize(h.frame_size);
      got_.assign(h.frag_count, false);
    } else if (h.frame_size != cur_size_ || h.frag_count != cur_count_ ||
               (h.flags & kFlagKeyframe) != (cur_flags_ & kFlagKeyframe)) {
      stats_.rejected++;
      return out;
    }
    if (got_[h.frag_index]) {
      stats_.duplicates++;
      return out;
    }
    got_[h.frag_index] = true;
    got_count_++;
    std::copy(d + kHeaderSize, d + kHeaderSize + h.payload_len, buf_.begin() + offset);
    if (got_count_ < cur_count_) return out;

    // Complete. Frame ids that never showed up before this one were lost whole.
    if (has_done_ && cur_id_ != done_id_ + 1) {
      stats_.skipped += (uint32_t)(cur_id_ - done_id_ - 1);
      pending_loss_ = true;
    }
    out.kind = Output::kFrame;
    out.loss = pending_loss_;
    out.new_session = pending_new_session_;
    out.session = session_;
    out.frame_id = cur_id_;
    out.keyframe = (cur_flags_ & kFlagKeyframe) != 0 || HasKeyframeNal(buf_.data(), buf_.size());
    out.data.swap(buf_);
    stats_.frames++;
    has_done_ = true;
    done_id_ = cur_id_;
    assembling_ = false;
    pending_loss_ = false;
    pending_new_session_ = false;
    return out;
  }

  // Forgets all in-flight state (receiver restart).
  void Clear() {
    Reset();
    ended_ = false;
  }

  const Stats& stats() const { return stats_; }

  static uint32_t FragmentCount(uint32_t size) {
    if (size == 0) return 0;
    uint32_t n = (uint32_t)((size + kMaxPayload - 1) / kMaxPayload);
    return n > 0xFFFFu ? 0 : n;
  }

 private:
  // Serial-number order on frame ids, so wrap-around at 2^32 keeps working.
  static bool After(uint32_t a, uint32_t b) { return a != b && (uint32_t)(a - b) < 0x80000000u; }

  void Abandon() {
    stats_.incomplete++;
    has_done_ = true;
    done_id_ = cur_id_;
    assembling_ = false;
    pending_loss_ = true;
  }

  void Reset() {
    has_session_ = false;
    has_done_ = false;
    assembling_ = false;
    pending_loss_ = false;
    pending_new_session_ = false;
    got_count_ = 0;
    buf_.clear();
    got_.clear();
  }

  Stats stats_;
  bool has_session_ = false;
  uint32_t session_ = 0;
  bool ended_ = false;
  uint32_t ended_session_ = 0;
  bool has_done_ = false;
  uint32_t done_id_ = 0;
  bool assembling_ = false;
  uint32_t cur_id_ = 0;
  uint32_t cur_size_ = 0;
  uint16_t cur_count_ = 0;
  uint8_t cur_flags_ = 0;
  uint16_t got_count_ = 0;
  bool pending_loss_ = false;
  bool pending_new_session_ = false;
  std::vector<uint8_t> buf_;
  std::vector<bool> got_;
};

// Decides which complete frames may reach the decoder. After a start, a new
// session or any loss, P-frames reference pictures the decoder never had, so
// nothing but a keyframe is admitted until one arrives.
class KeyframeGate {
 public:
  // Returns true when the frame may be decoded.
  bool Admit(bool keyframe) {
    if (keyframe) {
      waiting_ = false;
      return true;
    }
    return !waiting_;
  }
  void Invalidate() { waiting_ = true; }
  bool waiting() const { return waiting_; }

 private:
  bool waiting_ = true;
};

// Rate limit for KEYFRAME_REQ. The first request after a loss goes out at once;
// while the gate keeps waiting, a request is repeated every interval so a lost
// request (or a lost keyframe) cannot leave the picture frozen.
class RequestThrottle {
 public:
  explicit RequestThrottle(int64_t interval_ms) : interval_ms_(interval_ms) {}
  bool Allow(int64_t now_ms) {
    if (has_last_ && now_ms - last_ms_ < interval_ms_) return false;
    has_last_ = true;
    last_ms_ = now_ms;
    return true;
  }
  void Reset() { has_last_ = false; }

 private:
  int64_t interval_ms_;
  bool has_last_ = false;
  int64_t last_ms_ = 0;
};

// One entry of the receive -> decode queue.
struct QueueItem {
  enum Kind { kFrame, kEnd };
  Kind kind = kFrame;
  bool keyframe = false;
  uint32_t frame_id = 0;
  int end_reason = 0;
  std::vector<uint8_t> data;
};

// Bounds the decode backlog without ever creating a reference hole. Whole GOPs
// are dropped from the front (everything before the newest queued keyframe).
// If the backlog is one oversized GOP with no later keyframe to cut to, all
// queued frames are dropped and true is returned: the caller must gate on the
// next keyframe and request one. Dropping only some P-frames, as the old queue
// did, left the decoder predicting from pictures it never received.
// End markers are kept: the decoder must still tear down.
inline bool TrimQueue(std::deque<QueueItem>* q, std::size_t max_depth) {
  if (q->size() <= max_depth) return false;
  std::size_t cut = 0;
  bool found = false;
  for (std::size_t i = q->size(); i-- > 1;) {
    const QueueItem& it = (*q)[i];
    if (it.kind == QueueItem::kFrame && it.keyframe) {
      cut = i;
      found = true;
      break;
    }
  }
  std::deque<QueueItem> kept;
  bool flushed = false;
  if (found && q->size() - cut <= max_depth) {
    for (std::size_t i = 0; i < q->size(); ++i) {
      QueueItem& it = (*q)[i];
      if (i >= cut || it.kind == QueueItem::kEnd) kept.push_back(std::move(it));
    }
  } else {
    for (std::size_t i = 0; i < q->size(); ++i) {
      QueueItem& it = (*q)[i];
      if (it.kind == QueueItem::kEnd) kept.push_back(std::move(it));
    }
    flushed = true;
  }
  q->swap(kept);
  return flushed;
}

}  // namespace video_framing

#endif  // CARDBOARD_PLUS_PLUS_VIDEO_FRAMING_H
