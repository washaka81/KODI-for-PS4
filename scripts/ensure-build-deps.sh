#!/usr/bin/env bash
#
# ensure-build-deps.sh — recreate the host build dependency paths after a
# /tmp cleanup.  The real artifacts live under $KODI_BUILD_DEPS (persistent);
# the old /tmp paths are kept as symlinks because the existing CMake caches
# and FFmpeg configure files reference them.
#
set -euo pipefail

ROOT="${KODI_BUILD_DEPS:-${HOME}/kodi-build-deps}"
mkdir -p "$ROOT" /tmp/kodi-ps4

link() {
  local target="$1" path="$2"
  if [[ ! -e "$target" ]]; then
    printf 'ensure-build-deps: missing %s\n' "$target" >&2
    return 0
  fi
  if [[ -L "$path" ]]; then
    ln -sfn "$target" "$path"
  elif [[ ! -e "$path" ]]; then
    ln -s "$target" "$path"
  fi
}

link "$ROOT/nasm-ps4" /tmp/nasm-ps4
link "$ROOT/nasm-ps4" /tmp/kodi-ps4/nasm-ps4
link "$ROOT/swig-install" /tmp/kodi-ps4/swig-install
link "$ROOT/TexturePacker-build" /tmp/TexturePacker-build
link "$ROOT/opengnm" /tmp/kodi-ps4/opengnm
link "$ROOT/psbc" /tmp/kodi-ps4/psbc
link "$ROOT/ps4sdk-include-copy" /tmp/ps4sdk-include-copy
link "${HOME}/kodi-deps-ps4" /tmp/kodi-deps-ps4
link "$ROOT/kodi-ps4-host-tools" /tmp/kodi-ps4-host-tools

if [[ ! -d "$ROOT/kodi-ps4-host-tools" ]]; then
  printf 'ensure-build-deps: host tools missing; run scripts/setup-m1-deps.sh\n' >&2
fi
