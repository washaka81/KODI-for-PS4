#!/usr/bin/env bash
#
# build-kodi-ps4.sh — Build the Kodi core cross-build for PS4.
#
# Usage: ./scripts/build-kodi-ps4.sh [BUILD_DIR] [TARGET...]
#
# Defaults to building the `kodi` executable target.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-/tmp/kodi-ps4-configure}"
shift || true
TARGETS=("$@")
[[ ${#TARGETS[@]} -eq 0 ]] && TARGETS=(kodi)

JOBS="${JOBS:-$(nproc)}"

log() { printf '\033[1;36m[build]\033[0m %s\n' "$*"; }

[[ -d "$BUILD_DIR" ]] || die_noop=1
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  printf '\033[1;31m[fail]\033[0m no build tree at %s — run scripts/configure-kodi-ps4.sh first\n' "$BUILD_DIR" >&2
  exit 1
fi

log "BUILD_DIR = $BUILD_DIR"
log "TARGETS   = ${TARGETS[*]}  (JOBS=$JOBS)"

# Warn when src/kodi does not match the port/ capture (does not block builds
# from a clean base, but avoids building a half-applied tree by accident).
if [[ -d "$REPO_ROOT/port" && -x "$REPO_ROOT/scripts/kodi-port.sh" ]]; then
  if ! "$REPO_ROOT/scripts/kodi-port.sh" verify >/dev/null 2>&1; then
    printf '\033[1;33m[warn]\033[0m src/kodi does not match port/; check kodi-port.sh capture/apply\n' >&2
  fi
fi

# Check whether xbmc/xbmc master changed files the port has touched.
# This is informational: an overlap does not block the build, but it must
# be reviewed before accepting the result.
if [[ "${SKIP_UPSTREAM_CHECK:-0}" != "1" ]]; then
  log "revisando cambios de xbmc/xbmc master (informe: build/upstream-check.txt)"
  if ! "$REPO_ROOT/scripts/check-upstream.sh" --quiet; then
    printf '\033[1;33m[warn]\033[0m upstream check failed; continuing with the build\n' >&2
  fi
fi

cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" -j"$JOBS"
