#ifndef __NATIVE_DOS_DOS_H__
#define __NATIVE_DOS_DOS_H__

#include <stdint.h>

union REGS
{
    struct
    {
        uint32_t eax;
        uint32_t ebx;
        uint32_t ecx;
        uint32_t edx;
        uint32_t esi;
        uint32_t edi;
        uint32_t cflag;
    } x;

    struct
    {
        uint16_t ax;
        uint16_t _ax_hi;
        uint16_t bx;
        uint16_t _bx_hi;
        uint16_t cx;
        uint16_t _cx_hi;
        uint16_t dx;
        uint16_t _dx_hi;
        uint16_t si;
        uint16_t _si_hi;
        uint16_t di;
        uint16_t _di_hi;
        uint16_t cflag;
        uint16_t _cflag_hi;
    } w;

    struct
    {
        uint8_t al, ah;
        uint16_t _ax_hi;
        uint8_t bl, bh;
        uint16_t _bx_hi;
        uint8_t cl, ch;
        uint16_t _cx_hi;
        uint8_t dl, dh;
        uint16_t _dx_hi;
    } h;
};

int int386(int intnum, const union REGS *inregs, union REGS *outregs);

struct SREGS
{
    uint16_t es;
    uint16_t cs;
    uint16_t ss;
    uint16_t ds;
    uint16_t fs;
    uint16_t gs;
};

int int386x(int intnum, const union REGS *inregs, union REGS *outregs,
            struct SREGS *segregs);
void segread(struct SREGS *segregs);
void _disable(void);
void _enable(void);

/* Open Watcom-compatible DOS directory search interface. */
struct find_t
{
    unsigned attrib;
    char name[260];
};

int _dos_findfirst(const char *pattern, unsigned attrib, struct find_t *info);
int _dos_findnext(struct find_t *info);


#endif /* __NATIVE_DOS_DOS_H__ */
