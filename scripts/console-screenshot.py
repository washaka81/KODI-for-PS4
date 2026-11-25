#!/usr/bin/env python3
"""Descarga la última captura de pantalla de la PS4.

Busca primero en la galería del sistema (capturas hechas con Share) y luego
en las rutas donde Kodi guarda sus screenshots.  No necesita dependencias.

Uso:
  ./scripts/console-screenshot.py [destino]

Variables:
  CONSOLE=192.0.2.11   IP de la consola
  CONSOLE_PORT=2121      puerto FTP de GoldHEN
"""

from __future__ import annotations

import os
import re
import sys
import time
from ftplib import FTP

CONSOLE = os.environ.get("CONSOLE", "192.0.2.11")
CONSOLE_PORT = int(os.environ.get("CONSOLE_PORT", "2121"))
DEFAULT_DEST = os.path.join("build", "screenshots")

# Orden de búsqueda: galería del sistema y luego Kodi.
ROOTS = [
    "/user/av_contents/photo/NPXS20001/FGNM00012",
    "/user/av_contents/photo/NPXS20001/FGNM00011",
    "/data/kodi-ps4",
    "/data/kodi-ps4/.kodi/temp",
]

IMAGE_RE = re.compile(r"\.(jpe?g|png)$", re.IGNORECASE)
MONTHS = ["jan", "feb", "mar", "apr", "may", "jun",
          "jul", "aug", "sep", "oct", "nov", "dec"]


class Console:
    def __init__(self) -> None:
        self.ftp = FTP()
        self.ftp.connect(CONSOLE, CONSOLE_PORT, timeout=20)
        self.ftp.login()

    def close(self) -> None:
        try:
            self.ftp.quit()
        except Exception:
            pass

    def listdir(self, path: str) -> list[tuple[str, int, str, bool]]:
        try:
            lines: list[str] = []
            self.ftp.cwd(path)
            self.ftp.retrlines("LIST", lines.append)
        except Exception:
            return []

        entries = []
        for line in lines:
            parts = line.split()
            if len(parts) < 9 or parts[0].startswith("total"):
                continue
            name = parts[-1]
            if name in (".", ".."):
                continue
            try:
                size = int(parts[4])
            except ValueError:
                size = 0
            entries.append((name, size, " ".join(parts[5:8]), parts[0].startswith("d")))
        return entries

    def get(self, path: str) -> bytes | None:
        buf = bytearray()
        try:
            self.ftp.retrbinary(f"RETR {path}", buf.extend)
        except Exception:
            return None
        return bytes(buf)


def stamp_to_epoch(stamp: str) -> float:
    """Convierte una fecha de LIST (Unix) a epoch aproximado."""
    parts = stamp.split()
    if len(parts) < 3:
        return 0.0
    mon, day, third = parts[0].lower(), parts[1], parts[2]
    if mon not in MONTHS:
        return 0.0
    mi = MONTHS.index(mon) + 1
    try:
        if third.isdigit() and len(third) == 4:
            return time.mktime(time.strptime(f"{third}-{mi:02d}-{int(day):02d}", "%Y-%m-%d"))
        return time.mktime(time.strptime(
            f"{time.localtime().tm_year}-{mi:02d}-{int(day):02d} {int(third):02d}:00",
            "%Y-%m-%d %H:%M"))
    except Exception:
        return 0.0


def walk(console: Console, path: str, depth: int = 2):
    for name, size, stamp, is_dir in console.listdir(path):
        remote = f"{path}/{name}"
        if is_dir:
            if depth > 0:
                yield from walk(console, remote, depth - 1)
        elif IMAGE_RE.search(name):
            yield remote, name, size, stamp


def main() -> int:
    dest = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DEST
    os.makedirs(dest, exist_ok=True)

    try:
        console = Console()
    except OSError as exc:
        print(f"ERROR: no se pudo conectar a {CONSOLE}:{CONSOLE_PORT}: {exc}", file=sys.stderr)
        return 1

    try:
        candidates = []
        for root in ROOTS:
            for remote, name, size, stamp in walk(console, root):
                candidates.append((stamp_to_epoch(stamp), remote, name, size))
            # La galería del sistema es suficiente: no hace falta recorrer
            # también Kodi si ya hay capturas con Share.
            if candidates:
                break

        if not candidates:
            print("No se encontraron capturas .jpg/.png en las rutas conocidas.")
            print("Rutas probadas:")
            for root in ROOTS:
                print(f"  {root}")
            return 2

        # El listado FTP solo da la hora (no minutos); el nombre de Share
        # lleva la fecha completa, así que se usa como desempate.
        candidates.sort(key=lambda c: (c[0], c[2]), reverse=True)
        _, remote, name, size = candidates[0]
        data = console.get(remote)
        if data is None:
            print(f"ERROR: no se pudo descargar {remote}", file=sys.stderr)
            return 3

        local = os.path.join(dest, name)
        with open(local, "wb") as fh:
            fh.write(data)
        print(f"Captura: {remote}")
        print(f"Guardada: {local} ({size} bytes)")
        return 0
    finally:
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
