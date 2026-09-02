"""Serve E2E suite entry point.

Runs four suites against a built upix-cli:
  protocol     malformed frames, hostile paths, alloc bomb (F1/F2 regressions)
  concurrency  parallel clients, same-path collision, model hot-switch, inline+daemon
  io           corrupt inputs, dimension edges, hostile paths, format matrix
  lifecycle    crash recovery, double-daemon, stop/idle, signals, poisoned pidfile

One daemon serves protocol/concurrency/io; lifecycle manages its own
instances. Requires: python3, numpy, Pillow. Models must exist locally
(default ~/.local/share/upix/models) — CI fetches them via `upix model download`.

Usage:
  python3 tests/serve/run.py [--binary PATH] [--device cpu|gpu] [--model NAME]
                             [--model-b NAME] [--models-dir DIR]
                             [--suite NAME] [--keep-work]
Exit code 0 = all green.
"""

import argparse
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib import Checks, Daemon  # noqa: E402
import corpus  # noqa: E402
import suite_concurrency  # noqa: E402
import suite_io  # noqa: E402
import suite_lifecycle  # noqa: E402
import suite_protocol  # noqa: E402

DEFAULT_MODELS_DIR = os.path.expanduser("~/.local/share/upix/models")


def default_binary():
    plat, arch = os.uname().sysname.lower(), os.uname().machine
    plat = {"darwin": "macosx"}.get(plat, plat)
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    hits = [p for mode in ("release", "debug")
            if os.path.exists(p := os.path.join(
                root, "build", plat, arch, mode, "upix-cli"))]
    return hits[0] if hits else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--binary", default=default_binary(),
                    help="upix-cli path (auto: build/<plat>/<arch>/release)")
    ap.add_argument("--device", choices=("cpu", "gpu"), default="cpu")
    ap.add_argument("--model", default="NomosUni-SPAN-x2")
    ap.add_argument("--model-b", default="HFA2k-LUDVAE-SPAN-x2",
                    help="second model for hot-switching (skipped if absent)")
    ap.add_argument("--models-dir", default=DEFAULT_MODELS_DIR)
    ap.add_argument("--suite", choices=("protocol", "concurrency", "io",
                                        "lifecycle"), action="append")
    ap.add_argument("--keep-work", action="store_true",
                    help="keep the temp work dir for debugging")
    args = ap.parse_args()

    if not args.binary or not os.access(args.binary, os.X_OK):
        sys.exit(f"upix-cli not found: '{args.binary}' — pass --binary or build "
                 "with scripts/build_cli.sh")
    for m in (args.model, args.model_b):
        if m and not os.path.exists(os.path.join(args.models_dir,
                                                 f"{m}.param")):
            if m == args.model:
                sys.exit(f"model '{m}' not in {args.models_dir} — "
                         "run `upix-cli model download " + m + "` or pass "
                         "--models-dir/--model")
            print(f"note: model-b '{m}' absent — hot-switch cases will skip")
            args.model_b = ""

    work = tempfile.mkdtemp(prefix="upix-serve-tests-")
    print(f"work dir: {work}")
    print(f"binary: {args.binary}  device: {args.device}")
    all_failures = []
    started = time.time()
    try:
        imgs = corpus.build(work)

        def run_suite(name, fn):
            print(f"\n── {name} " + "─" * max(1, 60 - len(name)))
            checks = Checks(name)
            t0 = time.time()
            try:
                fn(checks)
            except Exception as e:  # noqa: BLE001 — suite crash is a failure
                checks.check("suite completed without exception", False,
                             f"{type(e).__name__}: {e}")
            print(f"  ({time.time() - t0:.1f}s, "
                  f"{'GREEN' if checks.verdict() else 'FAILURES'})")
            all_failures.extend(checks.failures)

        wanted = args.suite or ["protocol", "concurrency", "io", "lifecycle"]

        if any(s in wanted for s in ("protocol", "concurrency", "io")):
            daemon = Daemon(args.binary, work, args.models_dir, args.model,
                            device=args.device).start()
            if "protocol" in wanted:
                run_suite("protocol", lambda c: suite_protocol.run(
                    daemon, imgs, work, c))
            if "concurrency" in wanted:
                mb = args.model_b.encode() if args.model_b else b""
                run_suite("concurrency", lambda c: suite_concurrency.run(
                    daemon, imgs, work, c, model_b=mb or None))
            if "io" in wanted:
                run_suite("io", lambda c: suite_io.run(
                    daemon, imgs, work, c, args.model))
            daemon.stop()
            daemon.cleanup_files()

        if "lifecycle" in wanted:
            run_suite("lifecycle", lambda c: suite_lifecycle.run(
                args.binary, imgs, work, c, args.models_dir, args.model,
                device=args.device))
    finally:
        if args.keep_work:
            print(f"work dir kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    dt = time.time() - started
    print("\n" + "=" * 68)
    if all_failures:
        print(f"VERDICT: {len(all_failures)} FAILURE(S) in {dt:.0f}s")
        for f in all_failures:
            print("  -", f)
        return 1
    print(f"VERDICT: ALL GREEN in {dt:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
