#!/usr/bin/env python3
"""Mide el TTFB de cada petición HTTP de FFmpeg en un kodi.log.

Uso:
  ./scripts/hls-ttfb-stats.py build/logs-ct-*/kodi.log

Para cada `[https] request: GET` toma el tiempo hasta el primer
`[https] header='HTTP/1.x ...'` de la respuesta. Separa las peticiones
que abren conexión nueva (les precede un `[tcp] Starting connection
attempt`) de las que reutilizan una conexión keep-alive, porque en la
consola el TTFB es ~5 s solo en las reutilizadas.

No necesita dependencias.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from datetime import datetime
from urllib.parse import urlsplit

TS_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})")
REQ_RE = re.compile(r"\[https\] request: GET (\S+)")
HDR_RE = re.compile(r"\[https\] header='HTTP/1\.[01] (\d{3})")
TCP_START = "Starting connection attempt"


def parse_time(line: str) -> datetime | None:
    match = TS_RE.match(line)
    if not match:
        return None
    try:
        return datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return None


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = (len(ordered) - 1) * pct
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def kind(url: str) -> str:
    path = urlsplit(url).path.lower()
    if path.endswith(".ts"):
        return "segmento"
    if path.endswith(".m3u8"):
        return "playlist"
    return "otro"


def analyze(path: str) -> int:
    pending_new = False
    current: dict | None = None
    rows: list[tuple[bool, float, str, int | None]] = []

    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            when = parse_time(line)
            if when is None:
                continue
            if TCP_START in line:
                pending_new = True
                continue

            match = REQ_RE.search(line)
            if match:
                current = {"t": when, "new": pending_new, "url": match.group(1)}
                pending_new = False
                continue

            if current is None:
                continue
            match = HDR_RE.search(line)
            if match and "header=" in line:
                rows.append(
                    (
                        current["new"],
                        (when - current["t"]).total_seconds(),
                        kind(current["url"]),
                        int(match.group(1)),
                    )
                )
                current = None

    if not rows:
        print(f"{path}: sin peticiones HLS con respuesta")
        return 0

    print(f"== {path}: {len(rows)} peticiones")
    for label, flag in (("nuevas", True), ("reutilizadas", False)):
        values = [ttfb for new, ttfb, _, _ in rows if new is flag]
        if not values:
            continue
        print(
            f"   {label}: n={len(values)} p50 {statistics.median(values):.2f} s  "
            f"p95 {percentile(values, 0.95):.2f} s  max {max(values):.2f} s"
        )

    by_kind: dict[tuple[bool, str], list[float]] = {}
    for new, ttfb, url_kind, _ in rows:
        by_kind.setdefault((new, url_kind), []).append(ttfb)
    for (new, url_kind), values in sorted(by_kind.items()):
        label = "nueva" if new else "reutilizada"
        print(
            f"   {url_kind} {label}: n={len(values)} "
            f"p50 {statistics.median(values):.2f} s"
        )

    slow = [row for row in rows if row[1] > 2.0 and not row[0]]
    print(f"   reutilizadas con TTFB > 2 s: {len(slow)}")
    for new, ttfb, url_kind, status in slow[:5]:
        print(f"     {ttfb:.2f} s ({url_kind}, HTTP {status})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="TTFB por petición HLS nueva/reutilizada en kodi.log"
    )
    parser.add_argument("logs", nargs="+", help="kodi.log u hojas de log")
    args = parser.parse_args()

    for path in args.logs:
        try:
            analyze(path)
        except OSError as error:
            print(f"{path}: {error}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
