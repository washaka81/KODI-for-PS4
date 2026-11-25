#!/usr/bin/env bash
#
# build-pvr-iptvsimple.sh — Cross-build pvr.iptvsimple for the PS4 package.
#
# The addon is not part of the Kodi tree.  It is built out-of-tree against
# the Kodi build directory and installed into <KODI_BUILD_DIR>/addons so
# package-core.sh includes it in the PKG.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KODI_BUILD_DIR="${KODI_BUILD_DIR:-${HOME}/kodi-build-ps4}"
DEPS="${KODI_DEPS:-/tmp/kodi-deps-ps4}"
SRC="${PVR_IPTV_SRC:-/tmp/pvr.iptvsimple}"
BUILD="${PVR_IPTV_BUILD:-/tmp/pvr.iptvsimple-build}"
STATIC_BUILD="${PVR_IPTV_STATIC_BUILD:-/tmp/pvr.iptvsimple-static}"
COMMIT="${PVR_IPTV_COMMIT:-f917044}"
TOOLCHAIN="$REPO_ROOT/cmake/ps4sdk-direct.cmake"

log() { printf '\033[1;36m[pvr.iptvsimple]\033[0m %s\n' "$*"; }
die() {
  printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2
  exit 1
}

[[ -d "$KODI_BUILD_DIR" ]] || die "no existe KODI_BUILD_DIR=$KODI_BUILD_DIR"
[[ -f "$DEPS/lib/libpugixml.a" && -f "$DEPS/lib/liblzma.a" ]] ||
  die "faltan pugixml/liblzma en $DEPS (ver docs/PS4-PVR-IPTV.md)"

if [[ ! -d "$SRC/.git" ]]; then
  log "clonando pvr.iptvsimple"
  git clone https://github.com/kodi-pvr/pvr.iptvsimple "$SRC"
fi
git -C "$SRC" fetch --all --tags
git -C "$SRC" checkout "$COMMIT"
git -C "$SRC" reset --hard "$COMMIT"

# Local add-on fixes (read-error handling, size caps) live outside the Kodi
# tree so the upstream add-on commit stays untouched.
PATCH_DIR="$REPO_ROOT/port/addons/pvr.iptvsimple"
if [[ -d "$PATCH_DIR" ]]; then
  for patch in "$PATCH_DIR"/*.patch; do
    [[ -f "$patch" ]] || continue
    if git -C "$SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
      log "parche ya aplicado: $(basename "$patch")"
    else
      log "aplicando $(basename "$patch")"
      git -C "$SRC" apply "$patch" || die "no aplica $(basename "$patch")"
    fi
  done
fi

log "configurando (estático)"
rm -rf "$STATIC_BUILD"
cmake -S "$SRC" -B "$STATIC_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$KODI_BUILD_DIR/build;$DEPS" \
  -DKODI_BUILD_DIR="$KODI_BUILD_DIR" \
  -DBUILD_SHARED_LIBS=OFF

log "compilando (estático)"
cmake --build "$STATIC_BUILD" -j"${JOBS:-$(nproc)}"
STATIC_LIB="$STATIC_BUILD/pvr.iptvsimple.a"
[[ -f "$STATIC_LIB" ]] || die "no se generó $STATIC_LIB"
mkdir -p "$DEPS/lib"
cp "$STATIC_LIB" "$DEPS/lib/pvr.iptvsimple.a"
log "instalado $DEPS/lib/pvr.iptvsimple.a"

log "configurando"
rm -rf "$BUILD"
cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$KODI_BUILD_DIR/build;$DEPS" \
  -DKODI_BUILD_DIR="$KODI_BUILD_DIR"

log "compilando"
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}"

ADDON="$KODI_BUILD_DIR/addons/pvr.iptvsimple"
rm -rf "$ADDON"
mkdir -p "$ADDON"
cp "$BUILD/pvr.iptvsimple.so" "$ADDON/"
cp "$BUILD/pvr.iptvsimple/addon.xml" "$ADDON/"
cp "$SRC/pvr.iptvsimple/icon.png" "$ADDON/"
# PVR addons use instance-settings.xml for the per-instance settings dialog;
# settings.xml alone only contains the hidden migration category.
cp -a "$SRC/pvr.iptvsimple/resources" "$ADDON/"

# Basic M3U/HLS playback uses Kodi's built-in player.  The inputstream
# addons are optional and are not shipped in this package, so do not mark
# the PVR addon as broken when they are absent.
sed -i '/inputstream\./d' "$ADDON/addon.xml"

log "instalado en $ADDON"
