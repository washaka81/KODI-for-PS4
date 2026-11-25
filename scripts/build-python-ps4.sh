#!/usr/bin/env bash
#
# build-python-ps4.sh — Cross-build CPython 3.14 for the PS4 (Orbis).
#
# Kodi embeds CPython for Python addons.  This script builds a static
# libpython3.14.a, its headers and its standard library under
# $DEPS_PS4 (default /tmp/kodi-deps-ps4).  It is idempotent: if the library
# is already installed it does nothing unless --force is passed.
#
# Usage: ./scripts/build-python-ps4.sh [--force]
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PS4SDK="${PS4SDK:-${OO_PS4_TOOLCHAIN:-/opt/ps4/sdk}}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_PS4="${DEPS_PS4:-/tmp/kodi-deps-ps4}"
VERSION="${PYTHON_VERSION:-3.14.6}"
SRC="${PYTHON_PS4_SRC:-/tmp/python-ps4/Python-${VERSION}}"
BUILD="${PYTHON_PS4_BUILD:-/tmp/python-ps4/build-${VERSION}}"
TOOLS="$REPO_ROOT/tools/python-ps4"
HOST_PYTHON="${HOST_PYTHON:-$(command -v python3.14 || command -v python3)}"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

log() { printf '\033[1;36m[python]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -d "$PS4SDK/include" ]] || die "PS4 SDK not found at $PS4SDK (set PS4SDK=...)"
[[ -n "$HOST_PYTHON" && -x "$HOST_PYTHON" ]] || die "host python3 not found"

if [[ $FORCE -eq 0 && -f "$DEPS_PS4/lib/libpython3.14.a" &&
      -f "$DEPS_PS4/include/python3.14/Python.h" &&
      -d "$DEPS_PS4/lib/python3.14" ]]; then
  log "libpython3.14 already installed in $DEPS_PS4"
  exit 0
fi

log "CPython $VERSION -> $DEPS_PS4"
rm -rf "$SRC" "$BUILD"
mkdir -p "$(dirname "$SRC")" "$BUILD"

archive="/tmp/python-ps4/Python-${VERSION}.tar.xz"
mkdir -p "$(dirname "$archive")"
if [[ ! -f "$archive" ]]; then
  curl -L --fail -o "$archive" \
    "https://www.python.org/ftp/python/${VERSION}/Python-${VERSION}.tar.xz" \
    || die "cannot download CPython $VERSION"
fi
tar xf "$archive" -C "$(dirname "$SRC")"

log "aplicando parches Orbis"
patch -d "$SRC" -p1 --forward < "$TOOLS/cpython-${VERSION}-orbis.patch" \
  || die "CPython patch failed"
# La instrumentación de getpath escribe la excepción real en
# /data/kodi-ps4/python-getpath.log; sin ella solo se ve el genérico
# "error evaluating path".
patch -d "$SRC" -p1 --forward < "$TOOLS/cpython-${VERSION}-getpath-debug.patch" \
  || die "CPython getpath debug patch failed"
# Orbis rechaza SOCK_CLOEXEC/SOCK_NONBLOCK en socket()/accept4()/socketpair()
# con EPROTONOSUPPORT (43), igual que libcurl; no pasar esos flags.
patch -d "$SRC" -p1 --forward < "$TOOLS/cpython-${VERSION}-orbis-socket.patch" \
  || die "CPython Orbis socket patch failed"
# Orbis no implementa uname(): os.uname() devuelve ENOSYS y platform.uname()
# rompe service.xbmc.versioncheck.  Tratarlo como "no disponible".
patch -d "$SRC" -p1 --forward < "$TOOLS/cpython-${VERSION}-platform-orbis.patch" \
  || die "CPython Orbis platform patch failed"

CLANG_RES="$(clang -print-resource-dir)"
# libc.a has 4-byte wchar_t wide functions for Kodi's C++ (see
# scripts/patch-libc-wchar.sh).  CPython is 2-byte here, so route its wide
# calls to the private implementations in tools/python-ps4/ps4_wchar.c.
WCHAR_RENAMES="-Dwcslen=_Py_ps4_wcslen -Dwmemcpy=_Py_ps4_wmemcpy \
-Dwmemmove=_Py_ps4_wmemmove -Dwmemset=_Py_ps4_wmemset \
-Dwmemcmp=_Py_ps4_wmemcmp -Dwmemchr=_Py_ps4_wmemchr"
CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -fshort-wchar -funwind-tables \
-nostdinc -isysroot $PS4SDK -isystem $PS4SDK/include \
-isystem $PS4SDK/include/c++/v1 -isystem /tmp/ps4sdk-include-copy \
-isystem $CLANG_RES/include -I$DEPS_PS4/include -D__ORBIS__ -D_BSD_SOURCE \
-D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L $WCHAR_RENAMES"
LDFLAGS="-nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 \
-Wl,--script,$PS4SDK/link.x -Wl,--eh-frame-hdr -L$PS4SDK/lib \
-L$DEPS_PS4/lib -lc -lkernel -lc++"

(
  cd "$BUILD"
  CONFIG_SITE="$TOOLS/config.site" \
  CC=clang CXX=clang++ AR=llvm-ar RANLIB=llvm-ranlib \
  CFLAGS="$CFLAGS" CXXFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
  PKG_CONFIG_PATH="$DEPS_PS4/lib/pkgconfig" \
  "$SRC/configure" \
    --host=x86_64-unknown-freebsd12 \
    --build=x86_64-pc-linux-gnu \
    --with-build-python="$HOST_PYTHON" \
    --disable-shared --without-ensurepip --disable-test-modules \
    --with-openssl="$DEPS_PS4" --without-readline --disable-ipv6 \
    > configure.log 2>&1 || { tail -40 configure.log; die "CPython configure failed"; }

  cp "$TOOLS/Setup.local" Modules/Setup.local

  log "compilando libpython3.14.a"
  make -j"$(nproc)" libpython3.14.a > build.log 2>&1 \
    || { tail -60 build.log; die "CPython build failed"; }

  # libmpdec and HACL are static helper archives in a normal shared build.
  # Merge them into libpython for the static PS4 link, renaming the libmpdec
  # objects so they cannot collide with Python's own context.o.
  tmp_mpd="$(mktemp -d)"
  for f in Modules/_decimal/libmpdec/*.o; do
    cp "$f" "$tmp_mpd/mpd_$(basename "$f")"
  done
  tmp_expat="$(mktemp -d)"
  for f in Modules/expat/*.o; do
    cp "$f" "$tmp_expat/expat_$(basename "$f")"
  done
  # Private 2-byte wchar_t helpers (see the top of the file).
  tmp_wchar="$(mktemp -d)"
  clang $CFLAGS -c "$TOOLS/ps4_wchar.c" -o "$tmp_wchar/ps4_wchar.o" \
    || die "CPython wchar_t helpers failed to compile"
  llvm-ar rcs libpython3.14.a "$tmp_mpd"/*.o "$tmp_expat"/*.o \
    "$tmp_wchar/ps4_wchar.o" Modules/_hacl/*.o
  rm -rf "$tmp_mpd" "$tmp_expat" "$tmp_wchar"

  log "instalando headers, librería y stdlib"
  mkdir -p "$DEPS_PS4/include/python3.14" "$DEPS_PS4/lib/python3.14"
  cp -a "$SRC"/Include/. "$DEPS_PS4/include/python3.14/"
  cp pyconfig.h "$DEPS_PS4/include/python3.14/"
  cp libpython3.14.a "$DEPS_PS4/lib/"
  cp -a "$SRC"/Lib/. "$DEPS_PS4/lib/python3.14/"
  if [[ -f "$ROOT_DIR/port/python/sitecustomize.py" ]]; then
    cp "$ROOT_DIR/port/python/sitecustomize.py" "$DEPS_PS4/lib/python3.14/"
  fi
  find "$DEPS_PS4/lib/python3.14" -type d -name '__pycache__' \
    -prune -exec rm -rf {} + 2>/dev/null || true
)

log "instalado: $DEPS_PS4/lib/libpython3.14.a"
log "stdlib:    $DEPS_PS4/lib/python3.14"
