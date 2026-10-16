#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Squatch Tuner web bridge: serves the page and streams the tuner's readings (Server-Sent Events).

    serve.py [--bind 0.0.0.0] [--port 8740] [--www DIR] -- squatch-tuner-live [args...]

It starts the tuner program as a child, reads its JSON lines from stdout, and fans the newest
one out to every browser on /events. Standard library only. Routes:

    GET  /            the page (index.html and its files, from --www)
    GET  /events      text/event-stream, one `data:` line per reading (about 30 a second)
    GET  /clock       {"t": server wall-clock ms}, for the page's clock-offset estimate
    POST /a4          {"a4": 442} - reference pitch, 430..450, shared by every viewer
    GET  /healthz     200 once the tuner has produced a reading

When the tuner exits, so does the bridge, with the tuner's exit code.
"""
import argparse
import json
import mimetypes
import resource
import signal
import subprocess
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

STATIC = {"/": "index.html", "/index.html": "index.html", "/tuner.css": "tuner.css", "/tuner.js": "tuner.js",
          "/strobe.js": "strobe.js", "/hold.js": "hold.js", "/tuner-view.js": "tuner-view.js",
          "/favicon.svg": "favicon.svg", "/tokens.css": "tokens.css"}
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
       "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
STALE_SECONDS = 1.0
SEND_TIMEOUT = 10.0


class Feed:
    """The newest reading from the tuner, and a condition browsers wait on for the next one."""

    def __init__(self, child):
        self.child = child
        self.cond = threading.Condition()
        self.line = None
        self.seq = 0
        self.at = 0.0
        self.cpu = CpuMeter()
        self.stdin_lock = threading.Lock()

    def pump(self):
        """Reader thread: every stdout line of the tuner becomes the newest reading."""
        for raw in self.child.stdout:
            try:
                reading = json.loads(raw)
            except ValueError:
                continue
            reading["bridgeCpu"] = round(self.cpu.percent(), 2)
            data = json.dumps(reading, separators=(",", ":")).encode()
            with self.cond:
                self.line, self.seq, self.at = data, self.seq + 1, time.monotonic()
                self.cond.notify_all()
        with self.cond:
            self.line = None
            self.cond.notify_all()

    def next(self, seq, timeout):
        """Wait for a reading newer than `seq`; returns (seq, line or None when the tuner is gone)."""
        with self.cond:
            self.cond.wait_for(lambda: self.seq != seq or self.child.poll() is not None, timeout)
            return self.seq, self.line

    def stale(self):
        return self.line is None or time.monotonic() - self.at > STALE_SECONDS

    def set_a4(self, hz):
        with self.stdin_lock:
            try:
                self.child.stdin.write(f"a4 {hz:.2f}\n".encode())
                self.child.stdin.flush()
            except (BrokenPipeError, OSError):
                return False
        return True


class CpuMeter:
    """This process's CPU use, % of one core, since the previous call (at most once a second)."""

    def __init__(self):
        self.last = (self._cpu(), time.monotonic())
        self.value = 0.0

    @staticmethod
    def _cpu():
        r = resource.getrusage(resource.RUSAGE_SELF)
        return r.ru_utime + r.ru_stime

    def percent(self):
        cpu, now = self._cpu(), time.monotonic()
        if now - self.last[1] >= 1.0:
            self.value = 100.0 * (cpu - self.last[0]) / (now - self.last[1])
            self.last = (cpu, now)
        return self.value


class Handler(BaseHTTPRequestHandler):
    server_version = "squatch-tuner"
    sys_version = ""
    feed = None
    www = None

    def log_message(self, fmt, *args):  # one line per request is noise at 30 events a second
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/events":
            return self.events()
        if path == "/clock":
            return self.send_json({"t": time.time() * 1000.0})
        if path == "/healthz":
            ok = not self.feed.stale()
            return self.send_json({"ok": ok}, HTTPStatus.OK if ok else HTTPStatus.SERVICE_UNAVAILABLE)
        if path in STATIC:
            return self.send_file(self.www / STATIC[path])
        return self.send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def do_HEAD(self):
        path = self.path.split("?", 1)[0]
        if path in STATIC:
            return self.send_file(self.www / STATIC[path], body=False)
        return self.send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if self.path != "/a4":
            return self.send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
        refusal = self.refuse_post()
        if refusal:
            return self.send_json({"error": refusal}, HTTPStatus.BAD_REQUEST)
        try:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            hz = float(body["a4"])
        except (ValueError, KeyError, TypeError):
            return self.send_json({"error": "expected {\"a4\": 430..450}"}, HTTPStatus.BAD_REQUEST)
        if not 430.0 <= hz <= 450.0:
            return self.send_json({"error": "a4 must be 430..450"}, HTTPStatus.BAD_REQUEST)
        if not self.feed.set_a4(hz):
            return self.send_json({"error": "tuner not running"}, HTTPStatus.SERVICE_UNAVAILABLE)
        return self.send_json({"a4": hz})

    # A4 is shared by every viewer, so only this page may change it. application/json forces a
    # CORS preflight this server never answers, and a browser always sends Origin on a POST, so a
    # page on another site (open on any device on the LAN) cannot. A body over 256 bytes, or a
    # missing or negative length, is refused before anything is read.
    def refuse_post(self):
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return "Content-Length required"
        if not 0 < length <= 256:
            return "body must be 1..256 bytes"
        if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
            return "Content-Type must be application/json"
        origin = self.headers.get("Origin")
        if origin and urlsplit(origin).netloc != self.headers.get("Host", ""):
            return "cross-origin request refused"
        return None

    def events(self):
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        seq = 0
        self.connection.settimeout(SEND_TIMEOUT)  # a client that stops reading frees its thread
        try:
            while self.feed.child.poll() is None:
                seq, line = self.feed.next(seq, timeout=1.0)
                stale = line is None or self.feed.stale()
                self.wfile.write(b"event: offline\ndata: {}\n\n" if stale else b"data: " + line + b"\n\n")
                self.wfile.flush()
        except OSError:  # broken pipe, reset, or the send timeout
            return

    def send_json(self, obj, status=HTTPStatus.OK):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_file(self, path, body=True):
        data = path.read_bytes()
        kind = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", kind + ("; charset=utf-8" if kind.startswith(("text/", "application/javascript")) else ""))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if body:
            self.wfile.write(data)


def parse_args(argv):
    if "--" not in argv:
        sys.exit("usage: serve.py [--bind ADDR] [--port N] [--www DIR] -- squatch-tuner-live [args...]")
    split = argv.index("--")
    p = argparse.ArgumentParser(description="Squatch Tuner web bridge")
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8740)
    p.add_argument("--www", default=str(Path(__file__).resolve().parent))
    args = p.parse_args(argv[:split])
    args.command = argv[split + 1:]
    return args


def stop(child):
    """Ask the tuner to stop; if it has not within 3 s, kill it."""
    child.terminate()
    threading.Timer(3.0, child.kill).start()


def main(argv):
    args = parse_args(argv)
    mimetypes.add_type("image/svg+xml", ".svg")
    mimetypes.add_type("text/javascript", ".js")
    child = subprocess.Popen(args.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    feed = Feed(child)
    Handler.feed, Handler.www = feed, Path(args.www)
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    threading.Thread(target=feed.pump, daemon=True).start()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: stop(child))
    signal.signal(signal.SIGINT, lambda *_: stop(child))
    print(f"serve.py: http://{args.bind}:{args.port}/ (tuner pid {child.pid})", file=sys.stderr, flush=True)
    code = child.wait()
    server.shutdown()
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
