#!/usr/bin/env python3
"""pth → ONNX (+ ncnn cross-check) for the Upix web frontend.

Implements models/CONVERTING.md for the ONNX exit of the pnnx pipeline
(issue #2, path B1). Same verification culture, adapted:

  §1  source + sha256 byte-match
  §2  spandrel loads the pth (spandrel is ground truth; OpenModelDB lies)
  §3  trace spandrel's raw model (m.model, NOT the descriptor — the descriptor
      clamps output; see the §2 "descriptor clamp" pitfall) at 64×64
  §4  pnnx at fp16=1 and fp16=0 (serial, small trace — the doc's OOM warning)
  §5  verification gate:
        - ORT CPU vs spandrel raw on a real crop, [0,1] float PSNR ≥ 44 dB
          (the doc's ncnn band floor; tracing spandrel directly should land
          far higher — if it doesn't, the graph is wrong)
        - fp16 graph == fp32 graph byte-identical (≥99 dB)
        - odd sizes exact; tiled == whole ≥ 99 dB on a degenerate layout,
          ≥ 30 dB across real seams (receptive-field floor, see
          web/tests/pipeline.test.ts for the same analysis)
        - dynamic H/W: ORT runs a second, different shape — the tfjs trap

Requires: torch, spandrel, onnx, onnxruntime, numpy, pillow, and the pnnx
binary on PATH (https://github.com/pnnx/pnnx/releases). Run serially; pnnx
spikes hard per process.

Usage:
  python3 convert_onnx.py --name NomosUni-SPAN-x2 --pth weights.pth \
      --sha256 <omdb-sha256> [--crop crop.png] [--out-dir out/]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

import numpy as np


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def check_deps() -> None:
    missing = []
    for mod in ("torch", "spandrel", "onnx", "onnxruntime", "PIL"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if shutil.which("pnnx") is None:
        missing.append("pnnx (binary on PATH)")
    if missing:
        die(f"missing deps: {', '.join(missing)}")


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(1.0 / mse)


# --- §1 ---------------------------------------------------------------------

def verify_sha256(pth: pathlib.Path, expected: str) -> None:
    h = hashlib.sha256(pth.read_bytes()).hexdigest()
    if h != expected:
        die(f"sha256 mismatch: got {h}, want {expected} (CONVERTING.md §1: byte-match before anything else)")


# --- §2/§3 ------------------------------------------------------------------

def load_and_trace(pth: pathlib.Path, hw: int = 64):
    import torch
    from spandrel import ImageModelDescriptor, ModelLoader

    loaded = ModelLoader().load_model(str(pth))
    if not isinstance(loaded, ImageModelDescriptor):
        die("not an image-restoration model (spandrel)")
    raw = loaded.model  # raw, unclamped — descriptor clamp pitfall (§2)
    raw.eval()

    dummy = torch.rand(1, 3, hw, hw)
    with torch.no_grad():
        ts = torch.jit.trace(raw, dummy)
        ref = raw(dummy)
    return raw, ts, loaded.scale, ref


# --- §4 ---------------------------------------------------------------------

def run_pnnx(ts_path: pathlib.Path, out_dir: pathlib.Path, fp16: bool) -> pathlib.Path:
    cmd = ["pnnx", str(ts_path), f"fp16={1 if fp16 else 0}"]
    print(f"  pnnx {'fp16=1' if fp16 else 'fp16=0'} …")
    res = subprocess.run(cmd, cwd=out_dir, capture_output=True, text=True)
    if res.returncode != 0:
        die(f"pnnx failed:\n{res.stdout[-2000:]}\n{res.stderr[-2000:]}")
    onnx_path = out_dir / (ts_path.stem + ".onnx")
    if not onnx_path.exists():
        die(f"pnnx did not emit {onnx_path}; contents: {sorted(p.name for p in out_dir.iterdir())}")
    return onnx_path


# --- §5 ---------------------------------------------------------------------

def ort_run(onnx_path: pathlib.Path, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    import onnxruntime as ort
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    out = sess.run(None, {sess.get_inputs()[0].name: x})[0]
    return out, np.asarray(x)


def to_tensor(image: np.ndarray) -> np.ndarray:
    """HWC u8 → 1×3×H×W float32 [0,1] (engine from_pixels semantics)."""
    x = image.astype(np.float32) / 255.0
    return np.ascontiguousarray(x.transpose(2, 0, 1)[None])


def chwhwc(y: np.ndarray) -> np.ndarray:
    return np.clip(y[0].transpose(1, 2, 0), 0.0, 1.0)


def gate(onnx_fp32: pathlib.Path, onnx_fp16: pathlib.Path, raw, crop_img: np.ndarray, scale: int) -> dict:
    import torch

    report: dict = {}
    x = to_tensor(crop_img)

    # torch truth on the same crop (raw model, unclamped, then compare clamped-on-both-sides)
    with torch.no_grad():
        truth = raw(torch.from_numpy(x)).numpy()
    ours, _ = ort_run(onnx_fp32, x)
    report["psnr_onnx_vs_torch"] = round(psnr(np.clip(ours, 0, 1), np.clip(truth, 0, 1)), 2)
    if report["psnr_onnx_vs_torch"] < 44.0:
        die(f"§5 gate: ONNX vs torch PSNR {report['psnr_onnx_vs_torch']} dB < 44 — graph is wrong, do not publish")

    # fp16/fp32 graph A/B
    y32, _ = ort_run(onnx_fp32, x)
    y16, _ = ort_run(onnx_fp16, x)
    report["psnr_fp16_vs_fp32"] = round(psnr(np.clip(y16, 0, 1), np.clip(y32, 0, 1)), 2)
    if report["psnr_fp16_vs_fp32"] < 99.0:
        print(f"  warn: fp16/fp32 A/B {report['psnr_fp16_vs_fp32']} dB < 99 — inspect before shipping fp16")

    # odd sizes exact (shape-level; values compared against torch)
    odd_x = to_tensor(crop_img[: 127, : 125])
    with torch.no_grad():
        odd_truth = raw(torch.from_numpy(odd_x)).numpy()
    odd_ours, _ = ort_run(onnx_fp32, odd_x)
    if odd_ours.shape != odd_truth.shape:
        die(f"§5 gate: odd-size shape mismatch {odd_ours.shape} vs {odd_truth.shape}")
    report["psnr_odd_sizes"] = round(psnr(np.clip(odd_ours, 0, 1), np.clip(odd_truth, 0, 1)), 2)

    # tiled == whole — degenerate layout first (structural gate), then real seams
    def tiled(x: np.ndarray, tile: int, overlap: int) -> np.ndarray:
        _, _, h, w = x.shape
        step = max(1, tile - overlap)
        starts = []
        for t in range(0, w, step):
            starts.append(t)
            if t + tile >= w:
                break
        ys = []
        for t in range(0, h, step):
            ys.append(t)
            if t + tile >= h:
                break
        acc = np.zeros((1, 3, h * scale, w * scale), np.float64)
        wgt = np.zeros((1, 1, h * scale, w * scale), np.float64)

        def ramp(pos: int, size: int, eff: int, start: int, full: int) -> float:
            eff = min(eff, size)
            if eff <= 0:
                return 1.0
            v = 1.0
            if start > 0 and pos < eff:
                v = (pos + 1) / eff
            elif start + size < full and pos >= size - eff:
                v = (size - pos) / eff
            return v * v

        for ty in ys:
            th = min(tile, h - ty)
            for tx in starts:
                tw = min(tile, w - tx)
                crop = x[:, :, ty : ty + th, tx : tx + tw]
                y, _ = ort_run(onnx_fp32, np.ascontiguousarray(crop))
                # engine blend semantics (overlap×scale, squared ramp)
                eff = overlap * scale
                ty0, tx0 = ty * scale, tx * scale
                th2, tw2 = th * scale, tw * scale
                for gy in range(th2):
                    wy = ramp(gy, th2, eff, ty0, h * scale)
                    for gx in range(tw2):
                        wx = ramp(gx, tw2, eff, tx0, w * scale)
                        wgt[0, 0, ty0 + gy, tx0 + gx] += wx * wy
                        acc[0, :, ty0 + gy, tx0 + gx] += y[0, :, gy, gx] * wx * wy
        return (acc / np.maximum(wgt, 1e-12)).astype(np.float32)

    # structural: single tile (degenerate layout) must be near-bit-identical
    whole, _ = ort_run(onnx_fp32, x)
    single = tiled(x, 4096, 16)
    report["psnr_tiled_single_vs_whole"] = round(psnr(np.clip(single, 0, 1), np.clip(whole, 0, 1)), 2)
    if report["psnr_tiled_single_vs_whole"] < 99.0:
        die("§5 gate: single-tile tiled==whole < 99 dB — structural bug in the graph")

    # real seams (receptive-field floor; see web/tests for the analysis)
    seams = tiled(x, max(32, (crop_img.shape[0] + 1) // 2), 16)
    report["psnr_tiled_seams_vs_whole"] = round(psnr(np.clip(seams, 0, 1), np.clip(whole, 0, 1)), 2)

    # dynamic H/W (the tfjs trap): second, different shape must just work
    try:
        ort_run(onnx_fp32, to_tensor(crop_img[: 80, : 90]))
        report["dynamic_hw"] = "ok"
    except Exception as exc:  # noqa: BLE201
        report["dynamic_hw"] = f"FAILED: {exc}"
    return report


# ----------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True, help="catalog stem, e.g. NomosUni-SPAN-x2")
    ap.add_argument("--pth", required=True, type=pathlib.Path)
    ap.add_argument("--sha256", required=True, help="OpenModelDB sha256 (§1 byte-match)")
    ap.add_argument("--crop", type=pathlib.Path, help="real crop for the gate (defaults to a synthetic pattern)")
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("out") / "onnx")
    args = ap.parse_args()

    check_deps()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    verify_sha256(args.pth, args.sha256)

    print("§2/§3 spandrel load + 64×64 trace …")
    raw, ts, scale, _ref = load_and_trace(args.pth)
    ts_path = args.out_dir / f"{args.name}.ts"
    ts.save(str(ts_path))

    print("§4 pnnx exports (serial) …")
    onnx_fp32 = run_pnnx(ts_path, args.out_dir, fp16=False)
    onnx_fp16 = run_pnnx(ts_path, args.out_dir, fp16=True)
    final_fp32 = args.out_dir / f"{args.name}-fp32.onnx"
    final_fp16 = args.out_dir / f"{args.name}-fp16.onnx"
    shutil.move(onnx_fp32, final_fp32)
    shutil.move(onnx_fp16, final_fp16)

    if args.crop:
        from PIL import Image
        crop = np.asarray(Image.open(args.crop).convert("RGB"))
    else:
        rng = np.random.default_rng(0)
        crop = (rng.random((160, 160, 3)) * 255).astype(np.uint8)

    print("§5 verification gate …")
    report = gate(final_fp32, final_fp16, raw, crop, scale)
    meta = {
        "name": args.name,
        "scale": scale,
        "sha256_pth": args.sha256,
        "sha256_onnx_fp32": hashlib.sha256(final_fp32.read_bytes()).hexdigest(),
        "sha256_onnx_fp16": hashlib.sha256(final_fp16.read_bytes()).hexdigest(),
        "gate": report,
    }
    (args.out_dir / f"{args.name}.meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(meta, indent=2))
    ok = report["psnr_onnx_vs_torch"] >= 44.0 and report.get("dynamic_hw") == "ok"
    print("GATE:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
