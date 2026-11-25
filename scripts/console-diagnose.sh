#!/usr/bin/env bash
#
# console-diagnose.sh — Recoge hardware/logs de la PS4 y genera un informe.
#
# Uso:
#   CONSOLE=192.0.2.10 ./scripts/console-diagnose.sh
#   ./scripts/console-diagnose.sh --local build/logs-20261112-memtrace2
#
# Genera <dest>/report.md y <dest>/report.json con PASS/WARN/FAIL.
# No instala, no publica y no modifica la consola.
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"
DEST="${DEST:-$REPO_ROOT/build/diagnostics/$(date +%Y%m%d-%H%M%S)}"
LOCAL=""

if [[ "${1:-}" == "--local" ]]; then
  [[ -n "${2:-}" ]] || { echo "uso: $0 --local <directorio>" >&2; exit 2; }
  LOCAL="$2"
  DEST="$LOCAL"
fi

log() { printf '\033[1;36m[diag]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[aviso]\033[0m %s\n' "$*"; }

fetch() {
  local remote="$1" local="$2"
  mkdir -p "$(dirname "$local")"
  if timeout 60 curl -s --max-time 50 "${FTP}${remote}" -o "$local" && [[ -s "$local" ]]; then
    log "  ${remote} ($(stat -c%s "$local") B)"
  else
    rm -f "$local"
    warn "  no se pudo traer ${remote}"
  fi
}

fetch_remote() {
  mkdir -p "$DEST"
  log "consola ${CONSOLE}:${CONSOLE_PORT} -> ${DEST}"
  fetch "/data/kodi-ps4/.kodi/userdata/hardware-profile.json" \
        "$DEST/data/kodi-ps4/.kodi/userdata/hardware-profile.json"
  fetch "/data/kodi-ps4/core-canary.log" "$DEST/data/kodi-ps4/core-canary.log"
  fetch "/data/kodi-ps4/python-getpath.log" \
        "$DEST/data/kodi-ps4/python-getpath.log"
  fetch "/data/kodi-ps4/.kodi/temp/kodi.log" \
        "$DEST/data/kodi-ps4/.kodi/temp/kodi.log"
  fetch "/data/kodi-ps4/.kodi/temp/kodi.old.log" \
        "$DEST/data/kodi-ps4/.kodi/temp/kodi.old.log"
  fetch "/data/kodi-ps4/sqlite-vfs.log" "$DEST/data/kodi-ps4/sqlite-vfs.log"
  fetch "/data/kodi-ps4/glshim-shim.log" "$DEST/data/kodi-ps4/glshim-shim.log"
  fetch "/data/Loader_Logs.txt" "$DEST/data/Loader_Logs.txt"

  timeout 20 curl -s --max-time 15 "${FTP}/user/data/pkg/" \
    >"$DEST/pkg-list.txt" 2>/dev/null || true
  timeout 20 curl -s --max-time 15 "${FTP}/data/sce_coredumps/" \
    >"$DEST/coredumps-list.txt" 2>/dev/null || true

  CONSOLE="$CONSOLE" CONSOLE_PORT="$CONSOLE_PORT" \
    python3 "$REPO_ROOT/scripts/console-screenshot.py" "$DEST/screenshots" \
    >"$DEST/screenshot.log" 2>&1 || warn "sin captura de pantalla"
}

if [[ -z "$LOCAL" ]]; then
  fetch_remote
fi

python3 - "$DEST" <<'PY'
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

dest = Path(sys.argv[1])
checks: list[dict[str, str]] = []


def find(*names: str) -> Path | None:
    for name in names:
        matches = sorted(dest.rglob(name))
        if matches:
            return matches[-1]
    return None


def check(cid: str, status: str, detail: str) -> None:
    checks.append({"id": cid, "status": status, "detail": detail})


def read(path: Path | None, limit: int = 4_000_000) -> str:
    if path is None or not path.is_file():
        return ""
    try:
        return path.read_text(errors="replace")[-limit:]
    except OSError:
        return ""


profile_path = find("hardware-profile.json")
profile: dict = {}
if profile_path is None:
    check("perfil", "FAIL", "falta hardware-profile.json")
else:
    try:
        profile = json.loads(profile_path.read_text())
        check("perfil", "PASS", f"version={profile.get('version')} model={profile.get('model')} fw={profile.get('firmware')}")
    except Exception as exc:  # noqa: BLE001
        check("perfil", "FAIL", f"JSON inválido: {exc}")

if profile:
    version = profile.get("version", 1)
    if version < 2:
        check("perfil.v2", "WARN", "perfil v1: faltan memoria flexible/directa, CPU, storage detallado")
    else:
        check("perfil.v2", "PASS", "perfil v2")

    flex = profile.get("memory_flexible_free_mb", 0)
    direct = profile.get("memory_direct_free_mb", 0)
    if flex:
        check("memoria.flexible", "PASS", f"{flex} MB libres")
    else:
        check("memoria.flexible", "WARN", "no recogida (perfil v1 o API)")

    storage = profile.get("storage", {})
    user = storage.get("user_data", {}) if isinstance(storage, dict) else {}
    if user.get("rc") == 0 and user.get("free_mb", 0) > 0:
        check("almacenamiento.user_data", "PASS", f"{user.get('free_mb')} MB libres de {user.get('total_mb')} MB")
    else:
        check("almacenamiento.user_data", "WARN", f"rc={user.get('rc')} free={user.get('free_mb')}")

    video = profile.get("video", {})
    if video.get("caps_valid") or video.get("mode_valid") or video.get("monitor_valid"):
        check("video", "PASS", f"hdr10={video.get('hdr10')} output={video.get('hdr10_output')} caps_rc={video.get('caps_rc')} mode_rc={video.get('mode_rc')} monitor_rc={video.get('monitor_rc')}")
    else:
        check("video", "WARN", f"caps/mode/monitor no válidos (caps_rc={video.get('caps_rc')}, bus_rc={video.get('caps_bus_rc')}, mode_rc={video.get('mode_rc')}, monitor_rc={video.get('monitor_rc')})")

    network = profile.get("network", {})
    if network.get("dns_rc") == 0 and network.get("internet_rc") == 0:
        check("red.basica", "PASS", f"dns=0 tcp=0 mirror_ip={network.get('mirror_ip')}")
    else:
        check("red.basica", "FAIL", f"dns_rc={network.get('dns_rc')} internet_rc={network.get('internet_rc')}")

    mac = network.get("mac", "")
    check("red.mac", "PASS" if mac else "WARN", mac or "no recogida")

    temp_valid = profile.get("cpu_temp_valid")
    temp = profile.get("cpu_temp_c")
    check("cpu.temperatura", "PASS" if temp_valid else "WARN",
          f"{temp} C" if temp_valid else "no disponible")

    dumps = profile.get("coredumps", 0)
    check("coredumps", "WARN" if dumps else "PASS", f"{dumps} coredump(s)")

kodi = read(find("kodi.log", "kodi.old.log"))
canary = read(find("core-canary.log"))
sqlite = read(find("sqlite-vfs.log"))

if kodi:
    fetch_lines = re.findall(r"CRepository: fetched (\d+) bytes from '([^']*)'", kodi)
    if fetch_lines:
        size, url = fetch_lines[-1]
        if int(size) > 0:
            check("repo.descarga", "PASS", f"{size} B desde {url}")
        else:
            check("repo.descarga", "FAIL", f"0 B desde {url}")
    elif "CRepository:" in kodi:
        check("repo.descarga", "WARN", "log sin línea 'fetched ... bytes' (build antigua)")

    if "Timeout was reached" in kodi:
        check("red.timeout", "FAIL", "curl error 28 (timeout) en una descarga")
    if "Could not connect" in kodi:
        check("red.conexion", "FAIL", "Kodi no pudo conectar con el repositorio")
    if "wrong digest" in kodi:
        check("repo.digest", "FAIL", "digest incorrecto (posible cuerpo vacío)")
    if "Multi perform failed" in kodi:
        check("red.multi", "FAIL", "CURLM_BAD_HANDLE / multi perform")
    errors = len(re.findall(r"\berror <general>:", kodi))
    check("kodi.errores", "WARN" if errors else "PASS", f"{errors} errores en kodi.log")
else:
    check("kodi.log", "FAIL", "no encontrado")

if canary:
    # El canario se acumula entre ejecuciones.  Analizar solo la última:
    # desde el último "alloc-trace-on" (handler de la build actual).
    run_canary = canary.rsplit("alloc-trace-on", 1)[-1] if "alloc-trace-on" in canary else canary
    if "alloc-fail" in run_canary:
        line = [l for l in run_canary.splitlines() if "alloc-fail" in l][-1]
        check("oom.alloc-fail", "FAIL", line[:240])
    elif "std::bad_alloc" in run_canary:
        check("oom.bad_alloc", "FAIL", "std::bad_alloc en la ejecución actual")
    if "crash signal" in run_canary:
        check("crash", "FAIL", "señal de crash en la ejecución actual")
    elif "std::bad_alloc" in canary or "crash signal" in canary:
        check("crash.historico", "WARN", "hay crashes antiguos en el canario; no de esta ejecución")
    if "alloc-trace-on" in canary:
        check("instrumentacion", "PASS", "operator new instrumentado")
    if "platform-mime-init" in canary:
        check("mime.init", "PASS", "tabla MIME inicializada al arranque")
    if "repo-fetch-begin" in run_canary and "repo-parse-begin" not in run_canary:
        check("repo.parseo", "WARN", "se intentó el fetch pero no se llegó al parseo")
    if "repo-parse-done" in run_canary:
        check("repo.parseo", "PASS", "parseo completado")
    if "python-ready" in run_canary:
        check("python.init", "PASS", "python-ready")
    elif "python-init-fail:" in run_canary:
        line = [l for l in run_canary.splitlines() if "python-init-fail:" in l][-1]
        check("python.init", "FAIL", line[:240])
    elif "python-init-done" in run_canary:
        check("python.init", "WARN", "init OK; faltan python-modules-done/ready")
    if "python-config-fail:" in run_canary:
        line = [l for l in run_canary.splitlines() if "python-config-fail:" in l][-1]
        check("python.config", "FAIL", line[:240])
else:
    check("core-canary.log", "WARN", "no encontrado")

getpath_log = read(find("python-getpath.log"))
if getpath_log:
    # El log se acumula entre arranques; analizar solo el último.
    last_boot = "--- getpath config ---" + getpath_log.rsplit(
        "--- getpath config ---", 1)[-1]
    if "getpath error:" in last_boot:
        lines = [l for l in last_boot.splitlines() if l.startswith(("type=", "value="))]
        check("python.getpath", "FAIL", " | ".join(lines[-2:])[:240])
    else:
        check("python.getpath", "PASS", "sin excepción en el último arranque")

if sqlite:
    opens = len(re.findall(r"open rc=0", sqlite))
    size_init = len(re.findall(r"size-init", sqlite))
    status = "WARN" if size_init > 50 else "PASS"
    check("sqlite.vfs", status, f"{opens} open rc=0, {size_init} size-init")
else:
    check("sqlite.vfs", "WARN", "sin sqlite-vfs.log")

pkg_list = read(find("pkg-list.txt"))
if pkg_list.strip():
    pkgs = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.pkg)", pkg_list)))
    check("paquetes", "PASS", f"{len(pkgs)} PKG: {', '.join(pkgs[:4])}")

fails = [c for c in checks if c["status"] == "FAIL"]
warns = [c for c in checks if c["status"] == "WARN"]
summary = "FAIL" if fails else ("WARN" if warns else "PASS")

report = {
    "generated": __import__("datetime").datetime.now().isoformat(timespec="seconds"),
    "source": str(dest),
    "summary": summary,
    "checks": checks,
}

(dest / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False))

lines = [
    "# Diagnóstico PS4",
    "",
    f"- Fuente: `{dest}`",
    f"- Resultado: **{summary}**",
    f"- Checks: {len(checks)} ({len(fails)} FAIL, {len(warns)} WARN)",
    "",
    "| Check | Estado | Detalle |",
    "|---|---|---|",
]
for c in checks:
    detail = c["detail"].replace("|", "\\|")
    lines.append(f"| `{c['id']}` | {c['status']} | {detail} |")
lines.append("")

(dest / "report.md").write_text("\n".join(lines))
print("\n".join(lines))
sys.exit(1 if fails else 0)
PY
