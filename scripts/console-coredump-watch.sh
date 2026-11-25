#!/usr/bin/env bash
#
# console-coredump-watch.sh — Vigila /data/sce_coredumps y descarga el coredump.
#
# La consola borra los coredumps en cuanto los envia al servidor de errores,
# asi que hay que pillarlos en el mismo momento del crash.
#
# Uso:
#   ./scripts/console-coredump-watch.sh [minutos]
#
set -uo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
REMOTE_DIR="${REMOTE_DIR:-/data/sce_coredumps}"
OUT_DIR="${OUT_DIR:-build/coredumps}"
MINUTES="${1:-30}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"

mkdir -p "$OUT_DIR"
deadline=$(( $(date +%s) + MINUTES * 60 ))
seen=""

log() { printf '\033[1;36m[coredump]\033[0m %s\n' "$*"; }

while [[ $(date +%s) -lt $deadline ]]; do
  for dir in $(curl -s --max-time 10 "${FTP}${REMOTE_DIR}/" 2>/dev/null | awk '$NF ~ /BREW|FGNM/ {print $NF}'); do
    [[ " $seen " == *" $dir "* ]] && continue
    log "nuevo coredump: $dir"
    for f in $(curl -s --max-time 15 "${FTP}${REMOTE_DIR}/${dir}/" 2>/dev/null | awk '$NF ~ /dmp|orbisstate|xml|log$/ {print $NF}'); do
      curl -s --max-time 300 "${FTP}${REMOTE_DIR}/${dir}/${f}" -o "${OUT_DIR}/${dir}-${f}" \
        && log "guardado ${OUT_DIR}/${dir}-${f}" || log "no se pudo descargar ${f}"
    done
    seen="$seen $dir"
  done
  sleep 3
done

log "fin de la vigilancia; ficheros descargados:"
ls -la "$OUT_DIR"
