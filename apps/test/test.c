#include "dos-api.h"
#include "dos.h"
#include "io.h"
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

    /* PSP:0002 is the segment of the first paragraph beyond the main
       process allocation.  In the current native runtime neither startup
       stack lives in this MCB: the guest DOS stack is a separate DOS block
       and the native ARM stack comes from the PSRAM stack arena.

       Therefore the whole current process MCB is the resident image.  After
       AH=31h unwinds, task.c releases both startup-only stacks separately. */
    end_seg = *(volatile uint16_t *)dos_guest_far_ptr(psp_seg, 2);
    if (end_seg <= psp_seg)
        return 2;

    printf("test.exe: exit, but TSR\r\n");

    /*
     * A DOS EXEC child owns a separate environment MCB referenced by PSP:2Ch.
     * AH=31h deliberately keeps all child-owned blocks, so a TSR which does
     * not need its environment must free that block explicitly before
     * becoming resident.
     */
    {
        volatile uint16_t *ps_environ =
            (volatile uint16_t *)dos_guest_far_ptr(psp_seg, 0x2cu);
        uint16_t env_seg = *ps_environ;

        if (env_seg != 0) {
            union REGS regs = {0};
            struct SREGS sregs;

            segread(&sregs);
            sregs.es = env_seg;
            regs.h.ah = 0x49;
            int386x(0x21, &regs, &regs, &sregs);
            if (regs.x.cflag)
                return 3;

            *ps_environ = 0;
        }
    }

    {
        uint16_t main_mcb = (uint16_t)(psp_seg - 1u);
        uint16_t stack_seg = cpu->ext_accessors->get_seg16(cpu, 2);
        uint16_t stack_mcb = (uint16_t)(stack_seg - 1u);
        uint16_t main_paras =
            *(volatile uint16_t *)dos_guest_far_ptr(main_mcb, 3u);
        uint16_t stack_paras =
            *(volatile uint16_t *)dos_guest_far_ptr(stack_mcb, 3u);

        printf("test.exe: MCB main=%04x size=%u (%lu bytes), "
               "DOS-stack=%04x size=%u (%lu bytes)\n",
               main_mcb, main_paras, (unsigned long)main_paras * 16ul,
               stack_mcb, stack_paras, (unsigned long)stack_paras * 16ul);
    }

    /* Standard DOS TSR service.  Keep the complete main process MCB.
       Startup-only DOS/ARM stacks are outside this block and are released
       by the native EXEC teardown after the TSR request unwinds. */
    cpu->gprx[regax].r16 = 0x3100;
    cpu->gprx[regdx].r16 = (uint16_t)(end_seg - psp_seg);
    bios_intcall(cpu, 0x21, "test.exe TSR");

    /* bios_intcall() must unwind its native bridge, so execution reaches
       here even though DOS has already requested termination.  task.c keeps
       the AH=31h status and skips _fini()/process release on this path. */
    return 0;
}

int main(int argc, char **argv)
{
    dos_set_io_buffer_size(128);
    printf("test.exe: main entered\r\n");

    if (argc == 2 && is_resident_arg(argv[1]))
        return stay_resident();
    return 0;
}
