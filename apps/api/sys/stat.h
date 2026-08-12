#ifndef __NATIVE_DOS_SYS_STAT_H__
#define __NATIVE_DOS_SYS_STAT_H__

#include <stdint.h>

struct stat
{
    int32_t st_size;
};

int fstat(int handle, struct stat *info);

#endif /* __NATIVE_DOS_SYS_STAT_H__ */
