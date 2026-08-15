/*
 * elf2ez_host.h - host (Win32 / Linux console) compatibility layer for elf2ez.
 *
 * This header is compiled ONLY when the build defines ELF2EZ_HOST (see the
 * ELF2EZ_HOST CMake option). On the native RP2040 / DOS target it is never
 * included, so that build is completely unaffected.
 *
 * Design goals:
 *   - Use only the most portable, long-stable APIs: ISO C89/C99 <stdio.h>,
 *     <stdlib.h>, <string.h>. No POSIX-only, no Win32-only, no experimental
 *     interfaces. The exact same object code path works under GCC, Clang,
 *     MinGW and MSVC on both Windows and Linux (and other Unixes).
 *   - Reproduce the small slice of the native DOS API that elf2ez.c actually
 *     uses: the low-level fd file calls (open/read/write/close/lseek/access),
 *     the O_* / SEEK_SET constants.
 *
 * The DOS "work block" allocator (alloc_largest_block/free_dos_block) is NOT
 * provided here; elf2ez.c supplies a malloc/free based version under the same
 * ELF2EZ_HOST guard.
 */

#ifndef ELF2EZ_HOST_H
#define ELF2EZ_HOST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * Pull the pure on-disk / ABI data types straight from the project's own
 * headers so this shim never has to duplicate (and drift from) them. These
 * headers only need <stdint.h> and contain no emulator/runtime code:
 *   - ez.h (included by elf2ez.c itself) defines the EZ file structures and
 *     native_ez_process_requirements.
 */

/* open() flag bits - values mirror api/fcntl.h so the source is unchanged. */
#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif
#ifndef O_BINARY
#define O_BINARY 0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_CREAT
#define O_CREAT  0x0100
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif

/* SEEK_SET is provided by <stdio.h>; keep a fallback just in case. */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif

/*
 * Minimal fd table backing the fd-style calls with ISO C FILE * streams.
 * elf2ez opens at most two files at a time, so a tiny fixed table is ample.
 * Handles start at 3 so they never look like the standard streams 0/1/2.
 */
#ifndef ELF2EZ_HOST_MAX_FILES
#define ELF2EZ_HOST_MAX_FILES 16
#endif
#define ELF2EZ_HOST_FD_BASE 3

static FILE *elf2ez_host_files[ELF2EZ_HOST_MAX_FILES];

static FILE *elf2ez_host_fp(int fd)
{
    int i = fd - ELF2EZ_HOST_FD_BASE;
    if (i < 0 || i >= ELF2EZ_HOST_MAX_FILES)
        return NULL;
    return elf2ez_host_files[i];
}

static int elf2ez_host_open(const char *path, int flags, ...)
{
    int writing = (flags & (O_WRONLY | O_CREAT | O_TRUNC)) != 0;
    const char *mode = writing ? "wb" : "rb";
    int i;

    for (i = 0; i < ELF2EZ_HOST_MAX_FILES; ++i) {
        if (elf2ez_host_files[i] == NULL) {
            FILE *f = fopen(path, mode);
            if (f == NULL)
                return -1;
            elf2ez_host_files[i] = f;
            return i + ELF2EZ_HOST_FD_BASE;
        }
    }
    return -1; /* table full */
}

static int elf2ez_host_read(int fd, void *buf, unsigned int count)
{
    FILE *f = elf2ez_host_fp(fd);
    size_t got;

    if (f == NULL)
        return -1;
    got = fread(buf, 1, count, f);
    /*
     * A short read (including 0) ends the caller's read loop: read_exact_at
     * treats any result <= requested as failure, so EOF and a genuine read
     * error need not be told apart here. Avoiding ferror()/feof() also keeps
     * this free of any <stdio.h> declaration quirks on some MinGW setups.
     */
    return (int)got;
}

static int elf2ez_host_write(int fd, const void *buf, unsigned int count)
{
    FILE *f = elf2ez_host_fp(fd);
    size_t put;

    if (f == NULL)
        return -1;
    put = fwrite(buf, 1, count, f);
    if (put != (size_t)count)
        return -1;
    return (int)put;
}

static int32_t elf2ez_host_lseek(int fd, int32_t offset, int whence)
{
    FILE *f = elf2ez_host_fp(fd);
    long pos;

    if (f == NULL)
        return -1;
    if (fseek(f, (long)offset, whence) != 0)
        return -1;
    pos = ftell(f);
    return (int32_t)pos;
}

static int elf2ez_host_close(int fd)
{
    FILE *f = elf2ez_host_fp(fd);
    int i = fd - ELF2EZ_HOST_FD_BASE;

    if (f == NULL)
        return -1;
    elf2ez_host_files[i] = NULL;
    return fclose(f) == 0 ? 0 : -1;
}

/* access(path, 0) is only ever used to test for existence. */
static int elf2ez_host_access(const char *path, int mode)
{
    FILE *f;

    (void)mode;
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    fclose(f);
    return 0;
}

/* Route the source's bare fd calls to the host implementations. */
#define open   elf2ez_host_open
#define read   elf2ez_host_read
#define write  elf2ez_host_write
#define lseek  elf2ez_host_lseek
#define close  elf2ez_host_close
#define access elf2ez_host_access

#endif /* ELF2EZ_HOST_H */
