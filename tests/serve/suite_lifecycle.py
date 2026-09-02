"""Lifecycle suite: crash recovery, double-daemon refusal, stop/idle cleanup,
mid-request signals, pidfile poisoning (F3), junk file at socket path (F4).
Manages its own daemon instances; runs with no daemon pre-started."""

import os
import signal
import socket
import subprocess
import time

from lib import Checks, Daemon, send_request, serve_request

MODEL_NAME = b"NomosUni-SPAN-x2"


def run(binary, corpus, work_dir, checks: Checks, models_dir, model,
        device="cpu"):
    out = os.path.join(work_dir, "out")
    os.makedirs(out, exist_ok=True)
    inp = corpus["std256.png"]
    big = corpus["sq511.png"]

    def serve_ok(d, tag):
        try:
            r = send_request(d.sock, serve_request(
                MODEL_NAME, b"png", 0, 16, 90,
                str(inp).encode(), os.path.join(out, f"lc_{tag}.png").encode()),
                timeout=300)
            return r["ok"] == 1, str(r)
        except (ConnectionError, OSError, ValueError) as e:
            return False, str(e)

    def spawn(tag, idle="0", model_name=None):
        d = Daemon(binary, work_dir, models_dir, model_name or model,
                   device=device, idle_timeout=idle,
                   log_name=f"lc_{tag}.log")
        d.start()
        return d

    # ── L1: SIGKILL → stale socket+pid → auto-recovery via `upix process` ──
    d = spawn("l1")
    ok, detail = serve_ok(d, "l1a")
    checks.check("L1 serves before kill", ok, detail)
    os.kill(d.pid(), signal.SIGKILL)
    time.sleep(0.5)
    checks.check("L1 stale socket+pidfile remain",
                 os.path.exists(d.sock) and os.path.exists(d.pidfile))
    r = subprocess.run([binary, inp, os.path.join(out, "l1_recovered.png"),
                        "--model", model], env=d.env(), capture_output=True,
                       text=True, timeout=600)
    checks.check("L1 process auto-recovers", r.returncode == 0,
                 r.stderr[-200:])
    checks.check("L1 recovered output exists",
                 os.path.exists(os.path.join(out, "l1_recovered.png")))
    ok, detail = serve_ok(d, "l1b")
    checks.check("L1 fresh daemon serves", ok, detail)
    d.stop()
    d.cleanup_files()

    # ── L2: second daemon refused, first unharmed ───────────────────────────
    d = spawn("l2")
    first_pid = d.pid()
    r = subprocess.run([binary, "daemon", "--models-dir", models_dir,
                        "--model", model, "--idle-timeout", "0", "--fg"],
                       env=d.env(), capture_output=True, text=True,
                       timeout=300)
    checks.check("L2 second daemon exits nonzero", r.returncode != 0,
                 f"rc={r.returncode}")
    ok, detail = serve_ok(d, "l2")
    checks.check("L2 first daemon still serves", ok, detail)
    checks.check("L2 pidfile still first daemon", d.pid() == first_pid)
    d.stop()
    d.cleanup_files()

    # ── L3: `daemon stop` removes socket and pidfile ────────────────────────
    d = spawn("l3")
    r = subprocess.run([binary, "daemon", "stop"], env=d.env(),
                       capture_output=True, text=True, timeout=60)
    checks.check("L3 stop rc=0", r.returncode == 0, r.stderr[-150:])
    time.sleep(1.5)
    checks.check("L3 socket+pidfile removed",
                 not os.path.exists(d.sock) and not os.path.exists(d.pidfile))
    checks.check("L3 daemon process gone", not d.alive())

    # ── L4: idle timeout exit cleans up ─────────────────────────────────────
    d = spawn("l4", idle="2")
    deadline = time.time() + 30
    gone = False
    while time.time() < deadline:
        if not d.alive() and not os.path.exists(d.sock):
            gone = True
            break
        time.sleep(0.5)
    checks.check("L4 idle exit + cleanup", gone)

    # ── L5: client hangup mid-inference; daemon keeps serving ───────────────
    d = spawn("l5")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(d.sock)
    from lib import MSG_SERVE_REQUEST, frame
    s.sendall(frame(MSG_SERVE_REQUEST, serve_request(
        MODEL_NAME, b"png", 0, 16, 90, str(big).encode(),
        os.path.join(out, "lc_abandon.png").encode())))
    s.close()
    ok, detail = serve_ok(d, "l5_after")
    checks.check("L5 serves next request after hangup", ok, detail)

    # ── L6: SIGTERM mid-run → client sees clean EOF, no partial files ───────
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(d.sock)
    s.sendall(frame(MSG_SERVE_REQUEST, serve_request(
        MODEL_NAME, b"png", 0, 16, 90, str(big).encode(),
        os.path.join(out, "lc_term.png").encode())))
    time.sleep(0.05)
    os.kill(d.pid(), signal.SIGTERM)
    try:
        s.recv(4096)
        checks.check("L6 client sees clean close", True)
    except socket.timeout:
        checks.check("L6 client sees clean close", False, "recv timeout")
    s.close()
    d.proc.wait(timeout=30)
    checks.check("L6 daemon gone after SIGTERM", not d.alive())
    leftovers = [f for f in os.listdir(out) if ".upix-tmp-" in f]
    checks.check("L6 no staging leftovers", not leftovers, str(leftovers))
    term_out = os.path.join(out, "lc_term.png")
    if os.path.exists(term_out):
        # SIGTERM may land after a fast request finished: file must be complete
        from PIL import Image
        im = Image.open(term_out)
        im.load()
        checks.check("L6 output complete if present", im.size == (1022, 1022),
                     str(im.size))
    d.cleanup_files()

    # ── L7: poisoned pidfile (zombie pid) → fast inline fallback, no 30s wait
    os.makedirs(os.path.dirname(d.sock), exist_ok=True)
    zombie = subprocess.Popen(["sleep", "60"])
    os.kill(zombie.pid, signal.SIGKILL)  # parent deliberately does not reap
    time.sleep(0.3)
    with open(d.sock + ".pid", "w") as f:
        f.write(f"{zombie.pid}\n")
    t0 = time.time()
    r = subprocess.run([binary, inp, os.path.join(out, "l7.png"), "--model",
                        model], env=d.env(), capture_output=True, timeout=120)
    dt = time.time() - t0
    checks.check("L7 zombie pidfile → no 30s penalty",
                 r.returncode == 0 and dt < 10,
                 f"rc={r.returncode} {dt:.1f}s")
    checks.check("L7 inline fallback produced output",
                 os.path.exists(os.path.join(out, "l7.png")))
    zombie.wait()
    # the fallback above may have spawned a fresh daemon — take it down so
    # L8 can place its junk file on a quiet path
    subprocess.run([binary, "daemon", "stop"], env=d.env(),
                   capture_output=True, timeout=60)
    time.sleep(1.5)
    d.cleanup_files()

    # ── L8: junk regular file at socket path → daemon refuses, file survives
    junk = "junk regular file - must survive"
    with open(d.sock, "w") as f:
        f.write(junk)
    r = subprocess.run([binary, "daemon", "--models-dir", models_dir,
                        "--model", model, "--idle-timeout", "0", "--fg"],
                       env=d.env(), capture_output=True, text=True,
                       timeout=300)
    checks.check("L8 daemon refuses non-socket path", r.returncode != 0,
                 f"rc={r.returncode}")
    with open(d.sock) as f:
        checks.check("L8 junk file preserved", f.read() == junk)
    os.unlink(d.sock)
