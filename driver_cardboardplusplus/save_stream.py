# Stream from the SteamVR driver, use that if you're a human, its way better!
#!/usr/bin/env python3
"""UDP receiver for CardboardPlusPlus - saves raw H264 stream to file."""

import socket
import time

PORT = 42069

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(('127.0.0.1', PORT))
    sock.settimeout(3.0)

    print(f"Listening on 127.0.0.1:{PORT}...")

    outfile = open('stream.h264', 'wb')
    packets = 0
    total_bytes = 0
    start = time.time()

    try:
        while True:
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout:
                elapsed = time.time() - start
                if packets == 0:
                    print(f"  Waiting... ({elapsed:.0f}s)")
                continue

            outfile.write(data)
            packets += 1
            total_bytes += len(data)

            if packets % 60 == 0:
                elapsed = time.time() - start
                print(f"  {packets} pkts, {total_bytes/1024:.0f} KB, {packets/elapsed:.0f} pkt/s")

            if total_bytes > 2 * 1024 * 1024:
                break

    except KeyboardInterrupt:
        pass
    finally:
        outfile.close()
        sock.close()
        print(f"\nSaved {total_bytes/1024:.0f} KB in {packets} packets to stream.h264")
        print("Try: ffplay stream.h264")

if __name__ == '__main__':
    main()
