#!/usr/bin/env bash
#
# package-port.sh — Rebuild the port from port/, compile and package.
#
# Usage:
#   ./scripts/package-port.sh [name]
#
# The name is only used for the remote file (kodi-gnm-<name>.pkg).
# Changes must be captured in port/ first (kodi-port.sh capture).
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:-port}"
BUILD_DIR="${BUILD_DIR:-${HOME}/kodi-build-ps4}"
TOOLCHAIN="${OO_PS4_TOOLCHAIN:-/opt/ps4/sdk}"
export OO_PS4_TOOLCHAIN="$TOOLCHAIN"

CONTENT_ID="${CONTENT_ID:-IV0000-FGNM00011_00-KODIGNM000000000}"
TITLE_ID="${TITLE_ID:-FGNM00011}"
TITLE="${TITLE:-KODI}"

log() { printf '\033[1;36m[package]\033[0m %s\n' "$*"; }
die() {
  printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2
  exit 1
}

[[ -d "$BUILD_DIR" ]] || die "build tree $BUILD_DIR does not exist"
[[ -x "$TOOLCHAIN/bin/linux/create-fself" ]] || die "PS4 toolchain not found"

log "1/5 applying and verifying port/ on src/kodi"
"$REPO_ROOT/scripts/kodi-port.sh" apply
"$REPO_ROOT/scripts/kodi-port.sh" verify

log "2/5 generating shader binaries"
PSBC="${PSBC_BASE:-/tmp/kodi-ps4}/psbc/psbc"
[[ -x "$PSBC" ]] || die "psbc not found at $PSBC; run scripts/setup-psbc.sh first"
make -C "$REPO_ROOT/tools/gnm-engine" assets/misc/clear.frag.sb \
  GLSLC="${GLSLC:-glslc}" PSBC="$PSBC"
if [[ ! -d "$REPO_ROOT/build/shaderbin" ]]; then
  python3 "$REPO_ROOT/scripts/gen-shader-bins.py" \
    --shaders-dir "$REPO_ROOT/src/kodi/system/shaders/GLES/2.0" \
    --out "$REPO_ROOT/build/shaderbin" --psbc "$PSBC"
fi

log "3/5 building libglshim.a"
make -C "$REPO_ROOT/tools/gnm-engine/glshim" -j"$(nproc)"

log "4/5 building kodi-ps4"
JOBS="${JOBS:-$(nproc)}" "$REPO_ROOT/scripts/build-kodi-ps4.sh" "$BUILD_DIR" kodi

log "5/5 packaging"
cp "$BUILD_DIR/kodi-ps4" "$REPO_ROOT/build/kodi-core.elf"
python3 "$REPO_ROOT/scripts/patch-absolute-relocs.py" "$REPO_ROOT/build/kodi-core.elf"
"$TOOLCHAIN/bin/linux/create-fself" \
  -in="$REPO_ROOT/build/kodi-core.elf" \
  -out="$REPO_ROOT/build/kodi-core.oelf" \
  -eboot="$REPO_ROOT/build/kodi-core-eboot.bin" \
  -paid=0x3800000000000035 \
  -authinfo="000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

EBOOT="$REPO_ROOT/build/kodi-core-eboot.bin" \
CORE_ELF="$REPO_ROOT/build/kodi-core.elf" \
WITH_MEDIA=1 \
CONTENT_ID="$CONTENT_ID" \
TITLE_ID="$TITLE_ID" \
TITLE="$TITLE" \
  "$REPO_ROOT/scripts/package-core.sh"

PKG="$REPO_ROOT/dist/${CONTENT_ID}.pkg"
[[ -f "$PKG" ]] || die "$PKG was not generated"
sha256sum "$PKG"

log "siguiente: subir y verificar"
printf '  Upload %s to the console and install it.\n' "$PKG"
