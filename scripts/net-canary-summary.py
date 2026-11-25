#!/usr/bin/env python3
"""Resume las líneas PS4NET del canary de red de Kodi/PS4.

Uso:
  ./scripts/net-canary-summary.py kodi.log
  ./scripts/net-canary-summary.py build/logs-*/data/kodi-ps4/.kodi/temp/kodi.log

No necesita dependencias. Agrupa por host, calcula p50/p95 de DNS, connect,
TLS y TTFB, y resume MSS, retransmisiones y flags. También entiende las
líneas antiguas `CCurlFile::Stat - slow transfer ...`.
"""

from __future__ import annotations

import argparse
import datetime
import re
import statistics
import sys
from collections import Counter, defaultdict
from urllib.parse import urlsplit

PAIR_RE = re.compile(r"(\w+)=([^\s]+)")
TS_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})")
SLOW_RE = re.compile(
    r"slow transfer total=(?P<total>\d+) ms dns=(?P<dns>\d+) ms "
    r"connect=(?P<connect>\d+) ms tls=(?P<tls>\d+) ms"
)


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = (len(ordered) - 1) * pct
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    frac = index - low
    return ordered[low] * (1.0 - frac) + ordered[high] * frac


def number(pairs: dict[str, str], key: str) -> float | None:
    try:
        return float(pairs[key])
    except (KeyError, ValueError):
        return None


def host_of(url: str) -> str:
    try:
        host = urlsplit(url).hostname
        return host or url
    except ValueError:
        return url


def parse(lines: list[str]) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]:
    posts: list[dict[str, object]] = []
    pres: list[dict[str, object]] = []
    socks: list[dict[str, object]] = []
    last_sock: dict[str, datetime.datetime] = {}
    for line in lines:
        if "PS4NET canary" not in line:
            continue
        pairs = dict(PAIR_RE.findall(line))
        phase = pairs.get("phase")
        stamp = TS_RE.match(line)
        t = (
            datetime.datetime.strptime(stamp.group(1), "%Y-%m-%d %H:%M:%S.%f")
            if stamp
            else None
        )
        if phase == "post":
            posts.append(
                {
                    "fn": pairs.get("fn", "?"),
                    "url": pairs.get("url", "?"),
                    "host": host_of(pairs.get("url", "?")),
                    "total": number(pairs, "total") or 0.0,
                    "dns": number(pairs, "dns") or 0.0,
                    "connect": number(pairs, "connect") or 0.0,
                    "tls": number(pairs, "tls") or 0.0,
                    "ttfb": number(pairs, "ttfb") or 0.0,
                    "mss": int(number(pairs, "mss") or -1),
                    "snd_mss": int(number(pairs, "snd_mss") or 0),
                    "rcv_mss": int(number(pairs, "rcv_mss") or 0),
                    "pmtu": int(number(pairs, "pmtu") or 0),
                    "retrans": int(number(pairs, "retrans") or 0),
                    "conns": int(number(pairs, "conns") or -1),
                    "flags": pairs.get("flags", ""),
                }
            )
        elif phase == "sock":
            fd = pairs.get("fd")
            if fd:
                socks.append({"fd": fd, "t": t})
                if t is not None:
                    last_sock[fd] = t
        elif phase == "pre":
            fd = pairs.get("fd")
            handshake = None
            if t is not None and fd is not None and fd in last_sock:
                handshake = (t - last_sock[fd]).total_seconds()
            pres.append(
                {
                    "mss": int(number(pairs, "mss") or -1),
                    "snd_mss": int(number(pairs, "snd_mss") or 0),
                    "rcv_mss": int(number(pairs, "rcv_mss") or 0),
                    "pmtu": int(number(pairs, "pmtu") or 0),
                    "retrans": int(number(pairs, "retrans") or 0),
                    "fd": fd,
                    "t": t,
                    "handshake": handshake,
                }
            )
    return posts, pres, socks


def summarize(posts: list[dict[str, object]], pres: list[dict[str, object]],
              socks: list[dict[str, object]]) -> None:
    if not posts and not pres:
        print("No hay líneas PS4NET. ¿Está creado /data/kodi-ps4/net-canary y reiniciaste Kodi?")
        return

    by_host: dict[str, list[dict[str, object]]] = defaultdict(list)
    for post in posts:
        by_host[str(post["host"])].append(post)

    print(f"Peticiones con post: {len(posts)}  hosts: {len(by_host)}")
    print()
    print(f"{'host':44} {'n':>3} {'p50_tot':>8} {'p95_tot':>8} {'p50_tls':>8} {'p95_tls':>8} {'p50_ttfb':>9} {'mss':>5} {'retr':>5}")
    print("-" * 120)
    for host, items in sorted(by_host.items(), key=lambda kv: -len(kv[1])):
        totals = [float(x["total"]) for x in items]
        tlss = [float(x["tls"]) for x in items]
        ttfbs = [float(x["ttfb"]) for x in items]
        mss = max((int(x["snd_mss"]) for x in items), default=0)
        retr = sum(int(x["retrans"]) for x in items)
        print(
            f"{host[:44]:44} {len(items):3d} "
            f"{statistics.median(totals):8.0f} {percentile(totals, 0.95):8.0f} "
            f"{statistics.median(tlss):8.0f} {percentile(tlss, 0.95):8.0f} "
            f"{statistics.median(ttfbs):9.0f} {mss:5d} {retr:5d}"
        )

    flag_counts = Counter()
    for post in posts:
        for flag in str(post["flags"]).split(","):
            if flag:
                flag_counts[flag] += 1
    if flag_counts:
        print()
        print("Flags:")
        for flag, count in flag_counts.most_common():
            print(f"  {flag:14} {count}")

    if posts:
        print()
        print("Top 10 por total:")
        for post in sorted(posts, key=lambda x: -float(x["total"]))[:10]:
            print(
                f"  {float(post['total']):7.0f} ms  dns={float(post['dns']):5.0f} "
                f"conn={float(post['connect']):5.0f} tls={float(post['tls']):6.0f} "
                f"ttfb={float(post['ttfb']):6.0f}  {post['fn']:10} {post['url'][:80]}"
            )

    mss_values = [int(x["mss"]) for x in pres if int(x["mss"]) > 0]
    snd_values = [int(x["snd_mss"]) for x in pres if int(x["snd_mss"]) > 0]
    if mss_values:
        print()
        print(
            "MSS pre (phase=pre): "
            f"min={min(mss_values)} max={max(mss_values)} "
            f"moda={Counter(mss_values).most_common(1)[0][0]}"
        )
    if snd_values:
        print(
            "snd_mss (TCP_INFO): "
            f"min={min(snd_values)} max={max(snd_values)} "
            f"moda={Counter(snd_values).most_common(1)[0][0]}"
        )
    handshakes: list[float] = []
    for p in pres:
        value = p.get("handshake")
        if value is None:
            continue
        delta = float(value)  # type: ignore[arg-type]
        if 0.0 <= delta < 120.0:
            handshakes.append(delta)
    if handshakes:
        print()
        print(
            f"Handshake TCP+TLS (sock→pre): n={len(handshakes)} "
            f"p50={statistics.median(handshakes):.3f} s "
            f"p95={percentile(handshakes, 0.95):.3f} s "
            f"min={min(handshakes):.3f} s max={max(handshakes):.3f} s"
        )
    if any(int(x["retrans"]) > 0 for x in pres):
        print("Retransmisiones en pre: " + ", ".join(
            f"{int(x['retrans'])}" for x in pres if int(x["retrans"]) > 0
        )[:200])


def summarize_old(lines: list[str]) -> None:
    old = []
    for line in lines:
        match = SLOW_RE.search(line)
        if match:
            old.append({k: int(v) for k, v in match.groupdict().items()})
    if not old:
        return
    print()
    print(f"Líneas antiguas 'slow transfer': {len(old)}")
    totals = [float(x["total"]) for x in old]
    tlss = [float(x["tls"]) for x in old]
    print(
        f"  total p50={statistics.median(totals):.0f} ms p95={percentile(totals, 0.95):.0f} ms; "
        f"tls p50={statistics.median(tlss):.0f} ms p95={percentile(tlss, 0.95):.0f} ms"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", default="kodi.log")
    args = parser.parse_args()
    try:
        with open(args.log, encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
    except OSError as error:
        print(f"no puedo leer {args.log}: {error}", file=sys.stderr)
        return 1

    posts, pres, socks = parse(lines)
    summarize(posts, pres, socks)
    summarize_old(lines)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
