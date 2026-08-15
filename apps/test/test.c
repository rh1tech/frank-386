#include "dos-api.h"
#include <stdio.h>

static int is_resident_arg(const char *arg)
{
    return arg != 0 &&
           arg[0] == '-' && arg[1] == 'r' && arg[2] == '\0';
}

static int stay_resident(void)
{
    PC *machine = get_PC();
    CPU *cpu = machine->cpu;
    uint16_t psp_seg;
    uint16_t end_seg;

    /* Standard DOS: AH=51h returns the current PSP in BX. */
    cpu->gprx[regax].r16 = 0x5100;
    bios_intcall(cpu, 0x21, "test.com get PSP");
    psp_seg = cpu->gprx[regbx].r16;

    /* PSP:0002 is the segment of the first paragraph beyond the process
       allocation.  The ELF loader places its two startup-only stacks at
       the very end of that allocation: 256 bytes of guest DOS stack and
       4096 bytes of native ARM stack.  Both sizes are paragraph-aligned,
       so subtracting 272 paragraphs leaves the exact paragraph-rounded
       boundary immediately before the stacks.

       AH=31h only updates MCB ownership/size; it does not erase the released
       bytes.  On this synchronous core0 path no other DOS allocation can
       occur before bios_intcall()/main unwind back to the kernel MSP. */
    end_seg = *(volatile uint16_t *)dos_guest_far_ptr(psp_seg, 2);
    if (end_seg <= psp_seg + 272u)
        return 2;

    /* Standard DOS TSR service.  Keep metadata, reachable ELF sections and
       argv, but discard both startup stacks from the resident MCB. */
    cpu->gprx[regax].r16 = 0x3100;
    cpu->gprx[regdx].r16 = (uint16_t)(end_seg - psp_seg - 272u);
    bios_intcall(cpu, 0x21, "test.com TSR");

    /* bios_intcall() must unwind its native bridge, so execution reaches
       here even though DOS has already requested termination.  task.c keeps
       the AH=31h status and skips _fini()/process release on this path. */
    return 0;
}

int main(int argc, char **argv)
{
    printf("test.com: main entered\r\n");

    if (argc == 2 && is_resident_arg(argv[1]))
        return stay_resident();
    return 0;
}
