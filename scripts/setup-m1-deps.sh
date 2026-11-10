#!/usr/bin/env bash
#
# setup-m1-deps.sh — Recreate the ephemeral M1 build environment.
#
# The Kodi PS4 cross-build depends on artifacts that live outside the repo
# (/tmp) and disappear on reboot.  This script recreates them idempotently:
# running it twice is a no-op for already valid artifacts.
#
# Artifacts produced:
#   /tmp/ps4sdk-include-copy        Staging copy of the PS4 SDK headers
#   /tmp/kodi-ps4-host-includes     Host headers missing from the PS4 sysroot
#   /tmp/TexturePacker-build        Native (host) TexturePacker binary
#   /tmp/kodi-ps4-host-tools        Host build tools (flatc, JsonSchemaBuilder)
#   /tmp/kodi-deps-ps4              Target libraries (TinyXML, FreeType, curl, ...)
#   /tmp/nasm-ps4                   NASM assembler built from source
#
# Usage: ./scripts/setup-m1-deps.sh [--force]
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PS4SDK="${PS4SDK:-${OO_PS4_TOOLCHAIN:-/opt/ps4/sdk}}"
STAGE_SDK=/tmp/ps4sdk-include-copy
STAGE_HOST=/tmp/kodi-ps4-host-includes
TP_BUILD=/tmp/TexturePacker-build
DEPS_PS4=/tmp/kodi-deps-ps4
NASM_PREFIX=/tmp/nasm-ps4
NASM_VERSION=2.16.03
TINYXML_VERSION=2.6.2_2
FLATBUFFERS_VERSION=23.3.3
FREETYPE_VERSION=2.14.2
CURL_VERSION=8.20.0
ZLIB_VERSION=1.3.2
PCRE2_VERSION=10.47
SQLITE3_VERSION=3390400
FRIBIDI_VERSION=1.0.16
HARFBUZZ_VERSION=14.2.0
LIBASS_VERSION=0.17.5
OPENSSL_VERSION=3.5.7
LIBDVDNAV_VERSION=7.0.0
LIBDVDREAD_VERSION=7.0.1
HOST_TOOLS=/tmp/kodi-ps4-host-tools

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

log() { printf '\033[1;36m[setup]\033[0m %s\n' "$*"; }
ok()  { printf '\033[1;32m[ ok ]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -d "$PS4SDK/include" ]] || die "PS4 SDK not found at $PS4SDK (set PS4SDK=...)"

# --------------------------------------------------------------------------
# 1. SDK patches owned by this repo
# --------------------------------------------------------------------------
# The PS4 SDK ships libc++ 11, which has no <ranges>.  Install the polyfill
# kept under tools/ps4/include before the headers are staged.
if [[ $FORCE -eq 1 || ! -f "$PS4SDK/include/ranges" ]] || \
   ! cmp -s "$REPO_ROOT/tools/ps4/include/ranges" "$PS4SDK/include/ranges"; then
  log "installing std::ranges polyfill -> $PS4SDK/include/ranges"
  cp "$REPO_ROOT/tools/ps4/include/ranges" "$PS4SDK/include/ranges"
else
  ok "std::ranges polyfill up to date"
fi

if [[ $FORCE -eq 1 || ! -f "$PS4SDK/include/sys/sysctl.h" ]] || \
   ! cmp -s "$REPO_ROOT/tools/ps4/include/sys/sysctl.h" "$PS4SDK/include/sys/sysctl.h"; then
  log "installing sys/sysctl.h stub -> $PS4SDK/include/sys/sysctl.h"
  mkdir -p "$PS4SDK/include/sys"
  cp "$REPO_ROOT/tools/ps4/include/sys/sysctl.h" "$PS4SDK/include/sys/sysctl.h"
else
  ok "sys/sysctl.h stub up to date"
fi

if [[ $FORCE -eq 1 || ! -f "$PS4SDK/include/sys/endian.h" ]] || \
   ! cmp -s "$REPO_ROOT/tools/ps4/include/sys/endian.h" "$PS4SDK/include/sys/endian.h"; then
  log "installing sys/endian.h stub -> $PS4SDK/include/sys/endian.h"
  mkdir -p "$PS4SDK/include/sys"
  cp "$REPO_ROOT/tools/ps4/include/sys/endian.h" "$PS4SDK/include/sys/endian.h"
else
  ok "sys/endian.h stub up to date"
fi

# The SDK stropts.h redeclares ioctl with a signature that clashes with
# sys/ioctl.h, which breaks any translation unit that includes both.
if grep -q "^int ioctl(int, int, ...);" "$PS4SDK/include/stropts.h"; then
  log "removing the duplicate ioctl declaration from $PS4SDK/include/stropts.h"
  sed -i '/^int ioctl(int, int, ...);$/d' "$PS4SDK/include/stropts.h"
else
  ok "stropts.h ioctl declaration already removed"
fi

# libc++ <cmath> does "using ::abs;", but the SDK math.h does not declare
# abs, so any C++ translation unit that includes <cmath> first fails.
if ! grep -q "^int abs(int);" "$PS4SDK/include/math.h"; then
  log "adding the abs declaration to $PS4SDK/include/math.h"
  python3 - "$PS4SDK/include/math.h" <<'PY'
import sys

path = sys.argv[1]
with open(path) as f:
    text = f.read()

marker = "#ifdef __cplusplus\n}\n#endif\n"
idx = text.rfind(marker)
if idx == -1:
    sys.exit("cannot find the end of the C++ block in math.h")
text = text[:idx] + "int abs(int);\n\n" + text[idx:]
with open(path, "w") as f:
    f.write(text)
PY
else
  ok "math.h abs declaration present"
fi


# The SDK link.x only collects *(.tbss), so the .tbss.* sections (one per
# thread_local variable) are left as separate sections.  The PS4 loader
# does not handle them well and the process dies before running the
# constructors.  Merge them all into .tls.
if grep -qF '*(.tbss.*)' "$PS4SDK/link.x"; then
  ok "link.x already merges the .tbss.* sections"
else
  log "merging the .tbss.* sections in $PS4SDK/link.x"
  python3 - "$PS4SDK/link.x" <<'PY2'
import sys

path = sys.argv[1]
with open(path) as f:
    text = f.read()

old = "\t.tls : {\n\t\t*(.tdata);\n\t\t*(.tbss);\n\t}\n"
new = ("\t.tls : {\n\t\t*(.tdata);\n\t\t*(.tdata.*);\n"
       "\t\t*(.tbss);\n\t\t*(.tbss.*);\n\t}\n")
if old not in text:
    sys.exit("cannot find the .tls block in link.x")
with open(path, "w") as f:
    f.write(text.replace(old, new))
PY2
fi

if grep -q "SORT_BY_INIT_PRIORITY" "$PS4SDK/link.x"; then
  ok "link.x already sorts the init array by priority"
else
  log "adding SORT_BY_INIT_PRIORITY to $PS4SDK/link.x"
  python3 - "$PS4SDK/link.x" <<'PY'
import sys

path = sys.argv[1]
with open(path) as f:
    text = f.read()

old = """\t.init_array : {
\t\t*(.init_array);
\t}
"""
new = """\t.init_array : {
\t\tKEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
\t\tKEEP (*(.init_array))
\t}
"""
if old not in text:
    sys.exit("cannot find the .init_array block in link.x")
with open(path, "w") as f:
    f.write(text.replace(old, new))
PY
fi


# The SDK names the members of struct in6_addr differently from FreeBSD,
# which breaks the OpenSSL and libass sources.
if grep -q "__in6_union" "$PS4SDK/include/netinet/in.h"; then
  log "aligning struct in6_addr with the FreeBSD layout"
  python3 - "$PS4SDK/include/netinet/in.h" <<'PY'
import sys

path = sys.argv[1]
with open(path) as f:
    text = f.read()

old = """struct in6_addr {
\tunion {
\t\tuint8_t __s6_addr[16];
\t\tuint16_t __s6_addr16[8];
\t\tuint32_t __s6_addr32[4];
\t} __in6_union;
};
#define s6_addr __in6_union.__s6_addr
#define s6_addr16 __in6_union.__s6_addr16
#define s6_addr32 __in6_union.__s6_addr32"""

new = """struct in6_addr {
\tunion {
\t\tuint8_t __u6_addr8[16];
\t\tuint16_t __u6_addr16[8];
\t\tuint32_t __u6_addr32[4];
\t} __u6_addr;
};
#define s6_addr __u6_addr.__u6_addr8
#define s6_addr16 __u6_addr.__u6_addr16
#define s6_addr32 __u6_addr.__u6_addr32"""

if old not in text:
    sys.exit("unexpected in6_addr definition in in.h")
with open(path, "w") as f:
    f.write(text.replace(old, new))
PY
else
  ok "struct in6_addr already uses the FreeBSD layout"
fi

# --------------------------------------------------------------------------
# 2. Header staging
# --------------------------------------------------------------------------
if [[ $FORCE -eq 1 || ! -d "$STAGE_SDK/GLES2" ]]; then
  log "staging PS4 SDK headers -> $STAGE_SDK"
  rm -rf "$STAGE_SDK"
  cp -a "$PS4SDK/include" "$STAGE_SDK"
else
  ok "PS4 SDK headers staged ($STAGE_SDK)"
fi
# The staged copy is what the toolchain puts on the include path, so keep it
# in sync with the patches applied to the SDK.
cp "$PS4SDK/include/ranges" "$STAGE_SDK/ranges"
cp "$PS4SDK/include/stropts.h" "$STAGE_SDK/stropts.h"
cp "$PS4SDK/include/math.h" "$STAGE_SDK/math.h"
cp "$PS4SDK/include/netinet/in.h" "$STAGE_SDK/netinet/in.h"
mkdir -p "$STAGE_SDK/sys"
cp "$PS4SDK/include/sys/endian.h" "$STAGE_SDK/sys/endian.h"
cp "$PS4SDK/include/math.h" "$STAGE_SDK/math.h"
# The SDK does not ship the compiler intrinsics headers, which some of the
# dependencies (harfbuzz) need.  Only the x86 ones are staged, so the
# compiler provided std* headers keep their precedence.
if [[ ! -f "$STAGE_SDK/emmintrin.h" ]]; then
  log "staging compiler intrinsics headers -> $STAGE_SDK"
  clang_resource="$(clang++ -print-resource-dir)"
  for header in "$clang_resource"/include/*intrin*.h "$clang_resource"/include/mm_malloc.h; do
    [[ -f "$header" ]] && cp "$header" "$STAGE_SDK/"
  done
else
  ok "compiler intrinsics headers staged"
fi
# The SDK only ships GLES2 headers, but the Kodi GLES code needs the GLES3
# enumerations.  The Piglet GL implementation is GLES3 capable, so the host
# headers are staged next to the SDK ones.
if [[ -d /usr/include/GLES3 && ! -d "$STAGE_SDK/GLES3" ]]; then
  log "staging host GLES3 headers -> $STAGE_SDK/GLES3"
  cp -a /usr/include/GLES3 "$STAGE_SDK/GLES3"
fi

# The DVD navigation code only needs the dvdnav headers, the library itself
# is loaded at runtime through the dynamic dll wrapper.
stage_dvd_headers() {
  local name="$1" version="$2" srcdir="$3"
  local work="/tmp/${name}-headers"
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/${name}.tar.bz2" \
    "https://mirrors.kodi.tv/build-deps/sources/${name}-${version}.tar.bz2" \
    || die "cannot download $name"
  tar xf "$work/${name}.tar.bz2" -C "$work"
  mkdir -p "$STAGE_SDK/$srcdir"
  cp "$work"/${name}-${version}/src/${srcdir}/*.h "$STAGE_SDK/$srcdir/" 2>/dev/null || true
}

if [[ ! -d "$STAGE_SDK/dvdnav" ]]; then
  log "staging the dvdnav headers -> $STAGE_SDK/dvdnav"
  stage_dvd_headers libdvdnav "$LIBDVDNAV_VERSION" dvdnav
  # version.h is generated by the build, the headers only need the macros.
  sed -e "s/@DVDNAV_MAJOR@/7/" \
      -e "s/@DVDNAV_MINOR@/0/" \
      -e "s/@DVDNAV_SUB@/0/" \
      "/tmp/libdvdnav-headers/libdvdnav-${LIBDVDNAV_VERSION}/src/dvdnav/version.h.in" \
      >"$STAGE_SDK/dvdnav/version.h"
fi

if [[ ! -d "$STAGE_SDK/dvdread" ]]; then
  log "staging the dvdread headers -> $STAGE_SDK/dvdread"
  stage_dvd_headers libdvdread "$LIBDVDREAD_VERSION" dvdread
  if [[ -f "/tmp/libdvdread-headers/libdvdread-${LIBDVDREAD_VERSION}/src/dvdread/version.h.in" ]]; then
    sed -e "s/@DVDREAD_VERSION_MAJOR@/7/" \
        -e "s/@DVDREAD_VERSION_MINOR@/0/" \
        -e "s/@DVDREAD_VERSION_PATCH@/1/" \
        "/tmp/libdvdread-headers/libdvdread-${LIBDVDREAD_VERSION}/src/dvdread/version.h.in" \
        >"$STAGE_SDK/dvdread/version.h"
  fi
fi

if [[ $FORCE -eq 1 || ! -f "$STAGE_HOST/zlib_name_mangling.h" ]]; then
  log "staging host headers -> $STAGE_HOST"
  rm -rf "$STAGE_HOST"
  mkdir -p "$STAGE_HOST"
  cp -a /usr/include/openssl "$STAGE_HOST/" 2>/dev/null || true
  cp -a /usr/include/fontconfig "$STAGE_HOST/" 2>/dev/null || true
  cp /usr/include/zlib.h /usr/include/zconf.h /usr/include/zlib_name_mangling.h \
     "$STAGE_HOST/" 2>/dev/null || true
else
  ok "host headers staged ($STAGE_HOST)"
fi

# --------------------------------------------------------------------------
# 3. TexturePacker (host binary, consumes Kodi texture definitions)
# --------------------------------------------------------------------------
if [[ $FORCE -eq 1 || ! -x "$TP_BUILD/TexturePacker" ]]; then
  log "building TexturePacker (host) -> $TP_BUILD"
  rm -rf "$TP_BUILD"
  cmake -S "$REPO_ROOT/src/kodi/tools/depends/native/TexturePacker/src" \
        -B "$TP_BUILD" \
        -DKODI_SOURCE_DIR="$REPO_ROOT/src/kodi" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DARCH_DEFINES=TARGET_POSIX >"$TP_BUILD.log" 2>&1 \
    || { cat "$TP_BUILD.log"; die "TexturePacker configure/build failed"; }
  cmake --build "$TP_BUILD" -j"$(nproc)" >>"$TP_BUILD.log" 2>&1 \
    || { cat "$TP_BUILD.log"; die "TexturePacker build failed"; }
else
  ok "TexturePacker present ($TP_BUILD/TexturePacker)"
fi

# --------------------------------------------------------------------------
# 3. TinyXML v1 for the PS4 target
# --------------------------------------------------------------------------
if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libtinyxml.a" ]]; then
  log "building TinyXML $TINYXML_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/tinyxml-build-ps4
  rm -rf "$work" "$DEPS_PS4"
  mkdir -p "$work"
  curl -L --fail -o "$work/tinyxml.tar.gz" \
    "https://mirrors.kodi.tv/build-deps/sources/tinyxml-${TINYXML_VERSION}.tar.gz" \
    || die "cannot download TinyXML"
  tar xf "$work/tinyxml.tar.gz" -C "$work"
  pushd "$work/tinyxml-${TINYXML_VERSION}/src" >/dev/null
  clang++ --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -nostdinc \
    -isysroot "$PS4SDK" -isystem "$PS4SDK/include" \
    -isystem "$PS4SDK/include/c++/v1" -DTIXML_USE_STL -I../include \
    -c tinyxml.cpp tinyxmlerror.cpp tinyxmlparser.cpp tinystr.cpp \
    || die "TinyXML compile failed"
  ar rcs libtinyxml.a tinyxml.o tinyxmlerror.o tinyxmlparser.o tinystr.o
  mkdir -p "$DEPS_PS4/include" "$DEPS_PS4/lib"
  cp ../include/tinyxml.h ../include/tinystr.h "$DEPS_PS4/include/"
  cp libtinyxml.a "$DEPS_PS4/lib/"
  popd >/dev/null
else
  ok "TinyXML present ($DEPS_PS4/lib/libtinyxml.a)"
fi

# --------------------------------------------------------------------------
# 4. FreeType for the PS4 target
# --------------------------------------------------------------------------
# The SDK ships FreeType 2.5 headers, which are too old for the Kodi guilib
# and shadow the ones built here, so the target gets its own build.
if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libfreetype.a" ]]; then
  log "building FreeType $FREETYPE_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/freetype-ps4-build
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/freetype.tar.xz" \
    "https://mirrors.kodi.tv/build-deps/sources/freetype-${FREETYPE_VERSION}.tar.xz" \
    || die "cannot download FreeType"
  tar xf "$work/freetype.tar.xz" -C "$work"
  cmake -S "$work/freetype-${FREETYPE_VERSION}" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/ps4sdk-direct.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PS4" \
    -DCMAKE_SYSTEM_NAME=Orbis \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_ZLIB=ON \
    -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_REQUIRE_ZLIB=ON \
    >"$work/freetype.log" 2>&1 || { tail -30 "$work/freetype.log"; die "FreeType configure failed"; }
  cmake --build "$work/build" -j"$(nproc)" >>"$work/freetype.log" 2>&1 \
    || { tail -30 "$work/freetype.log"; die "FreeType build failed"; }
  cmake --install "$work/build" >>"$work/freetype.log" 2>&1 \
    || { tail -30 "$work/freetype.log"; die "FreeType install failed"; }
else
  ok "FreeType present ($DEPS_PS4/lib/libfreetype.a)"
fi

# The SDK FreeType headers are older than what Kodi needs and take
# precedence over the ones installed above.
rm -rf "$STAGE_SDK/freetype" "$STAGE_SDK/ft2build.h"

# --------------------------------------------------------------------------
# 5. zlib for the PS4 target
# --------------------------------------------------------------------------
# zlib is built before libcurl: curl needs its headers to compile.
if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libz.a" ]]; then
  log "building zlib $ZLIB_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/zlib-ps4-build
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/zlib.tar.xz" \
    "https://mirrors.kodi.tv/build-deps/sources/zlib-${ZLIB_VERSION}.tar.xz" \
    || die "cannot download zlib"
  tar xf "$work/zlib.tar.xz" -C "$work"
  cmake -S "$work/zlib-${ZLIB_VERSION}" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/ps4sdk-direct.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PS4" \
    -DZLIB_BUILD_TESTING=OFF \
    -DZLIB_BUILD_SHARED=OFF \
    >"$work/zlib.log" 2>&1 || { tail -30 "$work/zlib.log"; die "zlib configure failed"; }
  cmake --build "$work/build" -j"$(nproc)" >>"$work/zlib.log" 2>&1 \
    || { tail -30 "$work/zlib.log"; die "zlib build failed"; }
  cmake --install "$work/build" >>"$work/zlib.log" 2>&1 \
    || { tail -30 "$work/zlib.log"; die "zlib install failed"; }
else
  ok "zlib present ($DEPS_PS4/lib/libz.a)"
fi

# --------------------------------------------------------------------------
# 6. libcurl for the PS4 target
# --------------------------------------------------------------------------
# Kodi links libcurl directly, so the PS4 build needs its own copy.  The
# standard socket backend is used, which the PS4 libc provides on top of
# SceNet.
if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libcurl.a" ]]; then
  log "building libcurl $CURL_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/curl-ps4-build
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/curl.tar.xz" \
    "https://mirrors.kodi.tv/build-deps/sources/curl-${CURL_VERSION}.tar.xz" \
    || die "cannot download curl"
  tar xf "$work/curl.tar.xz" -C "$work"
  cmake -S "$work/curl-${CURL_VERSION}" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/ps4sdk-direct.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PS4" \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_TESTING=OFF \
    -DENABLE_MANUAL=OFF \
    -DENABLE_CURLDEBUG=OFF \
    -DCURL_USE_OPENSSL=OFF \
    -DUSE_NGHTTP2=OFF \
    -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF \
    -DUSE_LIBIDN2=OFF \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBSSH2=OFF \
    -DCURL_USE_GSSAPI=OFF \
    -DENABLE_ARES=OFF \
    -DCURL_DISABLE_LDAP=ON \
    -DCURL_CA_BUNDLE= \
    -DCURL_DISABLE_INSTALL=OFF \
    -DPS4_EXTRA_C_FLAGS=-DSOL_IP=0 \
    >"$work/curl.log" 2>&1 || { tail -40 "$work/curl.log"; die "curl configure failed"; }
  cmake --build "$work/build" -j"$(nproc)" >>"$work/curl.log" 2>&1 \
    || { tail -40 "$work/curl.log"; die "curl build failed"; }
  cmake --install "$work/build" >>"$work/curl.log" 2>&1 \
    || { tail -40 "$work/curl.log"; die "curl install failed"; }
else
  ok "libcurl present ($DEPS_PS4/lib/libcurl.a)"
fi

# --------------------------------------------------------------------------
# 7. pcre2 and sqlite3 for the PS4 target
# --------------------------------------------------------------------------
# These are required dependencies of Kodi that the CMake depends system has
# no build macro for, so they are cross built here.
if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libpcre2-8.a" ]]; then
  log "building pcre2 $PCRE2_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/pcre2-ps4-build
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/pcre2.tar.bz2" \
    "https://mirrors.kodi.tv/build-deps/sources/pcre2-${PCRE2_VERSION}.tar.bz2" \
    || die "cannot download pcre2"
  tar xf "$work/pcre2.tar.bz2" -C "$work"
  cmake -S "$work/pcre2-${PCRE2_VERSION}" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/ps4sdk-direct.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PS4" \
    -DPCRE2_BUILD_PCRE2GREP=OFF \
    -DPCRE2_BUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DPCRE2_SUPPORT_JIT=OFF \
    >"$work/pcre2.log" 2>&1 || { tail -30 "$work/pcre2.log"; die "pcre2 configure failed"; }
  cmake --build "$work/build" -j"$(nproc)" >>"$work/pcre2.log" 2>&1 \
    || { tail -30 "$work/pcre2.log"; die "pcre2 build failed"; }
  cmake --install "$work/build" >>"$work/pcre2.log" 2>&1 \
    || { tail -30 "$work/pcre2.log"; die "pcre2 install failed"; }
else
  ok "pcre2 present ($DEPS_PS4/lib/libpcre2-8.a)"
fi

if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libsqlite3.a" ]]; then
  log "building sqlite3 $SQLITE3_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/sqlite3-ps4-build
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/sqlite3.tar.gz" \
    "https://mirrors.kodi.tv/build-deps/sources/sqlite-autoconf-${SQLITE3_VERSION}.tar.gz" \
    || die "cannot download sqlite3"
  tar xf "$work/sqlite3.tar.gz" -C "$work"
  # The PS4 kernel refuses lstat (EPERM on xFullPathname); SQLite resolves
  # symlinks there with lstat/readlink, so drop those defines and let it take
  # the plain path.
  sed -i 's/#define HAVE_READLINK 1//; s/#define HAVE_LSTAT 1//' \
    "$work/sqlite-autoconf-${SQLITE3_VERSION}/sqlite3.c"
  # The amalgamation is compiled directly, the autotools wrapper cannot be
  # used for a cross build without a full toolchain file.
  # The PS4 libc treats pthread_mutexattr_t as an 8-byte handle while the SDK
  # header declares 4 bytes; SQLite's recursive mutexes use that attr, so use
  # the home-grown implementation instead.
  #
  # SQLite also stores libc function addresses in its VFS syscall table.  The
  # PS4 loader only resolves JUMP_SLOT entries, so an imported address would
  # become the module base.  fchmod has no PLT stub; wrap it in a local
  # function (the macro renames both the table entry and the calls).
  cat >"$work/ps4_syscalls.c" <<'EOF'
#include <sys/types.h>
#include <sys/stat.h>

int ps4_fchmod(int fd, mode_t mode)
{
    return fchmod(fd, mode);
}
EOF
  cat >"$work/ps4_trace_vfs.c" <<'EOF'
/* Trace VFS: wraps the dotfile VFS and logs every VFS call (open/delete/
 * access/fullpath) plus the per-file I/O (read/write/sync/truncate/size/
 * fetch) into /data/kodi-ps4/sqlite-vfs.log, so a failure can be pinned to
 * the exact call — including what a fresh descriptor sees of a database
 * another connection just wrote (file size + first bytes of page 1).
 */
#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void ps4_trace_raw(const char* line, int n)
{
    static size_t trace_bytes;
    int fd = open("/data/kodi-ps4/sqlite-vfs.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)n);
        trace_bytes += (size_t)n;
        if (trace_bytes > 4u * 1024u * 1024u) {
            (void)ftruncate(fd, 0);
            trace_bytes = 0;
        }
        (void)close(fd);
    }
}

static void ps4_trace_log(const char* op, int rc, const char* path)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "%s rc=%d err=%d errx=%s file=%s\n",
                     op, rc, errno, strerror(errno), path ? path : "");
    ps4_trace_raw(line, n);
}

typedef struct {
    sqlite3_vfs base;
    sqlite3_vfs* child;
} ps4_trace_vfs;

/* File shim: the io methods delegate to the child file and the interesting
 * ones are logged.  The child file object lives right after the shim struct
 * in the block SQLite allocates (base.szOsFile covers both). */
typedef struct {
    sqlite3_file base;
    sqlite3_file* child;
    unsigned serial;
    char name[96];
} ps4_trace_file;

static int ps4_trace_xclose(sqlite3_file* f);
static int ps4_trace_xread(sqlite3_file* f, void* buf, int amt, sqlite3_int64 ofst);
static int ps4_trace_xwrite(sqlite3_file* f, const void* buf, int amt,
                            sqlite3_int64 ofst);
static int ps4_trace_xtruncate(sqlite3_file* f, sqlite3_int64 size);
static int ps4_trace_xsync(sqlite3_file* f, int flags);
static int ps4_trace_xfilesize(sqlite3_file* f, sqlite3_int64* size);
static int ps4_trace_xlock(sqlite3_file* f, int level);
static int ps4_trace_xunlock(sqlite3_file* f, int level);
static int ps4_trace_xcheckreservedlock(sqlite3_file* f, int* res);
static int ps4_trace_xfilecontrol(sqlite3_file* f, int op, void* arg);
static int ps4_trace_xsectorsize(sqlite3_file* f);
static int ps4_trace_xdevicechar(sqlite3_file* f);
static int ps4_trace_xshmmap(sqlite3_file* f, int region, int sz, int w,
                             volatile void** pm);
static int ps4_trace_xshmlock(sqlite3_file* f, int off, int n, int flags);
static void ps4_trace_xshmbarrier(sqlite3_file* f);
static int ps4_trace_xshmunmap(sqlite3_file* f, int region);
static int ps4_trace_xfetch(sqlite3_file* f, sqlite3_int64 ofst, int amt, void** pm);
static int ps4_trace_xunfetch(sqlite3_file* f, sqlite3_int64 ofst, void* page);

/* Every entry delegates through the shimmed file's child at runtime. */
static sqlite3_io_methods g_ps4_trace_io = {
    3,                              /* iVersion */
    ps4_trace_xclose,
    ps4_trace_xread,
    ps4_trace_xwrite,
    ps4_trace_xtruncate,
    ps4_trace_xsync,
    ps4_trace_xfilesize,
    ps4_trace_xlock,
    ps4_trace_xunlock,
    ps4_trace_xcheckreservedlock,
    ps4_trace_xfilecontrol,
    ps4_trace_xsectorsize,
    ps4_trace_xdevicechar,
    ps4_trace_xshmmap,
    ps4_trace_xshmlock,
    ps4_trace_xshmbarrier,
    ps4_trace_xshmunmap,
    ps4_trace_xfetch,
    ps4_trace_xunfetch,
};

static unsigned g_ps4_trace_serial;

/* PS4: fstat() reports stale file sizes (a fresh descriptor sees the size of
 * an old on-disk state even though write()+fsync() succeeded and read()
 * returns the written bytes).  SQLite needs a trustworthy xFileSize to
 * validate the page count in the header, so track the end of every write
 * per path and report the maximum of both. */
typedef struct {
    char path[96];
    sqlite3_int64 size;
} ps4_size_entry;

static ps4_size_entry g_ps4_sizes[128];

static sqlite3_int64 ps4_size_lookup(const char* path)
{
    int i;
    for (i = 0; i < 128; i++)
        if (g_ps4_sizes[i].path[0] && strcmp(g_ps4_sizes[i].path, path) == 0)
            return g_ps4_sizes[i].size;
    return 0;
}

static void ps4_size_update(const char* path, sqlite3_int64 end)
{
    ps4_size_entry* free_slot = NULL;
    int i;
    for (i = 0; i < 128; i++) {
        if (g_ps4_sizes[i].path[0] && strcmp(g_ps4_sizes[i].path, path) == 0) {
            if (end > g_ps4_sizes[i].size)
                g_ps4_sizes[i].size = end;
            return;
        }
        if (!free_slot && !g_ps4_sizes[i].path[0])
            free_slot = &g_ps4_sizes[i];
    }
    if (free_slot) {
        snprintf(free_slot->path, sizeof(free_slot->path), "%s", path);
        free_slot->size = end;
    }
}

static void ps4_size_clear(const char* path)
{
    int i;
    for (i = 0; i < 128; i++)
        if (g_ps4_sizes[i].path[0] && strcmp(g_ps4_sizes[i].path, path) == 0)
            g_ps4_sizes[i].path[0] = 0;
}

/* A database opened from a previous run is not in the write-tracking table.
 * fstat() on the fresh descriptor lies (typically 0 or 128), so seed the
 * tracked size from the path before SQLite validates the page count.
 * stat()/lseek() normally see the real directory/inode size; if both lie,
 * fall back to reading the file once. */
static void ps4_size_init_from_path(const char* path)
{
    struct stat st;
    sqlite3_int64 size = 0;
    if (!path || !path[0])
        return;
    if (stat(path, &st) == 0 && st.st_size > 0)
        size = st.st_size;
    /* stat() and lseek() lie on PS4 (a fresh descriptor sees the size of an
     * old on-disk state).  Always count the bytes by reading the file once;
     * the databases here are small and this is the only trustworthy size. */
    {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[65536];
            ssize_t n;
            sqlite3_int64 readsize = 0;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                readsize += n;
            close(fd);
            if (readsize > size)
                size = readsize;
        }
    }
    if (size > 0) {
        char line[256];
        int n;
        ps4_size_update(path, size);
        n = snprintf(line, sizeof(line), "size-init size=%lld file=%s\n",
                     (long long)size, path);
        ps4_trace_raw(line, n);
    }
}

static void ps4_trace_flog(ps4_trace_file* p, const char* op, int rc,
                           long long a, long long b)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "f=%u %s rc=%d a=%lld b=%lld file=%s\n",
                     p->serial, op, rc, a, b, p->name);
    ps4_trace_raw(line, n);
}

static int ps4_trace_xclose(sqlite3_file* f)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = SQLITE_OK;
    if (p->child->pMethods)
        rc = p->child->pMethods->xClose(p->child);
    ps4_trace_flog(p, "xclose", rc, 0, 0);
    return rc;
}

static int ps4_trace_xread(sqlite3_file* f, void* buf, int amt, sqlite3_int64 ofst)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xRead(p->child, buf, amt, ofst);
    if (ofst == 0 || rc != SQLITE_OK) {
        char head[49];
        char* h = head;
        const unsigned char* b = (const unsigned char*)buf;
        int i;
        for (i = 0; i < 16; i++)
            h += snprintf(h, 4, "%02x", b[i]);
        char line[640];
        int n = snprintf(line, sizeof(line),
                         "f=%u xread rc=%d amt=%d ofst=%lld head=%s file=%s\n",
                         p->serial, rc, amt, ofst, head, p->name);
        ps4_trace_raw(line, n);
    }
    else {
        ps4_trace_flog(p, "xread", rc, amt, ofst);
    }
    return rc;
}

static int ps4_trace_xwrite(sqlite3_file* f, const void* buf, int amt,
                            sqlite3_int64 ofst)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xWrite(p->child, buf, amt, ofst);
    ps4_trace_flog(p, "xwrite", rc, amt, ofst);
    if (rc == SQLITE_OK) {
        ps4_size_update(p->name, ofst + amt);
        /* PS4: write() results are lost on crash (fsync()/close() are
         * not durable), so kick the kernel sync after every write; the
         * databases here are small (~64 KB) so this is cheap enough. */
        extern void sceKernelSync(void);
        sceKernelSync();
    }
    return rc;
}

static int ps4_trace_xtruncate(sqlite3_file* f, sqlite3_int64 size)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xTruncate(p->child, size);
    ps4_trace_flog(p, "xtruncate", rc, size, 0);
    if (rc == SQLITE_OK)
        ps4_size_update(p->name, size);
    return rc;
}

static int ps4_trace_xsync(sqlite3_file* f, int flags)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xSync(p->child, flags);
    ps4_trace_flog(p, "xsync-child", rc, flags, 0);
    /* PS4: fsync() returns 0 but files can still be truncated after a
     * crash (dirty page cache lost).  The all-buffer sync kicks the
     * kernel's own flush as a belt-and-braces measure. */
    extern void sceKernelSync(void);
    sceKernelSync();
    ps4_trace_flog(p, "xsync", rc, flags, 0);
    return rc;
}

static int ps4_trace_xfilesize(sqlite3_file* f, sqlite3_int64* size)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xFileSize(p->child, size);
    const sqlite3_int64 tracked = ps4_size_lookup(p->name);
    if (tracked > *size) {
        *size = tracked;
        ps4_trace_flog(p, "xfilesize-tracked", rc, *size, 0);
    }
    else {
        ps4_trace_flog(p, "xfilesize", rc, *size, 0);
    }
    return SQLITE_OK;
}

static int ps4_trace_xlock(sqlite3_file* f, int level)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xLock(p->child, level);
    /* Log only failures: with the dotfile VFS a stale <db>.lock directory
     * makes every retry return SQLITE_BUSY (5), which is the splash hang. */
    if (rc != SQLITE_OK)
        ps4_trace_flog(p, "xlock-fail", rc, level, 0);
    return rc;
}

static int ps4_trace_xunlock(sqlite3_file* f, int level)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xUnlock(p->child, level);
    if (rc != SQLITE_OK)
        ps4_trace_flog(p, "xunlock-fail", rc, level, 0);
    return rc;
}

static int ps4_trace_xcheckreservedlock(sqlite3_file* f, int* res)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xCheckReservedLock(p->child, res);
    if (rc != SQLITE_OK)
        ps4_trace_flog(p, "xcheckreserved-fail", rc, *res, 0);
    return rc;
}

static int ps4_trace_xfilecontrol(sqlite3_file* f, int op, void* arg)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    return p->child->pMethods->xFileControl(p->child, op, arg);
}

static int ps4_trace_xsectorsize(sqlite3_file* f)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    return p->child->pMethods->xSectorSize(p->child);
}

static int ps4_trace_xdevicechar(sqlite3_file* f)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    return p->child->pMethods->xDeviceCharacteristics(p->child);
}

static int ps4_trace_xshmmap(sqlite3_file* f, int region, int sz, int w,
                             volatile void** pm)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    return p->child->pMethods->xShmMap(p->child, region, sz, w, pm);
}

static int ps4_trace_xshmlock(sqlite3_file* f, int off, int n, int flags)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    return p->child->pMethods->xShmLock(p->child, off, n, flags);
}

static void ps4_trace_xshmbarrier(sqlite3_file* f)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    p->child->pMethods->xShmBarrier(p->child);
}

static int ps4_trace_xshmunmap(sqlite3_file* f, int region)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    return p->child->pMethods->xShmUnmap(p->child, region);
}

static int ps4_trace_xfetch(sqlite3_file* f, sqlite3_int64 ofst, int amt, void** pm)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xFetch(p->child, ofst, amt, pm);
    ps4_trace_flog(p, "xfetch", rc, ofst, amt);
    return rc;
}

static int ps4_trace_xunfetch(sqlite3_file* f, sqlite3_int64 ofst, void* page)
{
    ps4_trace_file* p = (ps4_trace_file*)f;
    int rc = p->child->pMethods->xUnfetch(p->child, ofst, page);
    ps4_trace_flog(p, "xunfetch", rc, ofst, 0);
    return rc;
}

static int ps4_trace_open(sqlite3_vfs* v, const char* zName, sqlite3_file* f,
                          int flags, int* zRes)
{
    ps4_trace_vfs* self = (ps4_trace_vfs*)v;
    sqlite3_file* childf = (sqlite3_file*)((char*)f + sizeof(ps4_trace_file));
    int rc = self->child->xOpen(self->child, zName, childf, flags, zRes);
    ps4_trace_log("open", rc, zName);
    if (rc == SQLITE_OK && childf->pMethods) {
        ps4_trace_file* p = (ps4_trace_file*)f;
        p->child = childf;
        p->serial = ++g_ps4_trace_serial;
        snprintf(p->name, sizeof(p->name), "%s", zName ? zName : "");
        ps4_size_init_from_path(zName);
        f->pMethods = &g_ps4_trace_io;
    }
    return rc;
}

static int ps4_trace_delete(sqlite3_vfs* v, const char* zName, int sync)
{
    ps4_trace_vfs* self = (ps4_trace_vfs*)v;
    int rc = self->child->xDelete(self->child, zName, sync);
    ps4_trace_log("delete", rc, zName);
    if (rc == SQLITE_OK && zName)
        ps4_size_clear(zName);
    return rc;
}

static int ps4_trace_access(sqlite3_vfs* v, const char* zName, int flags, int* res)
{
    ps4_trace_vfs* self = (ps4_trace_vfs*)v;
    int rc = self->child->xAccess(self->child, zName, flags, res);
    ps4_trace_log("access", rc, zName);
    return rc;
}

static int ps4_trace_fullpath(sqlite3_vfs* v, const char* zName, int nOut, char* zOut)
{
    ps4_trace_vfs* self = (ps4_trace_vfs*)v;
    int rc = self->child->xFullPathname(self->child, zName, nOut, zOut);
    ps4_trace_log("fullpath", rc, zName);
    return rc;
}

static ps4_trace_vfs g_ps4_trace_vfs;

void ps4_register_trace_vfs(void)
{
    if (g_ps4_trace_vfs.child)
        return;
    sqlite3_vfs* child = sqlite3_vfs_find("unix-dotfile");
    if (!child)
        child = sqlite3_vfs_find("unix");
    if (!child)
        return;
    memset(&g_ps4_trace_vfs, 0, sizeof(g_ps4_trace_vfs));
    g_ps4_trace_vfs.child = child;
    g_ps4_trace_vfs.base = *child;
    g_ps4_trace_vfs.base.zName = "ps4-trace";
    g_ps4_trace_vfs.base.pAppData = &g_ps4_trace_vfs;
    g_ps4_trace_vfs.base.szOsFile = child->szOsFile + (int)sizeof(ps4_trace_file);
    g_ps4_trace_vfs.base.xOpen = ps4_trace_open;
    g_ps4_trace_vfs.base.xDelete = ps4_trace_delete;
    g_ps4_trace_vfs.base.xAccess = ps4_trace_access;
    g_ps4_trace_vfs.base.xFullPathname = ps4_trace_fullpath;
    sqlite3_vfs_register(&g_ps4_trace_vfs.base, 0);
}

#define _BSD_SOURCE
#define _XOPEN_SOURCE 700
#include <sys/stat.h>

void ps4_lstat_probe(const char* path)
{
    struct stat st;
    memset(&st, 0, sizeof(st));
    errno = 0;
    int rc = lstat(path, &st);
    ps4_trace_log("lstat", rc, path);
}
EOF
  clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -nostdinc \
    -isysroot "$PS4SDK" -isystem "$PS4SDK/include" \
    -isystem "$PS4SDK/include/c++/v1" -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -I"$work/sqlite-autoconf-${SQLITE3_VERSION}" \
    -c "$work/ps4_trace_vfs.c" -o "$work/ps4_trace_vfs.o" \
    || die "sqlite3 trace vfs compile failed"
  clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -nostdinc \
    -isysroot "$PS4SDK" -isystem "$PS4SDK/include" \
    -isystem "$PS4SDK/include/c++/v1" -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -c "$work/ps4_syscalls.c" -o "$work/ps4_syscalls.o" \
    || die "sqlite3 syscall wrapper compile failed"
  clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -nostdinc \
    -isysroot "$PS4SDK" -isystem "$PS4SDK/include" \
    -isystem "$PS4SDK/include/c++/v1" -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_MAX_MMAP_SIZE=0 \
    -DSQLITE_HOMEGROWN_RECURSIVE_MUTEX=1 -Dfchmod=ps4_fchmod \
    -c "$work/sqlite-autoconf-${SQLITE3_VERSION}/sqlite3.c" \
    -o "$work/sqlite3.o" || die "sqlite3 compile failed"
  ar rcs "$DEPS_PS4/lib/libsqlite3.a" "$work/sqlite3.o" "$work/ps4_syscalls.o" "$work/ps4_trace_vfs.o"
  install -d "$DEPS_PS4/include"
  install -m644 "$work/sqlite-autoconf-${SQLITE3_VERSION}/sqlite3.h" \
    "$work/sqlite-autoconf-${SQLITE3_VERSION}/sqlite3ext.h" "$DEPS_PS4/include/"
  cat >"$DEPS_PS4/lib/pkgconfig/sqlite3.pc" <<EOF
prefix=$DEPS_PS4
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQLite database engine
Version: $SQLITE3_VERSION
Libs: -L\${libdir} -lsqlite3
Cflags: -I\${includedir}
EOF
else
  ok "sqlite3 present ($DEPS_PS4/lib/libsqlite3.a)"
fi

# --------------------------------------------------------------------------
# 8. fribidi, harfbuzz and libass for the PS4 target
# --------------------------------------------------------------------------
# libass needs the other two, so the order matters.
meson_cross_file() {
  # The SDK headers are not laid out as a sysroot, so the include paths and
  # the link script are passed as plain compiler and linker arguments.
  local clang_resource
  clang_resource="$(clang++ -print-resource-dir)"
  cat >"$1" <<EOF
[binaries]
c = '/usr/bin/clang'
cpp = '/usr/bin/clang++'
ar = '/usr/bin/ar'
strip = '/usr/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'

[built-in options]
c_args = ['--target=x86_64-pc-freebsd12-elf', '-fPIC', '-funwind-tables', '-nostdinc', '-isysroot', '$PS4SDK', '-isystem', '$PS4SDK/include', '-isystem', '$PS4SDK/include/c++/v1', '-isystem', '$STAGE_SDK', '-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200809L']
cpp_args = ['--target=x86_64-pc-freebsd12-elf', '-fPIC', '-funwind-tables', '-nostdinc', '-isysroot', '$PS4SDK', '-isystem', '$PS4SDK/include', '-isystem', '$PS4SDK/include/c++/v1', '-isystem', '$STAGE_SDK', '-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200809L']
c_link_args = ['-nostdlib', '-fuse-ld=lld', '-pie', '-Wl,-m,elf_x86_64', '-Wl,--script,$PS4SDK/link.x', '-Wl,--eh-frame-hdr', '-L$PS4SDK/lib', '-lc', '-lkernel', '-lc++', '$PS4SDK/lib/crt1.o']
cpp_link_args = ['-nostdlib', '-fuse-ld=lld', '-pie', '-Wl,-m,elf_x86_64', '-Wl,--script,$PS4SDK/link.x', '-Wl,--eh-frame-hdr', '-L$PS4SDK/lib', '-lc', '-lkernel', '-lc++', '$PS4SDK/lib/crt1.o']

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF
}

build_meson_dep() {
  local name="$1" version="$2" archive="$3"
  shift 3
  local work="/tmp/${name}-ps4-build"
  log "building $name $version for PS4 -> $DEPS_PS4"
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/${name}.tar.xz" \
    "https://mirrors.kodi.tv/build-deps/sources/${archive}" \
    || die "cannot download $name"
  tar xf "$work/${name}.tar.xz" -C "$work"
  meson_cross_file "$work/cross.txt"
  PKG_CONFIG_PATH="$DEPS_PS4/lib/pkgconfig" \
  PKG_CONFIG_LIBDIR="$DEPS_PS4/lib/pkgconfig" \
  meson setup "$work/build" "$work/${name}-${version}" \
    --cross-file "$work/cross.txt" \
    --prefix "$DEPS_PS4" --libdir lib --buildtype release \
    --default-library static "$@" >"$work/$name.log" 2>&1 \
    || { tail -40 "$work/$name.log"; die "$name configure failed"; }
  meson compile -C "$work/build" -j "$(nproc)" >>"$work/$name.log" 2>&1 \
    || { tail -40 "$work/$name.log"; die "$name build failed"; }
  meson install -C "$work/build" >>"$work/$name.log" 2>&1 \
    || { tail -40 "$work/$name.log"; die "$name install failed"; }
  # Meson installs the pkg-config files next to the libraries, keep them all
  # in one place.
  if [[ -d "$DEPS_PS4/libdata/pkgconfig" ]]; then
    cp -n "$DEPS_PS4/libdata/pkgconfig"/*.pc "$DEPS_PS4/lib/pkgconfig/" 2>/dev/null || true
  fi
}

if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libfribidi.a" ]]; then
  build_meson_dep fribidi "$FRIBIDI_VERSION" "fribidi-${FRIBIDI_VERSION}.tar.xz" \
    -Ddocs=false -Dtests=false
else
  ok "fribidi present ($DEPS_PS4/lib/libfribidi.a)"
fi

if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libharfbuzz.a" ]]; then
  build_meson_dep harfbuzz "$HARFBUZZ_VERSION" "harfbuzz-${HARFBUZZ_VERSION}.tar.xz" \
    -Dtests=disabled -Ddocs=disabled -Dbenchmark=disabled \
    -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled
else
  ok "harfbuzz present ($DEPS_PS4/lib/libharfbuzz.a)"
fi

# Meson installs the pkg-config files next to the libraries, keep them all
# in one place.
if [[ -d "$DEPS_PS4/libdata/pkgconfig" ]]; then
  cp -n "$DEPS_PS4/libdata/pkgconfig"/*.pc "$DEPS_PS4/lib/pkgconfig/" 2>/dev/null || true
fi

# The Kodi sources include the harfbuzz headers as <harfbuzz/hb-ft.h>, so
# the parent of the header directory has to be on the include path too.
if [[ -f "$DEPS_PS4/lib/cmake/harfbuzz/harfbuzz-config.cmake" ]]; then
  sed -i 's|INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include/harfbuzz"|INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include;${PACKAGE_PREFIX_DIR}/include/harfbuzz"|g' \
    "$DEPS_PS4/lib/cmake/harfbuzz/harfbuzz-config.cmake"
fi

if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libass.a" ]]; then
  build_meson_dep libass "$LIBASS_VERSION" "libass-${LIBASS_VERSION}.tar.xz" \
    -Dtest=disabled -Dcompare=disabled -Dprofile=disabled -Dfuzz=disabled \
    -Dcheckasm=disabled -Dfontconfig=disabled -Drequire-system-font-provider=false
else
  ok "libass present ($DEPS_PS4/lib/libass.a)"
fi

# --------------------------------------------------------------------------
# 9. OpenSSL for the PS4 target
# --------------------------------------------------------------------------
# Kodi uses the EVP interface for its digests, so the target needs its own
# OpenSSL.  It is built with the generic BSD target, the PS4 libc is
# FreeBSD based.
if [[ $FORCE -eq 1 || ! -f "$DEPS_PS4/lib/libcrypto.a" ]]; then
  log "building OpenSSL $OPENSSL_VERSION for PS4 -> $DEPS_PS4"
  work=/tmp/openssl-ps4-build
  rm -rf "$work"
  mkdir -p "$work"
  curl -L --fail -o "$work/openssl.tar.gz" \
    "https://mirrors.kodi.tv/build-deps/sources/openssl-${OPENSSL_VERSION}.tar.gz" \
    || die "cannot download OpenSSL"
  tar xf "$work/openssl.tar.gz" -C "$work"
  pushd "$work/openssl-${OPENSSL_VERSION}" >/dev/null
  clang_resource="$(clang++ -print-resource-dir)"
  # The PS4 SDK only has the XSI strerror_r, so _GNU_SOURCE must not be
  # defined or OpenSSL picks the wrong variant of it.  _BSD_SOURCE gives
  # access to the BSD extensions (like _setjmp) that it needs.
  CC=clang \
  CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -nostdinc -isysroot $PS4SDK -isystem $PS4SDK/include -isystem $PS4SDK/include/c++/v1 -isystem $STAGE_SDK -isystem $clang_resource/include -D_BSD_SOURCE -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L" \
  LDFLAGS="-nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 -Wl,--script,$PS4SDK/link.x -Wl,--eh-frame-hdr -L$PS4SDK/lib -lc -lkernel -lc++" \
  perl ./Configure BSD-x86_64 --prefix="$DEPS_PS4" --openssldir="$DEPS_PS4/ssl" \
    no-shared no-tests no-legacy no-dso no-engine no-async \
    >"$work/openssl.log" 2>&1 || { tail -30 "$work/openssl.log"; die "OpenSSL configure failed"; }
  make -j"$(nproc)" build_libs >>"$work/openssl.log" 2>&1 \
    || { tail -40 "$work/openssl.log"; die "OpenSSL build failed"; }
  # Only the libraries are needed, the command line tools do not build on the
  # PS4 (no makecontext, no resolver).
  make install_dev >>"$work/openssl.log" 2>&1 \
    || { tail -30 "$work/openssl.log"; die "OpenSSL install failed"; }
  popd >/dev/null
else
  ok "OpenSSL present ($DEPS_PS4/lib/libcrypto.a)"
fi

# --------------------------------------------------------------------------
# 10. NASM (required by the FFmpeg x86 asm)
# --------------------------------------------------------------------------
if [[ $FORCE -eq 1 || ! -x "$NASM_PREFIX/bin/nasm" ]]; then
  log "building NASM $NASM_VERSION -> $NASM_PREFIX"
  work=/tmp/nasm-build-ps4
  rm -rf "$work" "$NASM_PREFIX"
  mkdir -p "$work"
  curl -L --fail -o "$work/nasm.tar.xz" \
    "https://www.nasm.us/pub/nasm/releasebuilds/${NASM_VERSION}/nasm-${NASM_VERSION}.tar.xz" \
    || die "cannot download NASM"
  tar xf "$work/nasm.tar.xz" -C "$work"
  pushd "$work/nasm-${NASM_VERSION}" >/dev/null
  ./configure --prefix="$NASM_PREFIX" >"$work/nasm.log" 2>&1 || die "NASM configure failed"
  make -j"$(nproc)" >>"$work/nasm.log" 2>&1 || die "NASM build failed"
  make install >>"$work/nasm.log" 2>&1 || die "NASM install failed"
  popd >/dev/null
else
  ok "NASM present ($NASM_PREFIX/bin/nasm)"
fi

# --------------------------------------------------------------------------
# 5. flatc, the FlatBuffers schema compiler (host build tool)
# --------------------------------------------------------------------------
# The Kodi build runs flatc to generate headers from the .fbs schemas, so it
# has to run on the build machine.  Cross compiling it produces a PS4 binary
# that cannot be executed here.
if [[ $FORCE -eq 1 || ! -x "$HOST_TOOLS/bin/flatc" ]]; then
  log "building flatc $FLATBUFFERS_VERSION (host) -> $HOST_TOOLS/bin"
  work=/tmp/flatbuffers-host-build
  rm -rf "$work" "$HOST_TOOLS"
  mkdir -p "$work" "$HOST_TOOLS/bin"
  curl -L --fail -o "$work/flatbuffers.tar.gz" \
    "https://mirrors.kodi.tv/build-deps/sources/flatbuffers-${FLATBUFFERS_VERSION}.tar.gz" \
    || die "cannot download flatbuffers"
  tar xf "$work/flatbuffers.tar.gz" -C "$work"
  cmake -S "$work/flatbuffers-${FLATBUFFERS_VERSION}" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFLATBUFFERS_CODE_COVERAGE=OFF \
    -DFLATBUFFERS_BUILD_TESTS=OFF \
    -DFLATBUFFERS_INSTALL=OFF \
    -DFLATBUFFERS_BUILD_FLATLIB=OFF \
    -DFLATBUFFERS_BUILD_FLATC=ON \
    -DFLATBUFFERS_BUILD_FLATHASH=OFF \
    -DFLATBUFFERS_BUILD_GRPCTEST=OFF \
    -DFLATBUFFERS_BUILD_SHAREDLIB=OFF \
    >"$work/flatc.log" 2>&1 || { cat "$work/flatc.log"; die "flatc configure failed"; }
  cmake --build "$work/build" --target flatc -j"$(nproc)" >>"$work/flatc.log" 2>&1 \
    || { cat "$work/flatc.log"; die "flatc build failed"; }
  cp "$work/build/flatc" "$HOST_TOOLS/bin/flatc"
else
  ok "flatc present ($HOST_TOOLS/bin/flatc)"
fi

# --------------------------------------------------------------------------
# 6. JsonSchemaBuilder (host build tool)
# --------------------------------------------------------------------------
# Generates ServiceDescription.h from the JSON-RPC schema; it has to run on
# the build machine as well.
if [[ $FORCE -eq 1 || ! -x "$HOST_TOOLS/bin/JsonSchemaBuilder" ]]; then
  log "building JsonSchemaBuilder (host) -> $HOST_TOOLS/bin"
  work=/tmp/jsonschemabuilder-host-build
  rm -rf "$work"
  cmake -S "$REPO_ROOT/src/kodi/tools/depends/native/JsonSchemaBuilder/src" \
        -B "$work" -DCMAKE_BUILD_TYPE=Release >"$work.log" 2>&1 \
    || { cat "$work.log"; die "JsonSchemaBuilder configure failed"; }
  cmake --build "$work" -j"$(nproc)" >>"$work.log" 2>&1 \
    || { cat "$work.log"; die "JsonSchemaBuilder build failed"; }
  cp "$work/JsonSchemaBuilder" "$HOST_TOOLS/bin/JsonSchemaBuilder"
else
  ok "JsonSchemaBuilder present ($HOST_TOOLS/bin/JsonSchemaBuilder)"
fi

# --------------------------------------------------------------------------
# 5. SDK headers added by earlier sessions (idempotent)
# --------------------------------------------------------------------------
if [[ $FORCE -eq 1 || ! -f "$PS4SDK/include/sys/sysctl.h" ]] || \
   ! cmp -s "$REPO_ROOT/tools/ps4/include/sys/sysctl.h" "$PS4SDK/include/sys/sysctl.h"; then
  log "installing sys/sysctl.h stub -> $PS4SDK/include/sys/sysctl.h"
  mkdir -p "$PS4SDK/include/sys"
  cp "$REPO_ROOT/tools/ps4/include/sys/sysctl.h" "$PS4SDK/include/sys/sysctl.h"
else
  ok "sys/sysctl.h stub up to date"
fi

echo
ok "M1 build environment ready"
cat <<EOF

  TexturePacker : $TP_BUILD/TexturePacker
  TinyXML       : $DEPS_PS4
  NASM          : $NASM_PREFIX/bin/nasm
  flatc (host)  : $HOST_TOOLS/bin/flatc
  JsonSchemaBuilder: $HOST_TOOLS/bin/JsonSchemaBuilder
  PS4SDK        : $PS4SDK

Next:
  ./scripts/configure-kodi-ps4.sh
  ./scripts/build-kodi-ps4.sh
EOF
