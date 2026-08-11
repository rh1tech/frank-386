#ifndef __NATIVE_DOS_STRING_H__
#define __NATIVE_DOS_STRING_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static inline void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_STRING_H__ */
