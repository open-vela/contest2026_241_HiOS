/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Locale stubs for libstdc++ on NuttX.
 *
 * NuttX's libc does not implement full POSIX locale support, but libstdc++
 * (linked for C++ STL) references __locale_mb_cur_max, strcoll, strxfrm,
 * setlocale, and __xpg_strerror_r from its codecvt/collate/time/system_error
 * facets. These stubs provide minimal "C" locale behaviour so the linker is
 * satisfied and the facets return sane defaults.
 */

#include <stddef.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

/* libstdc++ internal: returns the max number of bytes in a multibyte character
 * for the current locale. The "C" locale is single-byte, so return 1. */
int __locale_mb_cur_max(void)
{
    return 1;
}

/* strcoll: compare two strings using the current locale's collation order.
 * For the "C" locale this is equivalent to strcmp. */
int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

/* strxfrm: transform src into dst so that strcmp on the result matches
 * strcoll on the original. For the "C" locale this is a plain copy. */
size_t strxfrm(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (dst != NULL && n > 0)
    {
        size_t copy = (len < n) ? len : n - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

/* setlocale: NuttX only supports the "C" locale. Return "C" for LC_ALL
 * queries, ignore set requests. */
char *setlocale(int category, const char *locale)
{
    (void)category;
    (void)locale;
    return "C";
}

/* __xpg_strerror_r: XPG version of strerror_r (returns int).
 * libstdc++ system_error calls this to render error_code messages.
 * NuttX's strerror is thread-safe enough for our use; wrap it. */
int __xpg_strerror_r(int errnum, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0)
        return EINVAL;

    const char *msg = strerror(errnum);
    if (msg == NULL)
    {
        msg = "Unknown error";
    }

    size_t len = strlen(msg);
    if (len >= buflen)
        len = buflen - 1;

    memcpy(buf, msg, len);
    buf[len] = '\0';
    return 0;
}
