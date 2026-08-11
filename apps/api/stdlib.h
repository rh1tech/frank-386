#ifndef __NATIVE_DOS_STDLIB_H__
#define __NATIVE_DOS_STDLIB_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal native-DOS stdlib compatibility layer.
 * Add declarations/implementations here only when the native runtime
 * actually provides them. Do not expose the toolchain libc by accident.
 */
static inline int abs(int value)
{
    return value < 0 ? -value : value;
}

static inline int atoi(const char *s)
{
    int sign = 1;
    int value = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        ++s;

    if (*s == '-' || *s == '+')
    {
        if (*s == '-')
            sign = -1;
        ++s;
    }

    while (*s >= '0' && *s <= '9')
    {
        value = value * 10 + (*s - '0');
        ++s;
    }

    return sign * value;
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_STDLIB_H__ */
