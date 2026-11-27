#!/usr/bin/env python3
"""Bridge preview loss harness: driver -> 127.0.0.1 UDP -> bridge -> ffmpeg.

Reproduces the preview corruption and checks the fix without SteamVR. A real
H.264 stream (libx264, baseline, zerolatency, GOP like the driver) is paced
over loopback UDP and decoded with the bridge's ffmpeg command line. Every
decoded frame is compared with a clean decode of the same bitstream; a frame
that matches no clean frame exactly is a visible artifact.

  --mode legacy  the pre-fix path: raw Annex-B in 1400 B chunks, bridge drain
                 loop that writes into ffmpeg and sleeps 2 ms (core.rs before
                 the fix). Kernel drops on keyframe bursts reach the decoder.
  --mode framed  the fixed path: CBPV header per fragment (CardboardWire.h),
                 receive thread decoupled from the ffmpeg writer, bounded
                 queue, whole-frame reassembly and drop-until-keyframe gate
                 (a Python port of bridge/.../net/preview.rs).

Pass --loss P to drop a fraction of datagrams on top of the kernel's drops,
and --reconnect N to restart the sender session every N frames (frame ids back
to 0, new session id, stream resumes mid-GOP like a driver restart).

Needs ffmpeg: `pip install imageio-ffmpeg` (or ffmpeg on PATH). Linux
reads kernel drop counters from /proc/net/udp; elsewhere they show as 0.
Exit code 0 when no corrupt frame was decoded.
"""
import argparse, os, queue, shutil, socket, struct, subprocess, sys, threading, time, random

W, H = 480, 270
FB = W * H * 4

def ff():
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        exe = shutil.which("ffmpeg")
        if not exe:
            sys.exit("ffmpeg not found: pip install imageio-ffmpeg, or put ffmpeg on PATH")
        return exe

def encode(w, h, fps, secs, mbps, gop):
    cmd = [ff(), "-v", "error", "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={fps}",
           "-t", str(secs), "-vf", "noise=alls=12:allf=t", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
           "-profile:v", "baseline", "-g", str(gop), "-bf", "0", "-b:v", f"{mbps}M",
           "-maxrate", f"{mbps}M", "-bufsize", f"{mbps//2}M", "-pix_fmt", "yuv420p",
           "-f", "h264", "pipe:1"]
    return subprocess.run(cmd, check=True, capture_output=True).stdout

def nals(data):
    i, n, starts = 0, len(data), []
    while i + 3 < n:
        if data[i] == 0 and data[i+1] == 0 and data[i+2] == 1:
            starts.append(i + 3); i += 3
        else:
            i += 1
    out = []
    for k, s in enumerate(starts):
        e = starts[k+1] - 3 if k + 1 < len(starts) else n
        while e > s and data[e-1] == 0 and k + 1 < len(starts):
            e -= 1
        out.append((s, e))
    return out

def access_units(data):
    """Split Annex-B into AUs: new AU at AUD/SPS, or a slice with first_mb == 0
    that is not already preceded by SPS/PPS in the current AU."""
    aus, cur_start, cur_has_slice, cur_key = [], None, False, False
    marks = []
    for s, e in nals(data):
        t = data[s] & 0x1F
        sc = s - 4 if s >= 4 and data[s-4] == 0 else s - 3
        new = False
        if t in (7, 9):
            new = cur_has_slice or cur_start is None
        elif t in (1, 5):
            first_mb_zero = (data[s+1] & 0x80) != 0
            if first_mb_zero and cur_has_slice:
                new = True
            if cur_start is None:
                new = True
        if new:
            if cur_start is not None:
                marks.append((cur_start, sc, cur_key))
            cur_start, cur_has_slice, cur_key = sc, False, False
        if t in (1, 5):
            cur_has_slice = True
        if t in (5, 7):
            cur_key = True
    if cur_start is not None:
        marks.append((cur_start, len(data), cur_key))
    return [(data[a:b], k) for a, b, k in marks]

def decoder_cmd(framed=False):
    # core.rs start_preview_decode. Legacy: the old line with -v error so the
    # harness sees decoder errors. Framed: the current line.
    low = ["-flags", "low_delay"] if framed else []
    return [ff()] + low + ["-f", "h264", "-probesize", "32768", "-analyzeduration", "0", "-i", "pipe:0",
            "-vf", f"scale={W}:{H}", "-f", "rawvideo", "-pix_fmt", "rgba", "-v", "error", "pipe:1"]

def decode_all(stream):
    p = subprocess.run(decoder_cmd(), input=stream, capture_output=True)
    raw = p.stdout
    return [raw[i:i+FB] for i in range(0, len(raw) - FB + 1, FB)]

def udp_drops(port):
    hexport = f"{port:04X}"
    try:
        with open("/proc/net/udp") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if parts[1].endswith(":" + hexport):
                    return int(parts[-1])
    except OSError:
        pass
    return 0

# --- CBPV framing (CardboardWire.h / preview.rs) -------------------------------
MAGIC, VERSION, HDR, MAX_DGRAM = 0x43425056, 1, 24, 1400
MAX_PAYLOAD = MAX_DGRAM - HDR
FLAG_KEY, FLAG_END = 0x01, 0x02
HDR_FMT = ">IBBHHHIII"
GOLDEN = bytes.fromhex("434250560101000100030560010203040A0B0C0D00000BB8")
assert struct.pack(HDR_FMT, MAGIC, VERSION, FLAG_KEY, 1, 3, 1376, 0x01020304, 0x0A0B0C0D, 3000) == GOLDEN

def packetize(au, session, fid, key):
    n = (len(au) + MAX_PAYLOAD - 1) // MAX_PAYLOAD
    for i in range(n):
        part = au[i * MAX_PAYLOAD:(i + 1) * MAX_PAYLOAD]
        yield struct.pack(HDR_FMT, MAGIC, VERSION, FLAG_KEY if key else 0, i, n, len(part),
                          session, fid, len(au)) + part

def has_sps(au):
    return any((au[s] & 0x1F) == 7 for s, _ in nals(au))

class Reassembler:
    """Port of preview.rs Reassembler: emits only complete frames."""
    MAX_PARTIALS = 4
    def __init__(self):
        self.session, self.partials, self.newest = None, {}, None
        self.incomplete = self.rejected = 0
    def push(self, d):
        if len(d) < HDR:
            self.rejected += 1; return None
        magic, ver, flags, idx, cnt, plen, sess, fid, size = struct.unpack(HDR_FMT, d[:HDR])
        if magic != MAGIC or ver != VERSION or plen != len(d) - HDR or cnt == 0 or idx >= cnt:
            self.rejected += 1; return None
        off = idx * MAX_PAYLOAD
        if size == 0 or (size + MAX_PAYLOAD - 1) // MAX_PAYLOAD != cnt or plen != min(MAX_PAYLOAD, size - off):
            self.rejected += 1; return None
        if sess != self.session:
            self.incomplete += len(self.partials)
            self.session, self.partials, self.newest = sess, {}, None
        if self.newest is not None and fid <= self.newest:
            self.rejected += 1; return None
        p = self.partials.setdefault(fid, [cnt, size, bool(flags & FLAG_KEY), set(), bytearray(size)])
        if idx not in p[3]:
            p[3].add(idx); p[4][off:off + plen] = d[HDR:]
        if len(p[3]) == p[0]:
            del self.partials[fid]
            for old in [k for k in self.partials if k < fid]:
                del self.partials[old]; self.incomplete += 1
            self.newest = fid
            return (sess, fid, p[2], bytes(p[4]))
        while len(self.partials) > self.MAX_PARTIALS:
            del self.partials[min(self.partials)]; self.incomplete += 1
        return None

class Gate:
    """Port of preview.rs DecodeGate: contiguous ids from a keyframe with SPS."""
    def __init__(self):
        self.need_key, self.session, self.next = True, None, 0
        self.gaps = self.held = 0
    def offer(self, sess, fid, key, data):
        if sess != self.session:
            self.session, self.need_key = sess, True
        elif not self.need_key and fid != self.next:
            self.gaps += 1; self.need_key = True
        if self.need_key:
            if key and has_sps(data):
                self.need_key = False
            else:
                self.held += 1; return False
        self.next = fid + 1
        return True

def run(args):
    print(f"[enc] {args.w}x{args.h}@{args.fps} {args.mbps} Mbps gop={args.gop} {args.secs}s")
    stream = encode(args.w, args.h, args.fps, args.secs, args.mbps, args.gop)
    aus = access_units(stream)
    keys = [len(a) for a, k in aus if k]
    print(f"[enc] {len(aus)} AUs, {len(keys)} keyframes, key size avg {sum(keys)//max(1,len(keys))} B, "
          f"P avg {sum(len(a) for a,k in aus if not k)//max(1,len(aus)-len(keys))} B")
    assert b"".join(a for a, _ in aus) == stream
    ref = decode_all(stream)
    print(f"[ref] clean decode: {len(ref)} frames")
    ref_index = {}
    for i, fr in enumerate(ref):
        ref_index.setdefault(hash(fr), []).append(i)

    port = args.port
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rcv = args.rcvbuf or (8 * 1024 * 1024 if args.mode == "framed" else 0)
    if rcv:
        rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcv)
    rx.bind(("127.0.0.1", port))
    rx.setblocking(False)
    eff = rx.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    print(f"[rx ] bound 127.0.0.1:{port}, SO_RCVBUF effective {eff} B")
    dec = subprocess.Popen(decoder_cmd(args.mode == "framed"), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
    frames, errs, stop = [], [], threading.Event()

    def reader():
        while True:
            chunk = dec.stdout.read(FB)
            if not chunk:
                return
            frames.append(chunk)

    def err_reader():
        for line in dec.stderr:
            errs.append(line.decode(errors="replace").strip())

    framed = args.mode == "framed"
    wq = queue.Queue(maxsize=16) if framed else queue.Queue()
    reasm, gate = Reassembler(), Gate()
    counters = {"delivered": 0, "overflows": 0}
    def writer():
        while True:
            b = wq.get()
            if b is None: return
            try:
                dec.stdin.write(b); dec.stdin.flush()
            except BrokenPipeError:
                return
    def receiver_decoupled():
        rx.setblocking(True); rx.settimeout(0.1)
        while not stop.is_set():
            try:
                n = rx.recv_into(rbuf)
            except (socket.timeout, BlockingIOError):
                continue
            wq.put(bytes(rbuf[:n]))
        wq.put(None)
    def receiver_framed():
        # Port of preview.rs receive_loop: never blocks on the decoder.
        rx.setblocking(True); rx.settimeout(0.1)
        while not stop.is_set():
            try:
                n = rx.recv_into(rbuf)
            except (socket.timeout, BlockingIOError):
                continue
            fr = reasm.push(bytes(rbuf[:n]))
            if fr and gate.offer(*fr):
                try:
                    wq.put_nowait(fr[3]); counters["delivered"] += 1
                except queue.Full:
                    gate.need_key = True; counters["overflows"] += 1
        wq.put(None)
    def receiver():
        # Port of the pre-fix bridge pump thread.
        while not stop.is_set():
            while True:
                try:
                    n = rx.recv_into(rbuf)
                except BlockingIOError:
                    break
                try:
                    dec.stdin.write(rbuf[:n]); dec.stdin.flush()
                except BrokenPipeError:
                    return
            time.sleep(0.002)

    rbuf = bytearray(65536)
    ts = [threading.Thread(target=reader), threading.Thread(target=err_reader),
          threading.Thread(target=receiver_framed if framed else
                           (receiver_decoupled if args.decoupled else receiver))]
    if args.decoupled or framed:
        ts.append(threading.Thread(target=writer))
    for t in ts: t.start()

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
    tx.setblocking(False)
    rng = random.Random(args.seed)
    log = []
    base = udp_drops(port)
    t0 = time.perf_counter()
    session, frame_id, reconnects = rng.getrandbits(32), 0, 0
    for fid, (au, key) in enumerate(aus):
        target = t0 + fid / args.fps
        while time.perf_counter() < target:
            time.sleep(0.0005)
        if args.reconnect and fid and fid % args.reconnect == 0:
            # Driver restart: new session, ids from 0, resumes mid-GOP.
            session, frame_id, reconnects = rng.getrandbits(32), 0, reconnects + 1
        if framed:
            chunks = list(packetize(au, session, frame_id, key))
            frame_id += 1
        else:
            chunks = [au[o:o + 1400] for o in range(0, len(au), 1400)]
        sent, aborted, induced = 0, False, 0
        for chunk in chunks:
            if args.loss and rng.random() < args.loss:
                induced += 1          # induced network loss: datagram vanishes
            else:
                try:
                    tx.sendto(chunk, ("127.0.0.1", port))
                except BlockingIOError:
                    aborted = True; break
            sent += 1
        d = udp_drops(port)
        log.append((fid, len(au), key, sent, induced, d - base, aborted))
        base = d
    time.sleep(0.5)
    stop.set()
    ts[2].join()
    if args.decoupled or framed: ts[3].join()
    dec.stdin.close()
    ts[0].join(); ts[1].join(); dec.wait()

    # Correlate.
    lossy = [(fid, size, key, sent, ind, kd) for fid, size, key, sent, ind, kd, ab in log if kd or ind or ab]
    print(f"[tx ] frames sent {len(log)}, frames with lost datagrams {len(lossy)} "
          f"(kernel rx drops {sum(l[5] for l in log)}, induced {sum(l[4] for l in log)}, "
          f"sender aborts {sum(1 for l in log if l[6])}, reconnects {reconnects})")
    if framed:
        print(f"[rx ] framed: delivered {counters['delivered']}, incomplete discarded {reasm.incomplete}, "
              f"rejected datagrams {reasm.rejected}, gaps {gate.gaps}, held for keyframe {gate.held}, "
              f"queue overflows {counters['overflows']}")
    for fid, size, key, sent, ind, kd in lossy[:args.show]:
        print(f"      frame {fid:4d} size {size:7d} key={int(key)} datagrams {sent:3d} "
              f"kernel_drop {kd:3d} induced {ind:2d}")
    corrupt, matched = [], 0
    for i, fr in enumerate(frames):
        if hash(fr) in ref_index:
            matched += 1
        else:
            corrupt.append(i)
    print(f"[dec] decoded {len(frames)} frames: {matched} exact, {len(corrupt)} CORRUPT "
          f"(visible artifacts), decoder error lines {len(errs)}")
    for e in errs[:args.show]:
        print("      ffmpeg:", e)
    return len(corrupt)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--w", type=int, default=1920); ap.add_argument("--h", type=int, default=1080)
    ap.add_argument("--fps", type=int, default=60); ap.add_argument("--secs", type=int, default=5)
    ap.add_argument("--mbps", type=int, default=20); ap.add_argument("--gop", type=int, default=10)
    ap.add_argument("--rcvbuf", type=int, default=0, help="0 = OS default")
    ap.add_argument("--loss", type=float, default=0.0); ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--port", type=int, default=43069); ap.add_argument("--show", type=int, default=8)
    ap.add_argument("--mode", choices=["legacy", "framed"], default="framed")
    ap.add_argument("--decoupled", action="store_true", help="legacy mode with recv/writer threads split")
    ap.add_argument("--reconnect", type=int, default=0, help="new sender session every N frames")
    sys.exit(0 if run(ap.parse_args()) == 0 else 1)
