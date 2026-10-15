"""Model-effect regression oracle (CPU, golden images).

Runs the real single-image path of a built upix-cli over a fixed set of catalog
models and compares every output against a committed golden PNG:

  golden drift   PSNR >= cases.MIN_PSNR and maxdiff <= cases.MAX_DIFF
                 (bit-identical on one machine+build; the tolerance covers CPU
                 ISA/ncnn scheduling drift — see docs/model-oracle-design.md)
  sanity         exact output dimensions, std >= 2, mean inside the plausible
                 range (catches black/constant/dead outputs without a golden)
  provenance     the generated input and the model files still hash to what
                 golden/meta.json recorded

This is the gate for engine-side changes (ncnn bump, custom layer, model_graph
analysis): it answers "do the pixels still look like the same model?".
`models/CONVERTING.md` answers the other question, "is the model itself still
faithful to its pth?", at conversion time.

Requires: python3 + numpy + Pillow, a built upix-cli, the case models in
--models-dir (upix-cli model download <name>).

Usage:
  python3 tests/model_oracle/run.py [--binary PATH] [--models-dir DIR]
      [--case NAME]... [--require-models] [--update-golden] [--keep-work]

  --require-models  a missing model is a failure (CI); default: skip with a note
  --update-golden   rewrite golden/<case>.png + meta.json after printing the
                    drift against the previous goldens (review it!)
Exit code 0 = all green.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cases import (CASES, GOLDEN_DIR, INPUT_SIZE, MAX_DIFF, MEAN_RANGE,  # noqa: E402
                   META_PATH, MIN_PSNR, MIN_STD, TILE_OVERLAP, TILE_SIZE,
                   input_sha256, make_input)

DEFAULT_MODELS_DIR = os.path.expanduser("~/.local/share/upix/models")


class Checks:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, name, ok, detail="", fail_detail=None):
        if ok:
            self.passed += 1
            print(f"ok   {name}" + (f" — {detail}" if detail else ""))
        else:
            # fail_detail exists for details that are instructions ("run with
            # --update-golden"): printing those next to `ok` reads as if the
            # golden were still missing.
            hint = detail if fail_detail is None else fail_detail
            self.failed.append((name, hint))
            print(f"FAIL {name}" + (f" — {hint}" if hint else ""))
        return ok

    def summary(self):
        print()
        if self.failed:
            print(f"{len(self.failed)} failure(s), {self.passed} check(s) passed")
            for name, detail in self.failed:
                print(f"  FAIL {name}" + (f" — {detail}" if detail else ""))
            return 1
        print(f"all green: {self.passed} check(s)")
        return 0


def default_binary():
    plat = {"darwin": "macosx"}.get(os.uname().sysname.lower(), os.uname().sysname.lower())
    arch = os.uname().machine
    root = Path(__file__).resolve().parent.parent.parent
    for mode in ("release", "debug"):
        candidate = root / "build" / plat / arch / mode / "upix-cli"
        if candidate.exists():
            return str(candidate)
    return ""


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def model_sha256(models_dir, model):
    """Hash of the .param + .bin pair, so a re-released model is noticed."""
    digest = hashlib.sha256()
    for suffix in (".param", ".bin"):
        path = Path(models_dir) / f"{model}{suffix}"
        digest.update(suffix.encode())
        digest.update(Path(path).read_bytes())
    return digest.hexdigest()


def load_pixels(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def compare(a, b):
    """(psnr, maxdiff) between two u8-ish float arrays."""
    if a.shape != b.shape:
        return float("nan"), float("nan")
    diff = np.abs(a - b)
    mse = float(np.mean(diff ** 2))
    return (float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)), float(diff.max())


def fmt(value, unit=""):
    return "inf" if value == float("inf") else f"{value:.2f}{unit}"


def write_diff_image(path, a, b):
    Image.fromarray(np.clip(np.abs(a - b) * 8.0, 0, 255).astype(np.uint8)).save(path)


def run_cli(binary, models_dir, model, src, dst):
    cmd = [binary, str(src), str(dst), "-m", model, "--models", str(models_dir),
           "--cpu", "--no-daemon", "--tile-size", str(TILE_SIZE),
           "--tile-overlap", str(TILE_OVERLAP)]
    start = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        tail = (proc.stdout + proc.stderr).strip().splitlines()[-4:]
        return None, time.time() - start, " | ".join(tail)
    if not Path(dst).exists():
        return None, time.time() - start, "no output file written"
    return load_pixels(dst), time.time() - start, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", default=default_binary(),
                        help="upix-cli path (auto: build/<plat>/<arch>/release)")
    parser.add_argument("--models-dir", default=DEFAULT_MODELS_DIR)
    parser.add_argument("--case", action="append", default=None,
                        help="restrict to this case name (repeatable)")
    parser.add_argument("--require-models", action="store_true",
                        help="fail when a case model is missing (CI)")
    parser.add_argument("--update-golden", action="store_true",
                        help="rewrite golden/<case>.png + meta.json (review the drift first)")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep the temp work dir even when green")
    args = parser.parse_args()

    checks = Checks()
    if not checks.check("upix-cli binary", os.access(args.binary, os.X_OK), args.binary):
        return checks.summary()

    selected = [case for case in CASES if not args.case or case[0] in args.case]
    if not checks.check("cases selected", bool(selected),
                        ",".join(case[0] for case in selected)):
        return checks.summary()

    work = Path(tempfile.mkdtemp(prefix="upix-oracle-"))
    src = work / f"input{INPUT_SIZE}.png"
    Image.fromarray(make_input(INPUT_SIZE)).save(src, format="PNG", optimize=False)
    src_hash = input_sha256(src)
    print(f"input: {src} sha256={src_hash[:16]}…")

    meta = json.loads(META_PATH.read_text()) if META_PATH.exists() else {}
    if meta and not args.update_golden:
        checks.check("goldens use CPU", meta.get("device") == "cpu",
                     fail_detail="goldens must be recorded with the explicit --cpu path")
        checks.check("input matches goldens", meta.get("input", {}).get("sha256") == src_hash,
                     fail_detail="regenerate with --update-golden if the input was changed on purpose")

    golden = {}
    for name, model, scale, _why in selected:
        param = Path(args.models_dir) / f"{model}.param"
        binfile = Path(args.models_dir) / f"{model}.bin"
        if not (param.exists() and binfile.exists()):
            if args.require_models:
                checks.check(f"{name}: model present", False,
                             f"{model} missing under {args.models_dir}")
            else:
                print(f"skip {name}: {model} not in {args.models_dir}")
            continue

        out = work / f"{name}.png"
        pixels, seconds, error = run_cli(args.binary, args.models_dir, model, src, out)
        if not checks.check(f"{name}: upix-cli ran", pixels is not None, error):
            continue

        expected = (INPUT_SIZE * scale, INPUT_SIZE * scale)
        size_ok = pixels.shape[0] == expected[1] and pixels.shape[1] == expected[0]
        checks.check(f"{name}: output size", size_ok, f"{pixels.shape[1]}x{pixels.shape[0]} want "
                                                        f"{expected[0]}x{expected[1]}")
        std = float(pixels.std())
        mean = float(pixels.mean())
        sane = std >= MIN_STD and MEAN_RANGE[0] <= mean <= MEAN_RANGE[1]
        checks.check(f"{name}: output not degenerate", sane,
                     f"std={std:.2f} mean={mean:.2f}")
        if not (size_ok and sane):
            continue

        entry = {"model": model, "scale": scale, "tile": TILE_SIZE,
                 "overlap": TILE_OVERLAP, "model_sha256": model_sha256(args.models_dir, model)}
        golden_path = GOLDEN_DIR / f"{name}.png"

        if args.update_golden:
            if golden_path.exists():
                prev = load_pixels(golden_path)
                psnr, maxdiff = compare(prev, pixels)
                print(f"     {name}: vs previous golden PSNR={fmt(psnr, ' dB')} "
                      f"maxdiff={maxdiff:.0f}")
                entry["psnr_vs_previous"] = None if psnr == float("inf") else round(psnr, 2)
                entry["maxdiff_vs_previous"] = maxdiff
            GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
            Image.fromarray(pixels.astype(np.uint8)).save(golden_path, format="PNG", optimize=True)
            entry["golden_sha256"] = sha256_file(golden_path)
            golden[name] = entry
            print(f"     {name}: golden updated ({seconds:.2f}s)")
            continue

        if not checks.check(f"{name}: golden exists", golden_path.exists(),
                            fail_detail="run with --update-golden to create it"):
            continue
        recorded = meta.get("cases", {}).get(name, {})
        if not checks.check(f"{name}: model unchanged since golden",
                            recorded.get("model_sha256") == entry["model_sha256"],
                            fail_detail="the .param/.bin must match golden/meta.json"):
            continue
        if not checks.check(f"{name}: golden unchanged since recording",
                            recorded.get("golden_sha256") == sha256_file(golden_path),
                            fail_detail="the golden PNG must match golden/meta.json"):
            continue
        ref = load_pixels(golden_path)
        psnr, maxdiff = compare(ref, pixels)
        drift_ok = psnr >= MIN_PSNR and maxdiff <= MAX_DIFF
        detail = f"PSNR={fmt(psnr, ' dB')} maxdiff={maxdiff:.0f} (min {MIN_PSNR:.0f} dB / {MAX_DIFF:.0f})"
        if not drift_ok:
            if ref.shape == pixels.shape:
                diff_path = work / f"{name}_diff.png"
                write_diff_image(diff_path, ref, pixels)
                detail += f"; diff x8: {diff_path} (kept {work})"
            else:
                detail += f"; golden shape {ref.shape} != output shape {pixels.shape}"
        checks.check(f"{name}: matches golden [{model}]", drift_ok, f"{detail}; {seconds:.2f}s")

    if args.update_golden and golden:
        META_PATH.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema": 1,
            "generated": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
            "binary": os.path.basename(args.binary),
            "device": "cpu",
            "input": {"size": INPUT_SIZE, "sha256": src_hash},
            "cases": {**meta.get("cases", {}), **golden},
        }
        META_PATH.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        print(f"wrote {META_PATH}")

    status = checks.summary()
    if status == 0 and not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"work dir kept: {work}")
    return status


if __name__ == "__main__":
    sys.exit(main())
