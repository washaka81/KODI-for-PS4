#!/usr/bin/env bash
#
# console-validate.sh — Deploy to the PS4 and check the runtime logs.
#
# The console only exposes an FTP server, so installing the package and
# launching the application has to be done from the console UI.  This script
# uploads the artifacts, waits for the application to run and then checks the
# logs it writes to /data/kodi-ps4.
#
# Usage:
#   ./scripts/console-validate.sh upload     # upload the pkg and the eboot
#   ./scripts/console-validate.sh wait       # wait for the app and report
#   ./scripts/console-validate.sh status     # report the current state
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"
LOG_DIR="/data/kodi-ps4"
PKG="$REPO_ROOT/dist/IV0000-BREW99999_00-KODIPS4000000000.pkg"
EBOOT="$REPO_ROOT/build/eboot.bin"
TIMEOUT="${TIMEOUT:-300}"

log()  { printf '\033[1;36m[consola]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[aviso]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail]\033[0m %s\n' "$*"; }

# Los coredumps son directorios (NPXS20001_..., BREW...), no ficheros.  El
# chequeo antiguo contaba lineas "^-" y siempre decia "sin coredumps".
count_coredumps() {
  curl -s --max-time 10 "${FTP}/data/sce_coredumps/" 2>/dev/null |
    awk '$1 ~ /^d/ && $NF != "." && $NF != ".." {n++} END {print n+0}'
}

console_reachable() {
  timeout 5 bash -c "cat < /dev/null > /dev/tcp/${CONSOLE}/${CONSOLE_PORT}" 2>/dev/null
}

upload() {
  console_reachable || { fail "consola $CONSOLE:$CONSOLE_PORT no accesible"; return 1; }
  [[ -f "$PKG" ]] || { fail "falta $PKG (ejecuta scripts/build.sh y scripts/package.sh)"; return 1; }
  log "subiendo el paquete a ${FTP}/data/pkg/"
  curl -s --ftp-create-dirs -T "$PKG" "${FTP}/data/pkg/" || { fail "fallo al subir el paquete"; return 1; }
  ok "paquete subido"
  # FPKGi solo busca en su carpeta de descargas.
  log "subiendo el paquete a ${FTP}/data/FPKGi/Downloads/"
  curl -s --ftp-create-dirs -T "$PKG" "${FTP}/data/FPKGi/Downloads/" \
    || warn "no se pudo copiar el paquete a la carpeta de FPKGi"
  if [[ -f "$EBOOT" ]]; then
    log "subiendo el eboot a ${FTP}${LOG_DIR}/"
    curl -s --ftp-create-dirs -T "$EBOOT" "${FTP}${LOG_DIR}/eboot.bin" || warn "no se pudo subir el eboot"
  fi
  cat <<EOF

La consola no se puede instalar ni lanzar de forma remota (solo expone el
FTP).  En la consola:

  1. Abre el instalador de paquetes (FPKGi o el Package Installer) e instala
     IV0000-BREW99999_00-KODIPS4000000000.pkg
     (esta en /data/pkg/ y en /data/FPKGi/Downloads/)
  2. Lanza la app "Kodi PS4" desde el XMB
  3. Vuelve a ejecutar:  ./scripts/console-validate.sh wait
EOF
}

fetch_log() {
  local name="$1"
  curl -s --max-time 10 "${FTP}${LOG_DIR}/${name}" 2>/dev/null
}

# The Kodi core writes its log in the profile directory.
fetch_kodi_log() {
  curl -s --max-time 10 "${FTP}${LOG_DIR}/.kodi/temp/kodi.log" 2>/dev/null
}

report() {
  local init render audio input kodi
  init="$(fetch_log init.log)"
  render="$(fetch_log render.log)"
  audio="$(fetch_log audio.log)"
  input="$(fetch_log input.log)"
  kodi="$(fetch_kodi_log)"

  if [[ -z "$init" && -z "$render" && -z "$kodi" ]]; then
    warn "ninguna app ha escrito logs todavia en ${LOG_DIR}/"
    return 1
  fi

  [[ -n "$init" ]] && { echo "----- init.log -----"; echo "$init"; }
  [[ -n "$render" ]] && { echo "----- render.log -----"; echo "$render"; }
  [[ -n "$audio" ]] && { echo "----- audio.log -----"; echo "$audio"; }
  [[ -n "$input" ]] && { echo "----- input.log -----"; echo "$input"; }
  [[ -n "$kodi" ]] && { echo "----- kodi.log (core) -----"; echo "${kodi:0:4000}"; }

  local status=0
  if [[ -n "$init" ]]; then
    grep -q "modules_ok\|piglet_config_ok" <<<"$init" || { fail "sin confirmacion de la configuracion de Piglet"; status=1; }
    grep -q "renderer_init_ok\|context_ok" <<<"$init" || { fail "sin confirmacion del contexto EGL/GLES"; status=1; }
    grep -q "frame=" <<<"$render" || { warn "render.log sin frames"; status=1; }
  fi
  if [[ -n "$kodi" ]]; then
    grep -qi "ERROR\|FATAL" <<<"$kodi" && { warn "el core de Kodi ha registrado errores"; }
  fi

  local dumps
  dumps="$(count_coredumps)"
  if [[ "${dumps:-0}" -gt 0 ]]; then
    fail "hay $dumps coredump(s) en /data/sce_coredumps"
    status=1
  else
    ok "sin coredumps"
  fi

  [[ $status -eq 0 ]] && ok "validacion de runtime correcta"
  return $status
}

wait_for_app() {
  log "esperando hasta ${TIMEOUT}s a que la app escriba sus logs"
  local deadline=$((SECONDS + TIMEOUT))
  while (( SECONDS < deadline )); do
    if [[ -n "$(fetch_log init.log)" ]]; then
      ok "la app ha arrancado"
      report
      return $?
    fi
    sleep 5
  done
  fail "la app no ha arrancado en ${TIMEOUT}s (¿instalada y lanzada?)"
  return 1
}

# El canario del core marca en que punto del arranque muere la aplicacion.
canary_report() {
  local canary coredumps
  canary="$(fetch_log core-canary.log)"
  coredumps="$(count_coredumps)"
  if [[ -z "$canary" ]]; then
    warn "sin canario: la app no llego a ejecutar ningun constructor"
  else
    echo "----- core-canary.log -----"
    echo "$canary"
  fi
  if [[ "${coredumps:-0}" -gt 0 ]]; then
    fail "hay $coredumps coredump(s) en /data/sce_coredumps"
  else
    ok "sin coredumps"
  fi
}

# La consola borra los coredumps en cuanto los sube al servidor de errores,
# asi que hay que pillarlos justo despues del crash.
coredump_fetch() {
  local dir
  mkdir -p build/coredumps
  # Descargar tambien los NPXS (XMB/SceShellCore): un fallo del sistema
  # puede ser la causa del bucle, no un fallo del core de Kodi.
  for dir in $(curl -s --max-time 10 "${FTP}/data/sce_coredumps/" 2>/dev/null | awk '$1 ~ /^d/ && $NF != "." && $NF != ".." {print $NF}'); do
    log "descargando coredump $dir"
    curl -s --max-time 120 "${FTP}/data/sce_coredumps/${dir}/report.log" -o "build/coredumps/${dir}-report.log" || true
    curl -s --max-time 300 "${FTP}/data/sce_coredumps/${dir}/" >/dev/null 2>&1 || true
    for f in $(curl -s --max-time 30 "${FTP}/data/sce_coredumps/${dir}/" 2>/dev/null | awk '$NF ~ /dmp|orbisstate|xml/ {print $NF}'); do
      curl -s --max-time 300 "${FTP}/data/sce_coredumps/${dir}/${f}" -o "build/coredumps/${dir}-${f}" \
        && ok "guardado build/coredumps/${dir}-${f}" || warn "no se pudo descargar ${f}"
    done
  done
  ls -la build/coredumps/ 2>/dev/null
}

# El loader de PS4 deja el motivo del rechazo en /data/sce_logs y en
# /data/Loader_Logs.txt (ver RESEARCH.md §12).
loader_logs() {
  log "contenido de /data/sce_logs/"
  curl -s --max-time 20 "${FTP}/data/sce_logs/" 2>/dev/null | awk '$5 > 0 {print "  " $NF}'
  for f in $(curl -s --max-time 20 "${FTP}/data/sce_logs/" 2>/dev/null | awk '$5 > 0 && $NF !~ /^[.]{1,2}$/ {print $NF}'); do
    echo "----- /data/sce_logs/$f -----"
    curl -s --max-time 30 "${FTP}/data/sce_logs/${f}" 2>/dev/null | tail -40
  done
  echo "----- /data/Loader_Logs.txt -----"
  curl -s --max-time 30 "${FTP}/data/Loader_Logs.txt" 2>/dev/null | tail -40
}

case "${1:-status}" in
  upload) upload ;;
  wait)   console_reachable || { fail "consola no accesible"; exit 1; }; wait_for_app ;;
  status) console_reachable || { fail "consola no accesible"; exit 1; }; report ;;
  canary) console_reachable || { fail "consola no accesible"; exit 1; }; canary_report ;;
  coredump) console_reachable || { fail "consola no accesible"; exit 1; }; coredump_fetch ;;
  loader) console_reachable || { fail "consola no accesible"; exit 1; }; loader_logs ;;
  *)      echo "uso: $0 {upload|wait|status|canary|coredump}"; exit 2 ;;
esac
