#ifndef __NATIVE_DOS_FCNTL_H__
#define __NATIVE_DOS_FCNTL_H__

#define O_RDONLY 0x0000
#define O_BINARY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT  0x0100
#define O_TRUNC  0x0200

int open(const char *path, int flags, ...);

#endif /* __NATIVE_DOS_FCNTL_H__ */
