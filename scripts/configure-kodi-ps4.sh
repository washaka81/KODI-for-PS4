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
"$REPO_ROOT/scripts/ensure-build-deps.sh" >/dev/null 2>&1 || true
PS4SDK="${PS4SDK:-${OO_PS4_TOOLCHAIN:-$HOME/ps4sdk/OpenOrbis/PS4Toolchain}}"
BUILD_DIR="${1:-$REPO_ROOT/build/kodi-ps4}"
DEPS_PS4="${DEPS_PS4:-${HOME}/kodi-deps-ps4}"
HOST_TOOLS="${HOST_TOOLS:-${HOME}/kodi-build-deps/kodi-ps4-host-tools}"
KODI_SRC_DIR="${KODI_SRC_DIR:-$REPO_ROOT/src/kodi}"
TEXTUREPACKER="${TEXTUREPACKER:-${HOME}/kodi-build-deps/TexturePacker-build/TexturePacker}"
NASM_EXECUTABLE="${NASM_EXECUTABLE:-${HOME}/kodi-build-deps/nasm-ps4/bin/nasm}"

export PS4SDK
export OO_PS4_TOOLCHAIN="$PS4SDK"
export DEPS_PS4
# Host build tools (flatc) must be found instead of the cross compiled ones.
export CMAKE_PROGRAM_PATH="$HOST_TOOLS/bin${CMAKE_PROGRAM_PATH:+:$CMAKE_PROGRAM_PATH}"
# The libraries cross built for the target have to win over the host packages
# that pkg-config would find otherwise.
export PKG_CONFIG_PATH="$DEPS_PS4/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_LIBDIR="$DEPS_PS4/lib/pkgconfig"
HOST_FLATC="$HOST_TOOLS/bin/flatc"
[[ -x "$HOST_FLATC" ]] || { printf '\033[1;31m[fail]\033[0m %s missing — run scripts/setup-m1-deps.sh\n' "$HOST_FLATC" >&2; exit 1; }

log() { printf '\033[1;36m[configure]\033[0m %s\n' "$*"; }

log "PS4SDK      = $PS4SDK"
log "BUILD_DIR   = $BUILD_DIR"
log "source      = $KODI_SRC_DIR"

PYTHON_ARGS=(-DENABLE_PYTHON=OFF)
if compgen -G "$DEPS_PS4/lib/libpython3.14.*" >/dev/null &&
   [[ -f "$DEPS_PS4/include/python3.14/Python.h" ]]; then
  log "python      = libpython3.14 encontrado; ENABLE_PYTHON=ON"
  PYTHON_ARGS=(-DENABLE_PYTHON=ON -DPYTHON_PATH="$DEPS_PS4" -DPYTHON_VER=3.14 -DPython3_INCLUDE_DIR="$DEPS_PS4/include/python3.14" -DPython3_LIBRARY="$DEPS_PS4/lib/libpython3.14.a")
else
  log "python      = sin libpython3.14 para PS4; ENABLE_PYTHON=OFF (docs/PS4-PYTHON.md)"
fi

STACK_CANARY_ARGS=()
if [[ "${PS4_STACK_CANARY:-0}" == "1" ]]; then
  log "stack canary = __stack_chk_fail logging enabled"
  STACK_CANARY_ARGS=(-DPS4_STACK_CANARY=ON)
fi

cmake -S "$KODI_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/ps4sdk-direct.cmake" \
  -DCORE_SYSTEM_NAME=freebsd \
  -DCORE_PLATFORM_NAME=ps4 \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPP_RENDER_SYSTEM=gles \
  -DWITH_TEXTUREPACKER="$TEXTUREPACKER" \
  -DNASM_EXECUTABLE="$NASM_EXECUTABLE" \
  -DFLATBUFFERS_FLATC_EXECUTABLE="$HOST_FLATC" \
  -DWITH_JSONSCHEMABUILDER="$HOST_TOOLS/bin" \
  -DCMAKE_PREFIX_PATH="$DEPS_PS4" \
  -DDEPS_PS4="$DEPS_PS4" \
  -DCURL_INCLUDE_DIR="$DEPS_PS4/include" \
  -DCURL_LIBRARY="$DEPS_PS4/lib/libcurl.a" \
  -DCMAKE_DISABLE_FIND_PACKAGE_Dav1d=TRUE \
  -DPS4_BUILTIN_PVR=ON \
  -DENABLE_INTERNAL_SWIG=OFF \
  "${PYTHON_ARGS[@]}" \
  "${STACK_CANARY_ARGS[@]}" \
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
