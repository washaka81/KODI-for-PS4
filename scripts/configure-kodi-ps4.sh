#!/usr/bin/env bash
#
# configure-kodi-ps4.sh — Configure the Kodi core cross-build for PS4.
#
# Requires ./scripts/setup-m1-deps.sh to have been run first.
#
# Usage: ./scripts/configure-kodi-ps4.sh [BUILD_DIR]
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PS4SDK="${PS4SDK:-${OO_PS4_TOOLCHAIN:-/opt/ps4/sdk}}"
BUILD_DIR="${1:-/tmp/kodi-ps4-configure}"

export PS4SDK
export OO_PS4_TOOLCHAIN="$PS4SDK"
# Host build tools (flatc) must be found instead of the cross compiled ones.
export CMAKE_PROGRAM_PATH="/tmp/kodi-ps4-host-tools/bin${CMAKE_PROGRAM_PATH:+:$CMAKE_PROGRAM_PATH}"
# The libraries cross built for the target have to win over the host packages
# that pkg-config would find otherwise.
export PKG_CONFIG_PATH="/tmp/kodi-deps-ps4/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_LIBDIR="/tmp/kodi-deps-ps4/lib/pkgconfig"
HOST_FLATC=/tmp/kodi-ps4-host-tools/bin/flatc
[[ -x "$HOST_FLATC" ]] || { printf '\033[1;31m[fail]\033[0m %s missing — run scripts/setup-m1-deps.sh\n' "$HOST_FLATC" >&2; exit 1; }

log() { printf '\033[1;36m[configure]\033[0m %s\n' "$*"; }

log "PS4SDK      = $PS4SDK"
log "BUILD_DIR   = $BUILD_DIR"
log "source      = $REPO_ROOT/src/kodi"

cmake -S "$REPO_ROOT/src/kodi" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/ps4sdk-direct.cmake" \
  -DCORE_SYSTEM_NAME=freebsd \
  -DCORE_PLATFORM_NAME=ps4 \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPP_RENDER_SYSTEM=gles \
  -DWITH_TEXTUREPACKER=/tmp/TexturePacker-build/TexturePacker \
  -DNASM_EXECUTABLE=/tmp/nasm-ps4/bin/nasm \
  -DFLATBUFFERS_FLATC_EXECUTABLE="$HOST_FLATC" \
  -DWITH_JSONSCHEMABUILDER=/tmp/kodi-ps4-host-tools/bin \
  -DCMAKE_PREFIX_PATH=/tmp/kodi-deps-ps4 \
  -DCMAKE_DISABLE_FIND_PACKAGE_Dav1d=TRUE \
  -DENABLE_PYTHON=OFF \
  -DENABLE_OPTICAL=OFF \
  -DENABLE_BLURAY=OFF \
  -DENABLE_INTERNAL_BLURAY=OFF \
  -DENABLE_INTERNAL_FFMPEG=ON \
  -DENABLE_INTERNAL_FLATBUFFERS=ON \
  -DENABLE_INTERNAL_FMT=ON \
  -DENABLE_INTERNAL_FSTRCMP=ON \
  -DENABLE_INTERNAL_FREETYPE=ON \
  -DENABLE_INTERNAL_FRIBIDI=ON \
  -DENABLE_INTERNAL_GTEST=ON \
  -DENABLE_INTERNAL_LZO2=ON \
  -DENABLE_INTERNAL_NLOHMANNJSON=ON \
  -DENABLE_INTERNAL_PCRE2=ON \
  -DENABLE_INTERNAL_RAPIDJSON=ON \
  -DENABLE_INTERNAL_SPDLOG=ON \
  -DENABLE_INTERNAL_TAGLIB=ON \
  -DENABLE_INTERNAL_TINYXML2=ON \
  -DENABLE_INTERNAL_CROSSGUID=ON \
  -DVERBOSE=ON \
  "$@"

printf '\n\033[1;32m[ ok ]\033[0m configure finished (exit 0)\n'
