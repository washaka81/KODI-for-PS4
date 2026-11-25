#!/usr/bin/env bash
#
# console-collect-logs.sh — Trae los logs importantes de la consola.
#
# Por defecto solo lo que es **reciente** (los ultimos MINUTOS) y lo que
# sirve para diagnosticar:
#   /data/kodi-ps4/         logs de las apps (init, render, core-canary, kodi.log)
#   /data/Loader_Logs.txt   log del loader de PS4
#   /data/sce_coredumps/    coredumps de las apps
#   listados de /user/data/pkg y /user/data/FPKGi/Downloads
#
# Se ignoran por defecto los ficheros enormes o cifrados que no aportan
# (p.ej. /data/sce_logs/log.bin, 16 MB cifrados) y lo que no ha cambiado
# hace tiempo.
#
# Uso:
#   ./scripts/console-collect-logs.sh [destino] [minutos]
#   MINUTOS=0   ./scripts/console-collect-logs.sh   # trae todo
#
set -uo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"
DEST="${1:-$PWD/build/logs-$(date +%Y%m%d-%H%M%S)}"
MAX_AGE="${2:-${MINUTOS:-180}}"
MAX_SIZE="${MAX_SIZE:-2097152}"   # 2 MB

mkdir -p "$DEST"

log()  { printf '\033[1;36m[logs]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[aviso]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; }

if ! timeout 8 bash -c "cat < /dev/null > /dev/tcp/${CONSOLE}/${CONSOLE_PORT}" 2>/dev/null; then
  fail "la consola ${CONSOLE}:${CONSOLE_PORT} no responde"
  exit 1
fi

# Antiguedad de una entrada del listado (columnas: perms links owner group size mes dia [ano|hora] nombre)
age_minutes() {
  local month="$1" day="$2" year="$3" name="$4" now mtime
  local -a months=(jan feb mar apr may jun jul aug sep oct nov dec)
  local mi=0
  for i in "${!months[@]}"; do
    [[ "${months[$i],,}" == "${month,,}" ]] && mi=$((i + 1)) && break
  done
  [[ $mi -eq 0 ]] && return 99999
  if [[ -n "$year" && "$year" =~ ^[0-9]{4}$ ]]; then
    mtime=$(date -d "${year}-$(printf '%02d' $mi)-$(printf '%02d' "$day") 00:00" +%s 2>/dev/null) || return 99999
  else
    local hh="${year}"
    [[ "$hh" =~ ^[0-9]{1,2}$ ]] || return 99999
    mtime=$(date -d "$(printf '%04d-%02d-%02d %02d:00' "$(date +%Y)" "$mi" "$day" "$hh")" +%s 2>/dev/null) || return 99999
  fi
  echo $(( ( $(date +%s) - mtime ) / 60 ))
}

fetch_file() {
  local remote="$1" local="$2"
  mkdir -p "$(dirname "$local")"
  if timeout 120 curl -s --max-time 110 "${FTP}${remote}" -o "$local"; then
    if [[ -s "$local" ]]; then
      ok "$(printf '%-44s' "$remote") $(stat -c%s "$local") bytes"
    else
      warn "$remote (vacio)"
    fi
  else
    warn "no se pudo traer $remote"
  fi
}

fetch_dir() {
  local remote="$1" local="$2" listing name size month day year age
  listing="$(curl -s --max-time 30 "${FTP}${remote}/" 2>/dev/null)"
  [[ -z "$listing" ]] && { warn "$remote no existe"; return; }
  ok "$remote/"
  while read -r _ _ _ _ size month day rest name; do
    [[ -z "${name:-}" ]] && continue
    [[ "$name" == "." || "$name" == ".." ]] && continue
    local sub="${remote%/}/${name}"
    if [[ "$rest" =~ ^[0-9]{4}$ ]]; then year="$rest"; hour=""; else hour="$rest"; year=""; fi
    if [[ "$size" -gt "$MAX_SIZE" ]]; then
      warn "se salta ${sub} (${size} bytes, demasiado grande o cifrado)"
      continue
    fi
    age="$(age_minutes "$month" "$day" "${year:-$hour}" "$name")"
    if [[ "$MAX_AGE" != "0" && "$age" -gt "$MAX_AGE" ]]; then
      warn "se salta ${sub} (hace ${age} min)"
      continue
    fi
    if echo "$listing" | grep -q "^d.* ${name}$"; then
      fetch_dir "$sub" "$local/$name"
    else
      fetch_file "$sub" "$local/$name"
    fi
  done < <(echo "$listing")
}

log "destino: $DEST   (logs de los ultimos ${MAX_AGE} min)"

# --- logs de las aplicaciones (lo importante) ------------------------------
fetch_dir "/data/kodi-ps4" "$DEST/kodi-ps4"
# el log de Kodi esta en .kodi/temp, se trae siempre (es pequeno)
fetch_file "/data/kodi-ps4/.kodi/temp/kodi.log" "$DEST/kodi-ps4/.kodi/temp/kodi.log"

# --- log del loader -------------------------------------------------------
fetch_file "/data/Loader_Logs.txt" "$DEST/Loader_Logs.txt"

# --- coredumps (solo los recientes) --------------------------------------
fetch_dir "/data/sce_coredumps" "$DEST/coredumps"

# --- listados de paquetes ------------------------------------------------
curl -s --max-time 30 "${FTP}/user/data/pkg/" > "$DEST/pkg-list.txt" 2>/dev/null
curl -s --max-time 30 "${FTP}/user/data/FPKGi/Downloads/" > "$DEST/fpkgi-list.txt" 2>/dev/null

# --- resumen -------------------------------------------------------------
echo
log "===== RESUMEN ====="

show() {
  local f="$1" lines="${2:-20}"
  if [[ -s "$f" ]]; then
    echo "----- ${f#$DEST/} -----"
    tail -"$lines" "$f"
    echo
  fi
}

show "$DEST/kodi-ps4/core-canary.log" 40
show "$DEST/kodi-ps4/init.log" 12
show "$DEST/kodi-ps4/render.log" 8
show "$DEST/kodi-ps4/.kodi/temp/kodi.log" 25
show "$DEST/Loader_Logs.txt" 10

if [[ -d "$DEST/coredumps" ]]; then
  echo "----- coredumps -----"
  find "$DEST/coredumps" -type f | sed "s|$DEST/||"
  echo
fi

echo "guardado en: $DEST"
