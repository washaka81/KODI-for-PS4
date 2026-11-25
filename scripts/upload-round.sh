#!/usr/bin/env bash
#
# upload-round.sh — Limpia la consola y deja solo los paquetes de esta ronda.
#
# 1. Borra todos los paquetes de /user/data/pkg y los logs viejos.
# 2. Sube los paquetes de la ronda y los renombra con nombres legibles (el
#    content ID va dentro de param.sfo, el nombre del fichero es libre).
#
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"
REMOTE_DIR="${REMOTE_DIR:-/user/data/pkg}"
LOG_DIR="${LOG_DIR:-/data/kodi-ps4}"

log()  { printf '\033[1;36m[consola]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; }

# Ejecuta un comando FTP y mira la respuesta.  No se puede usar "| grep -q"
# con pipefail: grep cierra la tubería, curl recibe SIGPIPE y la tubería
# acaba con error aunque grep haya encontrado la respuesta.
ftp_reply() {
  local out
  out="$(timeout 120 curl -s -v "${FTP}/" "$@" 2>&1)" || true
  grep -q "^< 250" <<<"$out"
}

# nombre local -> nombre en la consola
PACKAGES=(
  "IV0000-BREW99979_00-KODIHANDL0000000.pkg:kodi-10-test-handler.pkg"
  "IV0000-BREW99981_00-KODIBISEC0000000.pkg:kodi-11-bisect-constructores.pkg"
)

# logs antiguos que se borran para que los nuevos no se confundan
STALE_LOGS="core-canary.log sizetest.log reloc.log exreloc.log globdat.log ttls.log timp_all.log tcombo.log tmods.log tmods2.log tsyms.log tctors.log init.log render.log tthread.log audio.log input.log"

if ! timeout 8 bash -c "cat < /dev/null > /dev/tcp/${CONSOLE}/${CONSOLE_PORT}" 2>/dev/null; then
  fail "la consola ${CONSOLE}:${CONSOLE_PORT} no responde"
  exit 1
fi

# --- 1. limpieza -----------------------------------------------------------
log "borrando los paquetes de ${REMOTE_DIR}"
for f in $(curl -s --max-time 30 "${FTP}${REMOTE_DIR}/" | awk '$5 > 0 && $NF != "." && $NF != ".." {print $NF}'); do
  if ftp_reply -Q "DELE ${REMOTE_DIR}/${f}"; then
    ok "borrado $f"
  else
    fail "no se pudo borrar $f"
  fi
done

log "borrando los logs antiguos de ${LOG_DIR}"
for f in $STALE_LOGS; do
  ftp_reply -Q "DELE ${LOG_DIR}/${f}" && ok "borrado $f"
done

# --- 2. subida -------------------------------------------------------------
for entry in "${PACKAGES[@]}"; do
  local_pkg="${entry%%:*}"
  remote_name="${entry##*:}"
  src="$ROOT_DIR/dist/$local_pkg"
  [[ -f "$src" ]] || { fail "falta $src"; continue; }
  log "subiendo $remote_name"
  if ! timeout 900 curl -s --ftp-create-dirs -T "$src" "${FTP}${REMOTE_DIR}/${local_pkg}"; then
    fail "no se pudo subir $local_pkg"
    continue
  fi
  if ftp_reply -Q "RNFR ${REMOTE_DIR}/${local_pkg}" -Q "RNTO ${REMOTE_DIR}/${remote_name}"; then
    ok "$remote_name"
  else
    fail "no se pudo renombrar $local_pkg"
  fi
done

log "estado final de ${REMOTE_DIR}"
curl -s --max-time 30 "${FTP}${REMOTE_DIR}/" | awk '$5 > 0 && $NF != "." && $NF != ".." {printf "  %-32s %10s bytes\n", $NF, $5}'
