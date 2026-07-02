#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"

static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 2FH - ERROR: no handler defined ");
while(1); // remove it
    return true;
}

int UMB_get_largest(dos_far_ptr driverAddress, UCOUNT *seg, UCOUNT *size)
{
    CPU_DX = 0xffff;
    CPU_AX = 0x1000;

    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));

    if (CPU_BL != 0xb0)
        return 0;

    if (CPU_DX == 0)
        return 0;

    CPU_AX = 0x1000;
    /* DX оставляем равным largest size */
    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));

    if (CPU_AX != 1)
        return 0;

    *seg = CPU_BX;
    *size = CPU_DX;

    return CPU_AX;
}

bool fdos_2fh(CPU* cpu) {
    if (CPU_AX == 0x4300)
        CPU_AL = 0x80;
    else 
    if (CPU_AX == 0x4310) {
        /// TODO: handler
        SET_ES ( 0xF000 );
        CPU_DX = 0xFEFF ;
    }
    else 
        no_handler(cpu);
    return true;
}
