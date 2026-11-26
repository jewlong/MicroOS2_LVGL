/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal freestanding compiler ABI support. No libc, syscalls or semihosting. */
#include <stddef.h>
#include <stdint.h>
#include "bsp.h"
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
_Noreturn void abort(void);
void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n-- != 0u) { *d++ = *s++; }
    return dst;
}
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n-- != 0u) { *d++ = (unsigned char)c; }
    return dst;
}
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d <= (uintptr_t)s) {
        while (n-- != 0u) { *d++ = *s++; }
    } else {
        d += n; s += n;
        while (n-- != 0u) { *--d = *--s; }
    }
    return dst;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n-- != 0u) {
        if (*x != *y) { return (int)*x - (int)*y; }
        ++x; ++y;
    }
    return 0;
}
_Noreturn void abort(void) { bsp_panic("compiler runtime abort"); }
