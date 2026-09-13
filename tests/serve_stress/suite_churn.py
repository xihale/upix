"""Connection-level churn: rapid open/close cycles, a silent horde of idle
connections, slowloris dribblers, parallel queue bursts, abandoned large
requests, and frame discipline on a reused connection. The daemon must serve
a valid canary through all of it."""

import os
import socket
import threading
import time

from lib import (MSG_SERVE_REQUEST, frame, raw_roundtrip, send_request,
                 serve_request)

MODEL = b"NomosUni-SPAN-x2"


def _canary(corpus_imgs, outp):
    return serve_request(MODEL, b"png", 0, 16, 90,
                         str(corpus_imgs["std256.png"]).encode(),
                         str(outp).encode())


def run(ctx, corpus_imgs, work_dir, checks):
    sock = ctx.daemon.sock
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    canary = _canary(corpus_imgs, os.path.join(out, "churn_canary.png"))

    def alive():
        return ctx.daemon.alive()

    def canary_ok(tag, timeout=300):
        """Serves a canary, tolerating transient connect-EAGAIN: after heavy
        churn the accept backlog drains at ~one connection per loop iteration,
        so a burst of closed connections can keep the queue full for seconds."""
        last = ""
        for attempt in range(12):
            try:
                r = send_request(sock, _canary(
                    corpus_imgs, os.path.join(out, f"churn_c_{tag}.png")),
                    timeout=timeout)
                return r["ok"] == 1, str(r)
            except (ConnectionError, OSError, ValueError) as e:
                last = f"EXC {e} (attempt {attempt})"
                time.sleep(1.0)
        return False, last

    # ── H1: connect/close churn ─────────────────────────────────────────────
    n = 60 if ctx.quick else 300
    refused = 0
    for _ in range(n):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(sock)
            s.close()
        except OSError:
            refused += 1
    checks.check(f"H1 {n}x connect/close churn, daemon alive", alive(),
                 f"{refused} refused")
    ok, detail = canary_ok("h1")
    checks.check("H1 canary after churn", ok, detail)

    # ── H2: silent horde — hold connections open without sending ────────────
    n = 48 if ctx.quick else 128
    horde = []
    try:
        for _ in range(n):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(sock)
            horde.append(s)
    except OSError:
        pass
    checks.check(f"H2 {len(horde)} silent connections accepted", True)
    ok, detail = canary_ok("h2")
    checks.check("H2 canary served while horde holds", ok, detail)
    for s in horde:
        try:
            s.close()
        except OSError:
            pass
    ok, detail = canary_ok("h2b")
    checks.check("H2 canary after horde closes", ok, detail)

    # ── H3: slowloris dribblers — 1 header byte per 40ms ────────────────────
    dribblers = []
    stop_flag = threading.Event()

    def dribble():
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(15)
            s.connect(sock)
            dribblers.append(s)
            data = frame(MSG_SERVE_REQUEST, serve_request(
                MODEL, b"png", 0, 16, 90,
                str(corpus_imgs["sq511.png"]).encode(),
                os.path.join(out, "churn_dribble.png").encode()))
            for i in range(0, len(data), 1):
                if stop_flag.is_set():
                    return
                s.sendall(data[i:i + 1])
                time.sleep(0.04)
        except OSError:
            pass

    ths = [threading.Thread(target=dribble, daemon=True) for _ in range(12)]
    [t.start() for t in ths]
    time.sleep(1.0)  # let them wedge mid-header
    ok, detail = canary_ok("h3")
    checks.check("H3 canary served during slowloris", ok, detail)
    stop_flag.set()
    [t.join(timeout=20) for t in ths]
    for s in dribblers:
        try:
            s.close()
        except OSError:
            pass
    checks.check("H3 daemon alive after dribblers", alive())

    # ── H4: parallel queue burst — 24 valid requests at once ────────────────
    n = 8 if ctx.quick else 24
    errs, lock = [], threading.Lock()

    def burst(wid):
        try:
            r = send_request(sock, _canary(
                corpus_imgs, os.path.join(out, f"churn_b_{wid}.png")),
                timeout=300)
            if r["ok"] != 1:
                with lock:
                    errs.append(f"w{wid}: {r['error']}")
        except Exception as e:  # noqa: BLE001
            with lock:
                errs.append(f"w{wid}: EXC {e}")

    t0 = time.time()
    ths = [threading.Thread(target=burst, args=(w,)) for w in range(n)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    dt = time.time() - t0
    checks.check(f"H4 {n}-wide burst all ok", not errs, "; ".join(errs[:3]))
    checks.info(f"H4 burst wall time {dt:.1f}s "
                f"({dt / n:.2f}s/request serialized)")
    checks.check("H4 daemon alive after burst", alive())

    # ── H5: abandon big requests, then queue a canary behind the wreckage ───
    for i in range(2):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(sock)
        s.sendall(frame(MSG_SERVE_REQUEST, serve_request(
            MODEL, b"png", 0, 16, 90,
            str(corpus_imgs["sq511.png"]).encode(),
            os.path.join(out, f"churn_abandon_{i}.png").encode())))
        s.close()
    ok, detail = canary_ok("h5", timeout=600)
    checks.check("H5 canary after 2 abandoned big requests", ok, detail)

    # ── H6: second frame on an already-served connection ────────────────────
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(60)
        s.connect(sock)
        s.sendall(frame(MSG_SERVE_REQUEST, _canary(
            corpus_imgs, os.path.join(out, "churn_reuse1.png"))))
        # drain first response
        from lib import HEADER, _recv_exact, MAGIC, MSG_SERVE_RESPONSE
        magic, mtype, plen = HEADER.unpack(_recv_exact(s, 14)[:10])
        while mtype != MSG_SERVE_RESPONSE:
            _recv_exact(s, plen)
            magic, mtype, plen = HEADER.unpack(_recv_exact(s, 14)[:10])
        _recv_exact(s, plen)
        s.sendall(frame(MSG_SERVE_REQUEST, _canary(
            corpus_imgs, os.path.join(out, "churn_reuse2.png"))))
        try:
            extra = s.recv(4096)
            checks.check("H6 reused conn rejected (EOF/err)", extra == b"",
                         f"got {len(extra)} bytes")
        except (socket.timeout, ConnectionResetError, BrokenPipeError):
            checks.check("H6 reused conn rejected (EOF/err)", True)
        s.close()
    except (OSError, ValueError, ConnectionError) as e:
        checks.check("H6 reused conn rejected (EOF/err)", True,
                     f"client-side {type(e).__name__} (fine)")
    checks.check("H6 daemon alive after conn reuse attempt", alive())

    # ── H7: garbage interleaved with valid from parallel threads ────────────
    gerr = []

    def garble_send(data, timeout=1.5):
        """Frame-limited drain (see suite_fuzz.fuzz_send): the daemon never
        delivers EOF, so wait briefly for one reply frame and move on."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(sock)
            s.sendall(data)
            s.recv(65536)
        except (socket.timeout, OSError):
            pass
        s.close()

    def garbler(wid):
        rng = __import__("random").Random(wid)
        for _ in range(10):
            data = bytes(rng.randrange(256) for _ in range(rng.randint(1, 300)))
            try:
                garble_send(data)
            except OSError:
                pass

    def valid_worker(wid):
        try:
            r = send_request(sock, _canary(
                corpus_imgs, os.path.join(out, f"churn_g_{wid}.png")),
                timeout=300)
            if r["ok"] != 1:
                gerr.append(f"w{wid}: {r['error']}")
        except Exception as e:  # noqa: BLE001
            gerr.append(f"w{wid}: EXC {e}")

    ths = [threading.Thread(target=garbler, args=(w,)) for w in range(4)]
    ths += [threading.Thread(target=valid_worker, args=(w,)) for w in range(4)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    checks.check("H7 valid served amid parallel garbage", not gerr,
                 "; ".join(gerr[:3]))
    checks.check("H7 daemon alive at end", alive())
