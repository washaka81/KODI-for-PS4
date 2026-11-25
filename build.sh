#!/usr/bin/env bash
#
# build.sh — build Kodi for PS4 and produce a versioned PKG.
#
# Usage:
#   ./build.sh --all            # everything: toolchain, Kodi, deps, port, PKG
#   ./build.sh                  # build the version in VERSION (deps present)
#   ./build.sh --check          # validate toolchain/dependencies only
#   ./build.sh --bootstrap      # recreate the /tmp dependencies first
#   ./build.sh --bump patch     # bump VERSION and exit
#   ./build.sh --tag            # build and create the local v<version> tag
#   ./build.sh --push --validated  # publish to GitHub (after console test)
#
# The PKG is written to dist/kodi-ps4-<version>.pkg.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

HOST_OS="$(uname -s)"
VERSION_FILE="$ROOT_DIR/VERSION"
BUILD_DIR="${BUILD_DIR:-}"
DEPS_PS4="${DEPS_PS4:-/tmp/kodi-deps-ps4}"
OPENGNM_PATH="${OPENGNM_PATH:-/tmp/kodi-ps4/opengnm}"
FREEGNM_PATH="${FREEGNM_PATH:-/tmp/kodi-ps4/freegnm}"
SWIG_PREFIX="${SWIG_PREFIX:-/tmp/kodi-ps4/swig-install}"
PSBC_BIN="${PSBC:-$HOME/kodi-build-deps/psbc/psbc}"
KODI_SRC_DIR="${KODI_SRC_DIR:-$ROOT_DIR/src/kodi}"
PORT_DIR="${PORT_DIR:-port}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT_DIR/dist}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
SKIP_UPSTREAM_CHECK="${SKIP_UPSTREAM_CHECK:-0}"
ALLOW_MISSING_PVR="${ALLOW_MISSING_PVR:-0}"
DO_ALL=0
DO_BOOTSTRAP=0
DO_CHECK=0
DO_CLEAN=0
DO_TAG=0
DO_PUSH=0
VALIDATED=0
BUMP=""
SET_VERSION=""
KODI_CLONED=0

log()  { printf '\033[1;36m[build]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage:
  ./build.sh --all            everything: toolchain, Kodi, deps, port, PKG
  ./build.sh                  build the version in VERSION (deps present)
  ./build.sh --check          validate toolchain/dependencies only
  ./build.sh --bootstrap      recreate the /tmp dependencies first
  ./build.sh --bump patch     bump VERSION and exit
  ./build.sh --tag            build and create the local v<version> tag
  ./build.sh --push --validated  publish to GitHub (after console test)

Options:
  --all                   install missing toolchain/deps, clone Kodi and build
  --diag                  use the development series (PORT_DIR=port/diag)
  --port-dir DIR          port series directory (default: port)
  --kodi-dir DIR          Kodi checkout (default: src/kodi)
  --check                 validate toolchain, deps and build tools
  --bootstrap             run setup-m1-deps.sh and setup-psbc.sh
  --clean                 remove the build tree and build/core-stage
  --build-dir DIR         build tree (default: build/kodi-ps4 or ~/kodi-build-ps4)
  --toolchain DIR         OpenOrbis toolchain (PS4SDK/OO_PS4_TOOLCHAIN)
  --deps DIR              target libraries (DEPS_PS4)
  --opengnm DIR           opengnm checkout (OPENGNM_PATH)
  --output DIR            PKG output directory (default: dist/)
  --jobs N                parallel jobs
  --no-upstream-check     skip the xbmc/xbmc master check
  --allow-missing-pvr     package without pvr.iptvsimple
  --bump patch|minor|major  bump VERSION and exit
  --set-version X.Y.Z     set VERSION and exit
  --tag                   create the local v<version> tag after building
  --push                  push to origin (requires --validated)
  --validated             confirm that the PKG was tested on console
  -h, --help              show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --version) cat "$VERSION_FILE"; exit 0 ;;
    --all) DO_ALL=1; shift ;;
    --diag) PORT_DIR=port/diag; shift ;;
    --port-dir) PORT_DIR="${2:-}"; [[ -n "$PORT_DIR" ]] || die "--port-dir needs a value"; shift 2 ;;
    --kodi-dir) KODI_SRC_DIR="${2:-}"; [[ -n "$KODI_SRC_DIR" ]] || die "--kodi-dir needs a value"; shift 2 ;;
    --check) DO_CHECK=1; shift ;;
    --bootstrap) DO_BOOTSTRAP=1; shift ;;
    --clean) DO_CLEAN=1; shift ;;
    --build-dir) BUILD_DIR="${2:-}"; [[ -n "$BUILD_DIR" ]] || die "--build-dir needs a value"; shift 2 ;;
    --toolchain) PS4SDK="${2:-}"; OO_PS4_TOOLCHAIN="$PS4SDK"; [[ -n "$PS4SDK" ]] || die "--toolchain needs a value"; shift 2 ;;
    --deps) DEPS_PS4="${2:-}"; [[ -n "$DEPS_PS4" ]] || die "--deps needs a value"; shift 2 ;;
    --opengnm) OPENGNM_PATH="${2:-}"; [[ -n "$OPENGNM_PATH" ]] || die "--opengnm needs a value"; shift 2 ;;
    --output) OUTPUT_DIR="${2:-}"; [[ -n "$OUTPUT_DIR" ]] || die "--output needs a value"; shift 2 ;;
    --jobs) JOBS="${2:-}"; [[ -n "$JOBS" ]] || die "--jobs needs a value"; shift 2 ;;
    --no-upstream-check) SKIP_UPSTREAM_CHECK=1; shift ;;
    --allow-missing-pvr) ALLOW_MISSING_PVR=1; shift ;;
    --bump) BUMP="${2:-}"; [[ -n "$BUMP" ]] || die "--bump needs patch|minor|major"; shift 2 ;;
    --set-version) SET_VERSION="${2:-}"; [[ -n "$SET_VERSION" ]] || die "--set-version needs X.Y.Z"; shift 2 ;;
    --tag) DO_TAG=1; shift ;;
    --push) DO_PUSH=1; shift ;;
    --validated) VALIDATED=1; shift ;;
    --inside-container) shift ;;
    *) die "unknown option: $1 (use --help)" ;;
  esac
done

[[ -f "$VERSION_FILE" ]] || die "missing VERSION"
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]] || die "invalid VERSION: $VERSION"
[[ -d "$PORT_DIR" && -f "$PORT_DIR/base.conf" ]] || die "port series not found: $PORT_DIR"
export VERSION
export DEPS_PS4
export OPENGNM_PATH
export FREEGNM_PATH
export SWIG_PREFIX
export PORT_DIR
export KODI_SRC_DIR
export KODI_DIR="$KODI_SRC_DIR"
export OUTPUT_DIR
export JOBS
export ALLOW_MISSING_PVR
export SKIP_UPSTREAM_CHECK

bump_version() {
  local kind="$1" major minor patch
  IFS=. read -r major minor patch <<< "$VERSION"
  case "$kind" in
    major) major=$((major + 1)); minor=0; patch=0 ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    patch) patch=$((patch + 1)) ;;
    *) die "--bump must be patch, minor or major" ;;
  esac
  VERSION="$major.$minor.$patch"
  printf '%s\n' "$VERSION" > "$VERSION_FILE"
  log "VERSION -> $VERSION (update CHANGELOG.md before publishing)"
}

if [[ -n "$SET_VERSION" ]]; then
  [[ "$SET_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]] || die "invalid version: $SET_VERSION"
  VERSION="$SET_VERSION"
  printf '%s\n' "$VERSION" > "$VERSION_FILE"
  log "VERSION -> $VERSION (update CHANGELOG.md before publishing)"
  exit 0
fi

if [[ -n "$BUMP" ]]; then
  bump_version "$BUMP"
  exit 0
fi

if [[ "$DO_PUSH" == "1" && "$VALIDATED" != "1" ]]; then
  die "--push requires --validated: test the PKG on console and review the logs first"
fi
if [[ "$DO_PUSH" == "1" ]]; then
  DO_TAG=1
fi

# --- Host OS ---------------------------------------------------------------
case "$HOST_OS" in
  Linux) ;;
  Darwin)
    if [[ -n "${KODI_PS4_DOCKER_IMAGE:-}" ]] && command -v docker >/dev/null 2>&1; then
      [[ -n "${PS4SDK:-}" ]] || die "set PS4SDK to the toolchain path to mount it in Docker"
      log "macOS: running inside Docker (${KODI_PS4_DOCKER_IMAGE})"
      exec docker run --rm -it \
        -v "$ROOT_DIR:/work" -w /work \
        -v "$PS4SDK:/opt/ps4sdk:ro" \
        -e PS4SDK=/opt/ps4sdk -e OO_PS4_TOOLCHAIN=/opt/ps4sdk \
        -e BUILD_DIR=/work/build/kodi-ps4 \
        -e DEPS_PS4=/tmp/kodi-deps-ps4 \
        "$KODI_PS4_DOCKER_IMAGE" ./build.sh --inside-container
    fi
    die "macOS: the PS4 toolchain is Linux-only. Use Docker with KODI_PS4_DOCKER_IMAGE or a Linux VM."
    ;;
  MINGW*|MSYS*|CYGWIN*)
    die "Windows: use WSL2 (Ubuntu) and run ./build.sh inside WSL; the PS4 toolchain is Linux-only."
    ;;
  *)
    die "unsupported host OS: $HOST_OS"
    ;;
esac

# --- Toolchain -------------------------------------------------------------
find_toolchain() {
  local candidate
  for candidate in "${PS4SDK:-}" "${OO_PS4_TOOLCHAIN:-}" \
      "$HOME/ps4sdk/OpenOrbis/PS4Toolchain" \
      /opt/ps4/sdk /opt/ps4/oosdk /usr/local/ps4sdk; do
    [[ -n "$candidate" ]] || continue
    if [[ -x "$candidate/bin/linux/create-fself" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

install_toolchain() {
  local release="${OPENORBIS_RELEASE:-v0.5.4}"
  local archive="toolchain-llvm-18.tar.gz"
  local dest="$HOME/ps4sdk"
  local url="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/${release}/${archive}"
  command -v curl >/dev/null 2>&1 || die "curl is required to install the toolchain"
  command -v tar >/dev/null 2>&1 || die "tar is required to install the toolchain"
  mkdir -p "$dest"
  if [[ ! -f "$dest/$archive" ]]; then
    log "downloading OpenOrbis toolchain $release (~152 MB)"
    curl -L --fail --retry 3 --connect-timeout 20 -o "$dest/$archive" "$url" ||
      die "could not download $url"
  fi
  log "extracting OpenOrbis toolchain into $dest"
  tar -xzf "$dest/$archive" -C "$dest"
}

TOOLCHAIN="$(find_toolchain || true)"
if [[ -z "$TOOLCHAIN" && "$DO_ALL" == "1" ]]; then
  install_toolchain
  TOOLCHAIN="$(find_toolchain || true)"
fi
[[ -n "$TOOLCHAIN" ]] || die "OpenOrbis toolchain not found. Set PS4SDK=... (bin/linux/create-fself) or use --all."
export PS4SDK="$TOOLCHAIN"
export OO_PS4_TOOLCHAIN="$TOOLCHAIN"

# --- Build tree ------------------------------------------------------------
if [[ -z "$BUILD_DIR" ]]; then
  if [[ -d "$ROOT_DIR/build/kodi-ps4" ]]; then
    BUILD_DIR="$ROOT_DIR/build/kodi-ps4"
  elif [[ -d "$HOME/kodi-build-ps4" ]]; then
    BUILD_DIR="$HOME/kodi-build-ps4"
  else
    BUILD_DIR="$ROOT_DIR/build/kodi-ps4"
  fi
fi
export BUILD_DIR
export KODI_BUILD="$BUILD_DIR"

for tool in git cmake make python3 clang ar; do
  command -v "$tool" >/dev/null 2>&1 || die "missing $tool in PATH"
done

ensure_kodi() {
  if [[ -f "$KODI_SRC_DIR/CMakeLists.txt" ]]; then
    return 0
  fi
  if [[ -e "$KODI_SRC_DIR" && -n "$(ls -A "$KODI_SRC_DIR" 2>/dev/null)" ]]; then
    die "$KODI_SRC_DIR exists but is not a Kodi checkout; remove it or use --kodi-dir"
  fi
  [[ "$DO_ALL" == "1" ]] || die "Kodi source missing at $KODI_SRC_DIR; use --all or clone it manually"

  local url base
  url="$(sed -n 's/^KODI_UPSTREAM_URL=//p' "$PORT_DIR/base.conf" | head -1)"
  base="$(sed -n 's/^KODI_BASE=//p' "$PORT_DIR/base.conf" | head -1)"
  [[ -n "$url" && -n "$base" ]] || die "KODI_UPSTREAM_URL/KODI_BASE missing in $PORT_DIR/base.conf"

  log "cloning Kodi $base into $KODI_SRC_DIR"
  mkdir -p "$KODI_SRC_DIR"
  git init -q "$KODI_SRC_DIR"
  git -C "$KODI_SRC_DIR" remote add origin "$url"
  git -C "$KODI_SRC_DIR" fetch --depth 1 origin "$base" ||
    git -C "$KODI_SRC_DIR" fetch --filter=blob:none origin "$base" ||
    die "could not fetch Kodi commit $base"
  git -C "$KODI_SRC_DIR" checkout -q FETCH_HEAD
  KODI_CLONED=1
}

ensure_kodi

if [[ "$DO_ALL" == "1" && "$DO_CHECK" != "1" ]]; then
  log "applying port series $PORT_DIR"
  "$ROOT_DIR/scripts/kodi-port.sh" apply
fi

if [[ "$DO_ALL" == "1" ]]; then
  if [[ ! -f "$DEPS_PS4/lib/libcurl.a" || ! -d "$OPENGNM_PATH/include" ||
        ! -x "$SWIG_PREFIX/bin/swig" || ! -x "$PSBC_BIN" ]]; then
    DO_BOOTSTRAP=1
  fi
fi

if [[ "$DO_BOOTSTRAP" == "1" ]]; then
  log "recreating dependencies in $DEPS_PS4 and $OPENGNM_PATH"
  "$ROOT_DIR/scripts/setup-m1-deps.sh"
  "$ROOT_DIR/scripts/ensure-build-deps.sh" >/dev/null 2>&1 || true
  "$ROOT_DIR/scripts/setup-psbc.sh"
  if [[ -x "$ROOT_DIR/scripts/build-python-ps4.sh" ]]; then
    "$ROOT_DIR/scripts/build-python-ps4.sh"
  fi
  "$ROOT_DIR/scripts/ensure-build-deps.sh" >/dev/null 2>&1 || true
fi

check_deps() {
  local missing=() f
  for f in lib/libcurl.a lib/libssl.a lib/libcrypto.a lib/libz.a lib/libtinyxml.a; do
    [[ -f "$DEPS_PS4/$f" ]] || missing+=("$f")
  done
  [[ -d "$DEPS_PS4/include" ]] || missing+=("include/")
  if [[ ${#missing[@]} -gt 0 ]]; then
    printf 'missing in %s: %s\n' "$DEPS_PS4" "${missing[*]}" >&2
    return 1
  fi
  [[ -d "$OPENGNM_PATH/include" ]] || { printf 'missing %s/include (use --bootstrap)\n' "$OPENGNM_PATH" >&2; return 1; }
  return 0
}

ensure_swig() {
  if [[ -x "$SWIG_PREFIX/bin/swig" ]] &&
     "$SWIG_PREFIX/bin/swig" -version 2>/dev/null | grep -q "SWIG Version 4.5"; then
    export PATH="$SWIG_PREFIX/bin:$PATH"
    return 0
  fi
  if [[ "$DO_BOOTSTRAP" == "1" ]]; then
    "$ROOT_DIR/scripts/setup-swig.sh"
    export PATH="$SWIG_PREFIX/bin:$PATH"
    return 0
  fi
  return 1
}

if [[ "$DO_CLEAN" == "1" ]]; then
  log "removing $BUILD_DIR and build/core-stage"
  rm -rf "$BUILD_DIR" "$ROOT_DIR/build/core-stage"
fi

if ! check_deps; then
  die "dependencies are incomplete; run ./build.sh --bootstrap"
fi

if ! ensure_swig; then
  die "SWIG 4.5 not found; run ./build.sh --bootstrap"
fi

if [[ "$DO_CHECK" == "1" ]]; then
  log "toolchain: $TOOLCHAIN"
  log "build:     $BUILD_DIR"
  log "deps:      $DEPS_PS4"
  log "opengnm:   $OPENGNM_PATH"
  log "swig:      $("$SWIG_PREFIX/bin/swig" -version | grep -m1 'SWIG Version')"
  log "version:   $VERSION"
  log "check OK"
  exit 0
fi

# --- Privacy and upstream --------------------------------------------------
log "checking privacy of packageable material"
"$ROOT_DIR/scripts/privacy-check.sh" "$ROOT_DIR/port" "$ROOT_DIR/packaging"

if [[ "$SKIP_UPSTREAM_CHECK" != "1" ]]; then
  log "checking xbmc/xbmc master changes"
  if ! "$ROOT_DIR/scripts/check-upstream.sh" --no-fetch --quiet; then
    warn "unreviewed upstream overlaps; see build/upstream-check.txt"
  fi
else
  warn "upstream check skipped (SKIP_UPSTREAM_CHECK=1)"
fi

# --- Configure and build ---------------------------------------------------
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  log "configuring build tree at $BUILD_DIR"
  "$ROOT_DIR/scripts/configure-kodi-ps4.sh" "$BUILD_DIR"
fi

# --- Shader blobs ----------------------------------------------------------
# The GUI runtime hashes the exact ES source; stale blobs leave the GUI
# black.  Regenerate only when the current sources do not match.
SHADER_SRC_DIR="${KODI_SRC_DIR:-$ROOT_DIR/src/kodi}/system/shaders/GLES/2.0"
SHADERBIN_DIR="$ROOT_DIR/build/shaderbin"
if ! "$ROOT_DIR/scripts/check-shader-bins.py" \
    --shaders-dir "$SHADER_SRC_DIR" --out "$SHADERBIN_DIR"; then
  log "regenerating shader blobs for the current GLES sources"
  [[ -x "$PSBC_BIN" ]] || die "psbc not found at $PSBC_BIN; run ./build.sh --bootstrap or --all"
  "$ROOT_DIR/scripts/gen-shader-bins.py" --shaders-dir "$SHADER_SRC_DIR" \
    --psbc "$PSBC_BIN" --out "$SHADERBIN_DIR"
  "$ROOT_DIR/scripts/gen-video-shader-bins.py" --shaders-dir "$SHADER_SRC_DIR" \
    --psbc "$PSBC_BIN" --out "$SHADERBIN_DIR"
  "$ROOT_DIR/scripts/check-shader-bins.py" \
    --shaders-dir "$SHADER_SRC_DIR" --out "$SHADERBIN_DIR" ||
    die "shader blob generation did not produce a complete set"
fi

log "building Kodi $VERSION and packaging"
"$ROOT_DIR/scripts/package-port.sh" "kodi-ps4-$VERSION"

PKG="$OUTPUT_DIR/kodi-ps4-$VERSION.pkg"
[[ -f "$PKG" ]] || die "PKG was not created: $PKG"

log "PKG ready: $PKG"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$PKG"
else
  shasum -a 256 "$PKG"
fi
printf 'size: %s bytes\n' "$(wc -c < "$PKG")"

if [[ "$DO_TAG" == "1" ]]; then
  git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not a git repository"
  git rev-parse "v$VERSION" >/dev/null 2>&1 && die "tag v$VERSION already exists"
  if ! git diff --quiet -- . ':(exclude)src/kodi' || \
     ! git diff --cached --quiet -- . ':(exclude)src/kodi'; then
    die "working tree is dirty; commit before creating the tag"
  fi
  git tag -a "v$VERSION" -m "Kodi PS4 $VERSION"
  log "local tag created: v$VERSION"
fi

if [[ "$DO_PUSH" == "1" ]]; then
  remote="$(git remote get-url origin 2>/dev/null || true)"
  [[ -n "$remote" ]] || die "no origin remote; add one before --push"
  log "publishing $VERSION to $remote"
  git push origin HEAD --tags
fi

log "next: test $PKG on console and keep the logs"
