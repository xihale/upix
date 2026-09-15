"""Shared harness for the serve E2E suites: wire protocol client, daemon
process management (pidfile is ground truth — the daemonized process re-execs
with argv[0] "upix" so pgrep can't see it), and check collection."""

import os
import signal
import socket
import struct
import subprocess
import time
import zlib

import numpy as np
from PIL import Image

# ── wire protocol (mirror of share/core/distributed/protocol.hpp) ───────────

MAGIC = 0x53534358
MSG_SERVE_REQUEST = 0x0601
MSG_SERVE_RESPONSE = 0x0602
MSG_SERVE_PROGRESS = 0x0604
HEADER = struct.Struct("<IHI")  # magic, type, payload_length + trailing crc32


def encoder_wire(png_level=-1, jpeg_sub=420, webp_method=-1, webp_lossless=0,
                 avif_speed=6, avif_sub=420, avif_threads=0):
    """28-byte EncoderOptionsWire mirror (natural alignment, no padding)."""
    return struct.pack("<iiiB3xiii", png_level, jpeg_sub, webp_method,
                       webp_lossless, avif_speed, avif_sub, avif_threads)


def serve_request(model=b"", fmt=b"", tile=0, overlap=16, quality=90,
                  inp=b"", outp=b"", encoder=None):
    """ServeRequest payload: 64s model + 16s format + ints + paths."""
    payload = struct.pack("<64s16siiiII", model, fmt, tile, overlap, quality,
                          len(inp), len(outp))
    payload += encoder if encoder is not None else encoder_wire()
    return payload + inp + outp


def frame(msg_type, payload):
    # Wire rule (Protocol::serialize): CRC32 over the WHOLE frame — the 14B
    # header with the checksum slot zeroed, then the payload. HEADER packs
    # only the first 10 bytes; the slot joins as zeros.
    hdr = HEADER.pack(MAGIC, msg_type, len(payload))
    body = hdr + b"\x00\x00\x00\x00" + payload
    return hdr + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF) + payload


def split_frames(buf):
    """Yield (mtype, payload) from a raw byte stream. The daemon interleaves
    progress frames with the final response — callers must frame-split and
    dispatch on mtype, never strip a fixed header and parse."""
    off = 0
    while off + 14 <= len(buf):
        magic, mtype, plen = HEADER.unpack_from(buf, off)
        if magic != MAGIC:
            return
        yield mtype, buf[off + 14:off + 14 + plen]
        off += 14 + plen


def parse_response(payload):
    ok, w, h, ch, plen = struct.unpack_from("<iiiii", payload, 0)
    return dict(ok=ok, w=w, h=h, ch=ch,
                error=payload[20:148].split(b"\0")[0].decode(errors="replace"),
                model=payload[148:212].split(b"\0")[0].decode(errors="replace"),
                path=payload[212:212 + plen].decode(errors="replace"))


def send_request(sock_path, payload, timeout=180.0):
    """One request per connection: send, drain progress, return response."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall(frame(MSG_SERVE_REQUEST, payload))
    resp = None
    while resp is None:
        magic, mtype, plen = HEADER.unpack(_recv_exact(s, 14)[:10])
        if magic != MAGIC:
            raise ValueError("daemon sent bad magic")
        data = _recv_exact(s, plen) if plen else b""
        if mtype == MSG_SERVE_RESPONSE:
            resp = parse_response(data)
    s.close()
    return resp


def raw_roundtrip(sock_path, data, timeout=10.0):
    """Send raw bytes, collect everything the daemon replies until EOF/timeout.
    Returns the raw byte stream (progress + response frames interleaved)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall(data)
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except (socket.timeout, ConnectionResetError, BrokenPipeError):
        pass
    s.close()
    return out


def _recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("daemon closed mid-frame")
        buf += chunk
    return buf


# ── daemon process management ───────────────────────────────────────────────

class Daemon:
    """One serve daemon bound to an isolated XDG_CACHE_HOME. The pidfile is
    the only reliable liveness source (see module docstring)."""

    def __init__(self, binary, work_dir, models_dir, model, device="cpu",
                 idle_timeout=0, log_name="daemon.log"):
        self.binary = binary
        self.cache = os.path.join(work_dir, "cache")
        self.sock = os.path.join(self.cache, "upix", "serve.sock")
        self.pidfile = self.sock + ".pid"
        self.models_dir = models_dir
        self.model = model
        self.device = device
        self.idle_timeout = idle_timeout
        self.log_path = os.path.join(work_dir, log_name)
        self.proc = None

    def env(self):
        e = dict(os.environ)
        e["XDG_CACHE_HOME"] = self.cache
        if self.device == "cpu":
            e.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        return e

    def start(self, timeout_s=120):
        os.makedirs(os.path.dirname(self.sock), exist_ok=True)
        cmd = [self.binary, "daemon", "--models-dir", self.models_dir,
               "--model", self.model, "--idle-timeout", str(self.idle_timeout),
               "--fg"]
        if self.device == "cpu":
            cmd.append("--cpu")
        with open(self.log_path, "ab") as log:
            self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                         env=self.env())
        if not self.wait_ready(timeout_s):
            raise RuntimeError(f"daemon not ready in {timeout_s}s; log tail: "
                               + _tail(self.log_path))
        return self

    def wait_ready(self, timeout_s):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if os.path.exists(self.sock):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.settimeout(2)
                    s.connect(self.sock)
                    s.close()
                    return True
                except OSError:
                    pass
            time.sleep(0.1)
        return False

    def pid(self):
        try:
            return int(open(self.pidfile).read().strip())
        except (OSError, ValueError):
            return 0

    def alive(self):
        pid = self.pid()
        if pid <= 0:
            return False
        try:
            os.kill(pid, 0)
        except OSError:
            return False
        # kill(0) also passes for zombies and recycled pids — mirror the
        # CLI's pid_is_upix_daemon: require our binary's exe.
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            return False
        return "upix" in exe

    def kill9(self):
        pid = self.pid()
        if pid > 0:
            os.kill(pid, signal.SIGKILL)
        if self.proc and self.proc.poll() is None:
            self.proc.kill()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            subprocess.run([self.binary, "daemon", "stop"], env=self.env(),
                           capture_output=True, timeout=60)
        try:
            self.proc.wait(timeout=30)
        except (subprocess.TimeoutExpired, ValueError):
            pass

    def cleanup_files(self):
        for f in (self.sock, self.pidfile):
            try:
                os.unlink(f)
            except OSError:
                pass


def _tail(path, n=400):
    try:
        return "".join(open(path, errors="replace").readlines()[-n:])
    except OSError:
        return "(no log)"


# ── checks ──────────────────────────────────────────────────────────────────

class Checks:
    def __init__(self, suite):
        self.suite = suite
        self.failures = []

    def check(self, name, cond, detail=""):
        print(f"  {'PASS' if cond else 'FAIL'} {name}" + (f" — {detail}" if detail else ""))
        if not cond:
            self.failures.append(f"{self.suite}/{name}: {detail}")

    def info(self, msg):
        print(f"  INFO {msg}")

    def verdict(self):
        return not self.failures


def pixels(path):
    return np.asarray(Image.open(path).convert("RGB"))
