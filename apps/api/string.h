#ifndef __NATIVE_DOS_STRING_H__
#define __NATIVE_DOS_STRING_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy(char *dst, const char *src, size_t n);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
int strcmp(const char *a, const char *b);
int strcmpi(const char *a, const char *b);
void strupr(char *s);
void *memchr(const void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strncasecmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_STRING_H__ */
