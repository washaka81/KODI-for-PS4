#!/usr/bin/env bash
#
# console-debug-socket.sh — Escucha el socket de depuración de la PS4 (4646).
#
# La salida estándar de la app homebrew (stdout/stderr) va a ese socket, así
# que es la forma de ver los mensajes de Kodi (p.ej. "ERROR: Unable to create
# application. Exiting").  Véase docs/m1-plan.md §Testing.
#
# Uso:
#   ./scripts/console-debug-socket.sh [segundos]      # captura y muestra
#   ./scripts/console-debug-socket.sh [segundos] -f    # guarda en build/
#
set -uo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
PORT="${DEBUG_PORT:-4646}"
SECONDS_TO_RUN="${1:-60}"
SAVE=""
[[ "${2:-}" == "-f" ]] && SAVE=1

OUT="$(mktemp)"
echo "escuchando ${CONSOLE}:${PORT} durante ${SECONDS_TO_RUN}s (lanza la app en la consola ahora)"

timeout "${SECONDS_TO_RUN}" bash -c "exec 3<>/dev/tcp/${CONSOLE}/${PORT}; cat <&3" > "$OUT" 2>/dev/null &
PID=$!

sleep 1
if ! kill -0 $PID 2>/dev/null; then
  echo "no se pudo conectar al socket ${CONSOLE}:${PORT}"
  rm -f "$OUT"
  exit 1
fi

wait $PID 2>/dev/null

echo "----- salida recibida -----"
if [[ -s "$OUT" ]]; then
  cat "$OUT"
else
  echo "(vacio: el socket abrio pero la app no escribio nada)"
fi

if [[ -n "$SAVE" ]]; then
  mkdir -p build
  cp "$OUT" "build/debug-socket-$(date +%H%M%S).log"
  echo "guardado en build/"
fi
rm -f "$OUT"
