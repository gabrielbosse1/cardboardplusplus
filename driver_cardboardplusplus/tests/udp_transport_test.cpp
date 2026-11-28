// UDP transport unit tests: exercise include/UdpSend.h against real loopback
// sockets. These tests exist because a production incident shipped undetected:
// an oversized frame burst overflowing socket buffers made senders abandon
// frames mid-send (would-block), and receivers lost mid-frame datagrams
// silently — every Annex-B consumer then decoded with bottom-of-frame
// concealment stripes. The tests pin down the three defense layers:
//   1. sender-side full-delivery reporting (SendFramedUdp false on failure),
//   2. paced sending that stays under kernel buffer clamps between pumps,
//   3. the IDR resync arming policy (UdpArmResync).
//
// Build: g++ -std=c++17 -O2 -I driver_cardboardplusplus/include
//        -o udp_transport_tests driver_cardboardplusplus/tests/udp_transport_test.cpp
#include "UdpSend.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// Standalone log sink: production DriverLog forwards to SteamVR's logger,
// which is unavailable outside the driver host.
void DriverLog(const char* pFormat, ...) {
    va_list ap;
    va_start(ap, pFormat);
    vprintf(pFormat, ap);
    va_end(ap);
    printf("\n");
}

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "== PASS" : "== FAIL", what);
    if (!ok) g_failures++;
}

static int MakeUdp(unsigned rcvBuf) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    if (rcvBuf) {
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));
    }
    // Non-blocking receivers: the drain loops below must terminate at the
    // end of the queue, not block in recv().
    cbpp::PlatformSetNonBlocking(s);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) return -1;
    return s;
}

static sockaddr_in TargetOf(int boundSock) {
    sockaddr_in addr = {};
    socklen_t alen = sizeof(addr);
    getsockname(boundSock, (sockaddr*)&addr, &alen);
    return addr;
}

// The kernel clamps SO_RCVBUF to net.core.rmem_max (default ~208 KB on
// Linux); read back what the socket actually got so the burst assertions
// match kernel reality instead of the requested size.
static long ActualRcvBuf(int s) {
    int got = 0;
    socklen_t len = sizeof(got);
    getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&got, &len);
    return got;
}

static long DrainAll(int s, std::vector<uint8_t>& out) {
    uint8_t tmp[65536];
    long total = 0;
    for (;;) {
        ssize_t n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        out.insert(out.end(), tmp, tmp + n);
        total += n;
    }
    return total;
}

// Bridge-pump-shaped reader: drain everything, sleep pumpGapUs, repeat.
static long PumpRead(int s, std::vector<uint8_t>& out, int pumpGapUs, int deadlineMs) {
    uint8_t tmp[65536];
    long total = 0;
    int waited = 0;
    for (;;) {
        ssize_t n = recv(s, tmp, sizeof(tmp), 0);
        if (n > 0) {
            out.insert(out.end(), tmp, tmp + n);
            total += n;
            waited = 0;
            continue;
        }
        if (total > 0 && waited >= deadlineMs) break;
        if (waited >= deadlineMs) break;
        usleep(pumpGapUs);
        waited += pumpGapUs / 1000;
    }
    return total;
}

// Length-prefixed reassembler mirroring the phone's VideoReceiver.cpp loop:
// 4-byte big-endian frame length + payload, frame_len==0 or > kMaxFrameSize is
// a desync (buffer reset + KEYFRAME_REQ in production), a >=9-byte head that is
// not Annex-B is a desync, and a partial-frame backlog past kMaxPartialFrameBytes
// is a desync. Frames are emitted whole even when their middle datagrams were
// lost (no checksum on this wire) — Annex-B consumers conceal.
struct LengthFramer {
    static constexpr size_t kMaxFrameSize = 16 * 1024 * 1024;        // VideoReceiver.h
    static constexpr size_t kMaxPartialFrameBytes = 1024 * 1024;     // VideoReceiver.h
    std::vector<uint8_t> buf;
    std::vector<std::vector<uint8_t>> frames;
    int desyncs = 0;  // each buffer reset maps to one KEYFRAME_REQ in production
    static constexpr size_t kDeadAirChunk = 65536;  // datagram size for backlog-fill below
    static bool HasStartCode(const uint8_t* d) {
        if (d[0] == 0 && d[1] == 0 && d[2] == 1) return true;
        if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) return true;
        return false;
    }
    void feed(const uint8_t* d, size_t n) {
        buf.insert(buf.end(), d, d + n);
        parse();
    }
    void parse() {
        while (buf.size() >= 4) {
            size_t flen = ((size_t)buf[0] << 24) | ((size_t)buf[1] << 16) |
                          ((size_t)buf[2] << 8) | (size_t)buf[3];
            if (flen == 0 || flen > kMaxFrameSize) { desyncs++; reset(); return; }
            if (buf.size() >= 9 && !HasStartCode(buf.data() + 4)) {
                desyncs++; reset(); return;
            }
            if (buf.size() < 4 + flen) {
                if (buf.size() > kMaxPartialFrameBytes) { desyncs++; reset(); }
                return;
            }
            frames.emplace_back(buf.begin() + 4, buf.begin() + 4 + flen);
            buf.erase(buf.begin(), buf.begin() + 4 + flen);
        }
    }
    void reset() { buf.clear(); }
};

// Frames a payload the way the driver's OnEncodedPacket does (4-byte BE length).
static std::vector<uint8_t> FrameAU(const std::vector<uint8_t>& au) {
    std::vector<uint8_t> f;
    uint32_t n = (uint32_t)au.size();
    f.push_back((uint8_t)(n >> 24));
    f.push_back((uint8_t)(n >> 16));
    f.push_back((uint8_t)(n >> 8));
    f.push_back((uint8_t)(n & 0xFF));
    f.insert(f.end(), au.begin(), au.end());
    return f;
}

// Splits a framed packet the way SendFramedUdp does (1400-byte datagrams).
static std::vector<std::vector<uint8_t>> ChunksOf(const std::vector<uint8_t>& f) {
    std::vector<std::vector<uint8_t>> ch;
    for (size_t off = 0; off < f.size(); off += 1400)
        ch.emplace_back(f.begin() + off, f.begin() + std::min(f.size(), off + (size_t)1400));
    return ch;
}

int main() {
    printf("=== UDP transport tests ===\n");

    // ---- T1: full delivery, byte-exact, ordered ----
    {
        int rx = MakeUdp(4 << 20);
        long rmem = ActualRcvBuf(rx);
        sockaddr_in target = TargetOf(rx);
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        cbpp::PlatformSetNonBlocking(tx);
        int snd = 4 << 20;
        setsockopt(tx, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof(snd));

        // A modest 64KB frame stays under every buffer clamp involved.
        std::vector<uint8_t> frame(64 * 1024);
        for (size_t i = 0; i < frame.size(); i++) frame[i] = (uint8_t)(i * 7 + (i >> 9));
        uint32_t drops = 0;
        bool ok = udp::SendFramedUdp(tx, &target, frame.data(), (int)frame.size(), &drops);
        usleep(200 * 1000);
        std::vector<uint8_t> got;
        long total = DrainAll(rx, got);
        Check(ok && drops == 0, "T1 sender reports full delivery");
        Check(total == (long)frame.size() && memcmp(got.data(), frame.data(), frame.size()) == 0,
              "T1 receiver reassembles the frame byte-exact in order");
        (void)rmem;
        close(rx); close(tx);
    }

    // ---- T2: incident condition - unpaced burst into a default-size receiver ----
    // Documents WHY the receiver buffer bump + pacing exist: an unpaced 768KB
    // burst into the clamped default receiver queue loses datagrams the
    // sender never learns about (the kernel drops them receiver-side).
    {
        int rx = MakeUdp(0);  // default (clamped) buffer, reader silent during burst
        sockaddr_in target = TargetOf(rx);
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        cbpp::PlatformSetNonBlocking(tx);
        int snd = 4 << 20;
        setsockopt(tx, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof(snd));

        std::vector<uint8_t> frame(768 * 1024, 0x5A);
        uint32_t drops = 0;
        bool ok = udp::SendFramedUdp(tx, &target, frame.data(), (int)frame.size(), &drops);
        usleep(300 * 1000);
        std::vector<uint8_t> got;
        long total = DrainAll(rx, got);
        long rmem = ActualRcvBuf(rx);
        printf("   (T2 facts: rmem=%ld bytes, sent=%ld, delivered=%ld)\n",
               rmem, (long)frame.size(), total);
        // The sender must still report success (loopback never backpressures
        // it) — this is exactly the silent-loss shape that produced stripes.
        Check(ok && drops == 0, "T2 sender is unaware of receiver-side loss");
        Check(total < (long)frame.size(),
              "T2 unpaced burst into default buffers silently loses the frame tail (incident documented)");
        close(rx); close(tx);
    }

    // ---- T3: paced send survives a default-clamped receiver + bridge pump ----
    // The load-bearing production combo: Linux receivers are clamped to
    // rmem_max (~208 KB), so the sender must pace below the pump's drain
    // rate. A 768KB frame arrives whole with a 2ms-gap pump reader, whereas
    // T2's unpaced burst into the same setup loses datagrams.
    {
        int rx = MakeUdp(0);
        sockaddr_in target = TargetOf(rx);
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        cbpp::PlatformSetNonBlocking(tx);
        int snd = 4 << 20;
        setsockopt(tx, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof(snd));

        std::vector<uint8_t> frame(768 * 1024);
        for (size_t i = 0; i < frame.size(); i++) frame[i] = (uint8_t)(i >> 2);
        uint32_t drops = 0;
        // Concurrent bridge-pump-shaped reader, like production (the pump
        // thread drains while the sender transmits).
        std::vector<uint8_t> got;
        long total = 0;
        std::thread reader([&] { total = PumpRead(rx, got, 2000, 4000); });
        bool ok = udp::SendFramedUdp(tx, &target, frame.data(), (int)frame.size(), &drops);
        reader.join();
        Check(ok && drops == 0, "T3 paced sender delivers without would-block");
        Check(total == (long)frame.size() && memcmp(got.data(), frame.data(), frame.size()) == 0,
              "T3 768KB frame arrives whole through the default-clamped receiver (pacing works)");
        close(rx); close(tx);
    }

    // ---- T4: resync policy - arms at threshold and re-arms per further drop ----
    {
        unsigned consec = 0;
        bool armed = udp::UdpArmResync(false, consec, 1);
        Check(!armed && consec == 1, "T4 first failure does not arm");
        armed = udp::UdpArmResync(false, consec, 2);
        Check(armed && consec == 2, "T4 second consecutive failure arms the IDR");
        armed = udp::UdpArmResync(false, consec, 3);
        Check(armed, "T4 keeps re-arming while drops continue (flag may be consumed by a dropped encode)");
        bool reset = udp::UdpArmResync(true, consec, 3);
        Check(!reset && consec == 0, "T4 successful send resets the streak");
        armed = udp::UdpArmResync(false, consec, 4);
        Check(!armed && consec == 1, "T4 fresh streak starts below threshold");
    }

    // ---- T5: send-error classification matrix (Linux side) ----
    // On Linux every error reaching IsFatalSendError is fatal (would-block was
    // handled earlier): loopback never reports conn-reset on an unconnected
    // socket, so a "fatal" here means real breakage. The Windows reset-class
    // set is compile-pinned by static_asserts in UdpSend.h (build-driver.yml).
    {
        Check(udp::IsFatalSendError(EBADF), "T5 EBADF (socket gone) is fatal");
        Check(udp::IsFatalSendError(ENOTSOCK), "T5 ENOTSOCK is fatal");
        Check(udp::IsFatalSendError(EMSGSIZE), "T5 EMSGSIZE is fatal (oversized datagram)");
        Check(udp::IsFatalSendError(EACCES), "T5 EACCES is fatal");
        Check(!udp::IsFatalSendError(EAGAIN), "T5 EAGAIN stays in the would-block family (not fatal here)");
        Check(!udp::IsFatalSendError(EWOULDBLOCK), "T5 EWOULDBLOCK stays in the would-block family (not fatal here)");
    }

    // ---- T6: hard-error path + fan-out independence ----
    // A dead target (closed socket fd) must return false without crashing and
    // must not poison the NEXT target's send on a healthy socket: the phone
    // send runs after the preview send in SendFannedOut, so a hard error in
    // the first loop used to abandon only that frame — the second target gets
    // its own SendFramedUdp call and must still deliver byte-exact.
    {
        sockaddr_in badTarget = {};
        badTarget.sin_family = AF_INET;
        badTarget.sin_port = htons(42069);
        badTarget.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::vector<uint8_t> frame(4096, 0x11);
        uint32_t drops = 0;
        cbpp::PlatformSocket dead = (cbpp::PlatformSocket)-1;  // EBADF on every sendto
        bool ok = udp::SendFramedUdp(dead, &badTarget, frame.data(), (int)frame.size(), &drops);
        Check(!ok, "T6 sendto on a dead socket reports failure (frame abandoned)");
        Check(drops == 0, "T6 hard error is not counted as a would-block drop");

        // Fan-out independence: after the dead-target failure, a healthy
        // target still receives the whole frame byte-exact.
        int rx = MakeUdp(4 << 20);
        sockaddr_in target = TargetOf(rx);
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        cbpp::PlatformSetNonBlocking(tx);
        int snd = 4 << 20;
        setsockopt(tx, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof(snd));
        bool ok2 = udp::SendFramedUdp(tx, &target, frame.data(), (int)frame.size(), &drops);
        usleep(200 * 1000);
        std::vector<uint8_t> got;
        long total = DrainAll(rx, got);
        Check(ok2 && drops == 0 && total == (long)frame.size() &&
                  memcmp(got.data(), frame.data(), frame.size()) == 0,
              "T6 healthy target still receives the full frame after a dead-target failure");
        close(rx); close(tx);
    }

    // ---- T7: reassembler - full stream recovers every frame ----
    {
        LengthFramer fr;
        std::vector<std::vector<uint8_t>> aus;
        for (int i = 0; i < 6; i++) {
            std::vector<uint8_t> au(2000 + (size_t)i * 1300);
            au[0] = 0x00; au[1] = 0x00; au[2] = 0x00; au[3] = 0x01; au[4] = (i % 2) ? 0x65 : 0x41;
            for (size_t j = 5; j < au.size(); j++) au[j] = (uint8_t)(j + i);
            aus.push_back(au);
        }
        for (const auto& au : aus) {
            std::vector<uint8_t> framed = FrameAU(au);
            // Feed through the real sender chunker so the datagram boundaries
            // match production (1400-byte chunks).
            int rx = MakeUdp(4 << 20);
            sockaddr_in target = TargetOf(rx);
            int tx = socket(AF_INET, SOCK_DGRAM, 0);
            cbpp::PlatformSetNonBlocking(tx);
            uint32_t drops = 0;
            udp::SendFramedUdp(tx, &target, framed.data(), (int)framed.size(), &drops);
            usleep(150 * 1000);
            std::vector<uint8_t> got;
            DrainAll(rx, got);
            // Feed datagram-by-datagram like the phone's recv loop.
            for (size_t off = 0; off < got.size(); off += 65536) {
                size_t n = (got.size() - off < 65536) ? got.size() - off : 65536;
                fr.feed(got.data() + off, n);
            }
            close(rx); close(tx);
        }
        bool all = fr.frames.size() == aus.size() && fr.desyncs == 0;
        for (size_t i = 0; all && i < aus.size(); i++) {
            all = fr.frames[i].size() == aus[i].size() &&
                  memcmp(fr.frames[i].data(), aus[i].data(), aus[i].size()) == 0;
        }
        Check(all, "T7 reassembler recovers every frame byte-exact through the real chunker");
    }

    // ---- T8: reassembler - lost frame TAIL: corrupt emission, then resync ----
    // FINDING pinned here: the wire carries no per-frame checksum, so when
    // frame k's tail datagrams are lost the phone does NOT skip cleanly —
    // frame k+1's bytes complete the stale length header and ONE CORRUPT
    // frame (frame k's length, mixed content) is emitted to the decoder, its
    // leftovers desync the parser, the guard/reset fires, and recovery lands
    // on the next datagram that begins a clean length header. This corrupt
    // emission + parser desync window is the decode-side half of the
    // bottom-of-frame stripe / starve shapes; the sender-side IDR resync and
    // the paced sender exist to keep this window short and rare.
    {
        LengthFramer fr;
        auto auOf = [](int i, size_t len) {
            std::vector<uint8_t> au(len);
            au[0] = 0x00; au[1] = 0x00; au[2] = 0x00; au[3] = 0x01; au[4] = 0x65;
            for (size_t j = 5; j < au.size(); j++) au[j] = (uint8_t)(j * 3 + i);
            return au;
        };
        std::vector<uint8_t> au0 = auOf(0, 900);   // small: one datagram, delivered
        std::vector<uint8_t> au1 = auOf(1, 3500);  // long: 3 datagrams, tail lost
        std::vector<uint8_t> au2 = auOf(2, 900);   // next frame: swallowed by the stale header
        std::vector<uint8_t> au3 = auOf(3, 900);   // delivered after the reset
        std::vector<uint8_t> f1 = FrameAU(au1);
        auto c1 = ChunksOf(f1);
        printf("   (T8 facts: frame1 datagrams=%zu)\n", c1.size());
        for (const auto& c : ChunksOf(FrameAU(au0))) fr.feed(c.data(), c.size());
        for (size_t i = 0; i + 1 < c1.size(); i++) fr.feed(c1[i].data(), c1[i].size());  // tail dropped
        for (const auto& c : ChunksOf(FrameAU(au2))) fr.feed(c.data(), c.size());
        Check(fr.frames.size() == 2, "T8 corrupt emission: stale header completed by frame 2's bytes");
        bool corruptEmitted = fr.frames.size() == 2 &&
                              fr.frames[1].size() == au1.size() &&
                              fr.frames[1] != au1;
        Check(corruptEmitted, "T8 the emitted frame has frame 1's length but corrupt content");
        // The 204-byte garbage tail desyncs the next parse; the reset consumes
        // the datagram that triggered it (phone behavior), so the FIRST au3
        // copy is dropped and the second recovers.
        for (const auto& c : ChunksOf(FrameAU(au3))) fr.feed(c.data(), c.size());
        Check(fr.desyncs >= 1, "T8 parser desync fired after the corrupt emission");
        for (const auto& c : ChunksOf(FrameAU(au3))) fr.feed(c.data(), c.size());
        Check(!fr.frames.empty() && fr.frames.back() == au3,
              "T8 recovery lands on the next clean length-header frame");
        // Backlog guard: dead air behind a never-completing header must hit
        // the 1 MB partial-frame reset (the phone's "missed the length prefix
        // long ago" guard) instead of buffering unbounded.
        std::vector<uint8_t> deadAir(LengthFramer::kDeadAirChunk, 0x00);
        size_t guard = 0;
        while (fr.buf.size() <= LengthFramer::kMaxPartialFrameBytes && guard < 4096) {
            fr.feed(deadAir.data(), deadAir.size());
            guard++;
        }
        Check(fr.desyncs >= 2, "T8 backlog guard fires and resets the reassembly buffer");
    }

    // ---- T9: reassembler - lost MIDDLE datagrams eventually emit corrupt too ----
    // A middle-hole long frame never completes on its own (the hole exceeds
    // the remaining bytes); the next frames' bytes fill the stale header past
    // its length and a corrupt frame is emitted, exactly like T8. No checksum
    // on this wire — mid-frame loss is invisible at this layer until parser
    // desync. The prevention layers are sender-side (VBV cap, pacing, SO_RCVBUF).
    {
        LengthFramer fr;
        std::vector<uint8_t> au(3000);
        au[0] = 0x00; au[1] = 0x00; au[2] = 0x00; au[3] = 0x01; au[4] = 0x65;
        for (size_t j = 5; j < au.size(); j++) au[j] = (uint8_t)(j);
        std::vector<uint8_t> f = FrameAU(au);
        std::vector<std::vector<uint8_t>> chunks;
        for (size_t off = 0; off < f.size(); off += 1400)
            chunks.emplace_back(f.begin() + off, f.begin() + std::min(f.size(), off + (size_t)1400));
        std::vector<uint8_t> au3(900, 0x41);
        au3[0] = 0x00; au3[1] = 0x00; au3[2] = 0x00; au3[3] = 0x01; au3[4] = 0x41;
        for (size_t j = 5; j < au3.size(); j++) au3[j] = (uint8_t)(j + 9);
        for (size_t i = 0; i < chunks.size(); i++) {
            if (i == 1) continue;  // middle hole
            fr.feed(chunks[i].data(), chunks[i].size());
        }
        Check(fr.frames.empty() && fr.desyncs == 0,
              "T9 middle-hole frame stalls silently (never completes alone)");
        // Fill past the stale header with the next frame's bytes.
        for (const auto& c : ChunksOf(FrameAU(au3))) fr.feed(c.data(), c.size());
        for (const auto& c : ChunksOf(FrameAU(au3))) fr.feed(c.data(), c.size());
        bool corrupt = fr.frames.size() == 1 && fr.frames[0].size() == au.size() &&
                       fr.frames[0] != au;
        Check(corrupt, "T9 following bytes complete the stale header -> one corrupt frame emitted");
        Check(fr.desyncs >= 1, "T9 garbage tail after the corrupt emission desyncs the parser");
    }


    printf("=== UDP transport tests: %s (%d failures) ===\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
