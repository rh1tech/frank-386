#ifndef __NATIVE_DOS_IO_H__
#define __NATIVE_DOS_IO_H__

#include <stdint.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#ifndef R_OK
#define R_OK 4
#endif

int read(int handle, void *buffer, unsigned int count);
int write(int handle, const void *buffer, unsigned int count);
void dos_lock_term(_Bool lock);
int dos_set_io_buffer_size(unsigned int size);
int close(int handle);
int access(const char *path, int mode);
int32_t lseek(int handle, int32_t offset, int origin);
int32_t filelength(int handle);

#endif /* __NATIVE_DOS_IO_H__ */
