#pragma once
#include "Platform.h"
#include "DriverLog.h"
#include <cstdint>
#include <chrono>
#include <thread>
// sockaddr_in/inet_ntop come from Platform.h's platform includes above.

// UDP frame fan-out helpers shared by the driver and the transport unit tests.
// The stream semantics that consumers rely on: a "frame" (one encoded access
// unit) is chunked into kUdpChunkBytes datagrams; Annex-B consumers (bridge
// preview ffmpeg, phone decoder) resync at the next start code when datagrams
// are lost, so losing a frame TAIL shows as bottom-of-frame concealment
// stripes. Senders must therefore know whether a frame was delivered whole.
namespace udp {

static constexpr int kUdpChunkBytes = 1400;
// Gentle pacing: yield the CPU briefly every few datagrams so a bursting
// sender does not outrun the receiver's kernel buffer between pump cycles.
// An IDR frame is ~300 datagrams, so pacing adds ~6-8 ms on the encode
// thread, inside the frame budget, and it caps in-flight bytes below the
// Linux default rmem clamp (~208 KB) even when a receiver never enlarged
// its buffer. Windows is excluded: Sleep() granularity there is unreliable
// (timer coalescing can stretch a 250 us sleep to ~15 ms), and the Windows
// receivers instead get a 4 MB SO_RCVBUF, which Windows does not clamp.
static constexpr int kPaceBurstDatagrams = 12;
static constexpr int kPaceSleepUs = 250;

// Sends `framedSize` bytes as fixed-size datagrams (last chunk may be short).
// Returns true only when every chunk was accepted by the OS. A false return
// means the receiver lost the frame's tail mid-stream and will decode it with
// error concealment; the caller tracks consecutive failures to arm an IDR
// resync (see UdpArmResync).
static inline bool SendFramedUdp(cbpp::PlatformSocket socket, const sockaddr_in* addr,
                                 const uint8_t* framed, int framedSize,
                                 uint32_t* droppedCounter)
{
    int offset = 0;
    int burst = 0;
    while (offset < framedSize) {
        int chunkSize = (framedSize - offset > kUdpChunkBytes) ? kUdpChunkBytes : (framedSize - offset);
        long res = (long)sendto(socket, (const char*)(framed + offset), chunkSize, 0,
                                (sockaddr*)addr, sizeof(*addr));
        if (res < 0) {
            int err = cbpp::PlatformSocketError();
            if (cbpp::PlatformWouldBlock(err)) {
                (*droppedCounter)++;
                if (*droppedCounter % 30 == 1) {
                    DriverLog("[UDP] Send buffer full, dropping frame (dropped=%d)", (int)*droppedCounter);
                }
                return false;
            } else {
                char targetIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr->sin_addr, targetIp, sizeof(targetIp));
                DriverLog("[UDP] sendto error %d to %s:%d (size=%d)", err,
                          targetIp, ntohs(addr->sin_port), chunkSize);
            }
            break;
        }
        offset += chunkSize;
        // Pace only long frames: a P-frame (a few datagrams) must not stall.
        // Windows skips pacing (Sleep granularity); its receivers rely on the
        // enlarged SO_RCVBUF set by the bridge instead.
#ifndef _WIN32
        if (++burst >= kPaceBurstDatagrams && offset < framedSize) {
            burst = 0;
            std::this_thread::sleep_for(std::chrono::microseconds(kPaceSleepUs));
        }
#endif
    }
    return offset >= framedSize;
}

// Resync policy for abandoned frames: from the `threshold`-th consecutive
// failure on, every further failure re-arms the forced IDR (the flag is
// consumed by the next encode, which may itself be dropped, so continuous
// re-arming is required while capacity stays insufficient). Any fully
// delivered frame resets the streak. Returns true when the caller should
// call RequestKeyframe.
static inline bool UdpArmResync(bool allSent, unsigned& consecutiveDrops,
                                unsigned failedCounter, unsigned threshold = 2)
{
    if (allSent) {
        consecutiveDrops = 0;
        return false;
    }
    consecutiveDrops++;
    if (consecutiveDrops == threshold) {
        DriverLog("[UDP] %u consecutive dropped frames -> forcing IDR resync (total=%u)",
                  consecutiveDrops, failedCounter);
    }
    return consecutiveDrops >= threshold;
}

}  // namespace udp
