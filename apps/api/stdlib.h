#ifndef __NATIVE_DOS_STDLIB_H__
#define __NATIVE_DOS_STDLIB_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal native-DOS stdlib compatibility layer.
 * Add declarations/implementations here only when the native runtime
 * actually provides them. Do not expose the toolchain libc by accident.
 */
/* Native implementations are provided by the FDOS runtime layer. */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int status) __attribute__((noreturn));

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

static inline int abs(int value)
{
    return value < 0 ? -value : value;
}

int atoi(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_STDLIB_H__ */
