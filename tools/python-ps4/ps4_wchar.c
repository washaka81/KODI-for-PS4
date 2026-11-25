/*
 * Private 2-byte wchar_t helpers for the Orbis CPython build.
 *
 * Kodi's C++ is compiled with 4-byte wchar_t and scripts/patch-libc-wchar.sh
 * replaced the wide functions in libc.a with 4-byte versions.  CPython is
 * compiled with -fshort-wchar (2-byte wchar_t) to match the Orbis C ABI, so
 * its calls to wcslen/wmem* must not resolve to the patched 4-byte libc
 * versions.  build-python-ps4.sh renames those calls to the _Py_ps4_* names
 * below; they are unique and do not collide with Kodi's libc symbols.
 */
#undef wcslen
#undef wmemcpy
#undef wmemmove
#undef wmemset
#undef wmemcmp
#undef wmemchr

#include <stddef.h>
#include <wchar.h>

size_t _Py_ps4_wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

wchar_t *_Py_ps4_wmemcpy(wchar_t *dst, const wchar_t *src, size_t n) {
    wchar_t *out = dst;
    while (n--)
        *dst++ = *src++;
    return out;
}

wchar_t *_Py_ps4_wmemmove(wchar_t *dst, const wchar_t *src, size_t n) {
    if (dst < src) {
        for (size_t i = 0; i < n; i++)
            dst[i] = src[i];
    } else if (dst > src) {
        for (size_t i = n; i > 0; i--)
            dst[i - 1] = src[i - 1];
    }
    return dst;
}

wchar_t *_Py_ps4_wmemset(wchar_t *s, wchar_t c, size_t n) {
    wchar_t *out = s;
    while (n--)
        *s++ = c;
    return out;
}

int _Py_ps4_wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

wchar_t *_Py_ps4_wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c)
            return (wchar_t *)&s[i];
    }
    return NULL;
}
