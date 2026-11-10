#!/usr/bin/env bash
#
# setup-psbc.sh — Recreate the patched psbc used by gen-shader-bins.py.
#
# psbc is not vendored: it lives under /tmp (see plan-proxima-sesion.md).
# This script clones it plus its header dependencies and applies the local
# patch that teaches it to emit the PSSL/GNM bits Piglet needs:
#
#   * IMM_CONSTBUFFER usage slot for the __GLOBAL_CB__ (push constants)
#   * GnmShaderBinaryInfo.numinputusageslots
#   * user-varying semantics derived from the real location (15 + loc), so
#     FS shaders that consume a subset of the VS outputs still link
#   * VS input attribute semantics keep their real location
#
# Usage:
#   ./scripts/setup-psbc.sh [--force]
#
# Environment:
#   PSBC_BASE   work directory (default: /tmp/kodi-ps4)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="${PSBC_BASE:-/tmp/kodi-ps4}"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

log() { printf '\033[1;36m[psbc]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

command -v git >/dev/null || die "git not found"
command -v make >/dev/null || die "make not found"
command -v cc >/dev/null || die "cc not found"
command -v c++ >/dev/null || die "c++ not found"

mkdir -p "$BASE"

clone() {
  local url="$1" dest="$2"
  if [[ ! -d "$dest/.git" ]]; then
    log "cloning $url"
    git clone --depth 1 "$url" "$dest"
  fi
}

clone https://github.com/bizkut/psbc "$BASE/psbc"
clone https://github.com/PS4-OpenGNM/opengnm "$BASE/opengnm"
clone https://github.com/PS4-OpenGNM/opengnm-psbc "$BASE/opengnm-psbc"
clone https://github.com/KhronosGroup/Vulkan-Headers "$BASE/Vulkan-Headers"

# psbc expects the original freegnm header layout; OpenGNM provides the
# headers and opengnm-psbc provides the PSSL types.
if [[ $FORCE -eq 1 || ! -f "$BASE/freegnm/gnm/pssl/types.h" ]]; then
  log "staging the GNM/PSSL headers"
  rm -rf "$BASE/freegnm"
  mkdir -p "$BASE/freegnm/gnm/pssl"
  printf '#include <pssl_types.h>\n' > "$BASE/freegnm/gnm/pssl/types.h"
  cp "$BASE/opengnm/include/"*.h "$BASE/freegnm/"
fi

cd "$BASE/psbc"

if [[ ! -f "$BASE/psbc/.patched-kodi-ps4" ]]; then
  log "applying tools/patches/psbc-kodi-ps4.patch"
  if git apply --check "$ROOT_DIR/tools/patches/psbc-kodi-ps4.patch"; then
    git apply "$ROOT_DIR/tools/patches/psbc-kodi-ps4.patch"
  elif git apply --reverse --check "$ROOT_DIR/tools/patches/psbc-kodi-ps4.patch"; then
    log "psbc patch already applied"
  else
    die "the psbc patch does not apply (was the tree already modified?)"
  fi
  touch "$BASE/psbc/.patched-kodi-ps4"
fi

# Modern glibc needs these flags; harmless on older toolchains.
sed -i \
  -e 's/-D_XOPEN_SOURCE=501/-D_XOPEN_SOURCE=600/' \
  -e 's/-DHAVE_PTHREAD=1/-DHAVE_PTHREAD=1 -D_POSIX_C_SOURCE=200809L/' \
  -e 's/CFLAGS=-std=gnu11/CFLAGS=-std=gnu11 -include alloca.h -include strings.h/' \
  config.mak

log "building psbc"
make -j"${JOBS:-$(nproc)}"

[[ -x "$BASE/psbc/psbc" ]] || die "psbc was not produced"
log "ready: $BASE/psbc/psbc"
