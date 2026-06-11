#include "i286.h"
#include "bios.h"

/*
BIOS - GET EQUIPMENT LIST
Return:
(E)AX = BIOS equipment list word (see #00226,#03215 at INT 4B"Tandy")

Note: Since older BIOSes do not know of the existence of EAX, the high word of EAX should be cleared before this call if any of the high bits will be tested

See Also: INT 4B"Tandy 2000" - MEM 0040h:0010h


Bitfields for BIOS equipment list:

Bit(s)  Description     (Table 00226)
0      floppy disk(s) installed (number specified by bits 7-6)
1      80x87 coprocessor installed
3-2    number of 16K banks of RAM on motherboard (PC only).
Number of 64K banks of RAM on motherboard (XT only)
2      pointing device installed (PS)
3      unused (PS)
5-4    initial video mode.
00 EGA, VGA, or PGA.
01 40x25 color.
10 80x25 color.
11 80x25 monochrome
7-6    number of floppies installed less 1 (if bit 0 set)
8      DMA support installed (PCjr, Tandy 1400LT).
DMA support *not* installed (Tandy 1000's)
11-9   number of serial ports installed
12     game port installed
13     serial printer attached (PCjr).
Internal modem installed (PC/Convertible)
15-14  number of parallel ports installed
---Compaq, Dell, and many other 386/486 machines--
23     page tables set so that Weitek coprocessor addressable in real mode
24     Weitek math coprocessor present
---Compaq Systempro---
25     internal DMA parallel port available
26     IRQ for internal DMA parallel port (if bit 25 set).
0 = IRQ5.
1 = IRQ7
28-27  parallel port DMA channel.
00 DMA channel 0.
01 DMA channel 0 ???.
10 reserved.
11 DMA channel 3

Notes: Some implementations of Remote (Initial) Program Loader (RPL/RIPL) don't set bit 0 to indicate a "virtual" floppy drive, although the RPL requires access to its memory image through a faked drive A:. This may have caused problems with releases of DOS 3.3x and earlier,

which assumed A:
And B: to be invalid drives then and would discard
any attempts to access these drives.  Implementations of RPL should
set bit 0 to indicate a "virtual" floppy..
The IBM PC DOS 3.3x-2000 IBMBIO.COM contains two occurences of code
sequences like:
INT 11h
JMP SHORT skip
DB 52h,50h,53h; "RPS"

skip:
OR AX,1
TEST AX,1
While at the first glance this seems to be a bug since it just
wastes memory and the condition is always true, this could well be
a signature for an applyable patch to stop it from forcing AX bit 0
to be always on. MS-DOS IO.SYS does not contain these signatures,
however.

See Also: INT 12"BIOS" - #03215 at INT 4B"Tandy 2000"
*/
bool bios_11h() {
    /*
     * INT 11h - BIOS GET EQUIPMENT LIST
     *
     * IBM PC/AT-compatible source is BIOS Data Area word 0040:0010,
     * physical address 0x410. This word is initialized during POST /
     * load_bios_and_reset().
     */
    CPU_AX = pload16(0x0410);
    return true;
}
