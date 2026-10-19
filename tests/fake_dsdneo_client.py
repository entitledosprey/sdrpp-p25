#!/usr/bin/env python3
"""Exercise the module's rtl_tcp server the way the decoder does, without the decoder.

    python3 tests/fake_dsdneo_client.py FREQ_HZ [FREQ_HZ ...] [--port 14423] [--seconds 3]

For each frequency (ideally a steady carrier, e.g. a TV pilot or a NOAA weather station):
sends set-frequency and set-sample-rate like dsd-neo, then measures the delivered sample
rate, the strongest tone's offset from the requested frequency, and how long after the
retune the tone appeared. Needs numpy.
"""

import argparse
import socket
import struct
import time

import numpy as np

RATE = 1_536_000


def cmd(sock, code, value):
    sock.sendall(struct.pack(">BI", code, value))


def read_exact(sock, n):
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(min(1 << 16, n - len(out)))
        if not chunk:
            raise ConnectionError("server closed the connection")
        out += chunk
    return bytes(out)


def to_iq(raw):
    x = np.frombuffer(raw, np.uint8).astype(np.float32) - 127.5
    return (x[0::2] + 1j * x[1::2]) / 127.5


def peak_offset(iq, rate):
    n = 1 << 18
    seg = iq[-n:] * np.hanning(min(n, len(iq)))
    spec = np.abs(np.fft.fftshift(np.fft.fft(seg))) ** 2
    freqs = np.fft.fftshift(np.fft.fftfreq(len(seg), 1 / rate))
    near = np.abs(freqs) < 50e3
    i = np.flatnonzero(near)[np.argmax(spec[near])]
    snr = 10 * np.log10(spec[i] / np.median(spec[near]))
    return freqs[i], snr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("freqs", type=float, nargs="+")
    ap.add_argument("--port", type=int, default=14423)
    ap.add_argument("--seconds", type=float, default=3.0)
    args = ap.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    header = read_exact(sock, 12)
    magic, tuner, gains = header[:4], *struct.unpack(">II", header[4:])
    print(f"header {magic!r} tuner={tuner} gains={gains}")
    assert magic == b"RTL0", "bad rtl_tcp header"

    ok = True
    for f in args.freqs:
        cmd(sock, 0x01, int(f))
        t_cmd = time.monotonic()
        cmd(sock, 0x02, RATE)
        cmd(sock, 0x03, 1)  # gain mode: must be ignored without breaking anything

        # time until the tone shows up: read 20 ms blocks until the peak is near zero offset
        settle = None
        while time.monotonic() - t_cmd < 1.0:
            iq = to_iq(read_exact(sock, 2 * int(RATE * 0.02)))
            off, snr = peak_offset(iq, RATE)
            if abs(off) < 2e3 and snr > 15:
                settle = time.monotonic() - t_cmd
                break

        t0 = time.monotonic()
        raw = read_exact(sock, 2 * int(RATE * args.seconds))
        rate = len(raw) / 2 / (time.monotonic() - t0)
        off, snr = peak_offset(to_iq(raw), RATE)
        good = abs(rate / RATE - 1) < 0.01 and abs(off) < 2e3 and snr > 15
        ok &= good
        print(f"{f / 1e6:11.6f} MHz: rate {rate / 1e6:.4f} MS/s, peak {off:+7.0f} Hz at {snr:4.1f} dB, "
              f"tone after {('%.0f ms' % (settle * 1e3)) if settle else 'n/a'}  {'OK' if good else 'CHECK'}")
    print("all OK" if ok else "some checks need attention")


if __name__ == "__main__":
    main()
