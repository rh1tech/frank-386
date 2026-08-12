#ifndef __NATIVE_DOS_IO_H__
#define __NATIVE_DOS_IO_H__

#include <stdint.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

int read(int handle, void *buffer, unsigned int count);
int close(int handle);
int32_t lseek(int handle, int32_t offset, int origin);
int32_t filelength(int handle);

#endif /* __NATIVE_DOS_IO_H__ */
