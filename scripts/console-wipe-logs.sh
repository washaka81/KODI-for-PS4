#!/usr/bin/env bash
#
# console-wipe-logs.sh — Limpia los logs de la consola antes de una prueba.
#
# Los logs del sistema y de las apps son *append-only* (ver coding-rules.md
# §Testing: "Logs are append-only; wipe manually between crash runs"), asi que
# sin esto no se distingue lo de una prueba de lo de la anterior.
#
# Uso:
#   ./scripts/console-wipe-logs.sh
#
set -uo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"

log()  { printf '\033[1;36m[logs]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; }

if ! timeout 8 bash -c "cat < /dev/null > /dev/tcp/${CONSOLE}/${CONSOLE_PORT}" 2>/dev/null; then
  fail "la consola ${CONSOLE}:${CONSOLE_PORT} no responde"
  exit 1
fi

# No se puede usar "| grep -q" con pipefail: grep cierra la tuberia, curl
# recibe SIGPIPE y el pipeline devuelve error aunque la respuesta fuera 250.
ftp_reply() {
  local out
  out="$(timeout 60 curl -s -v "${FTP}/" "$@" 2>&1)" || true
  grep -q "^< 250" <<<"$out"
}

delete() {
  local path="$1"
  if ftp_reply -Q "DELE ${path}"; then
    ok "borrado ${path}"
  else
    printf '\033[1;33m[aviso]\033[0m no se pudo borrar %s\n' "$path"
  fi
}

log "limpiando los logs de las apps"
for f in core-canary.log init.log render.log audio.log input.log; do
  delete "/data/kodi-ps4/${f}"
done
delete "/data/kodi-ps4/.kodi/temp/kodi.log"

log "quedan:"
curl -s --max-time 20 "${FTP}/data/kodi-ps4/" | awk '$5 > 0 {printf "  %-24s %8s bytes\n", $NF, $5}'
