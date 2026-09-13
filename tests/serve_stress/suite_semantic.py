"""Hostile-but-well-formed request values against a live daemon: every case
uses a structurally VALID frame and probes one semantic extreme — tile/overlap/
quality ranges, junk formats and model names, special files as input, hostile
output targets, encoder knob overflow, FIFO input (teardown-hang probe) and a
memory-limit OOM bomb (allocation-failure containment)."""

import os
import resource
import struct
import subprocess
import time

from PIL import Image

from lib import (Checks, Daemon, MSG_SERVE_REQUEST, frame, send_request,
                 serve_request, encoder_wire)

MODEL = b"NomosUni-SPAN-x2"


def _tiny(work, name, w, h):
    import numpy as np
    p = os.path.join(work, "corpus", name)
    if not os.path.exists(p):
        rng = np.random.default_rng(7)
        Image.fromarray(rng.integers(0, 255, (h, w, 3), dtype=np.uint8),
                        "RGB").save(p)
    return p


def _req(inp, outp, tile=0, ovl=16, q=90, fmt=b"png", model=MODEL,
         encoder=None):
    return serve_request(model, fmt, tile, ovl, q,
                         str(inp).encode() if isinstance(inp, str) else inp,
                         str(outp).encode() if isinstance(outp, str) else outp,
                         encoder=encoder)


def run(ctx, corpus_imgs, work_dir, checks):
    d = ctx.daemon
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    inp = corpus_imgs["std256.png"]

    def ask(name, payload, timeout=120):
        """Returns (state, detail): state ∈ {'ok', 'error', 'exception'}.
        'error' = clean ServeResponse with ok=0 (the good kind of refusal)."""
        try:
            r = send_request(d.sock, payload, timeout=timeout)
            return ("ok" if r["ok"] == 1 else "error"), str(r)
        except (ConnectionError, OSError, ValueError) as e:
            return "exception", f"EXC {e}"

    def survived(name):
        """Assert daemon lived; restart and return False if not."""
        if not d.alive():
            checks.check(f"survived: {name}", False, "daemon died")
            ctx.restart("semantic", name)
            return False
        return True

    # ── S1 tile/overlap/quality extremes on real work ───────────────────────
    # NOTE: overlap >= tile_size degenerates the tile grid to step=1 — on a
    # 256px image that is 65536 full-size tiles (hours of CPU, single
    # request): a genuine DoS finding, so the degenerate-overlap cases run
    # against the 8px image where even the pathological grid stays seconds.
    tiny8 = _tiny(work_dir, "tiny8x8.png", 8, 8)
    for name, img, tile, ovl, want in (
            ("tile-1-on-8px", tiny8, 1, 0, "ok"),    # 64 tiles of 1px
            ("tile-2-ovl-1", tiny8, 2, 1, "ok"),     # overlapping micro-tiles
            ("tile-65536", inp, 65536, 16, "ok"),    # whole image as one tile
            ("tile-negative", tiny8, -5, 0, "ok"),   # max(1, negative) → 1px
            ("ovl-neg", inp, 0, -99, "ok"),          # clamp → 0
            ("ovl-huge", tiny8, 0, 0x7FFFFFFF, "ok"),  # step degenerates → 1
            ("ovl-eq-tile", tiny8, 4, 4, "ok"),      # step = max(1, 0) = 1
    ):
        state, detail = ask(name, _req(img, os.path.join(
            out, f"sem_{name}.png"), tile=tile, ovl=ovl))
        checks.check(f"S1 {name}", survived(name) and state == want,
                     detail[:120])

    # quality extremes must clamp, not error
    for q in (0, 1, 100, 101, 0x7FFFFFFF):
        name = f"quality-{q}"
        state, detail = ask(name, _req(inp, os.path.join(out, "sem_q.png"),
                                       q=q))
        checks.check(f"S1 {name}", survived(name) and state == "ok",
                     detail[:120])

    # ── S2 junk formats / model names ────────────────────────────────────────
    # format expectations encode the F-H fix: unsupported values must be
    # rejected UP FRONT (clean error), never after a full inference.
    for name, fmt, want in (("fmt-PNG-upper", b"PNG", "ok"),        # → png
                            ("fmt-junk", b"\x01\x02\x03", "error"),
                            # strnlen on the wire field truncates at the
                            # first NUL, so this IS "png" — served happily
                            ("fmt-embedded-nul", b"png\0jpg\0\0\0\0", "ok"),
                            ("fmt-16x-A", b"A" * 16, "error")):
        state, detail = ask(name, _req(inp, os.path.join(out, "sem_fmt.png"),
                                       fmt=fmt))
        checks.check(f"S2 {name} ({want})", survived(name) and state == want,
                     detail[:120])
    for name, model in (("model-junk", b"\xde\xad" * 32),
                        ("model-traversal", b"../../etc/passwd"),
                        ("model-with-space", b"no such model")):
        state, detail = ask(name, _req(inp, os.path.join(out, "sem_m.png"),
                                       model=model))
        checks.check(f"S2 {name} rejected-not-crashed",
                     survived(name) and state == "error", detail[:120])
    # daemon must still serve its own model after refused switches
    state, detail = ask("canary-after-S2",
                        _req(inp, os.path.join(out, "sem_canary.png")))
    checks.check("S2 canary after refused switches",
                 survived("canary-after-S2") and state == "ok", detail[:120])

    # ── S3 special files as input ────────────────────────────────────────────
    for name, path in (("in-devnull", "/dev/null"),
                       ("in-devzero", "/dev/zero"),
                       ("in-proc-status", "/proc/self/status"),
                       ("in-directory", work_dir),
                       ("in-nonexistent", "/nonexistent/file.png")):
        state, detail = ask(name, _req(path, os.path.join(out, "sem_in.png")))
        checks.check(f"S3 {name} clean", survived(name) and state == "error",
                     detail[:120])

    # ── S4 hostile output targets ────────────────────────────────────────────
    state, detail = ask("out-devfull", _req(inp, "/dev/full"))
    checks.check("S4 out=/dev/full clean-error",
                 survived("S4-devfull") and state == "error", detail[:120])
    state, detail = ask("out-is-dir", _req(inp, out))
    # resolve_output_path rewrites extension-less paths like the CLI does
    # (…/out → …/out.png), so success here is legal IF it landed on a
    # different file and the directory itself was not clobbered.
    ok_dir = os.path.isdir(out)
    legal = state == "error" or (
        state == "ok" and ok_dir)
    checks.check("S4 out=directory never clobbers the dir", legal,
                 f"state={state} dir_intact={ok_dir}")
    ro = os.path.join(work_dir, "ro_dir")
    os.makedirs(ro, exist_ok=True)
    os.chmod(ro, 0o500)
    state, detail = ask("out-readonly-dir", _req(inp, os.path.join(ro, "o.png")))
    os.chmod(ro, 0o700)
    checks.check("S4 readonly dir clean-error",
                 survived("S4-readonly") and state == "error", detail[:120])

    # 4095-byte real path (boundary accept) vs longer path (clean error)
    deep = os.path.join(work_dir, "deep")
    os.makedirs(deep, exist_ok=True)
    cur = deep
    while len(os.path.join(cur, "in.png").encode()) < 4095 - 200:
        cur = os.path.join(cur, "d" * 200)
    os.makedirs(cur, exist_ok=True)
    boundary_in = os.path.join(cur, "in.png")
    Image.open(inp).save(boundary_in)
    checks.check("S4 boundary path prepared",
                 len(boundary_in.encode()) <= 4095,
                 f"{len(boundary_in.encode())} bytes")
    state, detail = ask("boundary-4095-path", _req(
        boundary_in, os.path.join(out, "sem_boundary.png")))
    checks.check("S4 4095-byte path served",
                 survived("S4-boundary") and state == "ok", detail[:120])
    over = boundary_in + "x" * 50
    state, detail = ask("over-max-path", _req(over, os.path.join(out, "o.png")))
    checks.check("S4 >4095-byte path clean-error",
                 survived("S4-over") and state == "error", detail[:120])

    # ── S5 encoder knob overflow ─────────────────────────────────────────────
    state, detail = ask("encoder-png-level-max", _req(
        inp, os.path.join(out, "sem_enc.png"),
        encoder=encoder_wire(png_level=0x7FFFFFFF)))
    checks.check("S5 png_level=2^31-1", survived("S5-png") and state == "ok",
                 detail[:120])
    state, detail = ask("encoder-jpeg-knobs-max", _req(
        inp, os.path.join(out, "sem_enc2"), fmt=b"jpg",
        encoder=encoder_wire(jpeg_sub=0x7FFFFFFF, webp_method=0x7FFFFFFF,
                             avif_speed=0x7FFFFFFF, avif_sub=0x7FFFFFFF,
                             avif_threads=-5)))
    checks.check("S5 jpg knobs=2^31-1", survived("S5-jpg") and state == "ok",
                 detail[:120])

    # ── S5b checksum discipline probe (soft finding) ────────────────────────
    # The wire format defines a CRC over every frame; this checks whether the
    # daemon actually enforces it. A corrupted-but-processed request is a
    # documented gap, not a crash.
    good = serve_request(MODEL, b"png", 0, 16, 90,
                         str(inp).encode(),
                         str(os.path.join(out, "sem_crc.png")).encode())
    hdr = struct.pack("<IHI", 0x53534358, MSG_SERVE_REQUEST, len(good))
    bad_crc = hdr + struct.pack("<I", 0xDEADBEEF) + good
    state, detail = ask("corrupted-crc", bad_crc)
    checks.check("S5b corrupted-CRC frame: daemon alive", survived("S5b"))
    if state == "ok":
        checks.info("FINDING: header checksum is never verified — corrupted "
                    "frames are processed (design decision recorded)")
    else:
        checks.info(f"corrupted-CRC frame rejected (state={state})")

    # ── S6 FIFO input: inference isolation + graceful-teardown probe ────────
    fifo = os.path.join(work_dir, "in.fifo")
    try:
        os.mkfifo(fifo)
    except FileExistsError:
        pass
    import socket as pysock
    s = pysock.socket(pysock.AF_UNIX, pysock.SOCK_STREAM)
    s.settimeout(10)
    s.connect(d.sock)
    s.sendall(frame(MSG_SERVE_REQUEST, _req(
        fifo, os.path.join(out, "sem_fifo.png"))))
    s.close()
    # no writer will ever open the FIFO — the daemon thread blocks in
    # ifstream open(). Meanwhile the engine must stay usable (no lock held).
    state, detail = ask("fifo-canary", _req(
        inp, os.path.join(out, "sem_fifo_canary.png")), timeout=120)
    checks.check("S6 canary served while FIFO stuck",
                 survived("S6-canary") and state == "ok", detail[:120])
    # graceful stop must not hang on the stuck thread (watch-dog 30s)
    t0 = time.time()
    try:
        subprocess.run([ctx.binary, "daemon", "stop"], env=d.env(),
                       capture_output=True, timeout=30)
        stopped = not d.alive()
    except subprocess.TimeoutExpired:
        stopped = False
    dt = time.time() - t0
    checks.check("S6 daemon stop within 30s despite FIFO thread", stopped,
                 f"stop took {dt:.0f}s, alive={d.alive()}")
    if not stopped:
        checks.info("FINDING: FIFO thread hangs SIGTERM teardown — "
                    "SIGKILL fallback engaged")
        try:
            d.proc.kill()
        except OSError:
            pass
    d.cleanup_files()
    ctx.daemon = None  # stopped either way; next suite starts a fresh one

    # ── S7 OOM containment (CPU only): rlimit-capped daemon vs big image ────
    if ctx.device == "cpu":
        big = os.path.join(work_dir, "corpus", "bomb6000.png")
        if not os.path.exists(big):
            import numpy as np
            rng = np.random.default_rng(9)
            arr = rng.integers(0, 255, (6000, 6000, 3), dtype=np.uint8)
            Image.fromarray(arr, "RGB").save(big, compress_level=6)
        d2 = Daemon(ctx.binary, work_dir, ctx.models_dir, ctx.model,
                    device="cpu", log_name="stress_oom.log")

        def cap_mem():  # runs in the forked child before exec
            resource.setrlimit(resource.RLIMIT_AS, (2048 * 1024 * 1024,) * 2)
        os.makedirs(os.path.dirname(d2.sock), exist_ok=True)
        cmd = [ctx.binary, "daemon", "--models-dir", ctx.models_dir,
               "--model", ctx.model, "--idle-timeout", "0", "--fg", "--cpu"]
        with open(d2.log_path, "ab") as log:
            d2.proc = subprocess.Popen(cmd, stdout=log,
                                       stderr=subprocess.STDOUT,
                                       env=d2.env(), preexec_fn=cap_mem)
        if not d2.wait_ready(120):
            checks.check("S7 rlimit daemon up", False,
                         "not ready — see stress_oom.log")
            try:
                d2.proc.kill()
            except OSError:
                pass
        else:
            # small work first: daemon must serve fine under the cap
            state, detail = ask("oom-canary", _req(
                inp, os.path.join(out, "oom_canary.png")), timeout=120)
            checks.check("S7 small request under 2.5GB cap", state == "ok",
                         detail[:120])
            # the bomb: 6000² in → 12000² out under the cap. Contained means a
            # clean error response; losing the daemon is the finding.
            state, detail = ask("oom-bomb", _req(
                big, os.path.join(out, "oom_bomb.png")), timeout=300)
            checks.check("S7 OOM bomb contained",
                         state in ("ok", "error"),
                         f"state={state} detail={detail[:120]}")
            checks.check("S7 daemon alive after bomb", d2.alive(),
                         "daemon died under memory pressure")
            try:
                d2.proc.kill()
            except OSError:
                pass
            d2.cleanup_files()
        # control: the same bomb on an uncapped daemon must SUCCEED — proves
        # the request is legitimate, so a death under the cap is an
        # allocation-failure containment gap, not a malformed request.
        ctx.start_daemon("stress_shared_post_oom.log")
        state, detail = ask("oom-control", _req(
            big, os.path.join(out, "oom_control.png")), timeout=600)
        checks.check("S7 uncapped control serves the bomb",
                     state == "ok", detail[:120])