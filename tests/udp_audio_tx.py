#!/usr/bin/env python3
"""Send test tones to the module's audio port the way the decoder sends voice.

    python3 tests/udp_audio_tx.py [--port 23456] [--seconds 6]

Plays 2 s of 1 kHz on slot 1 (left), 2 s of 500 Hz on slot 2 (right), then 2 s of both,
as 8 kHz s16le stereo in 20 ms datagrams. You should hear each part in turn in SDR++ through
the "P25 Trunking" sink stream (and silence, not stutter, afterwards).
"""

import argparse
import socket
import struct
import time

import math


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=23456)
    ap.add_argument("--seconds", type=float, default=6.0)
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rate, frame = 8000, 160
    n = 0
    t0 = time.monotonic()
    while n < rate * args.seconds:
        part = int(n / (rate * args.seconds / 3))
        samples = []
        for i in range(frame):
            t = (n + i) / rate
            left = 0.3 * math.sin(2 * math.pi * 1000 * t) if part in (0, 2) else 0.0
            right = 0.3 * math.sin(2 * math.pi * 500 * t) if part in (1, 2) else 0.0
            samples += [int(left * 32767), int(right * 32767)]
        sock.sendto(struct.pack(f"<{len(samples)}h", *samples), ("127.0.0.1", args.port))
        n += frame
        time.sleep(max(0.0, t0 + n / rate - time.monotonic()))
    print(f"sent {n / rate:.1f} s of test audio")


if __name__ == "__main__":
    main()
