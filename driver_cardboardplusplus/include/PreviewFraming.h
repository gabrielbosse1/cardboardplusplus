#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "CardboardWire.h"
// Bridge preview packetizer (layout documented in CardboardWire.h). Header-only
// and socket-free so the driver tests exercise the exact bytes the driver sends.
namespace preview {

struct Header {
    uint8_t flags = 0;
    uint16_t fragIndex = 0;
    uint16_t fragCount = 0;
    uint16_t payloadLen = 0;
    uint32_t session = 0;
    uint32_t frameId = 0;
    uint32_t frameSize = 0;
};

inline void PutU16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void PutU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
inline uint16_t GetU16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t GetU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Writes the 24-byte header into out (caller guarantees kPreviewHeaderSize bytes).
inline void WriteHeader(uint8_t* out, const Header& h)
{
    PutU32(out + 0, wire::kPreviewMagic);
    out[4] = wire::kPreviewVersion;
    out[5] = h.flags;
    PutU16(out + 6, h.fragIndex);
    PutU16(out + 8, h.fragCount);
    PutU16(out + 10, h.payloadLen);
    PutU32(out + 12, h.session);
    PutU32(out + 16, h.frameId);
    PutU32(out + 20, h.frameSize);
}

// Parses and validates a header; false for foreign/garbled datagrams. Used by the tests
// as the reference decoder for what the bridge's preview.rs accepts.
inline bool ReadHeader(const uint8_t* in, std::size_t len, Header* h)
{
    if (len < wire::kPreviewHeaderSize) return false;
    if (GetU32(in) != wire::kPreviewMagic || in[4] != wire::kPreviewVersion) return false;
    h->flags = in[5];
    h->fragIndex = GetU16(in + 6);
    h->fragCount = GetU16(in + 8);
    h->payloadLen = GetU16(in + 10);
    h->session = GetU32(in + 12);
    h->frameId = GetU32(in + 16);
    h->frameSize = GetU32(in + 20);
    if ((std::size_t)h->payloadLen != len - wire::kPreviewHeaderSize) return false;
    if (h->payloadLen > wire::kPreviewMaxPayload) return false;
    if (h->fragCount == 0) return h->payloadLen == 0;  // control datagram
    if (h->fragIndex >= h->fragCount) return false;
    return true;
}

// Writes the STREAM_END control datagram for `session` into out (kPreviewHeaderSize
// bytes) and returns its length. It has no payload.
inline std::size_t WriteEnd(uint8_t* out, uint32_t session)
{
    Header h;
    h.flags = wire::kPreviewFlagEnd;
    h.session = session;
    WriteHeader(out, h);
    return wire::kPreviewHeaderSize;
}

// True when a parsed header is the STREAM_END control datagram.
inline bool IsEnd(const Header& h)
{
    return h.fragCount == 0 && (h.flags & wire::kPreviewFlagEnd) != 0;
}

// Fragments needed for an access unit of `size` bytes; 0 when it cannot be framed.
inline uint32_t FragmentCount(uint32_t size)
{
    if (size == 0) return 0;
    uint32_t n = (uint32_t)((size + wire::kPreviewMaxPayload - 1) / wire::kPreviewMaxPayload);
    return n > 0xFFFFu ? 0 : n;
}

// Result of handing one access unit to the transport.
struct SendResult {
    uint32_t fragments = 0;  // fragments the access unit needed
    uint32_t sent = 0;       // fragments the sink accepted before stopping
    bool complete() const { return fragments > 0 && sent == fragments; }
};

// Splits one access unit into header+fragment datagrams and hands each to
// sink(const uint8_t* datagram, size_t len) -> bool. A false return stops the
// frame: the bridge sees frag_count and discards the incomplete frame instead
// of decoding it. scratch must hold kPreviewMaxDatagram bytes.
template <typename Sink>
inline SendResult SendFrame(const uint8_t* data, uint32_t size, uint32_t session,
                            uint32_t frameId, bool keyframe, uint8_t* scratch, Sink sink)
{
    SendResult r;
    r.fragments = FragmentCount(size);
    if (r.fragments == 0) return r;
    Header h;
    h.flags = keyframe ? wire::kPreviewFlagKeyframe : 0;
    h.fragCount = (uint16_t)r.fragments;
    h.session = session;
    h.frameId = frameId;
    h.frameSize = size;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < r.fragments; i++) {
        uint32_t len = size - offset;
        if (len > wire::kPreviewMaxPayload) len = (uint32_t)wire::kPreviewMaxPayload;
        h.fragIndex = (uint16_t)i;
        h.payloadLen = (uint16_t)len;
        WriteHeader(scratch, h);
        memcpy(scratch + wire::kPreviewHeaderSize, data + offset, len);
        if (!sink((const uint8_t*)scratch, (std::size_t)(wire::kPreviewHeaderSize + len))) break;
        offset += len;
        r.sent++;
    }
    return r;
}

}  // namespace preview
