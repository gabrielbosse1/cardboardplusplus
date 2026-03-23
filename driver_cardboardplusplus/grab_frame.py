# Quick tester script that smart AIs can use!
#!/usr/bin/env python3
"""UDP receiver for CardboardPlusPlus SteamVR driver frames."""

import socket
import struct
import time
import sys

PORT = 42069

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(('127.0.0.1', PORT))
    sock.settimeout(5.0)

    print(f"Listening on 127.0.0.1:{PORT} for H264 frames...")
    print("Press Ctrl+C to stop\n")

    frame_data = bytearray()
    packets_received = 0
    bytes_received = 0
    start_time = time.time()
    last_report = start_time

    # H264 NAL unit start codes
    NAL_START = b'\x00\x00\x00\x01'
    NAL_START_SHORT = b'\x00\x00\x01'

    frame_count = 0
    keyframes = 0
    first_keyframe_saved = False

    try:
        while True:
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout:
                elapsed = time.time() - start_time
                if packets_received == 0:
                    print(f"  Waiting... ({elapsed:.0f}s elapsed, no data yet)")
                continue

            packets_received += 1
            bytes_received += len(data)
            frame_data.extend(data)

            # Check for H264 NAL start code at beginning of chunk
            is_keyframe_start = False
            if len(data) >= 5:
                # NAL type 5 = IDR (keyframe), type 7 = SPS, type 8 = PPS
                if data[:4] == NAL_START or data[:3] == NAL_START_SHORT:
                    nal_offset = 4 if data[:4] == NAL_START else 3
                    if nal_offset < len(data):
                        nal_type = data[nal_offset] & 0x1F
                        if nal_type in (5, 7, 8):
                            is_keyframe_start = True
                            keyframes += 1

            # Save first keyframe for analysis
            if is_keyframe_start and not first_keyframe_saved and len(frame_data) > 1000:
                filename = 'first_keyframe.h264'
                with open(filename, 'wb') as f:
                    f.write(frame_data)
                print(f"  >>> Saved first keyframe to {filename} ({len(frame_data)} bytes)")
                first_keyframe_saved = True
                frame_data = bytearray()

            # Report stats every 2 seconds
            now = time.time()
            if now - last_report >= 2.0:
                elapsed = now - start_time
                rate = packets_received / elapsed if elapsed > 0 else 0
                mbps = (bytes_received / elapsed) / (1024 * 1024) if elapsed > 0 else 0
                print(f"  Packets: {packets_received} | Rate: {rate:.1f} pkt/s | "
                      f"Throughput: {mbps:.2f} MB/s | Keyframes: {keyframes} | "
                      f"Buffer: {len(frame_data)} bytes")
                last_report = now

    except KeyboardInterrupt:
        print(f"\nStopped. Total: {packets_received} packets, {bytes_received} bytes, {keyframes} keyframes")
    finally:
        sock.close()

if __name__ == '__main__':
    main()
