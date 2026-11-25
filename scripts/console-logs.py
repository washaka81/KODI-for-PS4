#!/usr/bin/env python3
"""Trae y analiza los logs de la PS4.

El core de Kodi no arranca en la consola (CE-34878-0) y el motivo hay que
deducirlo de los logs que deja el sistema:

  /data/kodi-ps4/core-canary.log  traza de arranque del core
  /data/kodi-ps4/init.log        arranque de la app M0
  /data/kodi-ps4/render.log      bucle de render de la app M0
  /data/kodi-ps4/.kodi/temp/kodi.log   log de Kodi
  /data/Loader_Logs.txt          log del loader de PS4 (verificacion de firma)
  /data/sce_coredumps/           coredumps de las apps

Uso:
  ./scripts/console-logs.py fetch [destino]     # solo descargar
  ./scripts/console-logs.py analyze [destino]   # solo analizar lo descargado
  ./scripts/console-logs.py wipe                # borrar los logs de las apps
  ./scripts/console-logs.py all [destino]       # descargar + analizar
  ./scripts/console-logs.py watch               # seguir la traza en vivo

Opciones:
  --minutes N    solo logs modificados en los ultimos N minutos (0 = todos)
  --max-size N   saltar ficheros mayores de N bytes (por defecto 64 MiB)
"""

from __future__ import annotations

import argparse
import ftplib
import os
import re
import struct
import sys
import time
from ftplib import FTP

CONSOLE = os.environ.get("CONSOLE", "192.0.2.11")
CONSOLE_PORT = int(os.environ.get("CONSOLE_PORT", "2121"))
DEFAULT_MAX_AGE = 180
# core-canary.log crece ~10 KiB/s mientras Kodi sigue vivo (17-23 MiB en
# 20-30 min).  El limite antiguo de 2 MiB lo saltaba y parecia que no existia.
DEFAULT_MAX_SIZE = 64 * 1024 * 1024

# Rutas que interesan, en orden de importancia.
INTERESTING = [
    "/data/kodi-ps4",
    "/data/Loader_Logs.txt",
    "/data/sce_coredumps",
]

# Ficheros que nunca aportan (cifrados o enormes).
SKIP_NAMES = re.compile(r"^(log\.bin|.*\.sprx|.*\.prx|rsa\.pub)$")


class Console:
    """Cliente FTP de la consola (GoldHEN: anonymous sin password)."""

    def __init__(self, host: str = CONSOLE, port: int = CONSOLE_PORT):
        self.ftp = FTP()
        self.ftp.connect(host, port, timeout=20)
        self.ftp.login()

    def close(self) -> None:
        try:
            self.ftp.quit()
        except Exception:
            pass

    def listdir(self, path: str) -> list[tuple[str, int, str, bool]]:
        """Devuelve (nombre, tamaño, fecha, es_directorio) de un directorio."""
        try:
            lines = []
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
            is_dir = parts[0].startswith("d")
            stamp = " ".join(parts[5:8])
            entries.append((name, size, stamp, is_dir))
        return entries

    def get(self, path: str) -> bytes | None:
        buf = bytearray()
        try:
            self.ftp.retrbinary(f"RETR {path}", buf.extend)
        except Exception:
            return None
        return bytes(buf)

    def delete(self, path: str) -> bool:
        try:
            self.ftp.delete(path)
            return True
        except Exception:
            return False


def age_minutes(stamp: str) -> int:
    """Antiguedad en minutos de una fecha del listado FTP."""
    months = ["jan", "feb", "mar", "apr", "may", "jun",
              "jul", "aug", "sep", "oct", "nov", "dec"]
    parts = stamp.split()
    if len(parts) < 3:
        return 10**6
    mon, day = parts[0].lower(), parts[1]
    if mon not in months:
        return 10**6
    mi = months.index(mon) + 1
    third = parts[2]
    try:
        if third.isdigit() and len(third) == 4:      # año
            when = time.mktime(time.strptime(f"{third}-{mi:02d}-{int(day):02d}", "%Y-%m-%d"))
        else:                                          # hora
            when = time.mktime(time.strptime(
                f"{time.localtime().tm_year}-{mi:02d}-{int(day):02d} {int(third):02d}:00",
                "%Y-%m-%d %H:%M"))
    except Exception:
        return 10**6
    return int((time.time() - when) / 60)


def fetch(console: Console, remote: str, local: str, max_age: int, max_size: int,
          quiet: bool = False, force_dir: bool = False) -> int:
    """Descarga un fichero o un directorio recursivamente.  Devuelve los bytes."""
    if force_dir or (not os.path.isdir(local)
                     and "." not in os.path.basename(local)):
        entries = console.listdir(remote)
        if not entries:
            if not quiet:
                print(f"  -- {remote}: no existe")
            return 0
        if not quiet:
            print(f"  -- {remote}/")
        total = 0
        for name, size, stamp, is_dir in entries:
            if SKIP_NAMES.match(name) or size > max_size:
                if not quiet:
                    print(f"     (se salta {name}: {size} bytes)")
                continue
            if max_age and age_minutes(stamp) > max_age:
                if not quiet:
                    print(f"     (se salta {name}: hace {age_minutes(stamp)} min)")
                continue
            sub_remote = f"{remote.rstrip('/')}/{name}"
            sub_local = os.path.join(local, name)
            if is_dir:
                # .kodi contiene un punto: sin force_dir el heuristic de
                # arriba la trataria como fichero y no bajaria temp/kodi.log.
                total += fetch(console, sub_remote, sub_local, max_age, max_size,
                               quiet, True)
            else:
                data = console.get(sub_remote)
                if data is None:
                    continue
                os.makedirs(os.path.dirname(sub_local), exist_ok=True)
                with open(sub_local, "wb") as fh:
                    fh.write(data)
                total += len(data)
                if not quiet:
                    print(f"     {name}: {len(data)} bytes")
        return total

    data = console.get(remote)
    if data is None:
        if not quiet:
            print(f"  -- {remote}: no se pudo descargar")
        return 0
    os.makedirs(os.path.dirname(local) or ".", exist_ok=True)
    with open(local, "wb") as fh:
        fh.write(data)
    if not quiet:
        print(f"  -- {remote}: {len(data)} bytes")
    return len(data)


def read_text(path: str) -> str:
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


# --------------------------------------------------------------------------
# Analisis
# --------------------------------------------------------------------------

def elf_symbols(path: str) -> list[tuple[int, str]]:
    """Simbolos (direccion, nombre) de un ELF, ordenados por direccion."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return []
    if data[:4] != b"\x7fELF":
        return []
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
                for i in range(e_shnum)]
    shstr = sections[e_shstrndx][4]

    def name_of(section) -> str:
        end = data.find(b"\0", shstr + section[0])
        return data[shstr + section[0]:end].decode("utf-8", "replace")

    by_name = {name_of(s): s for s in sections}
    sym = by_name.get(".symtab")
    if sym is None:
        return []
    strtab = sections[sym[6]][4]
    out = []
    for i in range(sym[5] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", data, sym[4] + i * 24)
        if not st_value:
            continue
        end = data.find(b"\0", strtab + st_name)
        name = data[strtab + st_name:end].decode("utf-8", "replace")
        if name:
            out.append((st_value, name))
    out.sort()
    return out


def resolve(symbols: list[tuple[int, str]], addr: int) -> str:
    """Funcion que contiene la direccion (si se puede)."""
    if not symbols:
        return "?"
    lo, hi = 0, len(symbols) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        value, name = symbols[mid]
        if value <= addr:
            best = (value, name)
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return "?"
    return f"{best[1]}+0x{addr - best[0]:x}"


def remote_file(dest: str, path: str) -> str:
    """Ruta de un fichero descargado de la consola.

    Los ficheros remotos se guardan con su ruta completa (/data/... -> dest/data/...),
    pero algunos (los que se copian sueltos) quedan en el raiz del destino.
    """
    rel = path.lstrip("/")
    for candidate in (os.path.join(dest, rel), os.path.join(dest, os.path.basename(rel))):
        if os.path.exists(candidate):
            return candidate
    return os.path.join(dest, rel)


def constructor_candidates(number: int) -> list[str]:
    """Constructores que pueden ser el culpable segun la ultima marca del canario.

    El informe lo escribe scripts/patch-init-canaries.py: una linea por canario
    con la posicion de la entrada del init array y el constructor que sustituyo.
    """
    report = "build/kodi-core-bisect-constructors.txt"
    if not os.path.exists(report):
        return []
    entries = []
    for line in open(report, errors="replace"):
        parts = line.split("\t")
        if len(parts) == 3 and parts[0].isdigit():
            entries.append((int(parts[0]), parts[1], parts[2]))
    if not entries:
        return []
    step = 1
    for i in range(1, len(entries)):
        step = max(step, entries[i][0] - entries[i - 1][0])
    start = (number - 1) * step
    end = start + step
    out = []
    for pos, addr, name in entries:
        if start <= pos < end:
            out.append(f"init_array[{pos}] = {name}  (sustituido por el canario {number})")
    if not out:
        out.append(f"ningun canario entre las posiciones {start} y {end}")
    return out


def analyse(dest: str, elf: str | None) -> list[str]:
    findings: list[str] = []
    canary = read_text(remote_file(dest, "/data/kodi-ps4/core-canary.log"))
    kodi = read_text(remote_file(dest, "/data/kodi-ps4/.kodi/temp/kodi.log"))
    loader = read_text(remote_file(dest, "/data/Loader_Logs.txt"))
    initlog = read_text(remote_file(dest, "/data/kodi-ps4/init.log"))
    renderlog = read_text(remote_file(dest, "/data/kodi-ps4/render.log"))

    # los coredumps se guardan con la ruta remota (data/sce_coredumps/...)
    dumps = []
    for root, _dirs, files in os.walk(dest):
        if "sce_coredumps" in root or root.endswith("coredumps"):
            for name in files:
                dumps.append(os.path.relpath(os.path.join(root, name), dest))

    # --- 1. traza del core -------------------------------------------------
    if canary.strip():
        marks = [line.strip() for line in canary.splitlines() if line.strip()]
        unique = []
        for m in marks:
            if m not in unique:
                unique.append(m)
        findings.append("TRAZA DEL CORE: " + " -> ".join(unique))
        findings.append(f"  ultima marca: '{marks[-1]}'  (el core murio despues de esta)")

        # El canario lleva un contador: dice hasta que constructor llego.
        ctors = [m for m in marks if m.startswith("ctor-")]
        if ctors:
            last = ctors[-1]
            number = int(last.split("-")[1])
            findings.append(f"  el proceso llego al canario {last}: el fallo esta en ese")
            findings.append("  canario o en uno de los constructores hasta el siguiente.")
            for candidate in constructor_candidates(number):
                findings.append(f"    candidato: {candidate}")

        crash = [m for m in marks if m.startswith("crash ")]
        if crash:
            findings.append(f"  CRASH: {crash[-1]}")
            addr = re.search(r"addr=(\w+)", crash[-1])
            if addr and elf:
                value = int(addr.group(1), 16)
                symbols = elf_symbols(elf)
                findings.append(f"    {addr.group(1)} -> {resolve(symbols, value)}")
        else:
            findings.append("  sin crash capturado por el handler de senales")
    else:
        findings.append("TRAZA DEL CORE: (vacia)")
        if dumps:
            findings.append("  + hay coredump y ninguna marca: el core revienta antes de")
            findings.append("  que el canario corra (o el log no se pudo escribir).")
        else:
            findings.append("  (sin coredump: el core se cuelga antes del init_array)")

    # --- 2. log del loader -------------------------------------------------
    if loader.strip():
        good = loader.count("Success the signature is valid")
        bad = len(re.findall(r"pkcs1_verify returned", loader))
        findings.append(f"LOADER: {good} firmas validas, {bad} invalidas")
        if bad:
            findings.append("  OJO: una firma invalida = el SELF no cuadra con el contenido")
        if re.search(r"(cannot|failed|error|not found|denied)", loader, re.I):
            for line in loader.splitlines():
                if re.search(r"(cannot|failed|error|denied)", line, re.I):
                    findings.append(f"  loader: {line.strip()}")

    # --- 3. log de Kodi ----------------------------------------------------
    if kodi.strip():
        errors = [l for l in kodi.splitlines() if re.search(r"\b(ERROR|FATAL)\b", l)]
        findings.append(f"KODI LOG: {len(kodi.splitlines())} lineas, {len(errors)} de error")
        for line in errors[-8:]:
            findings.append(f"  {line.strip()}")
    else:
        findings.append("KODI LOG: (no existe: el core no llego a inicializar el log)")

    # --- 4. app M0 ---------------------------------------------------------
    if initlog.strip():
        runs = initlog.count("renderer_init_ok")
        frames = re.findall(r"frame=(\d+)", renderlog)
        findings.append(f"APP M0: {runs} arranques, "
                        f"{frames[-1] if frames else 0} frames en el ultimo")
    elif renderlog.strip():
        findings.append("APP M0: hay render.log pero no init.log")

    # --- 5. coredumps ------------------------------------------------------
    if dumps:
        findings.append(f"COREDUMPS: {len(dumps)} ficheros (la app revienta con una senal)")
        for d in dumps[:10]:
            findings.append(f"  {d}")
        for d in dumps:
            if d.endswith("report.log"):
                report = read_text(os.path.join(dest, d))
                reason = re.search(r'"reason":\s*"(\w+)"', report)
                title = re.search(r'"title name":\s*"([^"]*)"', report)
                if title:
                    findings.append(f"  app: {title.group(1)}")
                if reason:
                    findings.append(f"  reason: {reason.group(1)}")
    else:
        findings.append("COREDUMPS: ninguno")

    # --- 6. paquetes instalados -------------------------------------------
    pkgs = read_text(os.path.join(dest, "pkg-list.txt"))
    names = [l.split()[-1] for l in pkgs.splitlines() if len(l.split()) >= 9]
    if names:
        findings.append("PAQUETES EN /user/data/pkg:")
        for n in names:
            findings.append(f"  {n}")

    return findings


def watch(dest: str, seconds: int) -> None:
    """Sigue la traza del core mientras la app corre."""
    path = remote_file(dest, "/data/kodi-ps4/core-canary.log")
    seen = 0
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as fh:
                lines = [l.strip() for l in fh if l.strip()]
            for line in lines[seen:]:
                print(f"  {line}", flush=True)
            seen = len(lines)
        except OSError:
            pass
        time.sleep(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("accion", choices=["fetch", "analyze", "wipe", "all", "watch"])
    parser.add_argument("destino", nargs="?", default=None)
    parser.add_argument("--minutes", type=int, default=DEFAULT_MAX_AGE)
    parser.add_argument("--max-size", type=int, default=DEFAULT_MAX_SIZE)
    parser.add_argument("--elf", default="build/kodi-core.elf",
                        help="ELF del core para resolver direcciones")
    parser.add_argument("--seconds", type=int, default=120)
    args = parser.parse_args()

    dest = args.destino or f"build/logs-{time.strftime('%Y%m%d-%H%M%S')}"
    os.makedirs(dest, exist_ok=True)

    # "analyze" y "watch" trabajan sobre lo ya descargado: no hacen falta la consola.
    if args.accion == "analyze":
        for line in analyse(dest, args.elf):
            print(line)
        return 0
    if args.accion == "watch":
        watch(dest, args.seconds)
        return 0

    console = Console()
    try:
        if args.accion == "wipe":
            for name in ("core-canary.log", "init.log", "render.log",
                         "audio.log", "input.log", ".kodi/temp/kodi.log"):
                path = f"/data/kodi-ps4/{name}"
                print(("borrado " if console.delete(path) else "no se pudo borrar ") + path)
        elif args.accion in ("fetch", "all"):
            print(f"trayendo logs a {dest} (ultimos {args.minutes} min)")
            for remote in INTERESTING:
                fetch(console, remote, os.path.join(dest, remote.lstrip("/")),
                      args.minutes, args.max_size)
            for listing, name in (("/user/data/pkg/", "pkg-list.txt"),
                                  ("/user/data/FPKGi/Downloads/", "fpkgi-list.txt")):
                data = console.get(listing)
                if data:
                    with open(os.path.join(dest, name), "wb") as fh:
                        fh.write(data)
            console.close()
            if args.accion == "all":
                print()
                for line in analyse(dest, args.elf):
                    print(line)
            return 0
    finally:
        console.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
