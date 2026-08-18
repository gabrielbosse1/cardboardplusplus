#include "HmdDriver.h"
#include "DriverLog.h"
#include "CardboardWire.h"
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

// 4-byte big-endian length prefix + the payload, in one malloc'd buffer.
// Returns nullptr on allocation failure (caller must free the result).
static uint8_t* BuildLengthPrefixedPacket(const uint8_t* data, int size, int* outFramedSize)
{
    int framedSize = size + 4;
    uint8_t* framed = (uint8_t*)malloc(framedSize);
    if (framed) {
        framed[0] = (uint8_t)((size >> 24) & 0xFF);
        framed[1] = (uint8_t)((size >> 16) & 0xFF);
        framed[2] = (uint8_t)((size >> 8) & 0xFF);
        framed[3] = (uint8_t)(size & 0xFF);
        memcpy(framed + 4, data, size);
    }
    *outFramedSize = framedSize;
    return framed;
}

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

// libx264 keyframes: SPS and PPS have NAL start codes, but the IDR data follows
// without one. This scans the packet for the PPS NAL (type 8) and returns the
// byte offset where the IDR start code must be inserted, or -1 when no fix is
// needed (IDR start code already present / no PPS found / not a keyframe).
static int FindIdrInsertionPoint(const uint8_t* data, int size, bool keyframe)
{
    if (!keyframe || size <= 10) return -1;

    int ppsDataEnd = -1;
    for (int i = 0; i < size - 5; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 &&
            data[i+3] == 0x01 && (data[i+4] & 0x1F) == 8) {
            // Found PPS start. Find next NAL start code or end of small PPS.
            for (int j = i + 5; j < size - 3; j++) {
                if (data[j] == 0x00 && data[j+1] == 0x00 &&
                    ((data[j+2] == 0x01) || (data[j+2] == 0x00 && data[j+3] == 0x01))) {
                    ppsDataEnd = j;
                    break;
                }
            }
            if (ppsDataEnd < 0) ppsDataEnd = i + 5 + 4; // assume short PPS
            // Check if IDR start code already present. libx264 emits a 3-byte
            // 00 00 01 65 start code after PPS, so accept both 3- and 4-byte forms.
            bool idrScPresent = false;
            if (ppsDataEnd + 3 < size &&
                data[ppsDataEnd] == 0x00 && data[ppsDataEnd+1] == 0x00 &&
                data[ppsDataEnd+2] == 0x01 &&
                (data[ppsDataEnd+3] & 0x1F) == 5) {
                idrScPresent = true; // 3-byte start code: 00 00 01 65
            } else if (ppsDataEnd + 4 < size &&
                data[ppsDataEnd] == 0x00 && data[ppsDataEnd+1] == 0x00 &&
                data[ppsDataEnd+2] == 0x00 && data[ppsDataEnd+3] == 0x01 &&
                (data[ppsDataEnd+4] & 0x1F) == 5) {
                idrScPresent = true; // 4-byte start code: 00 00 00 01 65
            }
            if (idrScPresent) {
                return -1; // IDR start code already present, no fix needed
            }
            return ppsDataEnd;
        }
    }
    return -1; // no PPS NAL found
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

    // Target: localhost:42069
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    inet_pton(AF_INET, "127.0.0.1", &m_serverAddr.sin_addr);

    m_udpInitialized = true;
    DriverLog("UDP socket initialized. Sending to 127.0.0.1:%d", wire::kDataPort);
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
    int ppsEnd = FindIdrInsertionPoint(data, size, keyframe);

    if (ppsEnd > 0) {
        // Build fixed buffer: [up to PPS end] + [IDR start code] + [remaining IDR data].
        int fixedSize = size + 5; // extra 5 bytes for NAL start + IDR header
        uint8_t* fixed = (uint8_t*)malloc(fixedSize);
        memcpy(fixed, data, ppsEnd);
        memcpy(fixed + ppsEnd, kIdrPrefix, 5);
        memcpy(fixed + ppsEnd + 5, data + ppsEnd, size - ppsEnd);

        // Frame the fixed keyframe with a 4-byte big-endian length prefix so
        // the receiver can reconstruct the exact AVPacket (one full frame).
        int framedSize = 0;
        uint8_t* framed = BuildLengthPrefixedPacket(fixed, fixedSize, &framedSize);
        if (framed) {
            SendFramedUdp(m_udpSocket, &m_serverAddr, framed, framedSize, &m_udpDroppedFrames);
            free(framed);
        }
        free(fixed);
        DriverLog("[UDP] Fixed keyframe: inserted IDR start code at offset %d", ppsEnd);
        return;
    }

    // Frame the packet with a 4-byte big-endian length prefix so the receiver
    // can reconstruct the exact libx264 AVPacket (one full frame, including all
    // of its slices) regardless of UDP datagram boundaries. FFmpeg decodes the
    // in-band SPS/PPS/IDR Annex B stream directly.
    int framedSize = 0;
    uint8_t* framed = BuildLengthPrefixedPacket(data, size, &framedSize);
    if (framed) {
        SendFramedUdp(m_udpSocket, &m_serverAddr, framed, framedSize, &m_udpDroppedFrames);
        free(framed);
    }
    DriverLog("[UDP] Sent framed packet: payload=%d bytes", size);
}