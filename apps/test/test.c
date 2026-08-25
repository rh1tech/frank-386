#include "dos-api.h"
#include "dos.h"
#include "io.h"
#include "dos_mem.h"
#include "dos_phys.h"
#include "conio.h"
#include "dos_yield.h"
#include "dos_video.h"
#include "tsr_callback.h"
#include <stdio.h>
#include <string.h>

static volatile uint32_t tsr0_count;
static volatile uint32_t tsr1_count;
static volatile uint8_t *tsr_video;
static tsr_callback_t previous_tsr0;
static tsr_callback_t previous_tsr1;

static inline uint8_t hex_digit(uint32_t value)
{
    value &= 0x0fu;
    return (uint8_t)(value < 10u ? ('0' + value) : ('A' + value - 10u));
}

static void test_tsr0(void)
{
    uint32_t count = ++tsr0_count;

    tsr_video[0] = hex_digit(count);
    tsr_video[1] = 0x04;
    if (previous_tsr0 != 0)
        previous_tsr0();
}

static void test_tsr1(void)
{
    uint32_t count = ++tsr1_count;

    tsr_video[79u * 4u] = hex_digit(count);
    tsr_video[79u * 4u + 1u] = 0x04;
    if (previous_tsr1 != 0)
        previous_tsr1();
}

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

static void vbe_wait_key(void)
{
    uint32_t quiet_since = dos_yield();

    /* Ignore typematic repeats left by the key used for the previous mode. */
    for (;;) {
        if (kbhit()) {
            (void)getch();
            quiet_since = dos_yield();
            continue;
        }
        if ((uint32_t)(dos_yield() - quiet_since) >= 250000u)
            break;
    }

    (void)getch();
}

static uint8_t vbe_test_byte(uint16_t bpp, uint16_t width, uint16_t height,
                             uint16_t pitch, uint32_t linear)
{
    uint32_t y = linear / pitch;
    uint32_t row_off = linear % pitch;
    uint32_t bytespp = (bpp + 7u) / 8u;
    uint32_t x = row_off / bytespp;
    uint32_t byte = row_off % bytespp;
    uint32_t r = width > 1 ? (x * 255u) / (width - 1u) : 0;
    uint32_t g = height > 1 ? (y * 255u) / (height - 1u) : 0;
    uint32_t b = (r + g) >> 1;
    uint32_t pixel;

    if (bpp == 8)
        return (uint8_t)(((x >> 3) + (y >> 3) * 40u) & 0xffu);

    if (bpp == 15)
        pixel = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    else if (bpp == 16)
        pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    else
        pixel = b | (g << 8) | (r << 16);

    return (uint8_t)(pixel >> (byte * 8u));
}

static int test_vbe(void)
{
    static const uint16_t modes[] = { 0x0100, 0x010d, 0x010e, 0x010f };
    union REGS regs = {0};
    struct SREGS sregs;
    uint8_t *info;
    uint16_t info_seg;
    int rc = 0;
    int graphics_active = 0;

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

    for (unsigned mi = 0; mi < sizeof(modes) / sizeof(modes[0]); ++mi) {
        uint16_t mode = modes[mi];
        uint16_t width, height, pitch, bpp;
        uint32_t total;
        uint16_t banks;

        memset(info, 0, 256);
        regs.w.ax = 0x4f01;
        regs.w.cx = mode;
        regs.w.di = 0;
        if (!vbe_int10(&regs, &sregs)) {
            printf("test.exe: VBE 4F01 mode %03xh failed, AX=%04x\r\n",
                   mode, regs.w.ax);
            rc = 4;
            goto text_mode;
        }

        pitch = *(uint16_t *)(info + 0x10);
        width = *(uint16_t *)(info + 0x12);
        height = *(uint16_t *)(info + 0x14);
        bpp = info[0x19];
        total = (uint32_t)pitch * height;
        banks = (uint16_t)((total + 65535u) >> 16);

        printf("test.exe: mode %03xh: %ux%ux%u, pitch=%u, banks=%u\r\n",
               mode, width, height, bpp, pitch, banks);

        regs.w.ax = 0x4f02;
        regs.w.bx = mode;
        if (!vbe_int10(&regs, &sregs)) {
            printf("test.exe: VBE 4F02 mode %03xh failed, AX=%04x\r\n",
                   mode, regs.w.ax);
            rc = 5;
            goto text_mode;
        }
        graphics_active = 1;

        if (bpp == 8) {
            outp(0x3c8, 0);
            for (unsigned i = 0; i < 256; ++i) {
                outp(0x3c9, (uint8_t)((i & 0x07) * 9));
                outp(0x3c9, (uint8_t)(((i >> 3) & 0x07) * 9));
                outp(0x3c9, (uint8_t)(((i >> 6) & 0x03) * 21));
            }
        }

        for (uint16_t bank = 0; bank < banks; ++bank) {
            uint32_t first = (uint32_t)bank << 16;
            uint32_t count = 65536u;

            if (first + count > total)
                count = total - first;

            regs.w.ax = 0x4f05;
            regs.h.bh = 0x00;
            regs.h.bl = 0x00;
            regs.w.dx = bank;
            if (!vbe_int10(&regs, &sregs)) {
                printf("test.exe: VBE 4F05 mode %03xh bank %u failed, AX=%04x\r\n",
                       mode, bank, regs.w.ax);
                rc = 6;
                goto text_mode;
            }

            for (uint32_t off = 0; off < count; ++off)
                dos_phys_write8(0xA0000u + off,
                                 vbe_test_byte(bpp, width, height, pitch,
                                               first + off));
        }

        vbe_wait_key();
    }

text_mode:
    if (graphics_active) {
        regs.w.ax = 0x0003;
        int386x(0x10, &regs, &regs, &sregs);
    }

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
    uint32_t video_size = 0;
    uint16_t psp_seg;
    uint16_t end_seg;

    tsr_video = dos_video_get_buffer(&video_size);
    if (tsr_video == 0 || video_size < 80u * 25u * 2u)
        return 4;

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

    tsr0_count = 0;
    tsr1_count = 0;
    previous_tsr0 = set_tsr0_callback(test_tsr0);
    previous_tsr1 = set_tsr1_callback(test_tsr1);

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
