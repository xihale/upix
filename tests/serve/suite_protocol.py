"""Protocol robustness suite: every malformed case anchors a specific
validation branch in ServeDaemon::handle_client. Malformed cases assert
daemon liveness only; one canary at the end proves the daemon still serves."""

import os
import socket
import struct
import threading
import zlib

from lib import (Checks, Daemon, MSG_SERVE_REQUEST, frame, raw_roundtrip,
                 send_request, serve_request)

MODEL_NAME = b"NomosUni-SPAN-x2"


def run(daemon: Daemon, corpus, work_dir, checks: Checks):
    sock = daemon.sock
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    canary_in = corpus["std256.png"]
    canary_out = os.path.join(out, "proto_canary.png").encode()

    def alive():
        return daemon.alive()

    def well_formed(model=b"N", fmt=b"png", inp=canary_in,
                    outp=canary_out):
        return serve_request(model, fmt, 0, 16, 90,
                             str(inp).encode(), str(outp).encode())

    # ── malformed frames (each maps to one validation branch) ───────────────
    hdr10 = struct.pack("<IHI", 0x53534358, MSG_SERVE_REQUEST, 8)
    crc = zlib.crc32(hdr10 + b"\0" * 8) & 0xFFFFFFFF
    cases = [
        # unknown bytes: header parser must reject, not crash
        ("garbage-bytes", b"\xde\xad\xbe\xef" * 100),
        # right magic, wrong type: dispatcher must refuse non-SERVE_REQUEST
        ("wrong-type", frame(0x0701, well_formed())),
        # payload shorter than sizeof(ServeRequest)
        ("short-payload", frame(MSG_SERVE_REQUEST, b"\0" * 8)),
        # F1 regression: ~4GB declared payload must be capped, not allocated
        ("alloc-bomb-4GB",
         frame(MSG_SERVE_REQUEST, well_formed())[:10]
         + struct.pack("<I", 0xF0000000) + b"\0\0\0\0"),
        # path-length accounting: claim input_path_length=0 (offset 92) while
        # the input path bytes are still present
        ("path-length-lie", frame(MSG_SERVE_REQUEST,
                                  well_formed()[:92] + b"\0\0\0\0"
                                  + well_formed()[96:])),
        # fixed-width fields without NUL termination (F2: bounded_string)
        ("model-64x-nonul", frame(MSG_SERVE_REQUEST, serve_request(
            b"A" * 64, b"png", 0, 16, 90, str(canary_in).encode(),
            canary_out))),
        ("format-16x-nonul", frame(MSG_SERVE_REQUEST, serve_request(
            MODEL_NAME, b"J" * 16, 0, 16, 90, str(canary_in).encode(),
            canary_out))),
    ]
    for name, data in cases:
        raw_roundtrip(sock, data, timeout=15)
        checks.check(name, alive(), "" if alive() else "daemon died")

    # request-shaped but path-invalid payloads via the normal client
    for name, payload in [
        ("zero-input-path", serve_request(MODEL_NAME, b"png", 0, 16, 90,
                                          b"", canary_out)),
        ("non-utf8-path", serve_request(MODEL_NAME, b"png", 0, 16, 90,
                                        b"/tmp/\xff\xfe\xbad", canary_out)),
        ("outside-image-path", serve_request(MODEL_NAME, b"png", 0, 16, 90,
                                             b"/etc/hostname", canary_out)),
    ]:
        try:
            send_request(sock, payload, timeout=60)
        except (ConnectionError, OSError, ValueError):
            pass
        checks.check(name, alive(), "" if alive() else "daemon died")

    # ── load-shaped abuse ───────────────────────────────────────────────────
    socks = []
    try:
        for _ in range(64):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(sock)
            socks.append(s)
        checks.check("connect-storm-64", True)
    except OSError as e:
        # backlog exhaustion (EAGAIN) is acceptable — daemon must still live
        checks.check("connect-storm-64", alive(),
                     "" if alive() else f"all connects failed: {e}")
    finally:
        for s in socks:
            s.close()

    # hang up right after a valid request: MSG_NOSIGNAL must protect the
    # daemon and the response write to the dead peer must be non-fatal
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock)
    s.sendall(frame(MSG_SERVE_REQUEST, serve_request(
        MODEL_NAME, b"png", 0, 16, 90,
        str(corpus["sq511.png"]).encode(),
        os.path.join(out, "proto_abandon.png").encode())))
    s.close()
    checks.check("hangup-after-request", alive(),
                 "" if alive() else "daemon died")

    # ── single canary for the whole suite ───────────────────────────────────
    try:
        r = send_request(sock, serve_request(
            MODEL_NAME, b"png", 0, 16, 90,
            str(canary_in).encode(), canary_out), timeout=180)
        checks.check("canary-after-fuzz",
                     r["ok"] == 1 and r["w"] == 512 and r["h"] == 512, str(r))
    except (ConnectionError, OSError, ValueError) as e:
        checks.check("canary-after-fuzz", False, str(e))
