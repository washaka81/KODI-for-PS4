#!/usr/bin/env bash
#
# console-pkg.sh — Manage the packages that live in the console.
#
# The console only exposes an FTP server, so this is the way to upload the
# packages, give them a readable name and drop the ones that are not needed
# any more.
#
# Usage:
#   ./scripts/console-pkg.sh list
#   ./scripts/console-pkg.sh push <pkg> <name>     # upload and rename
#   ./scripts/console-pkg.sh prune <name>...       # keep only these
#
set -uo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
REMOTE_DIR="${REMOTE_DIR:-/user/data/pkg}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"

log() { printf '\033[1;36m[pkg]\033[0m %s\n' "$*"; }
ok()  { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; }

remote_list() {
  curl -s --max-time 30 "${FTP}${REMOTE_DIR}/" | awk '$5 > 0 && $NF != "." && $NF != ".." {print $NF}'
}

# The GoldHEN FTP server only accepts DELE and RNTO with an absolute path.
# No se puede usar "| grep -q" con pipefail: grep cierra la tubería, curl
# recibe SIGPIPE y la tubería acaba con error aunque la respuesta sea buena.
ftp_reply() {
  local out
  out="$(curl -s --max-time 120 -v "${FTP}/" "$@" 2>&1)" || true
  grep -q "^< 250" <<<"$out"
}

remote_delete() {
  ftp_reply -Q "DELE ${REMOTE_DIR}/$1"
}

remote_rename() {
  ftp_reply -Q "RNFR ${REMOTE_DIR}/$1" -Q "RNTO ${REMOTE_DIR}/$2"
}

case "${1:-list}" in
  list)
    sizes=$(curl -s --max-time 30 "${FTP}${REMOTE_DIR}/")
    for f in $(remote_list); do
      size=$(echo "$sizes" | awk -v n="$f" '$NF == n {print $5}')
      printf '%-34s %10s bytes\n' "$f" "$size"
    done
    ;;
  push)
    [[ $# -eq 3 ]] || { echo "uso: $0 push <pkg> <name>" >&2; exit 2; }
    [[ -f "$2" ]] || { fail "no existe $2"; exit 1; }
    log "subiendo $2 -> ${REMOTE_DIR}/$3"
    curl -s --max-time 900 --ftp-create-dirs -T "$2" "${FTP}${REMOTE_DIR}/$3" || { fail "fallo al subir"; exit 1; }
    ok "subido como $3"
    ;;
  rename)
    [[ $# -eq 3 ]] || { echo "uso: $0 rename <actual> <nuevo>" >&2; exit 2; }
    remote_rename "$2" "$3" && ok "renombrado $2 -> $3" || fail "no se pudo renombrar"
    ;;
  prune)
    shift
    [[ $# -gt 0 ]] || { echo "uso: $0 prune <nombre que se queda>..." >&2; exit 2; }
    local_keep=" $* "
    for f in $(remote_list); do
      case "$local_keep" in
        *" $f "*) log "se queda $f" ;;
        *) remote_delete "$f" && ok "borrado $f" || fail "no se pudo borrar $f" ;;
      esac
    done
    ;;
  delete)
    shift
    [[ $# -gt 0 ]] || { echo "uso: $0 delete <nombre>..." >&2; exit 2; }
    for f in "$@"; do
      remote_delete "$f" && ok "borrado $f" || fail "no se pudo borrar $f"
    done
    ;;
  *) echo "uso: $0 {list|push|rename|prune|delete}" >&2; exit 2 ;;
esac
