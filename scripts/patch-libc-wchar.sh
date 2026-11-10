#!/usr/bin/env bash
# Patch the OpenOrbis libc wide-character functions so they match the C++
# 4-byte wchar_t ABI used by Kodi.
#
# The prebuilt libc.a is compiled as C, where this target defines wchar_t as
# unsigned short (2 bytes).  C++ defines wchar_t as 4 bytes.  The result is
# that wmemcpy/wmemmove/wmemset/wcslen/wmemcmp/wmemchr copy only the low 16
# bits of each wchar_t, corrupting std::wstring data (GUI labels, keyboard
# text, etc.).  These replacements are compiled as C++ and use 4-byte
# elements.  The original libc.a is backed up next to it.
set -euo pipefail

TOOLCHAIN="${OO_PS4_TOOLCHAIN:-/opt/ps4/sdk}"
LIBC="$TOOLCHAIN/lib/libc.a"
CLANGXX="${CLANGXX:-/usr/bin/clang++}"

if [[ ! -f "$LIBC" ]]; then
  echo "libc.a not found at $LIBC" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/wmemcpy.cpp" <<'EOF'
#include <stddef.h>
#include <string.h>
#include <wchar.h>
extern "C" wchar_t *wmemcpy(wchar_t *__restrict s1, const wchar_t *__restrict s2, size_t n) {
    return (wchar_t *)memcpy(s1, s2, n * sizeof(wchar_t));
}
EOF

cat > "$TMP/wmemmove.cpp" <<'EOF'
#include <stddef.h>
#include <string.h>
#include <wchar.h>
extern "C" wchar_t *wmemmove(wchar_t *s1, const wchar_t *s2, size_t n) {
    return (wchar_t *)memmove(s1, s2, n * sizeof(wchar_t));
}
EOF

cat > "$TMP/wmemset.cpp" <<'EOF'
#include <stddef.h>
#include <wchar.h>
extern "C" wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) s[i] = c;
    return s;
}
EOF

cat > "$TMP/wcslen.cpp" <<'EOF'
#include <stddef.h>
#include <wchar.h>
extern "C" size_t wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
EOF

cat > "$TMP/wmemcmp.cpp" <<'EOF'
#include <stddef.h>
#include <wchar.h>
extern "C" int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return s1[i] < s2[i] ? -1 : 1;
    }
    return 0;
}
EOF

cat > "$TMP/wmemchr.cpp" <<'EOF'
#include <stddef.h>
#include <wchar.h>
extern "C" wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == c) return (wchar_t *)&s[i];
    return 0;
}
EOF

for f in wmemcpy wmemmove wmemset wcslen wmemcmp wmemchr; do
  "$CLANGXX" --target=x86_64-pc-freebsd12-elf -nostdinc \
    -isysroot "$TOOLCHAIN" \
    -isystem "$TOOLCHAIN/include" \
    -O2 -fno-builtin -c "$TMP/$f.cpp" -o "$TMP/$f.lo"
done

if [[ ! -f "$LIBC.bak-wchar" ]]; then
  cp -a "$LIBC" "$LIBC.bak-wchar"
  echo "backup: $LIBC.bak-wchar"
fi

cd "$TMP"
ar r "$LIBC" wmemcpy.lo wmemmove.lo wmemset.lo wcslen.lo wmemcmp.lo wmemchr.lo
ranlib "$LIBC" 2>/dev/null || true

echo "patched $LIBC:"
ar p "$LIBC" wmemcpy.lo > "$TMP/verify.lo"
if objdump -d "$TMP/verify.lo" | grep -q 'shl.*\$0x2'; then
  echo "  wmemcpy: 4-byte wchar_t OK"
else
  echo "  wmemcpy: WARNING, expected 4-byte scaling" >&2
fi
ar p "$LIBC" wcslen.lo > "$TMP/verify.lo"
if objdump -d "$TMP/verify.lo" | grep -q 'cmpl.*0x4'; then
  echo "  wcslen: 4-byte wchar_t OK"
else
  echo "  wcslen: WARNING, expected 4-byte scaling" >&2
fi
