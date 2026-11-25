#!/usr/bin/env python3
"""Resume las métricas TLS/HTTP de la PS4 en un kodi.log.

Uso:
  ./scripts/ps4-tls-metrics.py build/logs-*/kodi.log

Reconoce:
  PS4HTTPNATIVE  -> libSceHttp/libSceSsl (send_ms, first_read_ms, total_ms,
                    status, clen, bytes, err)
  PS4TLSTEST     -> CPython _ssl/OpenSSL (openssl, master/media)
  PS4TLS         -> mbedTLS de FFmpeg (full/resumed, handshake-start/end)
  PS4HTTP        -> path Curl (connect -> primer header)
"""

from __future__ import annotations

import argparse
import re
import sys

NATIVE = re.compile(
    r"PS4HTTPNATIVE (?P<label>\w+) host=(?P<host>\S+) send_ms=(?P<send>\d+) "
    r"first_read_ms=(?P<first>\d+) total_ms=(?P<total>\d+) status=(?P<status>\d+) "
    r"clen=(?P<clen>\d+) bytes=(?P<bytes>\d+) err=0x(?P<err>[0-9a-fA-F]+)"
)
PY_OPENSSL = re.compile(r"PS4TLSTEST: openssl=(?P<ver>.+)")
PY_FETCH = re.compile(
    r"PS4TLSTEST: (?P<what>master|media) ?(?:wait=(?P<wait>\d+) )?"
    r"(?P<status>\d+) (?P<bytes>\d+)B (?P<seconds>[\d.]+)s"
)
PY_ERROR = re.compile(r"PS4TLSTEST: (?P<error>ERROR|autoexec) (?P<detail>.*)")
MBED_HANDSHAKE = re.compile(r"PS4TLS: (?P<kind>full|resumed) handshake to (?P<host>\S+)")
CURL_CONNECT = re.compile(r"PS4HTTP connect this")
CURL_HEADER = re.compile(r"PS4HTTP header n=1 ")


def summarize(path: str) -> None:
    native = []
    py_openssl = []
    py_fetches = []
    py_errors = []
    mbed = []
    curl_connects = []
    last_http = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = NATIVE.search(line)
            if match:
                native.append(match.groupdict())
                continue
            match = PY_OPENSSL.search(line)
            if match:
                py_openssl.append(match.group("ver"))
                continue
            match = PY_FETCH.search(line)
            if match:
                py_fetches.append(match.groupdict())
                continue
            match = PY_ERROR.search(line)
            if match:
                py_errors.append((match.group("error"), match.group("detail")[:100]))
                continue
            match = MBED_HANDSHAKE.search(line)
            if match:
                mbed.append(match.groupdict())
                continue
            if CURL_CONNECT.search(line):
                last_http = "connect"
                continue
            if CURL_HEADER.search(line) and last_http == "connect":
                curl_connects.append(line.strip()[-120:])
                last_http = None

    print(f"== {path}")
    print(f"   PS4HTTPNATIVE: {len(native)}")
    for item in native:
        print(
            f"     {item['label']:6} {item['host']:34} "
            f"send={item['send']}ms first={item['first']}ms total={item['total']}ms "
            f"status={item['status']} clen={item['clen']} bytes={item['bytes']} "
            f"err=0x{item['err']}"
        )
    print(f"   PS4TLSTEST (CPython/OpenSSL): {len(py_fetches)} fetches")
    for version in py_openssl:
        print(f"     openssl={version}")
    for item in py_fetches:
        wait = f" wait={item['wait']}s" if item.get("wait") else ""
        print(
            f"     {item['what']:6}{wait} status={item['status']} "
            f"bytes={item['bytes']} {item['seconds']}s"
        )
    for error, detail in py_errors:
        print(f"     {error}: {detail}")
    print(f"   mbedTLS handshakes: {len(mbed)}")
    for item in mbed:
        print(f"     {item['kind']:7} {item['host']}")
    if curl_connects:
        print(f"   Curl connect->header: {len(curl_connects)}")
        for line in curl_connects:
            print(f"     ...{line}")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description="Métricas TLS/HTTP de la PS4")
    parser.add_argument("logs", nargs="+", help="kodi.log u hojas")
    args = parser.parse_args()
    for path in args.logs:
        try:
            summarize(path)
        except OSError as error:
            print(f"{path}: {error}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
