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
typedef enum dos_malloc_policy
{
    DOS_MALLOC_POLICY_RETURN_NULL = 0,
    DOS_MALLOC_POLICY_EXIT,
    DOS_MALLOC_POLICY_MESSAGE_EXIT
} dos_malloc_policy_t;

void dos_malloc_set_policy(dos_malloc_policy_t policy);
dos_malloc_policy_t dos_malloc_get_policy(void);

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
size_t malloc_largest_block(void);
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
