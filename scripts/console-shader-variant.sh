#!/usr/bin/env bash
#
# console-shader-variant.sh — Select the packaged shader blob variant.
#
# PS4_LoadShaderBinary reads /data/kodi-ps4/shader-variant.txt once per
# process.  Variant 0 is bin/, 1..9 are bin/v<N>/.  The app must be
# relaunched after changing it.
#
# Usage:
#   ./scripts/console-shader-variant.sh [0..9]
#
set -euo pipefail

CONSOLE="${CONSOLE:-192.0.2.11}"
CONSOLE_PORT="${CONSOLE_PORT:-2121}"
FTP="ftp://${CONSOLE}:${CONSOLE_PORT}"
REMOTE="/data/kodi-ps4/shader-variant.txt"
VARIANT="${1:-0}"

[[ "$VARIANT" =~ ^[0-9]$ ]] || { echo "uso: $0 [0..9]" >&2; exit 2; }

printf '%s\n' "$VARIANT" |
  curl -s --max-time 30 --ftp-create-dirs -T - "$FTP$REMOTE"
echo "shader variant = $VARIANT ($REMOTE)"
