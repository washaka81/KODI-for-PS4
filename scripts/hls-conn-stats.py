#!/usr/bin/env python3
"""Mide conexiones nuevas frente a reutilizadas en el path HLS de FFmpeg.

Uso:
  ./scripts/hls-conn-stats.py build/logs-cs.log
  ./scripts/hls-conn-stats.py build/logs-*.log

Lee las líneas que FFmpeg deja en kodi.log:

  [AVFormatContext|hls|https] Opening '<url>' for reading
  [tcp] Starting connection attempt to <host> port <p>
  [tls] PS4TLS: full handshake to <host>
  [tls] PS4TLS: resumed handshake to <host>
  [https] Connection: keep-alive|close

FFmpeg registra `Opening` **antes** de abrir el socket, así que una
apertura es **nueva** cuando le sigue un `Starting connection attempt`
antes de la siguiente apertura del mismo hilo; si no, es
**reutilizada** (la conexión keep-alive ya estaba abierta). También
resume stalls y saltos por `resync forward`.

No necesita dependencias.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, deque
from dataclasses import dataclass, field
from datetime import datetime
from urllib.parse import urlsplit

TS_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})")
THREAD_RE = re.compile(r"\bT:(\d+)")
OPEN_RE = re.compile(r"ffmpeg\[[^\]]*\]: .*?Opening '([^']+)' for reading")
HANDSHAKE_RE = re.compile(r"PS4TLS: (full|resumed) handshake")
CONN_HDR_RE = re.compile(r"\[https\] Connection: (keep-alive|close)")
RESYNC_RE = re.compile(
    r"resync forward :(\d+), prev:([\d.]+), curr:([\d.]+), diff:([\d.]+)"
)
STALL = "Stream stalled (video)"
TCP_START = "Starting connection attempt"


@dataclass
class ThreadState:
    unresolved_opens: deque[datetime] = field(default_factory=deque)
    connect_start: datetime | None = None


@dataclass
class Stats:
    path: str
    first: datetime | None = None
    last: datetime | None = None
    urls: list[str] = field(default_factory=list)
    connects: int = 0
    new_opens: list[datetime] = field(default_factory=list)
    reused_opens: list[datetime] = field(default_factory=list)
    open_latencies: list[float] = field(default_factory=list)
    handshakes: list[tuple[str, float]] = field(default_factory=list)
    closes: int = 0
    keepalives: int = 0
    stalls: list[datetime] = field(default_factory=list)
    resyncs: list[tuple[datetime, float]] = field(default_factory=list)


def parse_time(line: str) -> datetime | None:
    match = TS_RE.match(line)
    if not match:
        return None
    try:
        return datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return None


def thread_of(line: str) -> str:
    match = THREAD_RE.search(line)
    return match.group(1) if match else "?"


def resolve_reuse(stats: Stats, state: ThreadState) -> None:
    while state.unresolved_opens:
        stats.reused_opens.append(state.unresolved_opens.popleft())


def read_events(path: str) -> Stats:
    stats = Stats(path)
    threads: dict[str, ThreadState] = {}

    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            when = parse_time(line)
            if when is None:
                continue

            if "ffmpeg[" not in line:
                if STALL in line:
                    stats.stalls.append(when)
                match = RESYNC_RE.search(line)
                if match:
                    stats.resyncs.append((when, float(match.group(4)) / 1e6))
                continue

            state = threads.setdefault(thread_of(line), ThreadState())

            if TCP_START in line:
                stats.connects += 1
                # The connect belongs to the opening logged just before it.
                if state.unresolved_opens:
                    stats.new_opens.append(state.unresolved_opens.popleft())
                state.connect_start = when
                continue

            match = OPEN_RE.search(line)
            if match:
                state.unresolved_opens.append(when)
                stats.urls.append(match.group(1))
                if stats.first is None:
                    stats.first = when
                stats.last = when
                continue

            match = HANDSHAKE_RE.search(line)
            if match:
                if state.connect_start is not None:
                    stats.handshakes.append(
                        (match.group(1), (when - state.connect_start).total_seconds())
                    )
                if stats.first is None:
                    stats.first = when
                stats.last = when
                continue

            match = CONN_HDR_RE.search(line)
            if match:
                if match.group(1) == "close":
                    stats.closes += 1
                else:
                    stats.keepalives += 1
                if state.connect_start is not None:
                    stats.open_latencies.append(
                        (when - state.connect_start).total_seconds()
                    )
                    state.connect_start = None
                if stats.first is None:
                    stats.first = when
                stats.last = when

    # An opening with no following connect was served by a live connection.
    for state in threads.values():
        resolve_reuse(stats, state)

    stats.new_opens.sort()
    stats.reused_opens.sort()
    return stats


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


def url_kind(url: str) -> str:
    path = urlsplit(url).path.lower()
    if path.endswith(".ts"):
        return "segmento .ts"
    if path.endswith(".m3u8"):
        return "playlist"
    return "otro"


def resync_groups(stats: Stats) -> list[tuple[datetime, float]]:
    """Agrupa las líneas de audio/vídeo del mismo salto (~50 ms)."""
    groups: list[tuple[datetime, float]] = []
    for when, diff in stats.resyncs:
        if groups and abs((when - groups[-1][0]).total_seconds()) <= 0.05:
            groups[-1] = (groups[-1][0], max(groups[-1][1], diff))
        else:
            groups.append((when, diff))
    return groups


def fmt(value: float) -> str:
    return f"{value:.2f}"


def report(stats: Stats) -> None:
    total = len(stats.urls)
    reused = len(stats.reused_opens)
    kinds = Counter(url_kind(url) for url in stats.urls)
    full = [t for kind, t in stats.handshakes if kind == "full"]
    resumed = [t for kind, t in stats.handshakes if kind == "resumed"]
    groups = resync_groups(stats)
    skipped = sum(diff for _, diff in groups)

    duration = (stats.last - stats.first).total_seconds() if stats.first else 0.0
    print(f"== {stats.path}")
    if stats.first:
        print(
            f"   ventana {stats.first.strftime('%H:%M:%S')} - "
            f"{stats.last.strftime('%H:%M:%S')} ({fmt(duration)} s)"
        )
    print(
        "   aperturas FFmpeg: "
        + ", ".join(f"{count} {kind}" for kind, count in kinds.most_common())
    )
    share = 100.0 * reused / total if total else 0.0
    print(
        f"   conexiones: {stats.connects} nuevas, {reused} reutilizadas "
        f"({share:.0f} % de {total} aperturas)"
    )
    if stats.open_latencies:
        print(
            "   coste conexion->respuesta: "
            f"p50 {fmt(percentile(stats.open_latencies, 0.50))} s, "
            f"p95 {fmt(percentile(stats.open_latencies, 0.95))} s, "
            f"max {fmt(max(stats.open_latencies))} s"
        )
    if stats.handshakes:
        parts = []
        if full:
            parts.append(f"{len(full)} full p50 {fmt(percentile(full, 0.50))} s")
        if resumed:
            parts.append(
                f"{len(resumed)} resumed p50 {fmt(percentile(resumed, 0.50))} s"
            )
        print("   handshakes: " + ", ".join(parts))
    print(f"   Connection: keep-alive {stats.keepalives}, close {stats.closes}")
    if stats.reused_opens and stats.first:
        print(
            "   primer reuso: "
            f"{fmt((stats.reused_opens[0] - stats.first).total_seconds())} s "
            f"tras la primera apertura"
        )
    print(f"   stalls de video: {len(stats.stalls)}")
    if groups:
        jumps = ", ".join(f"{fmt(diff)} s" for _, diff in groups)
        print(f"   resync forward: {len(groups)} saltos ({jumps}); total {fmt(skipped)} s")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Conexiones nuevas/reutilizadas del HLS de FFmpeg en kodi.log"
    )
    parser.add_argument("logs", nargs="+", help="kodi.log u hojas de log")
    args = parser.parse_args()

    for path in args.logs:
        try:
            report(read_events(path))
        except OSError as error:
            print(f"{path}: {error}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
