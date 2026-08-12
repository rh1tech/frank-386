#ifndef __NATIVE_DOS_STDIO_H__
#define __NATIVE_DOS_STDIO_H__

typedef struct native_dos_FILE FILE;

FILE *fopen(const char *filename, const char *mode);
int fprintf(FILE *stream, const char *format, ...);
int fclose(FILE *stream);
int printf(const char *format, ...);
int sprintf(char *buffer, const char *format, ...);

#endif /* __NATIVE_DOS_STDIO_H__ */
