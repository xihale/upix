"""Serve daemon BRUTE-FORCE stress entry point.

Beyond tests/serve (handcrafted cases) this campaign randomizes and scales up:
  fuzz        seeded mutation fuzzing of the wire protocol, kill-vector capture
  semantic    hostile-but-well-formed field values, special files, OOM bomb
  churn       connection churn, silent horde, slowloris, queue bursts
  soak        sequential load with fd/thread/RSS leak detection
  lifecycle2  start/stop churn, poisoned pidfiles, SIGSTOP, socket-unlink orphan

One shared daemon serves fuzz/semantic/churn/soak; a daemon death is recorded,
the daemon restarted, and the campaign continues (finding ALL kill vectors).
lifecycle2 manages its own instances.

Usage:
  python3 tests/serve_stress/run.py [--binary PATH] [--device cpu|gpu]
      [--model NAME] [--models-dir DIR] [--suite NAME] [--seed N]
      [--quick] [--keep-work]
Exit code 0 = all green.
"""

import argparse
import faulthandler
import os
import shutil
import signal
import sys
import tempfile
import time

# SIGUSR1 dumps all thread stacks to stderr — the hang-diagnosis escape hatch
# for a campaign that deliberately talks to a daemon with hostile traffic.
faulthandler.register(signal.SIGUSR1, all_threads=True)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "serve"))

from lib import Checks, Daemon  # noqa: E402
import corpus  # noqa: E402
import suite_churn  # noqa: E402
import suite_fuzz  # noqa: E402
import suite_lifecycle2  # noqa: E402
import suite_semantic  # noqa: E402
import suite_soak  # noqa: E402

DEFAULT_MODELS_DIR = os.path.expanduser("~/.local/share/upix/models")


class Ctx:
    """Shared campaign context: the current daemon plus restart capability."""

    def __init__(self, binary, models_dir, model, device, work, seed, quick):
        self.binary = binary
        self.models_dir = models_dir
        self.model = model
        self.device = device
        self.work = work
        self.seed = seed
        self.quick = quick
        self.daemon = None
        self.restarts = 0
        self.deaths = []  # (suite, case, log tail)

    def start_daemon(self, log_name="stress_shared.log"):
        d = Daemon(self.binary, self.work, self.models_dir, self.model,
                   device=self.device, log_name=log_name)
        d.start()
        self.daemon = d
        return d

    def restart(self, suite, case):
        """Record a daemon death and bring a fresh one up on the same path."""
        tail = ""
        if self.daemon is not None:
            try:
                with open(self.daemon.log_path, errors="replace") as f:
                    tail = "".join(f.readlines()[-8:])
            except OSError:
                pass
            try:
                self.daemon.proc.kill()
            except OSError:
                pass
            self.daemon.cleanup_files()
        self.deaths.append((suite, case, tail.strip()[-400:]))
        self.restarts += 1
        self.start_daemon(f"stress_shared_{self.restarts}.log")


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
    ap.add_argument("--binary", default=default_binary())
    ap.add_argument("--device", choices=("cpu", "gpu"), default="cpu")
    ap.add_argument("--model", default="NomosUni-SPAN-x2")
    ap.add_argument("--models-dir", default=DEFAULT_MODELS_DIR)
    ap.add_argument("--suite", choices=("fuzz", "semantic", "churn", "soak",
                                        "lifecycle2"), action="append")
    ap.add_argument("--seed", type=int, default=20260913)
    ap.add_argument("--quick", action="store_true",
                    help="reduced iteration counts for a smoke pass")
    ap.add_argument("--keep-work", action="store_true")
    args = ap.parse_args()

    if not args.binary or not os.access(args.binary, os.X_OK):
        sys.exit(f"upix-cli not found: '{args.binary}' — pass --binary or "
                 "build with scripts/build_cli.sh")
    if not os.path.exists(os.path.join(args.models_dir,
                                       f"{args.model}.param")):
        sys.exit(f"model '{args.model}' not in {args.models_dir}")

    work = tempfile.mkdtemp(prefix="upix-serve-stress-")
    # DEBUG daemon logs make every rejection branch visible in stress logs
    os.environ.setdefault("UPIX_LOG_LEVEL", "debug")
    print(f"work dir: {work}")
    print(f"binary: {args.binary}  device: {args.device}  seed: {args.seed}"
          f"  mode: {'quick' if args.quick else 'full'}")
    all_failures = []
    started = time.time()
    try:
        imgs = corpus.build(work)

        ctx = Ctx(args.binary, args.models_dir, args.model, args.device,
                  work, args.seed, args.quick)

        def run_suite(name, fn, needs_daemon=True):
            print(f"\n── {name} " + "─" * max(1, 60 - len(name)))
            checks = Checks(name)
            t0 = time.time()
            try:
                if needs_daemon and ctx.daemon is None:
                    ctx.start_daemon()
                fn(ctx, imgs, work, checks)
            except Exception as e:  # noqa: BLE001 — suite crash is a failure
                checks.check("suite completed without exception", False,
                             f"{type(e).__name__}: {e}")
            print(f"  ({time.time() - t0:.1f}s, "
                  f"{'GREEN' if checks.verdict() else 'FAILURES'})")
            all_failures.extend(checks.failures)

        wanted = args.suite or ["fuzz", "semantic", "churn", "soak",
                                "lifecycle2"]

        if "fuzz" in wanted:
            run_suite("fuzz", suite_fuzz.run)
        if "semantic" in wanted:
            run_suite("semantic", suite_semantic.run)
        if "churn" in wanted:
            run_suite("churn", suite_churn.run)
        if "soak" in wanted:
            run_suite("soak", suite_soak.run)

        if ctx.daemon is not None:
            ctx.daemon.stop()
            ctx.daemon.cleanup_files()
            ctx.daemon = None

        if "lifecycle2" in wanted:
            run_suite("lifecycle2", suite_lifecycle2.run, needs_daemon=False)

        if ctx.deaths:
            print(f"\n── daemon deaths ({len(ctx.deaths)}) " + "─" * 30)
            for suite, case, tail in ctx.deaths:
                print(f"  ✗ [{suite}] {case}")
                if tail:
                    print(f"      log: {tail}")
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
