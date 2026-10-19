#!/usr/bin/env python3
"""Pretend to be the decoder's API so the panel can be checked without radio traffic.

    python3 tests/fake_api_server.py [--port 9911]

Start the module with "Launch dsd-neo" off, then run this. It sends a welcome line, then
cycles system/status/metrics telemetry, a clear call on slot 1, an encrypted call on slot 2,
and matching event rows. Commands the module sends are printed.
"""

import argparse
import json
import socket
import threading
import time

CC, VC = 851_012_500, 852_262_500


def serve(conn):
    def send(obj):
        conn.sendall((json.dumps(obj) + "\n").encode())

    def reader():
        buf = b""
        while True:
            data = conn.recv(4096)
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                req = json.loads(line)
                print("module sent:", req)
                send({"id": req.get("id"), "ok": True, "cmd": req.get("cmd"), "status": "queued"})

    send({"type": "welcome", "protocol": 1, "auth_required": False,
          "topics": ["call", "event", "system", "metrics", "quality", "status", "result"]})
    threading.Thread(target=reader, daemon=True).start()
    seq = 0
    while True:
        seq += 1
        now = time.time()
        send({"type": "system", "seq": seq, "protocol": "P25p2", "cc_freq_hz": CC, "vc_freq_hz": VC,
              "trunking": True, "p25_neighbors": [{"freq_hz": CC}]})
        send({"type": "status", "seq": seq, "message": None,
              "tg_policy": {"context": "1", "generation": 1}})
        send({"type": "metrics", "seq": seq, "snr_db": 18.5, "cfo_hz": -240, "carrier_lock": True})
        active = seq % 20 < 12
        slots = []
        if active:
            slots.append({"slot": 0, "state": 2, "name": "Example Dispatch", "tg_id": 1001,
                          "src_text": "Unit 12", "enc": False, "frequency_hz": VC, "elapsed_ms": (seq % 20) * 250})
            slots.append({"slot": 1, "state": 2, "name": "Example Tactical", "tg_id": 1002,
                          "src_text": "", "enc": True, "frequency_hz": VC, "elapsed_ms": 500})
        send({"type": "call", "seq": seq, "slots": slots})
        if seq % 20 == 1:
            for tg, name, enc in ((1001, "Example Dispatch", False), (1002, "Example Tactical", True)):
                send({"type": "event", "seq": seq, "category": 2, "target_id": tg, "t_name": name,
                      "source_id": 4321, "s_name": "", "enc": enc, "freq_hz": VC, "event_start_time": now,
                      "sysid_string": "P25_BEE00123_001_001", "access_code_kind": 2, "access_code": 0x123})
        time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9911)
    args = ap.parse_args()
    srv = socket.create_server(("127.0.0.1", args.port), reuse_port=True)
    print(f"fake decoder API on 127.0.0.1:{args.port}")
    while True:
        conn, _ = srv.accept()
        print("module connected")
        try:
            serve(conn)
        except (BrokenPipeError, ConnectionResetError):
            print("module disconnected")


if __name__ == "__main__":
    main()
