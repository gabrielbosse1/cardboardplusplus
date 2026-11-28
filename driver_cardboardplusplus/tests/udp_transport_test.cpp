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

    printf("=== UDP transport tests: %s (%d failures) ===\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
