#include "HmdDriver.h"
#include "DriverLog.h"
#include "CardboardWire.h"
#include "H264Utils.h"
#include <cstdlib>
#include <cstring>

using namespace vr;

// ---------------------------------------------------------------------------
// UDP transport: H264 packet framing + streaming to the bridge.
//
// Encoded packets arrive on OnEncodedPacket() (from the encoder thread via the
// callback) and are sent as 4-byte-big-endian-length-prefixed datagrams in
// <=60 KB chunks so the phone can reconstruct the exact AVPacket. The socket is
// non-blocking so a stalled receiver drops frames instead of blocking SteamVR.
// ---------------------------------------------------------------------------

namespace {

// Chunked non-blocking send. On a full send buffer the frame is dropped and the
// running drop counter incremented (logged every 30th drop). Returns after
// handling the error; the caller keeps its surrounding log + return semantics.
static void SendFramedUdp(SOCKET socket, const sockaddr_in* addr,
                          const uint8_t* framed, int framedSize,
                          uint32_t* droppedCounter)
{
    int offset = 0;
    while (offset < framedSize) {
        int chunkSize = (framedSize - offset > 60000) ? 60000 : (framedSize - offset);
        int res = sendto(socket, (const char*)(framed + offset), chunkSize, 0,
                         (sockaddr*)addr, sizeof(*addr));
        if (res == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                (*droppedCounter)++;
                if (*droppedCounter % 30 == 1) {
                    DriverLog("[UDP] Send buffer full, dropping frame (dropped=%d)", (int)*droppedCounter);
                }
            }
            break; // drop the rest of this frame
        }
        offset += chunkSize;
    }
}

} // namespace

bool HmdDriver::InitializeUDP()
{
    DriverLog("Initializing UDP socket...");

    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        DriverLog("WSAStartup failed! Error: %d", result);
        return false;
    }

    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        DriverLog("socket() failed! WSAError: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // Set socket buffer size
    int bufSize = 1024 * 1024; // 1MB send buffer
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    // CRITICAL: non-blocking sends. sendto() on a blocking socket stalls forever
    // when the receiver stops draining the buffer. That would freeze the encoder
    // thread (holding m_encoderMutex), then Present(), then the entire
    // compositor, and finally trigger vrserver's watchdog abort. With
    // non-blocking sends we drop frames instead of deadlocking.
    u_long nonBlocking = 1;
    ioctlsocket(m_udpSocket, FIONBIO, &nonBlocking);

    // Preview target: localhost:42069 — always present so the bridge / ffplay
    // can watch the stream without a phone connected. Fan-out to the phone's
    // target (m_serverAddr) happens on top of this once discovery sets it.
    m_previewAddr.sin_family = AF_INET;
    m_previewAddr.sin_port = htons(wire::kDataPort);
    inet_pton(AF_INET, "127.0.0.1", &m_previewAddr.sin_addr);

    // Phone target starts empty; SwitchDataTarget fills it in on discovery.
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    m_serverAddr.sin_addr.s_addr = INADDR_ANY;
    m_hasPhoneTarget.store(false, std::memory_order_relaxed);

    m_udpInitialized = true;
    DriverLog("UDP socket initialized. Preview target 127.0.0.1:%d always on; phone target added on discovery", wire::kDataPort);
    return true;
}

void HmdDriver::ShutdownUDP()
{
    DriverLog("Shutting down UDP socket...");

    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }

    m_udpInitialized = false;
    WSACleanup();
    DriverLog("UDP socket shutdown complete.");
}

void HmdDriver::OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe)
{
    if (keyframe && size >= 8) {
        DriverLog("[Encoded] size=%d, pts=%lld, keyframe=YES, first16=%02X %02X %02X %02X %02X %02X %02X %02X",
                  size, pts,
                  data[0], data[1], data[2], data[3],
                  data[4], data[5], data[6], data[7]);
    } else {
        DriverLog("[Encoded] size=%d, pts=%lld, keyframe=%s, data[0]=0x%02X",
                  size, pts, keyframe ? "YES" : "NO", data[0]);
    }

    if (!m_udpInitialized || m_udpSocket == INVALID_SOCKET || size <= 0) {
        return;
    }

    // libx264 keyframes: SPS and PPS have NAL start codes, but IDR data follows
    // without one. Insert IDR start code after PPS for a valid H264 stream.
    static const uint8_t kIdrPrefix[] = { 0x00, 0x00, 0x00, 0x01, 0x65 };
    int ppsEnd = h264::FindIdrInsertionPoint(data, size, keyframe);

    if (ppsEnd > 0) {
        // Build fixed buffer: [up to PPS end] + [IDR start code] + [remaining IDR data].
        int fixedSize = size + 5; // extra 5 bytes for NAL start + IDR header
        uint8_t* fixed = (uint8_t*)malloc(fixedSize);
        memcpy(fixed, data, ppsEnd);
        memcpy(fixed + ppsEnd, kIdrPrefix, 5);
        memcpy(fixed + ppsEnd + 5, data + ppsEnd, size - ppsEnd);

        // Phone: 4-byte length prefix so MediaCodec can reconstruct the AVPacket.
        int framedSize = 0;
        uint8_t* framed = h264::BuildLengthPrefixedPacket(fixed, fixedSize, &framedSize);

        // Preview: raw Annex-B (ffplay expects an unframed H.264 stream).
        SendFannedOut(fixed, fixedSize, framed, framedSize);
        free(framed);
        free(fixed);
        DriverLog("[UDP] Fixed keyframe: inserted IDR start code at offset %d", ppsEnd);
        return;
    }

    // Phone: 4-byte length prefix so the receiver can reconstruct the exact
    // libx264 AVPacket (one full frame, including all of its slices) regardless
    // of UDP datagram boundaries.
    int framedSize = 0;
    uint8_t* framed = h264::BuildLengthPrefixedPacket(data, size, &framedSize);

    // Preview: raw Annex-B — ffplay demuxes start codes directly.
    SendFannedOut(data, size, framed, framedSize);
    free(framed);
    DriverLog("[UDP] Sent framed packet: payload=%d bytes", size);
}

void HmdDriver::SendFannedOut(const uint8_t* raw, int rawSize,
                               const uint8_t* framed, int framedSize)
{
    // Local preview copy — always on so the bridge / ffplay can watch the
    // stream at any time without needing a phone connected. Raw Annex-B so
    // ffplay demuxes start codes directly (no length prefix).
    SendFramedUdp(m_udpSocket, &m_previewAddr, raw, rawSize, &m_udpDroppedFrames);
    m_udpFramesSent.fetch_add(1, std::memory_order_relaxed);

    // Phone copy (only while a real phone target is set). Length-prefixed so
    // MediaCodec can reconstruct the exact AVPacket.
    if (m_hasPhoneTarget.load(std::memory_order_relaxed)) {
        SendFramedUdp(m_udpSocket, &m_serverAddr, framed, framedSize, &m_udpDroppedFrames);
        m_udpFramesSent.fetch_add(1, std::memory_order_relaxed);
    }
}