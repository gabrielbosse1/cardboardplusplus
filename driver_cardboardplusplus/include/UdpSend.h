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

// Classifies a sendto error after the would-block family was already handled:
// false = transient (abandon this frame, count a drop, keep the sender alive),
// true = fatal (log and break; the frame is abandoned as well, but the error
// signals real breakage like an oversized datagram or a dead socket).
//
// Windows reports ICMP port-unreachable from a vanished UDP target as
// WSAECONNRESET on the NEXT sendto. The loopback preview target
// (127.0.0.1:42069) legitimately disappears whenever the bridge's ffmpeg pump
// restarts or the bridge exits, so classifying that as fatal abandoned the
// frame mid-send for BOTH targets (they share one socket: the phone send runs
// after the preview send), and the resulting allSent=false streak kept arming
// forced IDRs into the same dead buffer — the long black/starved window seen
// on Windows when the preview "gags". Reset-class errors are drops now; the
// phone send on the same socket still runs its own loop. Linux keeps its
// baseline: an unconnected socket never reports conn-reset, so every error
// reaching this function is fatal there.
// constexpr so the compile-time pins below can evaluate it (MSVC rejects a
// non-constexpr call in static_assert with C2131; found by the Windows CI leg).
static constexpr bool IsFatalSendError(int err)
{
#ifdef _WIN32
    switch (err) {
        case WSAECONNRESET:
        case WSAETIMEDOUT:
        case WSAENETRESET:
        case WSAECONNABORTED:
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
        case WSAEADDRNOTAVAIL:
        case WSAEINTR:
            return false;
        default:
            return true;
    }
#else
    // Linux baseline: the would-block family is handled by PlatformWouldBlock
    // before this function runs; keeping them false here is defensive only (an
    // EINTR stays fatal, exactly as the Linux baseline has always behaved).
    return err != EAGAIN && err != EWOULDBLOCK;
#endif
}

#ifdef _WIN32
// Compile-time pins (proven by the Windows build workflow): a vanished
// preview/phone target must classify as a drop, never as a hard error.
static_assert(!IsFatalSendError(WSAECONNRESET),
              "WSAECONNRESET (vanished UDP target) must be a drop, not a hard error");
static_assert(!IsFatalSendError(WSAEHOSTUNREACH),
              "WSAEHOSTUNREACH (phone left the network) must be a drop, not a hard error");
static_assert(IsFatalSendError(WSAEMSGSIZE),
              "WSAEMSGSIZE is a real configuration bug and must stop the send loop");
#endif

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
            }
            if (!IsFatalSendError(err)) {
                // Transient reset/unreach class (see IsFatalSendError): same
                // handling as a would-block drop so the phone target's send
                // still runs and the resync policy sees a normal failure.
                (*droppedCounter)++;
                if (*droppedCounter % 30 == 1) {
                    DriverLog("[UDP] transient send error %d, dropping frame (dropped=%d)",
                              err, (int)*droppedCounter);
                }
                return false;
            }
            char targetIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, targetIp, sizeof(targetIp));
            DriverLog("[UDP] sendto error %d to %s:%d (size=%d)", err,
                      targetIp, ntohs(addr->sin_port), chunkSize);
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
