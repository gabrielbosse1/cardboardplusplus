"""MediaPipe Hand Landmarker over TCP.

Listens on 127.0.0.1:42073, accepts connections from the Rust bridge.
Each connection: bridge sends raw JPEG bytes, server runs hand detection
and replies with a fixed-size binary frame.

Wire protocol (little-endian):
  Bridge -> Server:  raw JPEG bytes (length from TCP stream, no framing needed
                     because the server reads until connection close per frame
                     ... actually let's use length-prefixed framing).
  Actually: bridge sends:
    [4 bytes u32 LE: jpeg_length] [jpeg_length bytes: JPEG data]
  Server replies:
    [1 byte: num_hands]
    Per hand:
      [1 byte: handedness (0=left, 1=right)]
      [4 bytes f32 LE: score]
      [21 * 3 * 4 = 252 bytes: 21 landmarks as (x,y,z) f32 LE each]
    If num_hands == 0: just the 1 zero byte.

Usage:
    python mediapipe_server.py [--port 42073]
"""

import argparse
import socket
import struct
import sys
import time

import cv2
import numpy as np
from mediapipe.tasks.python import BaseOptions
from mediapipe.tasks.python.vision import HandLandmarker, HandLandmarkerOptions, RunningMode

MODEL_PATH = str(__import__("pathlib").Path(__file__).parent / "models" / "hand_landmarker.task")
PORT = 42073
MAX_HANDS = 2


def create_landmarker():
    opts = HandLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=MODEL_PATH),
        running_mode=RunningMode.VIDEO,
        num_hands=MAX_HANDS,
        min_hand_detection_confidence=0.5,
        min_hand_presence_confidence=0.5,
        min_tracking_confidence=0.5,
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


def handle_connection(conn, landmarker):
    frame_idx = 0
    try:
        while True:
            # Read length-prefixed JPEG frame
            hdr = recv_exact(conn, 4)
            if hdr is None:
                break
            jpeg_len = struct.unpack("<I", hdr)[0]
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
            timestamp_ms = int(time.time() * 1000)
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
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()

    print(f"[mediapipe] loading model from {MODEL_PATH}", file=sys.stderr)
    landmarker = create_landmarker()
    print(f"[mediapipe] model loaded, listening on 127.0.0.1:{args.port}", file=sys.stderr)
    sys.stderr.flush()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    srv.settimeout(1.0)

    while True:
        try:
            conn, addr = srv.accept()
            print(f"[mediapipe] bridge connected from {addr}", file=sys.stderr)
            sys.stderr.flush()
            handle_connection(conn, landmarker)
            print("[mediapipe] bridge disconnected", file=sys.stderr)
            sys.stderr.flush()
            conn.close()
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    main()
