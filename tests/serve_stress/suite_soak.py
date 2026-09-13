"""Sequential soak: leak detector for the long-lived daemon. Serves a mixed
stream of real and error-path requests while sampling /proc fd count, thread
count and RSS. After warmup the series must be FLAT — a per-connection fd or
thread ramp is the regression this suite exists to catch."""

import os
import time

from lib import send_request, serve_request

MODEL = b"NomosUni-SPAN-x2"


def _sample(pid):
    try:
        fds = len(os.listdir(f"/proc/{pid}/fd"))
        tasks = len(os.listdir(f"/proc/{pid}/task"))
        rss = 0
        with open(f"/proc/{pid}/status", errors="replace") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                    break
        return fds, tasks, rss
    except OSError:
        return 0, 0, 0


def run(ctx, corpus_imgs, work_dir, checks):
    d = ctx.daemon
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    inp = corpus_imgs["std256.png"]
    odd = corpus_imgs["odd201x199.png"]
    n = 40 if ctx.quick else 150

    # warmup: engine allocations, thread pools, caches settle here
    for i in range(5):
        send_request(d.sock, serve_request(
            MODEL, b"png", 0, 16, 90, str(inp).encode(),
            os.path.join(out, f"soak_warm_{i}.png").encode()), timeout=300)
    base_fds, base_tasks, base_rss = _sample(d.pid())
    checks.info(f"baseline after warmup: fds={base_fds} tasks={base_tasks} "
                f"rss={base_rss // 1024}MB")

    fd_series, errs = [], []
    t0 = time.time()
    lat = []
    for i in range(n):
        src = inp if i % 3 else odd
        if i % 7 == 5:
            # error-path request (corrupt input) — exercises refusal branches
            bad = os.path.join(work_dir, "corpus", "soak_bad.png")
            if not os.path.exists(bad):
                open(bad, "wb").write(b"\x89PNG\r\n\x1a\n" + b"\0" * 64)
            src = bad
        s = time.time()
        try:
            r = send_request(d.sock, serve_request(
                MODEL, b"png", 0, 16, 90, str(src).encode(),
                os.path.join(out, f"soak_{i}.png").encode()), timeout=300)
            lat.append(time.time() - s)
            if r["ok"] != 1 and i % 7 != 5:
                errs.append(f"#{i}: {r['error']}")
        except (ConnectionError, OSError, ValueError) as e:
            errs.append(f"#{i}: EXC {e}")
            if not d.alive():
                checks.check("soak daemon survived", False, f"died at #{i}")
                ctx.restart("soak", f"request-{i}")
                base_fds, base_tasks, base_rss = _sample(d.pid())
        if i % 10 == 9:
            fds, tasks, rss = _sample(d.pid())
            fd_series.append((i + 1, fds, tasks, rss))
    dt = time.time() - t0

    end_fds, end_tasks, end_rss = _sample(d.pid())
    fd_series.append((n, end_fds, end_tasks, end_rss))
    for i, fds, tasks, rss in fd_series:
        checks.info(f"after {i:3d} requests: fds={fds} ({fds - base_fds:+d}) "
                    f"tasks={tasks} rss={rss // 1024}MB")

    checks.check(f"soak {n} requests all served", not errs, "; ".join(errs[:3]))
    # THE leak assertions: fd/thread counts must return to baseline. One
    # retained fd per connection (watch-fd never closed until teardown) shows
    # up as a perfect ramp here.
    checks.check("fd count flat after soak", end_fds <= base_fds + 8,
                 f"baseline {base_fds} → end {end_fds}")
    checks.check("thread count flat after soak", end_tasks <= base_tasks + 4,
                 f"baseline {base_tasks} → end {end_tasks}")
    checks.check("RSS bounded after soak", end_rss <= base_rss + 400 * 1024,
                 f"baseline {base_rss // 1024}MB → end {end_rss // 1024}MB")
    lat.sort()
    if lat:
        checks.info(
            f"latency p50={lat[len(lat) // 2]:.2f}s "
            f"p95={lat[int(len(lat) * 0.95)]:.2f}s max={lat[-1]:.2f}s "
            f"total {dt:.0f}s")
