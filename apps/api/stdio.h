#ifndef __NATIVE_DOS_STDIO_H__
#define __NATIVE_DOS_STDIO_H__

#include <stddef.h>
#include <stdarg.h>

typedef struct native_dos_FILE FILE;

extern FILE *stdout;

FILE *fopen(const char *filename, const char *mode);
int fprintf(FILE *stream, const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int sscanf(const char *buffer, const char *format, ...);
int fclose(FILE *stream);
int feof(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
void rewind(FILE *stream);
int remove(const char *filename);

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
void setbuf(FILE *stream, char *buffer);
int getchar(void);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int putchar(int c);
int puts(const char *s);
int vprintf(const char *format, va_list args);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int printf(const char *format, ...);
int sprintf(char *buffer, const char *format, ...);

#endif /* __NATIVE_DOS_STDIO_H__ */
