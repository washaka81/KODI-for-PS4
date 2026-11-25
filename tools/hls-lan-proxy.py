#!/usr/bin/env python3
"""Minimal LAN HLS reverse proxy for PS4 diagnostics.

The PS4 pays several seconds of TLS setup for every HLS playlist/segment.
This proxy fetches the origin from a host with a fast TLS stack (the PC) and
serves plain HTTP on the LAN.  It is intentionally generic: it rewrites any
HLS master/variant playlist and proxies the segments on demand.

Usage:
  python3 hls-lan-proxy.py [--port 8099] [--bind 0.0.0.0]

Then open this URL in the player (replace the origin URL):
  http://<PC-IP>:8099/playlist.m3u8?u=<url-encoded-origin-master-or-variant>

Only the Python standard library and the `curl` binary are required.
"""

from __future__ import annotations

import argparse
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

USER_AGENT = (
    "Mozilla/5.0 (PlayStation 4 12.00) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko)"
)
CURL = ["curl", "-sS", "--max-time", "30", "-A", USER_AGENT]
PLAYLIST_TTL = 1.0
SEGMENT_TTL = 60.0
_cache: dict[str, tuple[float, bytes]] = {}
_cache_lock = threading.Lock()


def strip_session(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    query = urllib.parse.parse_qsl(parts.query, keep_blank_values=True)
    query = [(k, v) for (k, v) in query if k.lower() != "nimblesessionid"]
    return urllib.parse.urlunsplit(
        (parts.scheme, parts.netloc, parts.path, urllib.parse.urlencode(query), parts.fragment)
    )


def _cache_get(key: tuple[str, bool], ttl: float) -> bytes | None:
    now = time.monotonic()
    with _cache_lock:
        item = _cache.get(key)
        if item and now - item[0] <= ttl:
            return item[1]
        if item:
            _cache.pop(key, None)
    return None


def _cache_put(key: tuple[str, bool], body: bytes) -> None:
    with _cache_lock:
        if len(_cache) >= 512:
            oldest = min(_cache.items(), key=lambda kv: kv[1][0])[0]
            _cache.pop(oldest, None)
        _cache[key] = (time.monotonic(), body)


def fetch(url: str, want_playlist: bool) -> bytes:
    """Fetch url through curl; retry without the CDN session id on failure."""
    key = (url, want_playlist)
    ttl = PLAYLIST_TTL if want_playlist else SEGMENT_TTL
    cached = _cache_get(key, ttl)
    if cached is not None:
        return cached

    last = b""
    for candidate in (url, strip_session(url)):
        if not candidate:
            continue
        proc = subprocess.run(CURL + [candidate], capture_output=True)
        if proc.returncode != 0 or not proc.stdout:
            last = proc.stderr or last
            continue
        body = proc.stdout
        if want_playlist and not body.lstrip().startswith(b"#EXTM3U"):
            last = body[:200]
            continue
        if not want_playlist and (len(body) < 256 or body.lstrip().startswith(b"<html")):
            last = body[:200]
            continue
        _cache_put(key, body)
        return body
    raise RuntimeError(last.decode("utf-8", "replace") or "fetch failed")


def rewrite(playlist: str, base_url: str, master: bool) -> str:
    out = []
    for line in playlist.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            out.append(line)
            continue
        absolute = urllib.parse.urljoin(base_url, stripped)
        quoted = urllib.parse.quote(absolute, safe="")
        out.append(f"/variant.m3u8?u={quoted}" if master else f"/segment.ts?u={quoted}")
    return "\n".join(out) + "\n"


class Handler(BaseHTTPRequestHandler):
    server_version = "hls-lan-proxy/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        print("%s - %s" % (self.address_string(), fmt % args), flush=True)

    def _send(self, body: bytes, ctype: str, head: bool = False) -> None:
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if body and not head:
            self.wfile.write(body)

    def do_HEAD(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlsplit(self.path)
        target = urllib.parse.parse_qs(parsed.query).get("u", [""])[0]
        if not target:
            self.send_error(400, "missing u=")
            return
        try:
            if parsed.path == "/segment.ts":
                cached = _cache_get((target, False), SEGMENT_TTL)
                length = len(cached) if cached else 0
                self.send_response(200)
                self.send_header("Content-Type", "video/mp2t")
                self.send_header("Content-Length", str(length))
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                return
            body = fetch(target, want_playlist=True)
            text = body.decode("utf-8", "replace")
            master = "#EXT-X-STREAM-INF" in text
            rewritten = rewrite(text, target, master).encode("utf-8")
            self._send(rewritten, "application/vnd.apple.mpegurl", head=True)
        except Exception as exc:  # pragma: no cover - diagnostic path
            print("proxy HEAD error:", exc, flush=True)
            self.send_error(502, str(exc))

    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlsplit(self.path)
        target = urllib.parse.parse_qs(parsed.query).get("u", [""])[0]
        if not target:
            self.send_error(400, "missing u=")
            return
        try:
            if parsed.path == "/segment.ts":
                body = fetch(target, want_playlist=False)
                self._send(body, "video/mp2t")
            else:
                body = fetch(target, want_playlist=True)
                text = body.decode("utf-8", "replace")
                master = "#EXT-X-STREAM-INF" in text
                rewritten = rewrite(text, target, master).encode("utf-8")
                self._send(rewritten, "application/vnd.apple.mpegurl")
        except Exception as exc:  # pragma: no cover - diagnostic path
            print("proxy error:", exc, flush=True)
            self.send_error(502, str(exc))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8099)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"hls-lan-proxy listening on http://{args.bind}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
