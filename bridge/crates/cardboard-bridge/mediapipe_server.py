"""MediaPipe Hand Landmarker over TCP.

Listens on 127.0.0.1:42073, accepts connections from the Rust bridge.
Each connection: bridge sends raw JPEG bytes, server runs hand detection
and replies with a fixed-size binary frame.

Wire protocol (little-endian), one TCP connection, sequential requests:
  Bridge -> Server:  [4 bytes u32 LE: jpeg_length] [jpeg_length bytes: JPEG data]
  Server replies:
    [1 byte: num_hands]
    Per hand:
      [1 byte: handedness (0=left, 1=right)]
      [4 bytes f32 LE: score]
      [21 * 3 * 4 = 252 bytes: 21 landmarks as (x,y,z) f32 LE each]
    If num_hands == 0: just the 1 zero byte.

  Live config (no restart needed):
  Bridge -> Server:  [4 bytes u32 LE: 0xFFFFFFFF] [1 byte kind=0x01]
                     [4 bytes f32 LE: min_detection]
                     [4 bytes f32 LE: min_presence]
                     [4 bytes f32 LE: min_tracking]
  Server recreates the landmarker and replies b"\\x00" as ack.
  A JPEG can never be 4 GiB, so the sentinel can't collide with a frame.

Usage:
    python mediapipe_server.py
"""

import socket
import struct
import sys

import cv2
import numpy as np
from mediapipe.tasks.python import BaseOptions
from mediapipe.tasks.python.vision import HandLandmarker, HandLandmarkerOptions, RunningMode

MODEL_PATH = str(__import__("pathlib").Path(__file__).parent / "models" / "hand_landmarker.task")
PORT = 42073
MAX_HANDS = 2

# Length prefix that can never be a JPEG: selects a config frame instead.
CONFIG_SENTINEL = 0xFFFFFFFF
CONFIG_KIND_MODEL = 0x01
CONFIG_BODY_LEN = 1 + 3 * 4  # kind + 3× f32 LE

# The active landmarker. Recreated in place when a config frame arrives;
# only one connection is served at a time, so no locking is needed.
landmarker = None


def create_landmarker(min_detection=0.5, min_presence=0.5, min_tracking=0.5):
    opts = HandLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=MODEL_PATH),
        running_mode=RunningMode.VIDEO,
        num_hands=MAX_HANDS,
        min_hand_detection_confidence=min_detection,
        min_hand_presence_confidence=min_presence,
        min_tracking_confidence=min_tracking,
    )
    return HandLandmarker.create_from_options(opts)


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def handle_config(conn):
    """One sentinel-selected config frame: recreate the landmarker, ack 0x00."""
    global landmarker
    body = recv_exact(conn, CONFIG_BODY_LEN)
    if body is None:
        return
    if body[0] == CONFIG_KIND_MODEL:
        det, pres, track = struct.unpack("<fff", body[1:13])
        # Clamp so a corrupt frame can't wedge the model with NaNs.
        det = min(max(det, 0.01), 1.0)
        pres = min(max(pres, 0.01), 1.0)
        track = min(max(track, 0.01), 1.0)
        landmarker = create_landmarker(det, pres, track)
        print(f"[mediapipe] model config: detection={det:.2f} presence={pres:.2f} tracking={track:.2f}", file=sys.stderr)
        sys.stderr.flush()
    try:
        conn.sendall(b"\x00")
    except OSError:
        pass


def handle_connection(conn):
    global landmarker
    frame_idx = 0
    try:
        while True:
            # Read length-prefixed JPEG frame
            hdr = recv_exact(conn, 4)
            if hdr is None:
                break
            jpeg_len = struct.unpack("<I", hdr)[0]
            if jpeg_len == CONFIG_SENTINEL:
                handle_config(conn)
                continue
            if jpeg_len > 10_000_000:
                break  # sanity limit 10MB
            jpeg_data = recv_exact(conn, jpeg_len)
            if jpeg_data is None:
                break

            # Decode JPEG
            arr = np.frombuffer(jpeg_data, dtype=np.uint8)
            bgr = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if bgr is None:
                # Send zero-hands response
                conn.sendall(b"\x00")
                continue

            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            # Monotonic per-frame timestamp (VIDEO mode requires non-decreasing
            # timestamps; wall-clock can jump on NTP/sleep adjustments).
            timestamp_ms = frame_idx * 33
            mp_image = __import__("mediapipe").Image(image_format=__import__("mediapipe").ImageFormat.SRGB, data=rgb)

            result = landmarker.detect_for_video(mp_image, timestamp_ms)

            # Build binary response
            hands = result.hand_landmarks or []
            handednesses = result.handedness or []
            n = min(len(hands), MAX_HANDS)
            buf = bytearray([n])
            for i in range(n):
                lm = hands[i]
                hh = handednesses[i][0].category_name if handednesses[i] else "Right"
                h_code = 0 if hh == "Left" else 1
                score = handednesses[i][0].score if handednesses[i] else 0.0
                buf.append(h_code)
                buf.extend(struct.pack("<f", score))
                for pt in lm:
                    buf.extend(struct.pack("<fff", pt.x, pt.y, pt.z))

            conn.sendall(bytes(buf))
            frame_idx += 1
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass


def main():
    global landmarker
    print(f"[mediapipe] loading model from {MODEL_PATH}", file=sys.stderr)
    landmarker = create_landmarker()
    print(f"[mediapipe] model loaded, listening on 127.0.0.1:{PORT}", file=sys.stderr)
    sys.stderr.flush()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    srv.settimeout(1.0)

    while True:
        try:
            conn, addr = srv.accept()
            print(f"[mediapipe] bridge connected from {addr}", file=sys.stderr)
            sys.stderr.flush()
            handle_connection(conn)
            print("[mediapipe] bridge disconnected", file=sys.stderr)
            sys.stderr.flush()
            conn.close()
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    main()
