"""Input/output abuse suite: corrupt decoders, dimension edges, hostile
output paths, SR-marker idempotency, batch resilience, format matrix.
Runs against a live daemon (dimension/format cases) and the inline CLI."""

import os
import subprocess
import time

import numpy as np
from PIL import Image

from lib import Checks, send_request, serve_request

MODEL_NAME = b"NomosUni-SPAN-x2"


def run(daemon, corpus, work_dir, checks: Checks, model):
    binary = daemon.binary
    a = os.path.join(work_dir, "abuse")
    os.makedirs(a, exist_ok=True)
    inp_png = corpus["std256.png"]
    raw_png = open(inp_png, "rb").read()

    def cli(*args_, timeout=300):
        return subprocess.run([binary, *args_], env=daemon.env(),
                              capture_output=True, text=True, timeout=timeout)

    # ── A1: corrupt inputs → rc=1, clean error, no output, no crash ─────────
    corrupt = {
        "truncated-png": raw_png[:len(raw_png) // 2],
        "fake-jpeg": b"\xff\xd8\xff\xe0" + b"\0" * 100,
        "random-bytes": os.urandom(4096),
    }
    for name, data in corrupt.items():
        p = os.path.join(a, f"in_{name}")
        open(p, "wb").write(data)
        r = cli(p, os.path.join(a, f"out_{name}.png"), "--model", model,
                "--no-daemon")
        checks.check(f"A1 corrupt {name}", r.returncode == 1, f"rc={r.returncode}")
        checks.check(f"A1 corrupt {name}: no output",
                     not os.path.exists(os.path.join(a, f"out_{name}.png")))

    # ── A2: dimension edges through the daemon (scale from model = x2) ─────
    for name in ("one1x1.png", "wide300x1.png", "tall1x300.png",
                 "odd201x199.png", "gray256.png"):
        src = corpus[name]
        w, h = Image.open(src).size
        try:
            r = send_request(daemon.sock, serve_request(
                MODEL_NAME, b"png", 0, 16, 90, str(src).encode(),
                os.path.join(a, f"dim_{name}").encode()), timeout=300)
            checks.check(f"A2 {name}",
                         r["ok"] == 1 and r["w"] == w * 2 and r["h"] == h * 2,
                         f"want {w*2}x{h*2}, got {r}")
        except (ConnectionError, OSError, ValueError) as e:
            checks.check(f"A2 {name}", False, str(e))

    # EXIF-rotated JPEG: daemon bakes it upright (portrait out of landscape)
    src = corpus["exif_rotated.jpg"]
    w, h = Image.open(src).size
    try:
        r = send_request(daemon.sock, serve_request(
            MODEL_NAME, b"png", 0, 16, 90, str(src).encode(),
            os.path.join(a, "dim_exif.png").encode()), timeout=300)
        checks.check("A2 exif-rotated upright",
                     r["ok"] == 1 and r["w"] == h * 2 and r["h"] == w * 2,
                     f"want {h*2}x{w*2}, got {r}")
    except (ConnectionError, OSError, ValueError) as e:
        checks.check("A2 exif-rotated upright", False, str(e))

    # ── A3: hostile output paths (inline) ───────────────────────────────────
    nested = os.path.join(a, "deep", "deeper", "dir", "out.png")
    r = cli(inp_png, nested, "--model", model, "--no-daemon")
    checks.check("A3 nested output dir auto-created",
                 r.returncode == 0 and os.path.exists(nested),
                 f"rc={r.returncode}")

    r = cli(inp_png, a, "--model", model, "--no-daemon")
    checks.check("A3 output-is-directory rejected", r.returncode == 1,
                 f"rc={r.returncode}")

    ro = os.path.join(a, "readonly")
    os.makedirs(ro, exist_ok=True)
    os.chmod(ro, 0o500)
    r = cli(inp_png, os.path.join(ro, "out.png"), "--model", model,
            "--no-daemon")
    os.chmod(ro, 0o700)
    checks.check("A3 read-only dir rejected cleanly", r.returncode == 1,
                 f"rc={r.returncode}")

    weird_dir = os.path.join(a, "wei rd 'q\"uote 中文")
    os.makedirs(weird_dir, exist_ok=True)
    r = cli(inp_png, os.path.join(weird_dir, "o ut #1.png"), "--model", model,
            "--no-daemon")
    checks.check("A3 spaces/quotes/unicode path",
                 r.returncode == 0 and
                 os.path.exists(os.path.join(weird_dir, "o ut #1.png")),
                 f"rc={r.returncode}")

    # ── A4: SR-marker idempotency (end-to-end skip, not covered by unit test)
    marked = os.path.join(a, "marked.png")
    r = cli(inp_png, marked, "--model", model, "--no-daemon")
    checks.check("A4 produce marked output",
                 r.returncode == 0 and os.path.exists(marked),
                 f"rc={r.returncode}")
    t0 = time.time()
    r = cli(marked, os.path.join(a, "refeed.png"), "--model", model,
            "--no-daemon")
    dt = time.time() - t0
    checks.check("A4 refeed skipped fast", r.returncode == 0 and dt < 10,
                 f"rc={r.returncode} {dt:.1f}s")
    r = cli(marked, os.path.join(a, "refeed_forced.png"), "--model", model,
            "--no-daemon", "--force")
    checks.check("A4 --force processes", r.returncode == 0 and
                 os.path.exists(os.path.join(a, "refeed_forced.png")),
                 f"rc={r.returncode}")

    # ── A5: batch dir with a corrupt member keeps going ─────────────────────
    batch_in = os.path.join(a, "batch_in")
    os.makedirs(batch_in, exist_ok=True)
    for good in ("std256.png", "odd201x199.png"):
        open(os.path.join(batch_in, good), "wb").write(
            open(corpus[good], "rb").read())
    open(os.path.join(batch_in, "corrupt.png"), "wb").write(raw_png[:100])
    open(os.path.join(batch_in, "notimage.txt"), "wb").write(b"nope")
    batch_out = os.path.join(a, "batch_out")
    cli(batch_in, batch_out, "--model", model, "--no-daemon", timeout=600)
    n_out = len(os.listdir(batch_out)) if os.path.isdir(batch_out) else 0
    checks.check("A5 batch handles good files, survives corrupt", n_out >= 2,
                 f"{n_out} outputs")

    # ── A6: output format matrix through the daemon ─────────────────────────
    caps = subprocess.run([binary, "--version"], env=daemon.env(),
                          capture_output=True, text=True).stdout
    formats = ["png", "jpg", "webp", "avif"]
    for fmt in formats:
        if fmt == "avif" and "+avif-encoder" not in caps:
            checks.info(f"A6 avif skipped (no linked encoder): {caps.strip()}")
            continue
        opportunistic = fmt == "avif" and "+avif-encoder" not in caps
        try:
            r = send_request(daemon.sock, serve_request(
                MODEL_NAME, fmt.encode(), 0, 16, 90, str(inp_png).encode(),
                os.path.join(a, f"fmt_{fmt}").encode()), timeout=600)
            if r["ok"] != 1:
                if opportunistic:
                    checks.info(f"A6 avif unavailable at runtime, skipped: "
                                f"{r['error'][:80]}")
                else:
                    checks.check(f"A6 format {fmt}", False, r["error"])
                continue
            im = Image.open(r["path"])
            im.load()
            checks.check(f"A6 format {fmt} decodes at 512 wide",
                         im.size[0] == 512, str(im.size))
        except (ConnectionError, OSError, ValueError) as e:
            checks.check(f"A6 format {fmt}", False, str(e))

    # ── A7: RGBA input keeps its alpha through the daemon ───────────────────
    # The daemon decodes and forwards the image with its own channel count (4
    # here) while the loaded model is RGB: the alpha must be split off before
    # inference (a 3-channel net never emits a 4th channel), the result must
    # still be a full-size 4-channel image, and alpha must keep its endpoints —
    # neither dropped (→ RGB out) nor forced opaque.
    rgba_src = corpus["rgba_alpha.png"]
    in_alpha = np.asarray(Image.open(rgba_src))
    w, h = in_alpha.shape[1], in_alpha.shape[0]
    blk = max(16, min(w, h) // 6)
    out_rgba = os.path.join(a, "rgba_out.png")
    try:
        # tile=64 forces a multi-tile grid on 200x150, the shape that exposed
        # the shape-mismatch abort in the first place.
        r = send_request(daemon.sock, serve_request(
            MODEL_NAME, b"png", 64, 16, 90, str(rgba_src).encode(),
            out_rgba.encode()), timeout=600)
        ok = r["ok"] == 1
        checks.check("A7 rgba upscaled",
                     ok and r["w"] == w * 2 and r["h"] == h * 2,
                     f"want {w*2}x{h*2}, got {r}")
        checks.check("A7 rgba input really has alpha",
                     in_alpha.ndim == 3 and in_alpha.shape[2] == 4 and
                     in_alpha[:, :, 3].min() == 0 and
                     in_alpha[:, :, 3].max() == 255,
                     f"input shape {in_alpha.shape}")
        if ok:
            im = Image.open(out_rgba)
            checks.check("A7 output stays 4-channel", im.mode == "RGBA", im.mode)
            alpha = np.asarray(im.convert("RGBA"))[:, :, 3]
            checks.check("A7 alpha endpoints kept",
                         int(alpha.min()) == 0 and int(alpha.max()) == 255,
                         f"min={int(alpha.min())} max={int(alpha.max())}")
            checks.check("A7 alpha not flattened",
                         int(alpha.min()) != int(alpha.max()) and
                         int(((alpha > 8) & (alpha < 247)).sum()) > 0,
                         f"{len(np.unique(alpha))} distinct values")
            # Flat fully-transparent region: a flat input survives a normalized
            # bicubic resample exactly, two source pixels inside the block edge.
            checks.check("A7 transparent block preserved",
                         int(alpha[blk // 2, blk // 2]) == 0,
                         f"center={int(alpha[blk // 2, blk // 2])}")
            checks.check("A7 opaque block preserved",
                         int(alpha[2 * h - blk // 2, 2 * w - blk // 2]) == 255,
                         f"center={int(alpha[2 * h - blk // 2, 2 * w - blk // 2])}")
    except (ConnectionError, OSError, ValueError) as e:
        checks.check("A7 rgba upscaled", False, str(e))

    # The inline path normalizes model channels before tiling; it must retain
    # the same alpha as the daemon instead of compositing the image to RGB.
    inline_rgba = os.path.join(a, "rgba_inline.png")
    r = cli(str(rgba_src), inline_rgba, "--model", model,
            "--models", daemon.models_dir, "--no-daemon", "--tile-size", "64",
            *(["--cpu"] if daemon.device == "cpu" else []))
    checks.check("A7 inline rgba upscaled", r.returncode == 0,
                 f"rc={r.returncode}: {r.stderr[-500:]}")
    if r.returncode == 0:
        with Image.open(inline_rgba) as im:
            checks.check("A7 inline rgba shape",
                         im.mode == "RGBA" and im.size == (w * 2, h * 2),
                         f"mode={im.mode} size={im.size}")
            inline_alpha = np.asarray(im.convert("RGBA"))[:, :, 3]
        if os.path.exists(out_rgba):
            with Image.open(out_rgba) as im:
                daemon_alpha = np.asarray(im.convert("RGBA"))[:, :, 3]
            checks.check("A7 inline and daemon preserve the same alpha",
                         np.array_equal(inline_alpha, daemon_alpha))

    # The same RGB pixels must survive the alpha bypass. Comparing against the
    # RGB fast path catches black/partially initialized RGB with valid alpha.
    rgb_source = os.path.join(a, "rgba_colors_rgb.png")
    with Image.open(rgba_src) as im:
        im.convert("RGB").save(rgb_source)
    rgb_out = os.path.join(a, "rgb_channels.png")
    try:
        r = send_request(daemon.sock, serve_request(
            MODEL_NAME, b"png", 64, 16, 90, str(rgb_source).encode(),
            rgb_out.encode()), timeout=300)
        mode = Image.open(rgb_out).mode if r["ok"] == 1 else "n/a"
        checks.check("A7 rgb output stays 3-channel",
                     r["ok"] == 1 and mode == "RGB", f"mode={mode}")
        if r["ok"] == 1 and os.path.exists(out_rgba):
            with Image.open(out_rgba) as im:
                rgba_colors = np.asarray(im.convert("RGB"), dtype=np.float32)
            with Image.open(rgb_out) as im:
                rgb_colors = np.asarray(im, dtype=np.float32)
            mse = float(np.mean((rgba_colors - rgb_colors) ** 2))
            psnr = float("inf") if mse == 0 else 10 * np.log10(255 ** 2 / mse)
            maxdiff = float(np.abs(rgba_colors - rgb_colors).max())
            checks.check("A7 rgba colors match rgb inference",
                         psnr >= 24 and maxdiff <= 96,
                         f"PSNR={psnr:.1f}dB maxdiff={maxdiff:.0f}")
    except (ConnectionError, OSError, ValueError) as e:
        checks.check("A7 rgb output stays 3-channel", False, str(e))
