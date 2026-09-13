"""Lifecycle abuse (beyond tests/serve/suite_lifecycle): start/stop churn,
concurrent double-stop, poisoned pidfile variants (never kill an innocent
pid), SIGSTOP/SIGCONT mid-request, background (forked) start + status, and
the socket-unlink orphan scenario. Manages its own daemon instances."""

import os
import signal
import socket
import subprocess
import time

from lib import Checks, Daemon, send_request, serve_request

MODEL = b"NomosUni-SPAN-x2"


def run(ctx, corpus_imgs, work_dir, checks):
    binary = ctx.binary
    models_dir = ctx.models_dir
    model = ctx.model
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    inp = corpus_imgs["std256.png"]

    def serve_ok(d, tag):
        try:
            r = send_request(d.sock, serve_request(
                MODEL, b"png", 0, 16, 90, str(inp).encode(),
                os.path.join(out, f"lc2_{tag}.png").encode()), timeout=300)
            return r["ok"] == 1, str(r)
        except (ConnectionError, OSError, ValueError) as e:
            return False, str(e)

    def spawn(tag, idle="0", fg=True):
        d = Daemon(binary, work_dir, models_dir, model, device=ctx.device,
                   idle_timeout=idle, log_name=f"lc2_{tag}.log")
        d.start()
        return d

    # ── K1: start/serve/stop churn ×5 ────────────────────────────────────────
    n = 3 if ctx.quick else 5
    churn_errs = []
    for i in range(n):
        d = spawn(f"k1_{i}")
        ok, detail = serve_ok(d, f"k1_{i}")
        if not ok:
            churn_errs.append(f"cycle {i}: {detail}")
        d.stop()
        d.cleanup_files()
        if os.path.exists(d.sock) or os.path.exists(d.pidfile):
            churn_errs.append(f"cycle {i}: files left behind")
    checks.check(f"K1 {n}x start/serve/stop churn", not churn_errs,
                 "; ".join(churn_errs[:3]))

    # ── K2: concurrent double `daemon stop` ──────────────────────────────────
    d = spawn("k2")
    r1 = subprocess.Popen([binary, "daemon", "stop"], env=d.env(),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    r2 = subprocess.Popen([binary, "daemon", "stop"], env=d.env(),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    o1 = r1.communicate(timeout=60)
    o2 = r2.communicate(timeout=60)
    deadline = time.time() + 15
    while d.alive() and time.time() < deadline:
        time.sleep(0.3)
    checks.check("K2 double stop → daemon gone", not d.alive())
    checks.check("K2 socket+pidfile cleaned",
                 not os.path.exists(d.sock) and not os.path.exists(d.pidfile))
    checks.check("K2 neither stop crashed", r1.returncode in (0, 1) and
                 r2.returncode in (0, 1),
                 f"rc {r1.returncode}/{r2.returncode}")
    d.cleanup_files()

    # ── K3: poisoned pidfile variants — never kill an innocent pid ──────────
    d = spawn("k3")  # running daemon whose pidfile we will poison
    good_pid = d.pid()
    guardian = subprocess.Popen(["sleep", "300"])
    victims = {
        "empty": "",
        "garbage": "abc\n",
        "pid-1": "1\n",
        "own-pid": f"{os.getpid()}\n",
        "huge-pid": "4194304\n",
        "negative": "-5\n",
        "guardian-sleep": f"{guardian.pid}\n",
    }
    for name, content in victims.items():
        with open(d.pidfile, "w") as f:
            f.write(content)
        before_alive = guardian.poll() is None
        r = subprocess.run([binary, "daemon", "stop"], env=d.env(),
                           capture_output=True, text=True, timeout=60)
        checks.check(f"K3 stop with pidfile '{name}' → rc=1 clean",
                     r.returncode == 1,
                     f"rc={r.returncode} {r.stdout.strip()[:60]}")
        checks.check(f"K3 '{name}' killed nothing", guardian.poll() is None,
                     "guardian sleep was signalled!")
        # the REAL daemon must still be alive under its true pid (the pidfile
        # is poisoned, so liveness is checked via /proc, not the pidfile)
        checks.check(f"K3 '{name}' real daemon unharmed",
                     _alive(good_pid) and before_alive, f"pid {good_pid}")
    # CLI process path must also refuse to REUSE a poisoned pidfile
    r = subprocess.run([binary, inp, os.path.join(out, "k3_cli.png"),
                        "--model", model], env=d.env(), capture_output=True,
                       timeout=300)
    checks.check("K3 poisoned pidfile → CLI still works", r.returncode == 0,
                 f"rc={r.returncode}")
    checks.check("K3 guardian survived CLI path too", guardian.poll() is None)
    guardian.terminate()
    guardian.wait()
    # restore the true pid so the final stop (correctly) recognizes the daemon
    with open(d.pidfile, "w") as f:
        f.write(str(good_pid))
    subprocess.run([binary, "daemon", "stop"], env=d.env(),
                   capture_output=True, timeout=60)
    try:
        d.proc.wait(timeout=10)  # reap: a zombie would fool /proc liveness
    except subprocess.TimeoutExpired:
        pass
    checks.check("K3 real daemon stopped via restored pidfile",
                 not _alive(good_pid))
    d.cleanup_files()

    # ── K4: SIGSTOP mid-request → SIGCONT → request completes ───────────────
    d = spawn("k4")
    import threading
    result = {}

    def bg_request():
        t0 = time.time()
        ok, detail = serve_ok(d, "k4")
        result["ok"], result["detail"], result["dt"] = ok, detail, time.time() - t0
    th = threading.Thread(target=bg_request)
    th.start()
    time.sleep(0.4)  # request is now queued/in-flight
    os.kill(d.pid(), signal.SIGSTOP)
    time.sleep(2.0)
    os.kill(d.pid(), signal.SIGCONT)
    th.join(timeout=300)
    checks.check("K4 request completes across SIGSTOP/SIGCONT",
                 result.get("ok") is True, str(result))
    # signals must not have wedged the daemon
    ok, detail = serve_ok(d, "k4_after")
    checks.check("K4 daemon serves after freeze", ok, detail)
    d.stop()
    d.cleanup_files()

    # ── K5: background (forked) start + status roundtrip ─────────────────────
    r = subprocess.run([binary, "daemon", "--models-dir", models_dir,
                        "--model", model, "--idle-timeout", "60"],
                       env=d.env(), capture_output=True, text=True, timeout=120)
    checks.check("K5 background start rc=0", r.returncode == 0, r.stderr[-150:])
    d2 = Daemon(binary, work_dir, models_dir, model, device=ctx.device,
                idle_timeout="60", log_name="lc2_k5.log")
    deadline = time.time() + 60
    up = False
    while time.time() < deadline:
        if d2.alive():
            up = True
            break
        time.sleep(0.3)
    checks.check("K5 daemon came up in background", up)
    r = subprocess.run([binary, "daemon", "status"], env=d2.env(),
                       capture_output=True, text=True, timeout=30)
    checks.check("K5 status reports running", "running" in r.stdout,
                 r.stdout.strip())
    ok, detail = serve_ok(d2, "k5")
    checks.check("K5 background daemon serves", ok, detail)
    subprocess.run([binary, "daemon", "stop"], env=d2.env(),
                   capture_output=True, timeout=60)
    time.sleep(1.5)
    d2.cleanup_files()

    # ── K6: socket file unlinked mid-run → recovery must go through stop ────
    d = spawn("k6")
    pid_before = d.pid()
    os.unlink(d.sock)
    # clients see ENOENT (no daemon) — the old daemon is now unreachable
    ok, detail = serve_ok(d, "k6_should_fail")
    checks.check("K6 connect after unlink fails", not ok, detail[:100])
    checks.check("K6 old daemon still alive", d.alive())
    # a second start is REFUSED: the pidfile still names a live upix daemon
    # (this closes the old silent double-daemon / orphan window)
    d3 = Daemon(binary, work_dir, models_dir, model, device=ctx.device,
                idle_timeout="0", log_name="lc2_k6b.log")
    r = subprocess.run([binary, "daemon", "--models-dir", models_dir,
                        "--model", model, "--idle-timeout", "0", "--fg"],
                       env=d3.env(), capture_output=True, text=True,
                       timeout=120)
    checks.check("K6 second start refused while pidfile names live daemon",
                 r.returncode != 0 and "already running" in (r.stderr + r.stdout),
                 f"rc={r.returncode} {r.stderr.strip()[:80]}")
    # `daemon stop` still reaches the socket-less daemon through the pidfile
    subprocess.run([binary, "daemon", "stop"], env=d.env(),
                   capture_output=True, timeout=60)
    try:
        d.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    checks.check("K6 pidfile-reachable stop killed the old daemon",
                 not _alive(pid_before))
    # and a fresh daemon can then take over the path
    d3.start()
    ok, detail = serve_ok(d3, "k6_new")
    checks.check("K6 fresh daemon serves after stop", ok, detail[:100])
    d3.stop()
    try:
        d3.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    d3.cleanup_files()
    d.cleanup_files()


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False
