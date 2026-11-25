#!/usr/bin/env python3
"""Barrido de canales PVR de Kodi por JSON-RPC.

Requiere el servidor web de Kodi activo (Settings -> Services -> Control
-> Allow remote control via HTTP).  Abre cada canal, comprueba que el
reproductor avanza y lo cierra; no modifica canales ni configuración.

Uso:
  ./scripts/pvr-stream-sweep.py --url http://127.0.0.1:8080/jsonrpc \
      --limit 20 --settle 8 --out build/pvr-sweep.json

  ./scripts/pvr-stream-sweep.py --channel 106   # TVN 24 Horas
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
import urllib.error
import urllib.request


class RpcError(RuntimeError):
    pass


class KodiRpc:
    def __init__(self, url: str, user: str | None, password: str | None):
        self.url = url
        self.auth = None
        if user is not None:
            token = base64.b64encode(f"{user}:{password or ''}".encode()).decode()
            self.auth = f"Basic {token}"
        self.next_id = 0

    def call(self, method: str, params: dict | None = None):
        self.next_id += 1
        payload = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {},
            "id": self.next_id,
        }
        request = urllib.request.Request(
            self.url,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
        )
        if self.auth:
            request.add_header("Authorization", self.auth)
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                body = json.load(response)
        except (urllib.error.URLError, TimeoutError) as error:
            raise RpcError(f"{method}: {error}") from error
        if "error" in body:
            raise RpcError(f"{method}: {body['error']}")
        return body["result"]


def time_seconds(value: dict | None) -> float:
    if not value:
        return 0.0
    return (
        value.get("hours", 0) * 3600
        + value.get("minutes", 0) * 60
        + value.get("seconds", 0)
        + value.get("milliseconds", 0) / 1000.0
    )


def tv_group(rpc: KodiRpc) -> int:
    groups = rpc.call("PVR.GetChannelGroups", {"channeltype": "tv"})["groups"]
    if not groups:
        raise RpcError("no hay grupos de canales TV")
    for group in groups:
        if group.get("label", "").lower() in ("all channels", "todos los canales"):
            return group["channelgroupid"]
    return groups[0]["channelgroupid"]


def channels(rpc: KodiRpc, group: int) -> list[dict]:
    return rpc.call(
        "PVR.GetChannels",
        {
            "channelgroupid": group,
            "properties": [
                "channelnumber",
                "channel",
                "thumbnail",
                "hidden",
                "locked",
                "lastplayed",
            ],
        },
    )["channels"]


def player_state(rpc: KodiRpc) -> dict:
    return rpc.call(
        "Player.GetProperties",
        {"playerid": 1, "properties": ["speed", "time", "totaltime", "percentage", "paused"]},
    )


def test_channel(rpc: KodiRpc, channel: dict, settle: float) -> dict:
    result = {
        "channelid": channel.get("channelid"),
        "label": channel.get("label"),
        "channelnumber": channel.get("channelnumber"),
        "ok": False,
        "error": None,
        "speed": None,
        "start": None,
        "after": None,
    }
    try:
        rpc.call("Player.Open", {"item": {"channelid": channel["channelid"]}})
        time.sleep(settle)
        first = player_state(rpc)
        time.sleep(2.0)
        second = player_state(rpc)
        start = time_seconds(first.get("time"))
        after = time_seconds(second.get("time"))
        result.update(
            {
                "speed": second.get("speed"),
                "start": start,
                "after": after,
                "ok": bool(second.get("speed", 0) > 0 and after >= start),
            }
        )
        if not result["ok"]:
            result["error"] = "sin avance"
    except RpcError as error:
        result["error"] = str(error)
    finally:
        try:
            rpc.call("Player.Stop", {"playerid": 1})
        except RpcError:
            pass
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Barrido PVR por JSON-RPC")
    parser.add_argument("--url", default="http://127.0.0.1:8080/jsonrpc")
    parser.add_argument("--user", default=None, help="usuario HTTP (p. ej. kodi)")
    parser.add_argument("--password", default=None)
    parser.add_argument("--channel", type=int, help="channelid concreto")
    parser.add_argument("--start", type=int, default=0, help="índice inicial")
    parser.add_argument("--limit", type=int, default=20, help="máximo de canales; 0 = todos")
    parser.add_argument("--settle", type=float, default=8.0, help="segundos por canal")
    parser.add_argument("--out", default="build/pvr-sweep.json")
    args = parser.parse_args()

    rpc = KodiRpc(args.url, args.user, args.password)
    try:
        rpc.call("JSONRPC.Ping")
        version = rpc.call("Application.GetProperties", {"properties": ["version"]})
    except RpcError as error:
        print(f"error: {error}", file=sys.stderr)
        print("activa Settings -> Services -> Control -> Allow remote control via HTTP", file=sys.stderr)
        return 1

    if args.channel is not None:
        selection = [{"channelid": args.channel, "label": f"id {args.channel}"}]
    else:
        group = tv_group(rpc)
        selection = channels(rpc, group)
        selection = selection[args.start :]
        if args.limit:
            selection = selection[: args.limit]

    results = []
    for index, channel in enumerate(selection, start=1):
        result = test_channel(rpc, channel, args.settle)
        results.append(result)
        status = "OK" if result["ok"] else "FAIL"
        print(
            f"[{index}/{len(selection)}] {status} "
            f"{result['channelnumber']} {result['label']} "
            f"speed={result['speed']} t={result['start']}->{result['after']} "
            f"{result['error'] or ''}",
            flush=True,
        )

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump({"kodi": version, "results": results}, handle, ensure_ascii=False, indent=2)
    ok = sum(1 for item in results if item["ok"])
    print(f"{ok}/{len(results)} OK -> {args.out}")
    return 0 if ok == len(results) else 2


if __name__ == "__main__":
    raise SystemExit(main())
