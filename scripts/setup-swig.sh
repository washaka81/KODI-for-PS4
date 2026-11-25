#!/usr/bin/env bash
#
# setup-swig.sh — build a host SWIG 4.5.0 for the Kodi Python bindings.
#
# The PS4 cross toolchain cannot build the internal SWIG target, so this
# script builds a native SWIG in $SWIG_PREFIX (default /tmp/kodi-ps4/
# swig-install) and the caller adds it to PATH.
#
# Usage:
#   ./scripts/setup-swig.sh [--force]
#
set -euo pipefail

SWIG_VERSION="${SWIG_VERSION:-4.5.0}"
SWIG_PREFIX="${SWIG_PREFIX:-/tmp/kodi-ps4/swig-install}"
BUILD_DIR="${SWIG_BUILD_DIR:-/tmp/kodi-ps4/swig-build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

log() { printf '\033[1;36m[swig]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ $FORCE -eq 0 && -x "$SWIG_PREFIX/bin/swig" ]]; then
  if "$SWIG_PREFIX/bin/swig" -version 2>/dev/null | grep -q "SWIG Version $SWIG_VERSION"; then
    log "SWIG $SWIG_VERSION already installed in $SWIG_PREFIX"
    exit 0
  fi
fi

for tool in curl tar make cc c++ pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || die "missing $tool"
done
pkg-config --exists libpcre2-8 || die "missing PCRE2 development files (libpcre2-8)"

TARBALL="$BUILD_DIR/swig-$SWIG_VERSION.tar.gz"
SRC="$BUILD_DIR/swig-$SWIG_VERSION"
URL="https://downloads.sourceforge.net/project/swig/swig/swig-$SWIG_VERSION/swig-$SWIG_VERSION.tar.gz"

mkdir -p "$BUILD_DIR"
if [[ ! -f "$TARBALL" ]]; then
  log "downloading SWIG $SWIG_VERSION"
  curl -L --fail --max-time 300 -o "$TARBALL" "$URL"
fi

rm -rf "$SRC" "$SWIG_PREFIX"
tar -xzf "$TARBALL" -C "$BUILD_DIR"
cd "$SRC"
log "configuring SWIG in $SWIG_PREFIX"
./configure --prefix="$SWIG_PREFIX" --with-pcre2 >"$BUILD_DIR/configure.log" 2>&1
log "building SWIG"
make -j"$JOBS" >"$BUILD_DIR/make.log" 2>&1
make install >"$BUILD_DIR/install.log" 2>&1

"$SWIG_PREFIX/bin/swig" -version | head -1
log "ready: $SWIG_PREFIX/bin/swig"
