"""Mutation fuzzing of the serve wire protocol. Deterministic (seeded) so
kill vectors reproduce. Two passes:
  1. explicit interesting cases (header-field lies, boundary lengths,
     checksum corruption, structured field abuse)
  2. seeded random mutations (bit flips / spans / truncation / field rewrites)
Daemon liveness is asserted after EVERY case; a death is recorded with its
exact bytes, the daemon restarted, and the campaign continues.
"""

import os
import random
import socket
import struct
import time
import zlib

from lib import (MSG_SERVE_REQUEST, frame, send_request, serve_request,
                 encoder_wire)

SIZEOF_SERVE_REQUEST = 64 + 16 + 4 + 4 + 4 + 4 + 4 + 28  # 128


def _explicit_cases():
    """(name, bytes) pairs covering each header/field validation branch."""
    valid = frame(MSG_SERVE_REQUEST, serve_request(
        b"NomosUni-SPAN-x2", b"png", 256, 16, 90,
        b"/nonexistent/in.png", b"/nonexistent/out.png"))

    hdr = struct.pack("<IHI", 0x53534358, MSG_SERVE_REQUEST, 128)
    good_crc = struct.pack("<I", zlib.crc32(
        hdr + serve_request(b"", b"")[:128]) & 0xFFFFFFFF)

    cases = []
    # header type field sweep (dispatcher branch)
    for t in (0x0000, 0x0007, 0x0500, 0x0600, 0x0602, 0x0603, 0x0604,
              0x0700, 0x0701, 0xFFFF, 0x0601):
        body = serve_request(b"", b"", inp=b"/nonexistent/x.png")
        cases.append((f"type-{t:#06x}", frame(t, body)))
    # payload_length sweep (alloc-bound + framing branches)
    for n in (0, 1, 100, SIZEOF_SERVE_REQUEST - 1, SIZEOF_SERVE_REQUEST,
              SIZEOF_SERVE_REQUEST + 1, 5000, 0x1FFFFFFF, 0x7FFFFFFF,
              0xF0000000, 0xFFFFFFFF):
        h = struct.pack("<IHI", 0x53534358, MSG_SERVE_REQUEST, n)
        crc = struct.pack("<I", zlib.crc32(h) & 0xFFFFFFFF)
        cases.append((f"plen-{n:#x}", h + crc))
    # checksum: zeroed / flipped / stale (valid header, wrong crc)
    cases.append(("crc-zero", hdr + b"\0\0\0\0"
                  + serve_request(b"", b"", inp=b"/nonexistent/x.png")))
    cases.append(("crc-flip", valid[:10]
                  + bytes([valid[10] ^ 0xFF]) + valid[11:]))
    cases.append(("crc-truncated-frame", valid[:14]))
    # path-length field lies (offsets inside ServeRequest after 64+16+12)
    for inp_len in (0, 1, 4095, 4096, 4097, 0x80000000, 0xFFFFFFFF):
        body = bytearray(serve_request(
            b"", b"", inp=b"/nonexistent/a.png", outp=b"/nonexistent/b.png"))
        struct.pack_into("<I", body, 64 + 16 + 12, inp_len)
        cases.append((f"input-path-len-{inp_len:#x}",
                      frame(MSG_SERVE_REQUEST, bytes(body))))
    for out_len in (0, 1, 0xFFFFFFFF):
        body = bytearray(serve_request(
            b"", b"", inp=b"/nonexistent/a.png", outp=b"/nonexistent/b.png"))
        struct.pack_into("<I", body, 64 + 16 + 16, out_len)
        cases.append((f"output-path-len-{out_len:#x}",
                      frame(MSG_SERVE_REQUEST, bytes(body))))
    # max boundary: input 4095 + output 4095 → payload = 128+8190 (legal max)
    body = serve_request(b"", b"", inp=b"/x" * 2047, outp=b"/y" * 2047)
    cases.append(("paths-at-max", frame(MSG_SERVE_REQUEST, body)))
    # fixed-width fields without NUL: model / format full-width junk
    cases.append(("model-64x-junk", frame(MSG_SERVE_REQUEST, serve_request(
        bytes(range(64)), b"png", 0, 0, 0, b"/nonexistent/x.png"))))
    cases.append(("model-traversal", frame(MSG_SERVE_REQUEST, serve_request(
        b"../../../" * 7, b"png", 0, 0, 0, b"/nonexistent/x.png"))))
    cases.append(("model-abs-path", frame(MSG_SERVE_REQUEST, serve_request(
        b"/etc/passwd\0", b"png", 0, 0, 0, b"/nonexistent/x.png"))))
    cases.append(("format-16x-junk", frame(MSG_SERVE_REQUEST, serve_request(
        b"", bytes(range(16)), 0, 0, 0, b"/nonexistent/x.png"))))
    # tile / overlap / quality extreme values
    for name, tile, ovl, q in (
            ("tile-max-int", 0x7FFFFFFF, 16, 90),
            ("tile-min-int", -0x80000000, 16, 90),
            ("tile-1", 1, 0, 90),
            ("tile-65536", 65536, 16, 90),
            ("ovl-max-int", 256, 0x7FFFFFFF, 90),
            ("ovl-neg", 256, -16, 90),
            ("ovl-gt-tile", 4, 4, 90),
            ("qual-0", 0, 16, 0),
            ("qual-neg", 0, 16, -1),
            ("qual-max", 0, 16, 0x7FFFFFFF),
    ):
        cases.append((name, frame(MSG_SERVE_REQUEST, serve_request(
            b"", b"png", tile, ovl, q,
            b"/nonexistent/x.png", b"/nonexistent/y.png"))))
    # encoder wire extremes (junk knobs on a png output: png_level read)
    cases.append(("encoder-junk", frame(MSG_SERVE_REQUEST,
        serve_request(b"", b"png", 0, 16, 90, b"/nonexistent/x.png",
                      encoder=struct.pack("<iiiB3xiii",
                                          0x7FFFFFFF, 0x7FFFFFFF,
                                          0x7FFFFFFF, 200,
                                          0x7FFFFFFF, 0x7FFFFFFF, -5)))))
    # trailing junk after a valid payload / doubled frames on one connection
    cases.append(("trailing-junk+100", valid + b"\x55" * 100))
    cases.append(("double-frame", valid + valid))
    cases.append(("empty-conn"))
    return cases


def run(ctx, corpus_imgs, work_dir, checks):
    sock = ctx.daemon.sock
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    canary_in = corpus_imgs["std256.png"]
    canary_out = os.path.join(out, "fuzz_canary.png").encode()
    canary_req = serve_request(b"NomosUni-SPAN-x2", b"png", 0, 16, 90,
                               str(canary_in).encode(), canary_out)
    rng = random.Random(ctx.seed)
    n = 60 if ctx.quick else 600

    def alive():
        return ctx.daemon.alive()

    def fuzz_send(data, timeout=1.5):
        """Send mutated bytes, then read AT MOST ONE reply frame within a
        short deadline. The daemon never delivers EOF to the peer (its
        teardown watch-fd dup keeps the pipe open until process exit), so
        waiting for close would eat the full client timeout on every
        fast-rejected case; closing instead also unblocks a daemon thread
        still waiting for declared-but-unsent payload bytes."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        out = b""
        try:
            s.connect(sock)
            s.sendall(data)
            hdr = b""
            while len(hdr) < 14:
                chunk = s.recv(14 - len(hdr))
                if not chunk:
                    return out
                hdr += chunk
            import struct as _s
            plen = _s.unpack_from("<I", hdr, 6)[0]
            out = hdr
            payload = b""
            while len(payload) < min(plen, 65536):
                chunk = s.recv(min(plen, 65536) - len(payload))
                if not chunk:
                    break
                payload += chunk
            out += payload
        except (socket.timeout, ConnectionResetError, BrokenPipeError, OSError):
            pass
        s.close()
        return out

    def send_case(name, data):
        if data:
            try:
                print(f"    · {name}", flush=True)
                fuzz_send(data)
            except OSError:
                pass  # connection-level failure counts as suspicious below
        if not alive():
            checks.check(f"survived: {name}", False, "daemon died")
            ctx.restart("fuzz", name)
            return False
        return True

    # deterministic explicit pass
    t_fuzz = time.time()
    killed = 0
    expl = [(c if isinstance(c, tuple) else (c,)) for c in _explicit_cases()]
    for idx, (name, *rest) in enumerate(expl):
        data = rest[0] if rest else b""
        if not send_case(f"explicit/{name}", data):
            killed += 1
            if killed >= 8:
                checks.check("kill-vector cap", False,
                             "8 daemon deaths — aborting fuzz pass")
                return
        if (idx + 1) % 25 == 0:
            checks.info(f"explicit {idx + 1}/{len(expl)} "
                        f"({time.time() - t_fuzz:.1f}s)")

    # seeded random mutation pass off a valid frame. The base carries a
    # NONEXISTENT input path so survivors fail fast at load — mutations must
    # stress the parser/validation branches, not run real inference (a real
    # image here turns each surviving mutation into a multi-second upscale).
    dead = serve_request(b"NomosUni-SPAN-x2", b"png", 0, 16, 90,
                         b"/nonexistent/fuzz_in.png",
                         b"/nonexistent/fuzz_out.png")
    base = frame(MSG_SERVE_REQUEST, dead)
    for i in range(n):
        m = rng.random()
        data = bytearray(base)
        if m < 0.30:      # bit flips
            for _ in range(rng.randint(1, 8)):
                p = rng.randrange(len(data))
                data[p] ^= 1 << rng.randrange(8)
        elif m < 0.45:    # span substitution
            p = rng.randrange(len(data))
            ln = rng.randint(1, min(64, len(data) - p))
            data[p:p + ln] = bytes(rng.randrange(256) for _ in range(ln))
        elif m < 0.60:    # truncation
            data = data[:rng.randrange(1, len(data))]
        elif m < 0.70:    # payload_length rewrite (checksum left stale)
            struct.pack_into("<I", data, 6, rng.choice(
                (0, 1, 127, 128, 129, 4160, 8318, 0xFFFFFFFF)))
        elif m < 0.78:    # type rewrite
            struct.pack_into("<H", data, 4, rng.randrange(0x10000))
        elif m < 0.86:    # path-length rewrite
            struct.pack_into("<I", data, 14 + 64 + 16 + 12 + 8,
                             rng.choice((0, 1, 4095, 4096, 0xFFFFFFFF)))
        elif m < 0.93:    # random garbage frame
            data = bytes(rng.randrange(256)
                         for _ in range(rng.randint(1, 512)))
        else:             # trailing junk
            data = data + bytes(rng.randrange(256)
                                for _ in range(rng.randint(1, 256)))
        if not send_case(f"mut-{i}", bytes(data)):
            killed += 1
            if killed >= 8:
                checks.check("kill-vector cap", False,
                             "8 daemon deaths — aborting fuzz pass")
                return
        if (i + 1) % 100 == 0:
            checks.info(f"mutated {i + 1}/{n} ({time.time() - t_fuzz:.1f}s)")

    # canary: valid work still served after the whole campaign
    try:
        r = send_request(sock, canary_req, timeout=180)
        checks.check("canary-after-fuzz",
                     r["ok"] == 1 and r["w"] == 512, str(r))
    except (ConnectionError, OSError, ValueError) as e:
        checks.check("canary-after-fuzz", False, str(e))
    checks.info(f"{len(_explicit_cases())} explicit + {n} mutated cases, "
                f"{killed} daemon deaths, {ctx.restarts} restarts")
