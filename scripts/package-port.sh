#!/usr/bin/env bash
#
# package-port.sh — rebuild the port from port/, compile and package.
#
# Usage:
#   ./scripts/package-port.sh [name]
#
# The name is only used for the remote file (kodi-ps4-<name>.pkg).
# Changes must be captured in port/ (kodi-port.sh capture).
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:-port}"
if [[ -z "${BUILD_DIR:-}" ]]; then
  # Prefer the OpenSSL/external-FFmpeg tree: the internal-FFmpeg tree does
  # not contain the port's ps4http protocol or h264_videodec2 decoder.
  if [[ -f "$REPO_ROOT/build/kodi-ps4-openssl/CMakeCache.txt" ]]; then
    BUILD_DIR="$REPO_ROOT/build/kodi-ps4-openssl"
  else
    BUILD_DIR="$REPO_ROOT/build/kodi-ps4"
  fi
fi
TOOLCHAIN="${PS4SDK:-${OO_PS4_TOOLCHAIN:-$HOME/ps4sdk/OpenOrbis/PS4Toolchain}}"
export OO_PS4_TOOLCHAIN="$TOOLCHAIN"
export PS4SDK="$TOOLCHAIN"
export KODI_BUILD="$BUILD_DIR"

CONTENT_ID="${CONTENT_ID:-IV0000-FGNM00012_00-KODIGNM000000000}"
TITLE_ID="${TITLE_ID:-FGNM00012}"
TITLE="${TITLE:-KODI}"
VERSION="${VERSION:-0.0.1}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
if command -v sha256sum >/dev/null 2>&1; then
  SHA256=(sha256sum)
else
  SHA256=(shasum -a 256)
fi

log() { printf '\033[1;36m[package]\033[0m %s\n' "$*"; }
die() {
  printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2
  exit 1
}

[[ -d "$BUILD_DIR" ]] || die "build tree not found: $BUILD_DIR"
# Anchor relative BUILD_DIR values: the pvr.iptvsimple configure resolves
# its CMAKE_PREFIX_PATH from the addon build dir, so a relative tree path
# silently breaks find_package(Kodi).
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
[[ -x "$TOOLCHAIN/bin/linux/create-fself" ]] || die "PS4 toolchain not found"
mkdir -p "$REPO_ROOT/build"

log "1/4 applying and verifying port/ on src/kodi"
"$REPO_ROOT/scripts/kodi-port.sh" apply
"$REPO_ROOT/scripts/kodi-port.sh" verify

log "2/4 building libglshim.a"
make -C "$REPO_ROOT/tools/gnm-engine/glshim" -j"$JOBS" \
  OPENGNM_PATH="${OPENGNM_PATH:-${HOME}/kodi-build-deps/opengnm}" \
  FREEGNM_PATH="${FREEGNM_PATH:-${HOME}/kodi-build-deps/freegnm}"

# The static pvr.iptvsimple library must exist before Kodi is linked: the PS4
# SDK dlopen() is stubbed, so the addon is built into the executable.
if [[ -x "$REPO_ROOT/scripts/build-pvr-iptvsimple.sh" ]]; then
  if ! KODI_BUILD_DIR="$BUILD_DIR" "$REPO_ROOT/scripts/build-pvr-iptvsimple.sh"; then
    if [[ "${ALLOW_MISSING_PVR:-0}" == "1" ]]; then
      printf '\033[1;33m[warn]\033[0m pvr.iptvsimple failed; the PKG will not include IPTV\n' >&2
    else
      die "pvr.iptvsimple failed; use ALLOW_MISSING_PVR=1 to package without IPTV"
    fi
  fi
fi

log "3/4 building kodi-ps4"
JOBS="$JOBS" "$REPO_ROOT/scripts/build-kodi-ps4.sh" "$BUILD_DIR" kodi

if [[ -d "$REPO_ROOT/port/addons/script.ps4.launcher" ]]; then
  rm -rf "$BUILD_DIR/addons/script.ps4.launcher"
  cp -a "$REPO_ROOT/port/addons/script.ps4.launcher" "$BUILD_DIR/addons/"
fi

log "4/4 packaging"
cp "$BUILD_DIR/kodi-ps4" "$REPO_ROOT/build/kodi-core.elf"
python3 "$REPO_ROOT/scripts/patch-absolute-relocs.py" "$REPO_ROOT/build/kodi-core.elf"
python3 "$REPO_ROOT/scripts/patch-openssl-noadx.py" "$REPO_ROOT/build/kodi-core.elf"
"$TOOLCHAIN/bin/linux/create-fself" \
  -in="$REPO_ROOT/build/kodi-core.elf" \
  -out="$REPO_ROOT/build/kodi-core.oelf" \
  -eboot="$REPO_ROOT/build/kodi-core-eboot.bin" \
  -paid=0x3800000000000035 \
  -authinfo="000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

EBOOT="$REPO_ROOT/build/kodi-core-eboot.bin" \
CORE_ELF="$REPO_ROOT/build/kodi-core.elf" \
WITH_MEDIA=1 \
VERSION="$VERSION" \
KODI_BUILD="$BUILD_DIR" \
PYTHON_LIB="${PYTHON_LIB:-${DEPS_PS4:-${HOME}/kodi-deps-ps4}/lib/python3.14}" \
CONTENT_ID="$CONTENT_ID" \
TITLE_ID="$TITLE_ID" \
TITLE="$TITLE" \
  "$REPO_ROOT/scripts/package-core.sh"

OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT/dist}"
PKG="$OUTPUT_DIR/${CONTENT_ID}.pkg"
[[ -f "$PKG" ]] || die "PKG was not created: $PKG"
log "validating PKG"
DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
LD_LIBRARY_PATH="$REPO_ROOT/tools/ssl-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$TOOLCHAIN/bin/linux/PkgTool.Core" pkg_validate "$PKG" >/dev/null
VERSIONED_PKG="$OUTPUT_DIR/kodi-ps4-${VERSION}.pkg"
cp "$PKG" "$VERSIONED_PKG"
"${SHA256[@]}" "$PKG" "$VERSIONED_PKG"

log "next: upload and verify on console"
printf '  CONSOLE=<ip> REMOTE_DIR=/data/pkg ./scripts/console-pkg.sh push %s kodi-ps4-%s.pkg\n' "$VERSIONED_PKG" "$VERSION"
