#include "dos-api.h"
#include "dos.h"
#include "io.h"
#include "dos_mem.h"
#include "dos_phys.h"
#include "conio.h"
#include <stdio.h>
#include <string.h>

static int is_resident_arg(const char *arg)
{
    return arg != 0 &&
           arg[0] == '-' && arg[1] == 'r' && arg[2] == '\0';
}

static int is_vbe_arg(const char *arg)
{
    return arg != 0 &&
           arg[0] == '-' && arg[1] == 'v' && arg[2] == 'b' &&
           arg[3] == 'e' && arg[4] == '\0';
}

static int vbe_int10(union REGS *regs, struct SREGS *sregs)
{
    int386x(0x10, regs, regs, sregs);
    return regs->w.ax == 0x004f;
}

static int test_vbe(void)
{
    union REGS regs = {0};
    struct SREGS sregs;
    uint8_t *info;
    uint16_t info_seg;
    uint32_t pos;
    int rc = 0;

    info = (uint8_t *)dos_alloc_low(256);
    if (info == 0) {
        printf("test.exe: VBE: unable to allocate info buffer\r\n");
        return 2;
    }
    info_seg = dos_ptr_segment(info);
    if (info_seg == 0) {
        dos_free_low(info);
        return 2;
    }

    segread(&sregs);
    sregs.es = info_seg;

    memset(info, 0, 256);
    regs.w.ax = 0x4f00;
    regs.w.di = 0;
    if (!vbe_int10(&regs, &sregs) ||
        info[0] != 'V' || info[1] != 'E' ||
        info[2] != 'S' || info[3] != 'A') {
        printf("test.exe: VBE 4F00 failed, AX=%04x\r\n", regs.w.ax);
        rc = 3;
        goto out;
    }
    printf("test.exe: VBE %u.%u detected\r\n", info[5], info[4]);

    memset(info, 0, 256);
    regs.w.ax = 0x4f01;
    regs.w.cx = 0x0100;
    regs.w.di = 0;
    if (!vbe_int10(&regs, &sregs)) {
        printf("test.exe: VBE 4F01 mode 100h failed, AX=%04x\r\n", regs.w.ax);
        rc = 4;
        goto out;
    }
    printf("test.exe: mode 100h: %ux%ux%u, pitch=%u, window=%uK\r\n",
           *(uint16_t *)(info + 0x12), *(uint16_t *)(info + 0x14),
           info[0x19], *(uint16_t *)(info + 0x10),
           *(uint16_t *)(info + 0x06));

    regs.w.ax = 0x4f02;
    regs.w.bx = 0x0100;
    if (!vbe_int10(&regs, &sregs)) {
        printf("test.exe: VBE 4F02 mode 100h failed, AX=%04x\r\n", regs.w.ax);
        rc = 5;
        goto out;
    }

    /* Make all 256 DAC entries visible and deterministic. */
    outp(0x3c8, 0);
    for (unsigned i = 0; i < 256; ++i) {
        outp(0x3c9, (uint8_t)((i & 0x07) * 9));
        outp(0x3c9, (uint8_t)(((i >> 3) & 0x07) * 9));
        outp(0x3c9, (uint8_t)(((i >> 6) & 0x03) * 21));
    }

    /* 640*400 = 256000 bytes.  Exercise every 64K VBE bank through the
       emulated A000h aperture; do not bypass VGA semantics with a raw ptr. */
    for (uint16_t bank = 0; bank < 4; ++bank) {
        uint32_t first = (uint32_t)bank << 16;
        uint32_t count = 65536u;

        if (first + count > 640u * 400u)
            count = 640u * 400u - first;

        regs.w.ax = 0x4f05;
        regs.h.bh = 0x00;
        regs.h.bl = 0x00;
        regs.w.dx = bank;
        if (!vbe_int10(&regs, &sregs)) {
            rc = 6;
            break;
        }

        for (uint32_t off = 0; off < count; ++off) {
            uint32_t linear = first + off;
            uint16_t x = (uint16_t)(linear % 640u);
            uint16_t y = (uint16_t)(linear / 640u);
            uint8_t color = (uint8_t)(((x >> 4) + (y >> 4) * 40u) & 0xffu);
            dos_phys_write8(0xA0000u + off, color);
        }
    }

    if (rc == 0) {
        printf("test.exe: VBE image drawn; press Enter to return to text mode\r\n");
        getchar();
    }

    regs.w.ax = 0x0003;
    int386x(0x10, &regs, &regs, &sregs);

    if (rc == 0)
        printf("test.exe: VBE test OK\r\n");

out:
    dos_free_low(info);
    return rc;
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
    if (argc == 2 && is_vbe_arg(argv[1]))
        return test_vbe();
    return 0;
}
