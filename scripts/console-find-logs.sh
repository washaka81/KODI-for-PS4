#!/usr/bin/env bash
#
# console-find-logs.sh — Busca los logs del sistema y del loader en la consola.
#
# En la PS4 el kernel deja traza de las denegaciones de servicios (audit) y
# el loader deja su propio log.  Con GoldHEN el FTP da la vista completa del
# sistema de ficheros, así que se pueden leer por FTP.
#
# Uso:
#   ./scripts/console-find-logs.sh
#
set -uo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"

log()  { printf '\033[1;36m[logs]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; }

# Rutas donde el loader y el kernel guardan sus logs en PS4 / GoldHEN.
CANDIDATES=(
  "/data/Loader_Logs.txt"
  "/data/sce_logs/"
  "/data/audit/"
  "/data/audit.log"
  "/system/audit/"
  "/system/sce_logs/"
  "/system/Loader_Logs.txt"
  "/system_ex/common_ex/sce_logs/"
  "/hostapp/sce_logs/"
  "/user/data/sce_logs/"
  "/data/GoldHEN/"
  "/mnt/sandbox/"
)

for path in "${CANDIDATES[@]}"; do
  listing="$(curl -s --max-time 15 "${FTP}${path}" 2>/dev/null)"
  if [[ -n "$listing" ]]; then
    ok "existe ${path}"
    echo "$listing" | awk '$5 > 0 {printf "        %-40s %10s bytes\n", $NF, $5}'
    # Si es un fichero suelto, se muestra su contenido
    if ! echo "$listing" | grep -q "^d"; then
      echo "----- ${path} -----"
      curl -s --max-time 30 "${FTP}${path}" 2>/dev/null | tail -30
    fi
  fi
done
