#include <time.h>
#include <stdbool.h>
#include <pico.h>
#include "i386.h"
#include "cpu.h"
#include "bios/bios.h"
#include "fdos/fdos.h"

/*
 * Platform notification is deliberately asynchronous: the CPU core must not
 * sleep while reporting an unsupported guest instruction.
 */
extern void emulator_unsupported_cpu_feature(const char *mnemonic,
                                             uint16_t cs, uint16_t ip);
extern void request_terminate(uint8_t exit_code, uint8_t exit_type);

#define IRAM_ATTR __not_in_flash()
#define INLINE __always_inline

static uint32_t segregs32[6];

#undef CPU_CS
#undef CPU_DS
#undef CPU_ES
#undef CPU_SS
#define CPU_CS    segregs[regcs << 1]
#define CPU_DS    segregs[regds << 1]
#define CPU_ES    segregs[reges << 1]
#define CPU_SS    segregs[regss << 1]
#define CPU_FS    segregs[regfs << 1]
#define CPU_GS    segregs[reggs << 1]

u8 get_reg8(struct CPU* cpu, u8 regn);
u16 get_reg16(const struct CPU* cpu, u8 regn);
u32 get_reg32(struct CPU* cpu, u8 regn);
void set_reg8(struct CPU* cpu, u8 regn, u8 v);
void set_reg16(struct CPU* cpu, u8 regn, u16 v);
void set_reg32(struct CPU* cpu, u8 regn, u32 v);
static u16 IRAM_ATTR get_seg16(const struct CPU* _cpu, u8 segn) {
    (void)_cpu;
	return getsegreg(segn);
}
static void IRAM_ATTR set_seg16(struct CPU* _cpu, u8 segn, u16 v) {
    (void)_cpu;
    putsegreg(segn, v);
}
static void IRAM_ATTR set_flag(CPU* cpu, u32 mask, bool val)
{
	if (val)
		cpu->flags.value |= mask;
	else
		cpu->flags.value &= ~mask;
}
static u32 IRAM_ATTR get_flags(CPU* cpu, u32 mask)
{
	return cpu->flags.value & mask;
}
static void IRAM_ATTR set_flags(CPU* cpu, uword set_mask, uword clear_mask)
{
	cpu->flags.value |= set_mask;
	cpu->flags.value &= ~clear_mask;
	cpu->flags.value &= cpu->flags_mask;
}
void cpu_enable_fpu(CPU* cpu);

#include "disk.h"
#include "ff.h"

static INLINE void push(CPU* cpu, uint16_t pushval) {
    CPU_SP = CPU_SP - 2;
    putmem8(CPU_SS, CPU_SP, pushval & 0xff);
    putmem8(CPU_SS, (CPU_SP + 1) & 0xffff, pushval >> 8);
}

static void reset(CPU* cpu) {
    if (cpu->gen == 2) {
        CPU_CS = 0xF000;
        SET_IP ( 0xFFF0 );
    } else {
        CPU_CS = 0xFFFF;
        SET_IP ( 0x0000 );
    }
    cpu->flags.value = 2;
}

#include "./fpu.h"

static void i286_step(CPU* cpu, int stepcount);
void raise_irq(CPU* cpu);
void setexc(CPU* cpu, int excno, uword excerr);
static void i286_abort(CPU* cpu, int code)
{
//	dolog("abort: %d %x\n", code, code);
	abort();
}

handler_t handlers[256];

#include <stdio.h>
#define printf(...) bios_printf(cpu, __VA_ARGS__)
void cpu_err_msg(CPU* cpu, const char* msg) {
    char buf[80];
    uint32_t ip = ((uint32_t)CPU_CS << 4) + CPU_IP;
    uint32_t sp = ((uint32_t)CPU_SS << 4) + CPU_SP;
    uint16_t fw = cpu_getflags(cpu);
    print_line(msg, 1);

    snprintf(buf, sizeof(buf), "AX=%04X BX=%04X CX=%04X DX=%04X ",
             CPU_AX, CPU_BX, CPU_CX, CPU_DX); print_line(buf, 2);
    snprintf(buf, sizeof(buf), "SI=%04X DI=%04X BP=%04X SP=%04X ",
             CPU_SI, CPU_DI, CPU_BP, CPU_SP); print_line(buf, 3);
    snprintf(buf, sizeof(buf), "CS:IP=%04X:%04X  SS:SP=%04X:%04X ",
             CPU_CS, CPU_IP, CPU_SS, CPU_SP); print_line(buf, 4);
    snprintf(buf, sizeof(buf), "DS=%04X ES=%04X FS=%04X GS=%04X ",
             CPU_DS, CPU_ES, CPU_FS, CPU_GS); print_line(buf, 5);

    snprintf(buf, sizeof(buf), "FLAGS=%04X %c%c%c%c%c%c%c%c%c ",
             fw,
             (fw & 0x0800) ? 'O' : '-',
             (fw & 0x0400) ? 'D' : '-',
             (fw & 0x0200) ? 'I' : '-',
             (fw & 0x0100) ? 'T' : '-',
             (fw & 0x0080) ? 'S' : '-',
             (fw & 0x0040) ? 'Z' : '-',
             (fw & 0x0010) ? 'A' : '-',
             (fw & 0x0004) ? 'P' : '-',
             (fw & 0x0001) ? 'C' : '-');
    print_line(buf, 6);

    snprintf(buf, sizeof(buf), "OP=%02X %02X %02X %02X %02X %02X ",
             read86(ip + 0), read86(ip + 1), read86(ip + 2),
             read86(ip + 3), read86(ip + 4), read86(ip + 5));
    print_line(buf, 7);

    snprintf(buf, sizeof(buf), "STACK=%04X %04X %04X %04X ",
             readw86(sp + 0), readw86(sp + 2),
             readw86(sp + 4), readw86(sp + 6));
    print_line(buf, 8);

    snprintf(buf, sizeof(buf), "RET? IP=%04X CS=%04X FL=%04X ",
             readw86(sp + 0), readw86(sp + 2), readw86(sp + 4));
    print_line(buf, 9);
}
static bool no_handler(CPU* cpu) {
#ifdef NO_HANDLER_DETECTOR
    cpu_err_msg(cpu, "ERROR: no handler defined");
while(1); // remove it
#endif
    return true;
}

void cpu_init_286(CPU* cpu) {
	CPU_ext_accessors_t* cpue = cpu->ext_accessors;
    cpue->get_reg8 = get_reg8;
    cpue->get_reg16 = get_reg16;
    cpue->get_reg32 = get_reg32;
    cpue->set_reg8 = set_reg8;
    cpue->set_reg16 = set_reg16;
    cpue->set_reg32 = set_reg32;
    cpue->get_seg16 = get_seg16;
    cpue->set_seg16 = set_seg16;
    cpue->get_flags = get_flags;
    cpue->set_flag = set_flag;
    cpue->set_flags = set_flags;
    cpue->enable_fpu = cpu_enable_fpu;
    cpue->reset = reset;
    cpue->step = i286_step;
    cpue->raise_irq = raise_irq;
    cpue->setexc = setexc;
    cpue->abort = i286_abort;
}

void cpu_install_bios_handlers(CPU* cpu) {
    for(int i = 0; i < 256; ++i) {
        if (i == 0x21 || i == 0x29 || i == 0x2f) continue;
        handlers[i] = no_handler;
    }
    handlers[0x00] = bios_00h; // DIVIDE BY ZERO
    handlers[0x05] = bios_05h; // PRINT SCREEN / BOUND EXCEPTION
    handlers[0x08] = bios_08h; // IRQ0: Timer
    handlers[0x09] = bios_09h; // IRQ1: Keyboard
    handlers[0x10] = bios_10h; // VIDEO
    handlers[0x11] = bios_11h; // EQUIPMENT LIST
    handlers[0x12] = bios_12h; // Conventional RAM count
    handlers[0x13] = bios_13h; // DISK
    handlers[0x14] = bios_14h; // SERIAL
    handlers[0x15] = bios_15h; // TSR
    handlers[0x16] = bios_16h; // KEYBOARD
    handlers[0x17] = bios_17h; // PRINTERS
    handlers[0x18] = bios_18h; // BASIC
    handlers[0x19] = bios_19h; // BOOTSTRAP
    handlers[0x1A] = bios_1Ah; // CMOS TIME
    handlers[0x33] = bios_33h; // MS MOUSE
    handlers[0x74] = bios_74h; // IRQ12: PS/2 mouse
    handlers[0xFF] = bios_FFh; // W/A BIOS callback
}

void cpu_install_dos_handlers(CPU* cpu) {
    handlers[0x20] = fdos_20h; // old-style (CP/M) terminate
	pstore16(0x20*4, 0x0020);
	pstore16(0x20*4 + 2, 0xFFE0);

    handlers[0x21] = fdos_21h; // main DOS handler
	pstore16(0x21*4, 0x0021);
	pstore16(0x21*4 + 2, 0xFFE0);

    handlers[0x25] = fdos_25h; // absolute disk read
	pstore16(0x25*4, 0x0025);
	pstore16(0x25*4 + 2, 0xFFE0);

    handlers[0x26] = fdos_26h; // absolute disk write
	pstore16(0x26*4, 0x0026);
	pstore16(0x26*4 + 2, 0xFFE0);

    handlers[0x27] = fdos_27h; // old-style TSR
	pstore16(0x27*4, 0x0027);
	pstore16(0x27*4 + 2, 0xFFE0);

    handlers[0x28] = fdos_28h; // DOS idle
	pstore16(0x28*4, 0x0028);
	pstore16(0x28*4 + 2, 0xFFE0);
    
    handlers[0x29] = fdos_29h; // fast console output.
	pstore16(0x29*4, 0x0029);
	pstore16(0x29*4 + 2, 0xFFE0);

    handlers[0x2F] = fdos_2fh; // XMS
	pstore16(0x2f*4, 0x002f);
	pstore16(0x2f*4 + 2, 0xFFE0);

    /*
     * CP/M CALL-5 gateway. This is NOT a software INT 30h: PSP:0005h does
     * CALL FAR 0000:00C0, and 0000:00C0 (written in PSPInit) is a JMP FAR to
     * FFE0:0030. Landing on that fake-BIOS page dispatches here. fdos_30h()
     * consumes the far-call frame itself and returns false, so the common
     * IRET path is not applied.
     */
    handlers[0x30] = fdos_30h;
}

//#define CPU_ALLOW_ILLEGAL_OP_EXCEPTION
//#define CPU_LIMIT_SHIFT_COUNT
#define CPU_NO_SALC
//#define CPU_SET_HIGH_FLAGS
#define CPU_286_STYLE_PUSH_SP

u8 segoverride, reptype;
u16 useseg, oldsp;
uint8_t tempcf, oldcf, mode, reg, rm, sib;
bool operandSizeOverride = false;
bool addressSizeOverride = false;
u8 nestlev;
u16 saveip, savecs, oper1, oper2, res16, temp16, dummy, stacksize, frametemp;
uint32_t disp32;
#define disp16 (*(uint16_t*)&disp32)
uint32_t ea;

static const bool __not_in_flash("cpu.pf") parity[0x100] = {
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

__not_in_flash() void modregrm(CPU* cpu) {
    register uint8_t addrbyte = getmem8(CPU_CS, CPU_IP);
    StepIP(1);
    mode = addrbyte >> 6;
    reg = (addrbyte >> 3) & 7;
    rm = addrbyte & 7;
    switch (mode) {
        case 0:
            if (rm == 6) {
                disp16 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
            } else {
                disp16 = 0;
            }
            if (((rm == 2) || (rm == 3)) && !segoverride) {
                useseg = CPU_SS;
            }
            break;
        case 1:
            disp16 = signext(getmem8(CPU_CS, CPU_IP));
            StepIP(1);
            if (((rm == 2) || (rm == 3) || (rm == 6)) && !segoverride) {
                useseg = CPU_SS;
            }
            break;
        case 2:
            disp16 = getmem16(CPU_CS, CPU_IP);
            StepIP(2);
            if (((rm == 2) || (rm == 3) || (rm == 6)) && !segoverride) {
                useseg = CPU_SS;
            }
            break;
        default:
            disp16 = 0;
    }
}

__not_in_flash() void getea(CPU* cpu, uint8_t rmval) {
    register uint32_t tempea = 0;
    switch (mode) {
        case 0:
            switch (rmval) {
                case 0: tempea = CPU_BX + CPU_SI;
                    break;
                case 1: tempea = CPU_BX + CPU_DI;
                    break;
                case 2: tempea = CPU_BP + CPU_SI;
                    break;
                case 3: tempea = CPU_BP + CPU_DI;
                    break;
                case 4: tempea = CPU_SI;
                    break;
                case 5: tempea = CPU_DI;
                    break;
                case 6: tempea = disp16;
                    break;
                case 7: tempea = CPU_BX;
                    break;
            }
            break;

        case 1:
        case 2:
            switch (rmval) {
                case 0: tempea = CPU_BX + CPU_SI + disp16;
                    break;
                case 1: tempea = CPU_BX + CPU_DI + disp16;
                    break;
                case 2: tempea = CPU_BP + CPU_SI + disp16;
                    break;
                case 3: tempea = CPU_BP + CPU_DI + disp16;
                    break;
                case 4: tempea = CPU_SI + disp16;
                    break;
                case 5: tempea = CPU_DI + disp16;
                    break;
                case 6: tempea = CPU_BP + disp16;
                    break;
                case 7: tempea = CPU_BX + disp16;
                    break;
            }
            break;
    }
    ea = (tempea & 0xFFFF) + (useseg << 4);
}

static INLINE uint16_t pop(CPU* cpu) {
    uint16_t tempval = getmem8(CPU_SS, CPU_SP)
                     | ((uint16_t)getmem8(CPU_SS, (CPU_SP + 1) & 0xffff) << 8);
    CPU_SP = CPU_SP + 2;
    return tempval;
}

static INLINE uint32_t readrm32(CPU* cpu, uint8_t rmval) {
    if (mode < 3) {
        getea(cpu, rmval);
        return readw86(ea);
    }
    return getreg32(rmval);
}

static INLINE uint16_t readrm16(CPU* cpu, uint8_t rmval) {
    if (mode < 3) {
        getea(cpu, rmval);
        return readw86(ea);
    }
    return getreg16(rmval);
}

static INLINE uint8_t readrm8(CPU* cpu, uint8_t rmval) {
    if (mode < 3) {
        getea(cpu, rmval);
        return read86(ea);
    }
    return getreg8(rmval);
}

static INLINE void writerm16(CPU* cpu, uint8_t rmval, uint16_t value) {
    if (mode < 3) {
        getea(cpu, rmval);
        writew86(ea, value);
    } else {
        putreg16(rmval, value);
    }
}

static INLINE void writerm32(CPU* cpu, uint8_t rmval, uint32_t value) {
    if (mode < 3) {
        getea(cpu, rmval);
        writedw86(ea, value);
    } else {
        putreg32(rmval, value);
    }
}

static INLINE void writerm8(CPU* cpu, uint8_t rmval, uint8_t value) {
    if (mode < 3) {
        getea(cpu, rmval);
        write86(ea, value);
    } else {
        putreg8(rmval, value);
    }
}

static INLINE uint16_t makeflagsword(CPU* cpu) {
    if (cpu->gen == 2)
        return 2 | (cpu->flags.value & cpu->flags_mask);
    return 0xF002 | cpu->flags.value;
}

static INLINE void decodeflagsword(CPU* cpu, uint16_t x) {
    cpu->flags.value = x;
}

static INLINE void intcall86(CPU* cpu, uint8_t intnum) {
    #if BIOS_DEBUG
    if (intnum != 0x10 && intnum != 0x1C && intnum != 0x08) {
        char buf[80];
        u16 new_cs = getmem16(0, (uint16_t) intnum * 4 + 2);
        u16 new_ip = getmem16(0, (uint16_t) intnum * 4);
        snprintf(buf, 79, "INT %02Xh DOS? %04X:%04X->%04X:%04X AX:%04X", intnum, CPU_CS, CPU_IP, new_cs, new_ip, CPU_AX);
        print_line(buf, 0);
    }
    #endif
    push(cpu, makeflagsword(cpu));
    push(cpu, CPU_CS);
    push(cpu, CPU_IP);
    CPU_CS = getmem16(0, (uint16_t) intnum * 4 + 2);
    SET_IP ( getmem16(0, (uint16_t) intnum * 4) );
    ifl = 0;
    tf = 0;
}

static inline void flag_szp8(CPU* cpu, uint8_t value) {
    zf = value == 0;
    sf = value >> 7;
    pf = parity[value];
}

static inline void flag_szp16(CPU* cpu, uint16_t value) {
    zf = value == 0;
    sf = value >> 15;
    pf = parity[value & 255];
}

static inline void flag_szp32(CPU* cpu, uint32_t value) {
    zf = value == 0;
    sf = value >> 31;
    pf = parity[value & 255];
}

static inline void flag_log8(CPU* cpu, uint8_t value) {
    flag_szp8(cpu, value);
    cpu->flags.value &= ~FLAG_CF_OF_MASK;
}

static inline void flag_log16(CPU* cpu, uint16_t value) {
    flag_szp16(cpu, value);
    cpu->flags.value &= ~FLAG_CF_OF_MASK;
}

static inline void flag_adc8(CPU* cpu, uint8_t v1, uint8_t v2, uint8_t v3) {
    /* v1 = destination operand, v2 = source operand, v3 = carry flag */
    uint32_t dst = (uint32_t) v1 + (uint32_t) v2 + (uint32_t) v3;
    flag_szp8(cpu, (uint8_t) dst);
    of = ((dst ^ (uint32_t)v1) & (dst ^ (uint32_t)v2) & 0x80) != 0;
    cf = (dst & 0xFF00) != 0;
    af = (((uint32_t)v1 ^ (uint32_t)v2 ^ dst) & 0x10) != 0;
}

static inline void flag_adc16(CPU* cpu, uint16_t v1, uint16_t v2, uint16_t v3) {
    register uint32_t dst = (uint32_t) v1 + (uint32_t) v2 + (uint32_t) v3;
    flag_szp16(cpu, (uint16_t) dst);
    of = (((dst ^ (uint32_t)v1) & (dst ^ (uint32_t)v2)) & 0x8000) != 0;
    cf = (dst & 0xFFFF0000) != 0;
    af = (((uint32_t)v1 ^ (uint32_t)v2 ^ dst) & 0x10) != 0;
}

static inline void flag_add32(CPU* cpu, uint32_t v1, uint32_t v2, uint32_t res32) {
    /* v1 = destination operand, v2 = source operand */
    flag_szp32(cpu, res32);
    cf = (((uint64_t) v1 + (uint64_t) v2) & 0xF00000000) != 0;
    of = ((res32 ^ v1) & (res32 ^ v2) & 0x8000) != 0;
    af = ((v1 ^ v2 ^ res32) & 0x10) != 0;
}

static inline uint8_t sbb8(CPU* cpu, uint8_t v1, uint8_t v2, uint8_t v3) {
    /* v1 = destination operand, v2 = source operand, v3 = carry flag */
    register uint32_t dst = (uint32_t)v1 - (uint32_t)v2 - (uint32_t)v3;
    flag_szp8(cpu, (uint8_t) dst);
    cf = ((dst >> 8) & 1) != 0;
    of = ((dst ^ v1) & (v1 ^ v2) & 0x80) != 0;
    af = ((v1 ^ v2 ^ dst ^ v3) & 0x10) != 0;
    return (uint8_t)dst;
}

static inline uint16_t sbb16(CPU* cpu, uint16_t v1, uint16_t v2, uint8_t v3) {
    /* v1 = destination operand, v2 = source operand, v3 = carry flag */
    register uint32_t dst = (uint32_t)v1 - (uint32_t)v2 - (uint32_t)v3;
    flag_szp16(cpu, (uint16_t) dst);
    cf = ((dst >> 16) & 1) != 0;
    of = ((dst ^ (uint32_t)v1) & (v1 ^ (uint32_t)v2) & 0x8000) != 0;
    af = ((v1 ^ v2 ^ dst ^ v3) & 0x10) != 0;
    return (uint16_t)dst;
}

static inline uint32_t sbb32(CPU* cpu, uint32_t v1, uint32_t v2, uint8_t v3) {
    /* v1 = destination operand, v2 = source operand, v3 = carry flag */
    register uint64_t dst = (uint64_t)v1 - (uint64_t)v2 - (uint64_t)v3;
    flag_szp32(cpu, (uint32_t) dst);
    cf = ((dst >> 32) & 1) != 0;
    of = ((dst ^ v1) & (v1 ^ v2) & 0x80000000) != 0;
    af = ((v1 ^ v2 ^ dst ^ v3) & 0x10) != 0;
    return (uint32_t)dst;
}

static inline void flag_sub8(CPU* cpu, uint8_t v1, uint8_t v2) {
    /* v1 = destination operand, v2 = source operand */
    uint32_t dst = (uint32_t) v1 - (uint32_t) v2;
    flag_szp8(cpu, (uint8_t) dst);
    cf = (dst & 0xFF00) != 0;
    of = ((dst ^ (uint32_t)v1) & (v1 ^ v2) & 0x80) != 0;
    af = ((v1 ^ v2 ^ dst) & 0x10) != 0;
}

static inline void flag_sub16(CPU* cpu, uint16_t v1, uint16_t v2) {
    /* v1 = destination operand, v2 = source operand */
    register uint32_t dst = (uint32_t) v1 - (uint32_t) v2;
    flag_szp16(cpu, (uint16_t) dst);
    cf = (dst & 0xFFFF0000) != 0;
    of = ((dst ^ (uint32_t)v1) & ((uint32_t)v1 ^ (uint32_t)v2) & 0x8000) != 0;
    af = (((uint32_t)v1 ^ (uint32_t)v2 ^ dst) & 0x10) != 0;
}

#define op_adc8() { res8 = oper1b + oper2b + cf; flag_adc8(cpu, oper1b, oper2b, cf); }
#define op_adc16() { res16 = oper1 + oper2 + cf; flag_adc16(cpu, oper1, oper2, cf); }
#define op_add8() { \
    register uint32_t dst = (uint32_t)oper1b + (uint32_t)oper2b; \
    res8 = dst; \
    flag_szp8(cpu, res8); \
    cf = (dst & 0xFF00) != 0; \
    of = ((dst ^ (uint32_t)oper1b) & (dst ^ (uint32_t)oper2b) & 0x80) != 0; \
    af = ((oper1b ^ oper2b ^ dst) & 0x10) != 0; \
}
#define op_add16() { \
    register uint32_t dst = (uint32_t)oper1 + (uint32_t)oper2; \
    res16 = dst; \
    flag_szp16(cpu, dst); \
    cf = (dst & 0xFFFF0000) != 0; \
    of = (((dst ^ (uint32_t)oper1) & (dst ^ (uint32_t)oper2) & 0x8000) != 0); \
    af = (((oper1 ^ oper2 ^ dst) & 0x10) != 0); \
}
#define op_add32() { res32 = oper1 + oper2; flag_add32(cpu, oper1, oper2, res32); }
#define op_and8() { res8 = oper1b & oper2b; flag_log8(cpu, res8); }
#define op_and16() { res16 = oper1 & oper2; flag_log16(cpu, res16); }
#define op_and32() { res32 = oper1 & oper2; flag_log32(res32); }
#define op_or8() { res8 = oper1b | oper2b; flag_log8(cpu, res8); }
#define op_or16() { res16 = oper1 | oper2; flag_log16(cpu, res16); }
#define op_or32() { res32 = oper1 | oper2; flag_log32(res32); }
#define op_xor8() { res8 = oper1b ^ oper2b; flag_log8(cpu, res8); }
#define op_xor16() { res16 = oper1 ^ oper2; flag_log16(cpu, res16); }
#define op_xor32() { res32 = oper1 ^ oper2; flag_log32(res32); }
#define op_sub8() { res8 = oper1b - oper2b; flag_sub8(cpu, oper1b, oper2b); }
#define op_sub16() { \
    register uint32_t dst = (uint32_t) oper1 - (uint32_t) oper2; \
    flag_szp16(cpu, (uint16_t) dst); \
    cf = (dst & 0xFFFF0000) != 0; \
    of = ((dst ^ (uint32_t)oper1) & (oper1 ^ oper2) & 0x8000) != 0; \
    af = ((oper1 ^ oper2 ^ dst) & 0x10) != 0; \
    res16 = (uint16_t) dst; \
}
#define op_sub32() { res32 = oper1 - oper2; flag_sub32(oper1, oper2); }
#define op_sbb8(cpu) { res8 = sbb8(cpu, oper1b, oper2b, cf); }
#define op_sbb16(cpu) { res16 = sbb16(cpu, oper1, oper2, cf); }
#define op_sbb32(cpu) { res32 = sbb32(cpu, oper1, oper2, cf); }

static __not_in_flash() uint8_t op_grp2_8(CPU* cpu, uint8_t cnt, uint8_t oper1b) {
    uint16_t s = oper1b;
#ifdef CPU_LIMIT_SHIFT_COUNT
    cnt &= 0x1F;
#endif
    switch (reg) {
        case 0: /* ROL r/m8 */
            for (int shift = 1; shift <= cnt; shift++) {
                if (s & 0x80) {
                    cf = 1;
                } else {
                    cf = 0;
                }

                s = s << 1;
                s = s | cf;
            }

            if (cnt == 1) {
                // of = cf ^ ( (s >> 7) & 1);
                if ((s & 0x80) && cf)
                    of = 1;
                else
                    of = 0;
            } else
                of = 0;
            break;

        case 1: /* ROR r/m8 */
            for (int shift = 1; shift <= cnt; shift++) {
                cf = s & 1;
                s = (s >> 1) | (cf << 7);
            }

            if (cnt == 1) {
                of = (s >> 7) ^ ((s >> 6) & 1);
            }
            break;

        case 2: /* RCL r/m8 */
            for (int shift = 1; shift <= cnt; shift++) {
                register bool oldcf = cf;
                if (s & 0x80) {
                    cf = 1;
                } else {
                    cf = 0;
                }

                s = s << 1;
                s = s | oldcf;
            }

            if (cnt == 1) {
                of = cf ^ ((s >> 7) & 1);
            }
            break;

        case 3: /* RCR r/m8 */
            for (int shift = 1; shift <= cnt; shift++) {
                register uint8_t oldcf = cf;
                cf = s & 1;
                s = (s >> 1) | (oldcf << 7);
            }

            if (cnt == 1) {
                of = (s >> 7) ^ ((s >> 6) & 1);
            }
            break;

        case 4:
        case 6: /* SHL r/m8 */
            for (int shift = 1; shift <= cnt; shift++) {
                if (s & 0x80) {
                    cf = 1;
                } else {
                    cf = 0;
                }

                s = (s << 1) & 0xFF;
            }

            if ((cnt == 1) && (cf == (s >> 7))) {
                of = 0;
            } else {
                of = 1;
            }

            flag_szp8(cpu, (uint8_t) s);
            break;

        case 5: /* SHR r/m8 */
            if ((cnt == 1) && (s & 0x80)) {
                of = 1;
            } else {
                of = 0;
            }

            for (int a = 1; a <= cnt; a++) {
                cf = s & 1;
                s = s >> 1;
            }

            flag_szp8(cpu, (uint8_t) s);
            break;

        case 7: /* SAR r/m8 */
            for (int a = 1; a <= cnt; a++) {
                unsigned int msb = s & 0x80;
                cf = s & 1;
                s = (s >> 1) | msb;
            }

            of = 0;
            flag_szp8(cpu, (uint8_t) s);
            break;
    }

    return s & 0xFF;
}

static __not_in_flash() uint16_t op_grp2_16(CPU* cpu, uint8_t cnt) {
    register uint32_t s = oper1;
#ifdef CPU_LIMIT_SHIFT_COUNT
    cnt &= 0x1F;
#endif
    switch (reg) {
        case 0: /* ROL r/m16 */
            for (int shift = 1; shift <= cnt; shift++) {
                if (s & 0x8000) {
                    cf = 1;
                } else {
                    cf = 0;
                }

                s = s << 1;
                s = s | cf;
            }

            if (cnt == 1) {
                of = cf ^ ((s >> 15) & 1);
            }
            break;

        case 1: /* ROR r/m16 */
            for (int shift = 1; shift <= cnt; shift++) {
                cf = s & 1;
                s = (s >> 1) | (cf << 15);
            }

            if (cnt == 1) {
                of = (s >> 15) ^ ((s >> 14) & 1);
            }
            break;

        case 2: /* RCL r/m16 */
            for (int shift = 1; shift <= cnt; shift++) {
                register bool oldcf = cf;
                if (s & 0x8000) {
                    cf = 1;
                } else {
                    cf = 0;
                }

                s = s << 1;
                s = s | oldcf;
            }

            if (cnt == 1) {
                of = cf ^ ((s >> 15) & 1);
            }
            break;

        case 3: /* RCR r/m16 */
            for (int shift = 1; shift <= cnt; shift++) {
                register uint32_t oldcf = cf;
                cf = s & 1;
                s = (s >> 1) | (oldcf << 15);
            }

            if (cnt == 1) {
                of = (s >> 15) ^ ((s >> 14) & 1);
            }
            break;

        case 4:
        case 6: /* SHL r/m16 */
            for (unsigned int shift = 1; shift <= cnt; shift++) {
                if (s & 0x8000) {
                    cf = 1;
                } else {
                    cf = 0;
                }

                s = (s << 1) & 0xFFFF;
            }

            if ((cnt == 1) && (cf == (s >> 15))) {
                of = 0;
            } else {
                of = 1;
            }

            flag_szp16(cpu, (uint16_t) s);
            break;

        case 5: /* SHR r/m16 */
            if ((cnt == 1) && (s & 0x8000)) {
                of = 1;
            } else {
                of = 0;
            }

            for (int shift = 1; shift <= cnt; shift++) {
                cf = s & 1;
                s = s >> 1;
            }

            flag_szp16(cpu, (uint16_t) s);
            break;

        case 7: /* SAR r/m16 */
            for (int shift = 1, msb; shift <= cnt; shift++) {
                msb = s & 0x8000;
                cf = s & 1;
                s = (s >> 1) | msb;
            }

            of = 0;
            flag_szp16(cpu, (uint16_t) s);
            break;
    }

    return (uint16_t) s & 0xFFFF;
}

static inline void op_div8(CPU* cpu, uint16_t valdiv, uint8_t divisor) {
    if (divisor == 0 || valdiv / divisor > 0xFF) {
   //     printf("[op_div8] %d / %d\n", valdiv, divisor);
        intcall86(cpu, 0);
        return;
    }
    CPU_AH = (uint8_t) (valdiv % divisor);
    CPU_AL = (uint8_t) (valdiv / divisor);
}

static inline void op_idiv8(CPU* cpu, uint16_t valdiv, int8_t divisor) {
    if (divisor == 0) {
   //     printf("[op_idiv8] %d / 0\n", valdiv);
        intcall86(cpu, 0);
        return;
    }
    int16_t dividend = (int16_t) valdiv;
    int16_t quotient  = dividend / divisor;
    int16_t remainder = dividend % divisor;
    if (quotient < -128 || quotient > 127) {
   //     printf("[op_idiv8] %d / %d overflow\n", dividend, divisor);
        intcall86(cpu, 0);
        return;
    }
    CPU_AH = (uint8_t)remainder;
    CPU_AL = (uint8_t)quotient;
}

static inline void op_div16(CPU* cpu, uint32_t valdiv, uint16_t divisor) {
    if (divisor == 0 || valdiv / divisor > 0xFFFF) {
//        CPU_DX = 0;
//        CPU_AX = 0xFFFF;
//        printf("[op_div16] %d / %d\n", valdiv, divisor);
        intcall86(cpu, 0);
        return;
    }

    CPU_DX = (uint16_t) (valdiv % divisor);
    CPU_AX = (uint16_t) (valdiv / divisor);
}

static inline void op_idiv16(CPU* cpu, uint32_t valdiv, uint16_t divisor) {
    int32_t dividend = (int32_t)valdiv;
    int16_t divisor_signed = (int16_t)divisor;
    if (divisor_signed == 0) {
    //    printf("[op_idiv16] %d / 0\n", dividend);
        intcall86(cpu, 0);
        return;
    }
    int32_t quotient  = dividend / divisor_signed;
    int32_t remainder = dividend % divisor_signed;
    if (quotient < -32768 || quotient > 32767) {
    //    printf("[op_idiv16] %d / %d overflow\n", dividend, divisor_signed);
        intcall86(cpu, 0);
        return;
    }
    CPU_AX = (uint16_t)quotient;
    CPU_DX = (uint16_t)remainder;
}


static __not_in_flash() void op_grp3_16(CPU* cpu) {
    switch (reg) {
        case 0:
        case 1: /* TEST */
            flag_log16(cpu, oper1 & getmem16(CPU_CS, CPU_IP));
            StepIP(2);
            break;

        case 2: /* NOT */
            res16 = ~oper1;
            break;

        case 3: /* NEG */
            res16 = (~oper1) + 1;
            flag_sub16(cpu, 0, oper1);
            if (res16) {
                cf = 1;
            } else {
                cf = 0;
            }
            break;

        case 4: {
            /* MUL */
            register uint32_t temp1 = (uint32_t) oper1 * (uint32_t) CPU_AX;
            CPU_AX = temp1 & 0xFFFF;
            CPU_DX = temp1 >> 16;
            flag_szp16(cpu, (uint16_t) temp1);
            if (CPU_DX) {
                cpu->flags.value |= FLAG_CF_OF_MASK;
            } else {
                cpu->flags.value &= ~FLAG_CF_OF_MASK;
            }
#ifdef CPU_CLEAR_ZF_ON_MUL
            zf = 0;
#endif
            break;
        }
        case 5: {
            /* IMUL */
            register int32_t temp1 = (int32_t)(int16_t)CPU_AX * (int32_t)(int16_t)oper1;
			int16_t truncated = (int16_t)temp1;
            CPU_AX = truncated; /* into register ax */
            CPU_DX = (uint16_t)(temp1 >> 16); /* into register dx */
            if (temp1 != (int32_t)truncated) {
                cpu->flags.value |= FLAG_CF_OF_MASK;
            } else {
                cpu->flags.value &= ~FLAG_CF_OF_MASK;
            }
#ifdef CPU_CLEAR_ZF_ON_MUL
            zf = 0;
#endif
            break;
        }
        case 6: /* DIV */
            op_div16(cpu, ((uint32_t) CPU_DX << 16) + CPU_AX, oper1);
            break;

        case 7: /* DIV */
            op_idiv16(cpu, ((uint32_t) CPU_DX << 16) + CPU_AX, oper1);
            break;
    }
}

static __not_in_flash() void op_grp5(CPU* cpu) {
    switch (reg) {
        case 0: /* INC Ev */
            oper2 = 1;
            tempcf = cf;
            op_add16();
            cf = tempcf;
            writerm16(cpu, rm, res16);
            break;

        case 1: /* DEC Ev */
            oper2 = 1;
            tempcf = cf;
            op_sub16();
            cf = tempcf;
            writerm16(cpu, rm, res16);
            break;

        case 2: /* CALL Ev */
            push(cpu, CPU_IP);
            SET_IP ( oper1 );
            break;

        case 3: /* CALL Mp */
            push(cpu, CPU_CS);
            push(cpu, CPU_IP);
            getea(cpu, rm);
            SET_IP ( (uint16_t) read86(ea) + (uint16_t) read86(ea + 1) * 256 );
            CPU_CS = (uint16_t) read86(ea + 2) + (uint16_t) read86(ea + 3) * 256;
            break;

        case 4: /* JMP Ev */
            SET_IP ( oper1 );
            break;

        case 5: /* JMP Mp */
            getea(cpu, rm);
            SET_IP ( (uint16_t) read86(ea) + (uint16_t) read86(ea + 1) * 256 );
            CPU_CS = (uint16_t) read86(ea + 2) + (uint16_t) read86(ea + 3) * 256;
            break;

        case 6: /* PUSH Ev */
            push(cpu, oper1);
            break;
    }
}

#if PDB_DEBUG
void dpb_watch_check_chain(const char *tag);
static void dpb_watch_native_checkpoint(CPU* cpu, const char *where, uint8_t intnum)
{
    static char tags[16][48];
    static unsigned tag_idx;

    char *tag = tags[tag_idx++ & 15];
    snprintf(tag, 48, "NATIVE-%s INT=%02x AX=%04x", where, intnum, CPU_AX);
    dpb_watch_check_chain(tag);
}
#else
#define dpb_watch_native_checkpoint(...)
#endif

/// TODO: inline
bool rp2350_bios_handler(CPU* cpu, uint8_t intnum) {
#if BIOS_DEBUG
    print_line2("BIOS", 0, 8);
#endif
    dpb_watch_native_checkpoint(cpu, "entry", intnum);
    bool res = handlers[intnum](cpu);
    dpb_watch_native_checkpoint(cpu, "exit", intnum);
    return res;
}

/*
 * Отложенный single-step трап (TF).
 *
 * Семантика реального x86: если TF=1 в НАЧАЛЕ инструкции, после её
 * завершения возбуждается INT 1. Инструкция, устанавливающая TF (POPF/
 * IRET), даёт трап только ПОСЛЕ СЛЕДУЮЩЕЙ инструкции - отсюда двухфазная
 * схема pending_ss_trap ("после текущей") <- tf ("после следующей").
 *
 * Программный INT n / INT3 / INTO сбрасывает TF в составе своего
 * исполнения и ПОДАВЛЯЕТ отложенный трап: на реальном CPU трассировщик
 * "перешагивает" INT n целиком и продолжает уже после IRET обработчика.
 * (Интерраптные трассировщики типа Norton поэтому НЕ исполняют INT n,
 * а эмулируют его вручную: pushf + far jmp по вектору.)
 *
 * Состояние выведено на уровень файла: bios_intcall() исполняет гостевой
 * обработчик ВЛОЖЕННЫМ pc_step()-циклом, и висящий pending-трап внешнего
 * потока не должен сработать внутри вложенного (и наоборот) -
 * см. cpu_pending_trap()/cpu_pending_trap_set().
 */
static bool pending_ss_trap;

bool cpu_pending_trap(void)        { return pending_ss_trap; }
void cpu_pending_trap_set(bool v)  { pending_ss_trap = v; }

static void IRAM_ATTR i286_step(CPU* cpu, int execloops) {
    static uint16_t firstip;

    for (uint32_t loopcount = 0; loopcount < execloops; loopcount++) {
        if (cpu->native_done) break;
        if ((cpu->flags.value & IF) && cpu->intr) {
            cpu->intr = false;
            int no = cpu->cb.pic_read_irq(cpu->cb.pic);
            intcall86(cpu, no);
        }
        if (!cpu->bios) {
            u32 ip32 = (((u32)CPU_CS << 4) + CPU_IP);
            if ((ip32 >> 8) == 0xFFE) {
            //    printf("fake BIOS trap phys=%05lx int=%02x CS:IP=%04x:%04x\n",
            //           (unsigned long)ip32, (uint8_t)ip32, CPU_CS, CPU_IP);
                if (rp2350_bios_handler(cpu, (uint8_t)ip32)) { // normal flow IRET is expected
                    SET_IP ( 0x0006 );
                    CPU_CS = 0xFFF0; // reusable IRET (pc.c)
                }
                else {// internal using INT in JMP style (INT 19h...)
                    continue; // to allow to recheck IRQ before next step
                }
            }
        }
        reptype = 0;
        segoverride = 0;
        useseg = CPU_DS;
        uint8_t docontinue = 0;
        firstip = CPU_IP;
        register uint8_t opcode;

        while (!docontinue) {
            ///         CPU_CS &= 0xFFFF;
            ///         CPU_IP &= 0xFFFF;
            //            savecs = CPU_CS;
            //            saveip = ip;
            // W/A-hack: last byte of interrupts table (actually should not be ever used as CS:IP)
        //    if (unlikely(CPU_CS == XMS_FN_CS && ip == XMS_FN_IP)) {
        //        // hook for XMS
        //        opcode = xms_handler(); // always returns RET TODO: far/short ret?
        //    } else {
                opcode = getmem8(CPU_CS, CPU_IP);
        //    }

            StepIP(1);

            switch (opcode) {
                /* segment prefix check */
                case 0x2E: /* segment CPU_CS */
                    useseg = CPU_CS;
                    segoverride = 1;
                    break;

                case 0x3E: /* segment CPU_DS */
                    useseg = CPU_DS;
                    segoverride = 1;
                    break;

                case 0x26: /* segment CPU_ES */
                    useseg = CPU_ES;
                    segoverride = 1;
                    break;

                case 0x36: /* segment CPU_SS */
                    useseg = CPU_SS;
                    segoverride = 1;
                    break;

                case 0x64: /* segment CPU_FS */
                    useseg = CPU_FS;
                    segoverride = 1;
                    break;

                case 0x65: /* segment CPU_GS */
                    useseg = CPU_GS;
                    segoverride = 1;
                    break;

                case 0xF0: /* LOCK (блокировка шины, для атомарных операций) */
                    /// TODO:
                    break;

                case 0xF2: /* REPNE/REPNZ */
                    reptype = 2;
                    break;

                /* repetition prefix check */
                case 0xF3: /* REP/REPE/REPZ */
                    reptype = 1;
                    break;

                default:
                    docontinue = 1;
                    break;
            }
        }

        register uint32_t res32;
        register uint8_t res8;
        register uint8_t oper1b;
        register uint8_t oper2b;
        switch (opcode) {
            case 0x0: /* 00 ADD Eb Gb */
                modregrm(cpu);
                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_add8();
                writerm8(cpu, rm, res8);
                break;

            case 0x1: /* 01 ADD Ev Gv */
                modregrm(cpu);
                if (operandSizeOverride) {
                    register uint32_t oper1 = readrm32(cpu, rm);
                    register uint32_t oper2 = getreg32(reg);
                    op_add32();
                    writerm32(cpu, rm, res32);
                } else {
                    register uint32_t oper1 = readrm16(cpu, rm);
                    register uint32_t oper2 = getreg16(reg);
                    op_add16();
                    writerm16(cpu, rm, res16);
                }
                break;

            case 0x2: /* 02 ADD Gb Eb */
                modregrm(cpu);
                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_add8();
                putreg8(reg, res8);
                break;

            case 0x3: {
                /* 03 ADD Gv Ev */
                modregrm(cpu);
                register uint32_t oper1 = getreg16(reg);
                register uint32_t oper2 = readrm16(cpu, rm);
                op_add16();
                putreg16(reg, res16);
                break;
            }
            case 0x4: /* 04 ADD CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_add8();
                CPU_AL = res8;
                break;

            case 0x5: {
                /* 05 ADD eAX Iv */
                register uint32_t oper1 = CPU_AX;
                register uint32_t oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_add16();
                CPU_AX = res16;
                break;
            }
            case 0x6: /* 06 PUSH CPU_ES */
                push(cpu, CPU_ES);
                break;

            case 0x7: /* 07 POP CPU_ES */
                CPU_ES = pop(cpu);
                break;

            case 0x8: /* 08 OR Eb Gb */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_or8();
                writerm8(cpu, rm, res8
                );
                break;

            case 0x9: /* 09 OR Ev Gv */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                oper2 = getreg16(reg);
                op_or16();
                writerm16(cpu, rm, res16
                );
                break;

            case 0xA: /* 0A OR Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_or8();
                putreg8(reg, res8
                );
                break;

            case 0xB: /* 0B OR Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                op_or16();
                /*                if ((oper1 == 0xF802) && (oper2 == 0xF802)) {
                                    sf = 0;    *//* cheap hack to make Wolf 3D think we're a 286 so it plays */ /*
                }*/

                putreg16(reg, res16);
                break;

            case 0xC: /* 0C OR CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_or8();
                CPU_AL = res8;
                break;

            case 0xD: /* 0D OR eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_or16();
                CPU_AX = res16;
                break;

            case 0xE: /* 0E PUSH CPU_CS */
                push(cpu, CPU_CS);
                break;

#ifdef CPU_8086 //only the 8086/8088 does this.
            case 0xF: //0F POP CS
                CPU_CS = pop(cpu);
                break;
#else
            case 0x0F: {
                /*
                 * The 286 core is real-mode-only.  In native BIOS/FDOS mode we
                 * can nevertheless decode the 0F 01 system group far enough to
                 * fail cleanly when a guest tries to enter protected mode.
                 *
                 * Do not change guest-BIOS behaviour: request_terminate() is a
                 * native FDOS process-lifecycle hook and cannot safely unwind
                 * an arbitrary guest DOS.
                 */
                if (cpu->bios || getmem8(CPU_CS, CPU_IP) != 0x01)
                    break;

                StepIP(1);       /* consume 01 */
                modregrm(cpu);   /* consume ModR/M + displacement */

                switch (reg) {
                case 0: /* SGDT m16&24 */
                    if (mode < 3) {
                        getea(cpu, rm);
                        writew86(ea + 0, 0xFFFF); /* real-mode placeholder */
                        writew86(ea + 2, 0x0000);
                        writew86(ea + 4, 0x0000);
                    }
                    break;

                case 1: /* SIDT m16&24 */
                    if (mode < 3) {
                        getea(cpu, rm);
                        writew86(ea + 0, 0x03FF); /* real-mode IVT */
                        writew86(ea + 2, 0x0000);
                        writew86(ea + 4, 0x0000);
                    }
                    break;

                case 2: /* LGDT m16&24 */
                case 3: /* LIDT m16&24 */
                    /*
                     * Consume the operand but do not install hidden PM state.
                     * Native FDOS remains in real mode; a following LMSW PE=1
                     * is the point where execution must be stopped.
                     */
                    break;

                case 4: /* SMSW r/m16 */
                    writerm16(cpu, rm, 0x0000); /* PE=0, real mode */
                    break;

                case 6: { /* LMSW r/m16 */
                    uint16_t msw = readrm16(cpu, rm);
                    if (msw & 0x0001u) {
                        emulator_unsupported_cpu_feature(
                            "LMSW: Protected Mode is not implemented",
                            CPU_CS, firstip);

                        /*
                         * CheckIt 3.0 disables the 8042 keyboard interface
                         * immediately before entering protected mode (OUT 64h,
                         * ADh) and reenables it only on its normal PM-exit path
                         * (OUT 64h, AEh).  Forced termination at LMSW skips
                         * that cleanup, leaving IRQ1 suppressed by i8042.
                         *
                         * Restore only that controller gate here.  Do not
                         * fabricate a key, touch the keyboard scan-enable
                         * state, or alter AUX state.
                         */
                        cpu->cb.io_write8(cpu->cb.io, 0x64, 0xAE);

                        request_terminate(0xFF, 0);
                    }
                    break;
                }

                default:
                    break;
                }
                break;
            }
#endif

            case 0x10: /* 10 ADC Eb Gb */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_adc8();
                writerm8(cpu, rm, res8);
                break;

            case 0x11: /* 11 ADC Ev Gv */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                oper2 = getreg16(reg);
                op_adc16();
                writerm16(cpu, rm, res16);
                break;

            case 0x12: /* 12 ADC Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_adc8();
                putreg8(reg, res8);
                break;

            case 0x13: /* 13 ADC Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                op_adc16();
                putreg16(reg, res16
                );
                break;

            case 0x14: /* 14 ADC CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_adc8();
                CPU_AL = res8;
                break;

            case 0x15: /* 15 ADC eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_adc16();
                CPU_AX = res16;
                break;

            case 0x16: /* 16 PUSH CPU_SS */
                push(cpu, CPU_SS);
                break;

            case 0x17: /* 17 POP CPU_SS */
                CPU_SS = pop(cpu);
                break;

            case 0x18: /* 18 SBB Eb Gb */
                modregrm(cpu);
                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_sbb8(cpu);
                writerm8(cpu, rm, res8);
                break;

            case 0x19: /* 19 SBB Ev Gv */
                modregrm(cpu);
                oper1 = readrm16(cpu, rm);
                oper2 = getreg16(reg);
                op_sbb16(cpu);
                writerm16(cpu, rm, res16);
                break;

            case 0x1A: /* 1A SBB Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_sbb8(cpu);
                putreg8(reg, res8
                );
                break;

            case 0x1B: /* 1B SBB Gv Ev */
                modregrm(cpu);
                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                op_sbb16(cpu);
                putreg16(reg, res16);
                break;

            case 0x1C: /* 1C SBB CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_sbb8(cpu);
                CPU_AL = res8;
                break;

            case 0x1D: /* 1D SBB eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_sbb16(cpu);
                CPU_AX = res16;
                break;

            case 0x1E: /* 1E PUSH CPU_DS */
                push(cpu, CPU_DS);
                break;

            case 0x1F: /* 1F POP CPU_DS */
                CPU_DS = pop(cpu);
                break;

            case 0x20: /* 20 AND Eb Gb */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_and8();
                writerm8(cpu, rm, res8);
                break;

            case 0x21: /* 21 AND Ev Gv */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                oper2 = getreg16(reg);
                op_and16();
                writerm16(cpu, rm, res16
                );
                break;

            case 0x22: /* 22 AND Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_and8();
                putreg8(reg, res8
                );
                break;

            case 0x23: /* 23 AND Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                op_and16();
                putreg16(reg, res16
                );
                break;

            case 0x24: /* 24 AND CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_and8();
                CPU_AL = res8;
                break;

            case 0x25: /* 25 AND eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_and16();
                CPU_AX = res16;
                break;

            case 0x27: /* 27 DAA */
            {
                uint8_t old_al;
                old_al = CPU_AL;
                if (((CPU_AL & 0x0F) > 9) || af) {
                    oper1 = (uint16_t) CPU_AL + 0x06;
                    CPU_AL = oper1 & 0xFF;
                    if (oper1 & 0xFF00)
                        cf = 1;
                    if ((oper1 & 0x000F) < (old_al & 0x0F))
                        af = 1;
                }
                if (((CPU_AL & 0xF0) > 0x90) || cf) {
                    oper1 = (uint16_t) CPU_AL + 0x60;
                    CPU_AL = oper1 & 0xFF;
                    if (oper1 & 0xFF00)
                        cf = 1;
                    else
                        cf = 0;
                }
                flag_szp8(cpu, CPU_AL);
                break;
            }

            case 0x28: /* 28 SUB Eb Gb */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_sub8();
                writerm8(cpu, rm, res8
                );
                break;

            case 0x29: {
                /* 29 SUB Ev Gv */
                modregrm(cpu);
                register uint32_t oper1 = readrm16(cpu, rm);
                register uint32_t oper2 = getreg16(reg);
                register uint32_t dst = oper1 - oper2;
                flag_szp16(cpu, (uint16_t) dst);
                cf = (dst & 0xFFFF0000) != 0;
                of = ((dst ^ oper1) & (oper1 ^ oper2) & 0x8000) != 0;
                af = ((oper1 ^ oper2 ^ dst) & 0x10) != 0;
                writerm16(cpu, rm, (uint16_t) dst);
                break;
            }
            case 0x2A: /* 2A SUB Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_sub8();
                putreg8(reg, res8
                );
                break;

            case 0x2B: /* 2B SUB Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                op_sub16();
                putreg16(reg, res16
                );
                break;

            case 0x2C: /* 2C SUB CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_sub8();
                CPU_AL = res8;
                break;

            case 0x2D: /* 2D SUB eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_sub16();
                CPU_AX = res16;
                break;

            case 0x2F: /* 2F DAS */
            {
                uint8_t old_al;
                old_al = CPU_AL;
                if (((CPU_AL & 0x0F) > 9) || af) {
                    oper1 = (uint16_t) CPU_AL - 0x06;
                    CPU_AL = oper1 & 0xFF;
                    if (oper1 & 0xFF00)
                        cf = 1;
                    if ((oper1 & 0x000F) >= (old_al & 0x0F))
                        af = 1;
                }
                if (((CPU_AL & 0xF0) > 0x90) || cf) {
                    oper1 = (uint16_t) CPU_AL - 0x60;
                    CPU_AL = oper1 & 0xFF;
                    if (oper1 & 0xFF00)
                        cf = 1;
                    else
                        cf = 0;
                }
                flag_szp8(cpu, CPU_AL);
                break;
            }

            case 0x30: /* 30 XOR Eb Gb */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                op_xor8();
                writerm8(cpu, rm, res8
                );
                break;

            case 0x31: /* 31 XOR Ev Gv */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                oper2 = getreg16(reg);
                op_xor16();
                writerm16(cpu, rm, res16
                );
                break;

            case 0x32: /* 32 XOR Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                op_xor8();
                putreg8(reg, res8
                );
                break;

            case 0x33: /* 33 XOR Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                op_xor16();
                putreg16(reg, res16
                );
                break;

            case 0x34: /* 34 XOR CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                op_xor8();
                CPU_AL = res8;
                break;

            case 0x35: /* 35 XOR eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                op_xor16();
                CPU_AX = res16;
                break;

            case 0x37: /* 37 AAA ASCII */
                if (((CPU_AL & 0xF) > 9) || (af == 1)) {
                    CPU_AX = CPU_AX + 0x106;
                    cpu->flags.value |= FLAG_CF_AF_MASK;
                } else {
                    cpu->flags.value &= ~FLAG_CF_AF_MASK;
                }

                CPU_AL = CPU_AL & 0xF;
                break;

            case 0x38: /* 38 CMP Eb Gb */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getreg8(reg);
                flag_sub8(cpu, oper1b, oper2b
                );
                break;

            case 0x39: /* 39 CMP Ev Gv */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                oper2 = getreg16(reg);
                flag_sub16(cpu, oper1, oper2
                );
                break;

            case 0x3A: /* 3A CMP Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                flag_sub8(cpu, oper1b, oper2b
                );
                break;

            case 0x3B: /* 3B CMP Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                flag_sub16(cpu, oper1, oper2
                );
                break;

            case 0x3C: /* 3C CMP CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                flag_sub8(cpu, oper1b, oper2b
                );
                break;

            case 0x3D: /* 3D CMP eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                flag_sub16(cpu, oper1, oper2
                );
                break;

            case 0x3F: /* 3F AAS ASCII */
                if (((CPU_AL & 0xF) > 9) || (af == 1)) {
                    CPU_AX = CPU_AX - 6;
                    CPU_AH = CPU_AH - 1;
                    cpu->flags.value |= FLAG_CF_AF_MASK;
                } else {
                    cpu->flags.value &= ~FLAG_CF_AF_MASK;
                }

                CPU_AL = CPU_AL & 0xF;
                break;

            case 0x40: {
                /* 40 INC eAX */
                register uint32_t oper1 = CPU_AX;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_AX = (uint16_t) dst;
                break;
            }
            case 0x41: {
                /* 41 INC eCX */
                register uint32_t oper1 = CPU_CX;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_CX = (uint16_t) dst;
                break;
            }
            case 0x42: {
                /* 42 INC eDX */
                register uint32_t oper1 = CPU_DX;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_DX = (uint16_t) dst;
                break;
            }
            case 0x43: {
                /* 43 INC eBX */
                register uint32_t oper1 = CPU_BX;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_BX = (uint16_t) dst;
                break;
            }
            case 0x44: {
                /* 44 INC eSP */
                register uint32_t oper1 = CPU_SP;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_SP = (uint16_t) dst;
                break;
            }
            case 0x45: {
                /* 45 INC eBP */
                register uint32_t oper1 = CPU_BP;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_BP = (uint16_t) dst;
                break;
            }
            case 0x46: {
                /* 46 INC eSI */
                register uint32_t oper1 = CPU_SI;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_SI = (uint16_t) dst;
                break;
            }
            case 0x47: {
                /* 47 INC eDI */
                register uint32_t oper1 = CPU_DI;
                register uint32_t dst = oper1 + 1;
                flag_szp16(cpu, dst);
                of = (((dst ^ oper1) & (dst ^ 1) & 0x8000) != 0);
                af = (((oper1 ^ 1 ^ dst) & 0x10) != 0);
                CPU_DI = (uint16_t) dst;
                break;
            }
            case 0x48: /* 48 DEC eAX */
                oldcf = cf;
                oper1 = CPU_AX;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_AX = res16;
                break;

            case 0x49: /* 49 DEC eCX */
                oldcf = cf;
                oper1 = CPU_CX;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_CX = res16;
                break;

            case 0x4A: /* 4A DEC eDX */
                oldcf = cf;
                oper1 = CPU_DX;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_DX = res16;
                break;

            case 0x4B: /* 4B DEC eBX */
                oldcf = cf;
                oper1 = CPU_BX;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_BX = res16;
                break;

            case 0x4C: /* 4C DEC eSP */
                oldcf = cf;
                oper1 = CPU_SP;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_SP = res16;
                break;

            case 0x4D: /* 4D DEC eBP */
                oldcf = cf;
                oper1 = CPU_BP;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_BP = res16;
                break;

            case 0x4E: /* 4E DEC eSI */
                oldcf = cf;
                oper1 = CPU_SI;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_SI = res16;
                break;

            case 0x4F: /* 4F DEC eDI */
                oldcf = cf;
                oper1 = CPU_DI;
                oper2 = 1;
                op_sub16();
                cf = oldcf;
                CPU_DI = res16;
                break;

            case 0x50: /* 50 PUSH eAX */
                push(cpu, CPU_AX);
                break;

            case 0x51: /* 51 PUSH eCX */
                push(cpu, CPU_CX);
                break;

            case 0x52: /* 52 PUSH eDX */
                push(cpu, CPU_DX);
                break;

            case 0x53: /* 53 PUSH eBX */
                push(cpu, CPU_BX);
                break;

            case 0x54: /* 54 PUSH eSP */
#ifdef CPU_286_STYLE_PUSH_SP
                push(cpu, CPU_SP);
#else
                push(cpu, CPU_SP - 2);
#endif
                break;

            case 0x55: /* 55 PUSH eBP */
                push(cpu, CPU_BP);
                break;

            case 0x56: /* 56 PUSH eSI */
                push(cpu, CPU_SI);
                break;

            case 0x57: /* 57 PUSH eDI */
                push(cpu, CPU_DI);
                break;

            case 0x58: /* 58 POP eAX */
                CPU_AX = pop(cpu);
                break;

            case 0x59: /* 59 POP eCX */
                CPU_CX = pop(cpu);
                break;

            case 0x5A: /* 5A POP eDX */
                CPU_DX = pop(cpu);
                break;

            case 0x5B: /* 5B POP eBX */
                CPU_BX = pop(cpu);
                break;

            case 0x5C: /* 5C POP eSP */
                CPU_SP = pop(cpu);
                break;

            case 0x5D: /* 5D POP eBP */
                CPU_BP = pop(cpu);
                break;

            case 0x5E: /* 5E POP eSI */
                CPU_SI = pop(cpu);
                break;

            case 0x5F: /* 5F POP eDI */
                CPU_DI = pop(cpu);
                break;

#ifndef CPU_8086
            case 0x60: /* 60 PUSHA (80186+) */
                oldsp = CPU_SP;
                push(cpu, CPU_AX);
                push(cpu, CPU_CX);
                push(cpu, CPU_DX);
                push(cpu, CPU_BX);
                push(cpu, oldsp);
                push(cpu, CPU_BP);
                push(cpu, CPU_SI);
                push(cpu, CPU_DI);
                break;

            case 0x61: /* 61 POPA (80186+) */
                CPU_DI = pop(cpu);
                CPU_SI = pop(cpu);
                CPU_BP = pop(cpu);
                CPU_SP += 2;
                CPU_BX = pop(cpu);
                CPU_DX = pop(cpu);
                CPU_CX = pop(cpu);
                CPU_AX = pop(cpu);
                break;

            case 0x62: /* 62 BOUND Gv, Ev (80186+) */
                modregrm(cpu);

                getea(cpu, rm);
                if (
                    signext32(getreg16(reg)
                    ) <
                    signext32(getmem16(ea >> 4, ea & 15)
                    )) {
                    intcall86(cpu, 5); //bounds check exception
                } else {
                    ea += 2;
                    if (
                        signext32(getreg16(reg)
                        ) >
                        signext32(getmem16(ea >> 4, ea & 15)
                        )) {
                        intcall86(cpu, 5); //bounds check exception
                    }
                }
                break;
            case 0x68: /* 68 PUSH Iv (80186+) */
                push(cpu, getmem16(CPU_CS, CPU_IP)
                );
                StepIP(2);
                break;

            case 0x69: {
                /* 69 IMUL Gv Ev Iv (80186+) */
                modregrm(cpu);
                register int32_t temp1 = (int32_t)(int16_t)readrm16(cpu, rm);
                register int32_t temp2 = (int32_t)(int16_t)getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                temp1 *= temp2;
                putreg16(reg, (int16_t)temp1);
                if (temp1 != (int32_t)(int16_t)temp1) {
                    cpu->flags.value |= FLAG_CF_OF_MASK;
                } else {
                    cpu->flags.value &= ~FLAG_CF_OF_MASK;
                }
                break;
            }
            case 0x6A: /* 6A PUSH Ib (80186+) */
                push(cpu, (uint16_t) signext(getmem8(CPU_CS, CPU_IP)));
                StepIP(1);
                break;

            case 0x6B: {
                /* 6B IMUL Gv Eb Ib (80186+) */
                modregrm(cpu);
                register int32_t temp1 = (int32_t)(int16_t)readrm16(cpu, rm);
                register int32_t temp2 = (int32_t)(int16_t)signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                temp1 *= temp2;
				putreg16(reg, (int16_t)temp1);
                if (temp1 != (int32_t)(int16_t)temp1) {
                    cpu->flags.value |= FLAG_CF_OF_MASK;
                } else {
                    cpu->flags.value &= ~FLAG_CF_OF_MASK;
                }
                break;
            }
            case 0x6C: /* 6E INSB */
                if (reptype && (CPU_CX == 0)) {
                    break;
                }

                putmem8(CPU_ES, CPU_DI, cpu->cb.io_read8(cpu->cb.io, CPU_DX));
                if (df) {
                    CPU_SI = CPU_SI - 1;
                    CPU_DI = CPU_DI - 1;
                } else {
                    CPU_SI = CPU_SI + 1;
                    CPU_DI = CPU_DI + 1;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0x6D: /* 6F INSW */
                if (reptype && (CPU_CX == 0)) {
                    break;
                }

                putmem16(CPU_ES, CPU_DI, cpu->cb.io_read16(cpu->cb.io, CPU_DX));
                if (df) {
                    CPU_SI = CPU_SI - 2;
                    CPU_DI = CPU_DI - 2;
                } else {
                    CPU_SI = CPU_SI + 2;
                    CPU_DI = CPU_DI + 2;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0x6E: /* 6E OUTSB */
                if (reptype && (CPU_CX == 0)) {
                    break;
                }
                cpu->cb.io_write8(cpu->cb.io, CPU_DX, getmem8(useseg, CPU_SI));
                if (df) {
                    CPU_SI = CPU_SI - 1;
                    CPU_DI = CPU_DI - 1;
                } else {
                    CPU_SI = CPU_SI + 1;
                    CPU_DI = CPU_DI + 1;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0x6F: /* 6F OUTSW */
                if (reptype && (CPU_CX == 0)) {
                    break;
                }
                cpu->cb.io_write16(cpu->cb.io, CPU_DX, getmem16(useseg, CPU_SI));
                if (df) {
                    CPU_SI = CPU_SI - 2;
                    CPU_DI = CPU_DI - 2;
                } else {
                    CPU_SI = CPU_SI + 2;
                    CPU_DI = CPU_DI + 2;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;
#endif

            case 0x70: /* 70 JO Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (of) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x71: /* 71 JNO Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!of) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x72: /* 72 JB Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (cf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x73: /* 73 JNB Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!cf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x74: /* 74 JZ Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (zf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x75: /* 75 JNZ Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!zf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x76: /* 76 JBE Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (cf || zf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x77: /* 77 JA Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!cf && !zf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x78: /* 78 JS Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (sf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x79: /* 79 JNS Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!sf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x7A: /* 7A JPE Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (pf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x7B: /* 7B JPO Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!pf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x7C: /* 7C JL Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (sf != of) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x7D: /* 7D JGE Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (sf == of) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x7E: /* 7E JLE Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if ((sf != of) || zf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x7F: /* 7F JG Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!
                    zf && (sf
                           == of)) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0x80:
            case 0x82: /* 80/82 GRP1 Eb Ib */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                switch (reg) {
                    case 0:
                        op_add8();
                        break;
                    case 1:
                        op_or8();
                        break;
                    case 2:
                        op_adc8();
                        break;
                    case 3:
                        op_sbb8(cpu);
                        break;
                    case 4:
                        op_and8();
                        break;
                    case 5:
                        op_sub8();
                        break;
                    case 6:
                        op_xor8();
                        break;
                    case 7:
                        flag_sub8(cpu, oper1b, oper2b
                        );
                        break;
                    default:
                        break; /* to avoid compiler warnings */
                }

                if (reg < 7) {
                    writerm8(cpu, rm, res8
                    );
                }
                break;

            case 0x81: /* 81 GRP1 Ev Iv */
            case 0x83: /* 83 GRP1 Ev Ib */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                if (opcode == 0x81) {
                    oper2 = getmem16(CPU_CS, CPU_IP);
                    StepIP(2);
                } else {
                    oper2 = signext(getmem8(CPU_CS, CPU_IP));
                    StepIP(1);
                }

                switch (reg) {
                    case 0:
                        op_add16();
                        break;
                    case 1:
                        op_or16();
                        break;
                    case 2:
                        op_adc16();
                        break;
                    case 3:
                        op_sbb16(cpu);
                        break;
                    case 4:
                        op_and16();
                        break;
                    case 5:
                        op_sub16();
                        break;
                    case 6:
                        op_xor16();
                        break;
                    case 7:
                        flag_sub16(cpu, oper1, oper2
                        );
                        break;
                    default:
                        break; /* to avoid compiler warnings */
                }

                if (reg < 7) {
                    writerm16(cpu, rm, res16
                    );
                }
                break;

            case 0x84: /* 84 TEST Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                oper2b = readrm8(cpu, rm);
                flag_log8(cpu, oper1b
                          & oper2b);
                break;

            case 0x85: /* 85 TEST Gv Ev */
                modregrm(cpu);

                oper1 = getreg16(reg);
                oper2 = readrm16(cpu, rm);
                flag_log16(cpu, oper1
                           & oper2);
                break;

            case 0x86: /* 86 XCHG Gb Eb */
                modregrm(cpu);

                oper1b = getreg8(reg);
                putreg8(reg, readrm8(cpu, rm)
                );
                writerm8(cpu, rm, oper1b
                );
                break;

            case 0x87: /* 87 XCHG Gv Ev */
                modregrm(cpu);
                oper1 = getreg16(reg);
                putreg16(reg, readrm16(cpu, rm));
                writerm16(cpu, rm, oper1);
                break;

            case 0x88: /* 88 MOV Eb Gb */
                modregrm(cpu);

                writerm8(cpu, rm, getreg8(reg)
                );
                break;

            case 0x89: /* 89 MOV Ev Gv */
                modregrm(cpu);

                writerm16(cpu, rm, getreg16(reg)
                );
                break;

            case 0x8A: /* 8A MOV Gb Eb */
                modregrm(cpu);

                putreg8(reg, readrm8(cpu, rm)
                );
                break;

            case 0x8B: /* 8B MOV Gv Ev */
                modregrm(cpu);
                putreg16(reg, readrm16(cpu, rm));
                break;

            case 0x8C: /* 8C MOV Ew Sw */
                modregrm(cpu);
                writerm16(cpu, rm, getsegreg(reg));
                break;

            case 0x8D: /* 8D LEA Gv M */
                modregrm(cpu);

                getea(cpu, rm);
                putreg16(reg, ea
                         -
                         segbase(useseg)
                );
                break;

            case 0x8E: /* 8E MOV Sw Ew */
                modregrm(cpu);
                putsegreg(reg, readrm16(cpu, rm));
                break;

            case 0x8F: /* 8F POP Ev */
                modregrm(cpu);
                writerm16(cpu, rm, pop(cpu));
                break;

            case 0x90: /* 90 NOP */
                break;

            case 0x91: /* 91 XCHG eCX eAX */
                oper1 = CPU_CX;
                CPU_CX = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x92: /* 92 XCHG eDX eAX */
                oper1 = CPU_DX;
                CPU_DX = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x93: /* 93 XCHG eBX eAX */
                oper1 = CPU_BX;
                CPU_BX = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x94: /* 94 XCHG eSP eAX */
                oper1 = CPU_SP;
                CPU_SP = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x95: /* 95 XCHG eBP eAX */
                oper1 = CPU_BP;
                CPU_BP = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x96: /* 96 XCHG eSI eAX */
                oper1 = CPU_SI;
                CPU_SI = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x97: /* 97 XCHG eDI eAX */
                oper1 = CPU_DI;
                CPU_DI = CPU_AX;
                CPU_AX = oper1;
                break;

            case 0x98: /* 98 CBW */
                if ((CPU_AL & 0x80) == 0x80) {
                    CPU_AH = 0xFF;
                } else {
                    CPU_AH = 0;
                }
                break;

            case 0x99: /* 99 CWD */
                if ((CPU_AH & 0x80) == 0x80) {
                    CPU_DX = 0xFFFF;
                } else {
                    CPU_DX = 0;
                }
                break;

            case 0x9A: /* 9A CALL Ap */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                push(cpu, CPU_CS);
                push(cpu, CPU_IP);
                SET_IP ( oper1 );
                CPU_CS = oper2;
                break;

            case 0x9B: /* 9B WAIT */
                /// TODO:
                break;

            case 0x9C: /* 9C PUSHF */
                push(cpu, makeflagsword(cpu));
                break;

            case 0x9D: /* 9D POPF */
                decodeflagsword(cpu, pop(cpu) & cpu->flags_mask);
                break;

            case 0x9E: /* 9E SAHF */
                decodeflagsword(cpu, (makeflagsword(cpu) & 0xFF00) | CPU_AH);
                break;

            case 0x9F: /* 9F LAHF */
                CPU_AH = makeflagsword(cpu) & 0xFF;
                break;

            case 0xA0: /* A0 MOV CPU_AL Ob */
                CPU_AL = getmem8(useseg, getmem16(CPU_CS, CPU_IP));
                StepIP(2);
                break;

            case 0xA1: /* A1 MOV eAX Ov */
                oper1 = getmem16(useseg, getmem16(CPU_CS, CPU_IP));
                StepIP(2);
                CPU_AX = oper1;
                break;

            case 0xA2: /* A2 MOV Ob CPU_AL */
                putmem8(useseg, getmem16(CPU_CS, CPU_IP), CPU_AL);
                StepIP(2);
                break;

            case 0xA3: /* A3 MOV Ov eAX */
                putmem16(useseg, getmem16(CPU_CS, CPU_IP), CPU_AX);
                StepIP(2);
                break;

            case 0xA4: /* A4 MOVSB */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                putmem8(CPU_ES, CPU_DI, getmem8(useseg, CPU_SI)
                );
                if (df) {
                    CPU_SI = CPU_SI - 1;
                    CPU_DI = CPU_DI - 1;
                } else {
                    CPU_SI = CPU_SI + 1;
                    CPU_DI = CPU_DI + 1;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xA5: /* A5 MOVSW */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                putmem16(CPU_ES, CPU_DI, getmem16(useseg, CPU_SI)
                );
                if (df) {
                    CPU_SI = CPU_SI - 2;
                    CPU_DI = CPU_DI - 2;
                } else {
                    CPU_SI = CPU_SI + 2;
                    CPU_DI = CPU_DI + 2;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xA6: /* A6 CMPSB */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                oper1b = getmem8(useseg, CPU_SI);
                oper2b = getmem8(CPU_ES, CPU_DI);
                if (df) {
                    CPU_SI = CPU_SI - 1;
                    CPU_DI = CPU_DI - 1;
                } else {
                    CPU_SI = CPU_SI + 1;
                    CPU_DI = CPU_DI + 1;
                }

                flag_sub8(cpu, oper1b, oper2b
                );
                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                if ((reptype == 1) && !zf) {
                    break;
                } else if ((reptype == 2) && (zf == 1)) {
                    break;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xA7: /* A7 CMPSW */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                oper1 = getmem16(useseg, CPU_SI);
                oper2 = getmem16(CPU_ES, CPU_DI);
                if (df) {
                    CPU_SI = CPU_SI - 2;
                    CPU_DI = CPU_DI - 2;
                } else {
                    CPU_SI = CPU_SI + 2;
                    CPU_DI = CPU_DI + 2;
                }

                flag_sub16(cpu, oper1, oper2
                );
                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                if ((reptype == 1) && !zf) {
                    break;
                }

                if ((reptype == 2) && (zf == 1)) {
                    break;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xA8: /* A8 TEST CPU_AL Ib */
                oper1b = CPU_AL;
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                flag_log8(cpu, oper1b
                          & oper2b);
                break;

            case 0xA9: /* A9 TEST eAX Iv */
                oper1 = CPU_AX;
                oper2 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                flag_log16(cpu, oper1
                           & oper2);
                break;

            case 0xAA: /* AA STOSB */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                putmem8(CPU_ES, CPU_DI, CPU_AL
                );
                if (df) {
                    CPU_DI = CPU_DI - 1;
                } else {
                    CPU_DI = CPU_DI + 1;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xAB: /* AB STOSW */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                putmem16(CPU_ES, CPU_DI, CPU_AX
                );
                if (df) {
                    CPU_DI = CPU_DI - 2;
                } else {
                    CPU_DI = CPU_DI + 2;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xAC: /* AC LODSB */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                CPU_AL = getmem8(useseg, CPU_SI);
                if (df) {
                    CPU_SI = CPU_SI - 1;
                } else {
                    CPU_SI = CPU_SI + 1;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xAD: /* AD LODSW */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                oper1 = getmem16(useseg, CPU_SI);
                CPU_AX = oper1;
                if (df) {
                    CPU_SI = CPU_SI - 2;
                } else {
                    CPU_SI = CPU_SI + 2;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xAE: /* AE SCASB */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                oper1b = CPU_AL;
                oper2b = getmem8(CPU_ES, CPU_DI);
                flag_sub8(cpu, oper1b, oper2b
                );
                if (df) {
                    CPU_DI = CPU_DI - 1;
                } else {
                    CPU_DI = CPU_DI + 1;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                if ((reptype == 1) && !zf) {
                    break;
                } else if ((reptype == 2) && (zf == 1)) {
                    break;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xAF: /* AF SCASW */
                if (
                    reptype && (CPU_CX
                                == 0)) {
                    break;
                }

                oper1 = CPU_AX;
                oper2 = getmem16(CPU_ES, CPU_DI);
                flag_sub16(cpu, oper1, oper2
                );
                if (df) {
                    CPU_DI = CPU_DI - 2;
                } else {
                    CPU_DI = CPU_DI + 2;
                }

                if (reptype) {
                    CPU_CX = CPU_CX - 1;
                }

                if ((reptype == 1) && !zf) {
                    break;
                } else if ((reptype == 2) && (zf == 1)) {
                    //did i fix a typo bug? this used to be & instead of &&
                    break;
                }

                loopcount++;
                if (!reptype) {
                    break;
                }

                SET_IP ( firstip );
                break;

            case 0xB0: /* B0 MOV CPU_AL Ib */
                CPU_AL = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB1: /* B1 MOV CPU_CL Ib */
                CPU_CL = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB2: /* B2 MOV CPU_DL Ib */
                CPU_DL = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB3: /* B3 MOV CPU_BL Ib */
                CPU_BL = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB4: /* B4 MOV CPU_AH Ib */
                CPU_AH = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB5: /* B5 MOV CPU_CH Ib */
                CPU_CH = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB6: /* B6 MOV CPU_DH Ib */
                CPU_DH = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB7: /* B7 MOV CPU_BH Ib */
                CPU_BH = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                break;

            case 0xB8: /* B8 MOV eAX Iv */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                CPU_AX = oper1;
                break;

            case 0xB9: /* B9 MOV eCX Iv */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                CPU_CX = oper1;
                break;

            case 0xBA: /* BA MOV eDX Iv */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                CPU_DX = oper1;
                break;

            case 0xBB: /* BB MOV eBX Iv */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                CPU_BX = oper1;
                break;

            case 0xBC: /* BC MOV eSP Iv */
                CPU_SP = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                break;

            case 0xBD: /* BD MOV eBP Iv */
                CPU_BP = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                break;

            case 0xBE: /* BE MOV eSI Iv */
                CPU_SI = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                break;

            case 0xBF: /* BF MOV eDI Iv */
                CPU_DI = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                break;

            case 0xC0: /* C0 GRP2 byte imm8 (80186+) */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                oper2b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                writerm8(cpu, rm, op_grp2_8(cpu, oper2b, oper1b));
                break;

            case 0xC1: /* C1 GRP2 word imm8 (80186+) */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                oper2 = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                writerm16(cpu, rm, op_grp2_16(cpu, (uint8_t) oper2)
                );
                break;

            case 0xC2: /* C2 RET Iw */
                oper1 = getmem16(CPU_CS, CPU_IP);
                SET_IP ( pop(cpu) );
                CPU_SP = CPU_SP + oper1;
                break;

            case 0xC3: /* C3 RET */
                SET_IP ( pop(cpu) );
                break;

            case 0xC4: /* C4 LES Gv Mp */
                modregrm(cpu);

                getea(cpu, rm);
                putreg16(reg, read86(ea) + read86(ea + 1) * 256);
                CPU_ES = read86(ea + 2) + read86(ea + 3) * 256;
                break;

            case 0xC5: /* C5 LDS Gv Mp */
                modregrm(cpu);

                getea(cpu, rm);
                putreg16(reg, read86(ea) + read86(ea + 1) * 256);
                CPU_DS = read86(ea + 2) + read86(ea + 3) * 256;
                break;

            case 0xC6: /* C6 MOV Eb Ib */
                modregrm(cpu);

                writerm8(cpu, rm, getmem8(CPU_CS, CPU_IP)
                );
                StepIP(1);
                break;

            case 0xC7: /* C7 MOV Ev Iv */
                modregrm(cpu);

                writerm16(cpu, rm, getmem16(CPU_CS, CPU_IP)
                );
                StepIP(2);
                break;

            case 0xC8: /* C8 ENTER (80186+) */
                stacksize = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                nestlev = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                push(cpu, CPU_BP);
                frametemp = CPU_SP;
                if (nestlev) {
                    for (
                        temp16 = 1;
                        temp16 < nestlev;
                        ++temp16) {
                        CPU_BP = CPU_BP - 2;
                        push(cpu, CPU_BP);
                    }

                    push(cpu, frametemp); //CPU_SP);
                }

                CPU_BP = frametemp;
                CPU_SP = CPU_BP - stacksize;

                break;

            case 0xC9: /* C9 LEAVE (80186+) */
                CPU_SP = CPU_BP;
                CPU_BP = pop(cpu);
                break;

            case 0xCA: /* CA RETF Iw */
                oper1 = getmem16(CPU_CS, CPU_IP);
                SET_IP ( pop(cpu) );
                CPU_CS = pop(cpu);
                CPU_SP = CPU_SP + oper1;
                break;

            case 0xCB: /* CB RETF */
                SET_IP ( pop(cpu) );
                CPU_CS = pop(cpu);
                break;

            case 0xCC: /* CC INT 3 */
                intcall86(cpu, 3);
                pending_ss_trap = false; /* INT подавляет single-step трап */
                break;

            case 0xCD: /* CD INT Ib */
                oper1b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                intcall86(cpu, oper1b);
                pending_ss_trap = false; /* INT подавляет single-step трап */
                break;

            case 0xCE: /* CE INTO */
                if (of) {
                    intcall86(cpu, 4);
                    pending_ss_trap = false; /* см. выше */
                }
                break;

            case 0xCF: /* CF IRET */
                SET_IP ( pop(cpu) );
                CPU_CS = pop(cpu);
                decodeflagsword(cpu, pop(cpu) & cpu->flags_mask);

                /*
                 * if (net.enabled) net.canrecv = 1;
                 */
                break;

            case 0xD0: /* D0 GRP2 Eb 1 */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                writerm8(cpu, rm, op_grp2_8(cpu, 1, oper1b));
                break;

            case 0xD1: /* D1 GRP2 Ev 1 */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                writerm16(cpu, rm, op_grp2_16(cpu, 1));
                break;

            case 0xD2: /* D2 GRP2 Eb CPU_CL */
                modregrm(cpu);

                oper1b = readrm8(cpu, rm);
                writerm8(cpu, rm, op_grp2_8(cpu, CPU_CL, oper1b));
                break;

            case 0xD3: /* D3 GRP2 Ev CPU_CL */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                writerm16(cpu, rm, op_grp2_16(cpu, CPU_CL)
                );
                break;

            case 0xD4: /* D4 AAM I0 */
                oper1 = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                if (!oper1) {
                    intcall86(cpu, 0);
                    break;
                } /* division by zero */

                CPU_AH = (CPU_AL / oper1) & 255;
                CPU_AL = (CPU_AL % oper1) & 255;
                flag_szp16(cpu, CPU_AX);
                break;

            case 0xD5: /* D5 AAD I0 */
                oper1 = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                CPU_AL = (CPU_AH * oper1 + CPU_AL) & 255;
                CPU_AH = 0;
                flag_szp16(cpu, CPU_AH
                           * oper1 + CPU_AL);
                sf = 0;
                break;

            case 0xD6: /* D6 XLAT on V20/V30, SALC on 8086/8088 */
#ifndef CPU_NO_SALC
                CPU_AL = CPU_FL_CF ? 0xFF : 0x00;
                break;
#endif

            case 0xD7: /* D7 XLAT */
                CPU_AL = read86(useseg * 16 + (CPU_BX) + CPU_AL);
                break;

            case 0xD8:
            case 0xD9:
            case 0xDA:
            case 0xDB:
            case 0xDC:
            case 0xDE:
            case 0xDD:
            case 0xDF: /* escape to x87 FPU */
                OpFpu(cpu, opcode);
                break;

            case 0xE0: /* E0 LOOPNZ Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                CPU_CX = CPU_CX - 1;
                if ((CPU_CX) && !zf) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0xE1: /* E1 LOOPZ Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                CPU_CX = CPU_CX - 1;
                if (CPU_CX && (zf == 1)) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0xE2: /* E2 LOOP Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                CPU_CX = CPU_CX - 1;
                if (CPU_CX) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0xE3: /* E3 JCXZ Jb */
                temp16 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                if (!CPU_CX) {
                    SET_IP ( CPU_IP + temp16 );
                }
                break;

            case 0xE4: /* E4 IN CPU_AL Ib */
                oper1b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                CPU_AL = (uint8_t)cpu->cb.io_read8(cpu->cb.io, oper1b);
                break;

            case 0xE5: /* E5 IN eAX Ib */
                oper1b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                CPU_AX = cpu->cb.io_read16(cpu->cb.io, oper1b);
                break;

            case 0xE6: /* E6 OUT Ib CPU_AL */
                oper1b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                cpu->cb.io_write8(cpu->cb.io, oper1b, CPU_AL);
                break;

            case 0xE7: /* E7 OUT Ib eAX */
                oper1b = getmem8(CPU_CS, CPU_IP);
                StepIP(1);
                cpu->cb.io_write16(cpu->cb.io, oper1b, CPU_AX);
                break;

            case 0xE8: /* E8 CALL Jv */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                push(cpu, CPU_IP);
                SET_IP ( CPU_IP + oper1 );
                break;

            case 0xE9: /* E9 JMP Jv */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                SET_IP ( CPU_IP + oper1 );
                break;

            case 0xEA: /* EA JMP Ap */
                oper1 = getmem16(CPU_CS, CPU_IP);
                StepIP(2);
                oper2 = getmem16(CPU_CS, CPU_IP);
                SET_IP ( oper1 );
                CPU_CS = oper2;
                break;

            case 0xEB: /* EB JMP Jb */
                oper1 = signext(getmem8(CPU_CS, CPU_IP));
                StepIP(1);
                SET_IP ( CPU_IP + oper1 );
                break;

            case 0xEC: /* EC IN CPU_AL regdx */
                oper1 = CPU_DX;
                CPU_AL = (uint8_t)cpu->cb.io_read8(cpu->cb.io, oper1);
                break;

            case 0xED: /* ED IN eAX regdx */
                oper1 = CPU_DX;
                CPU_AX = cpu->cb.io_read16(cpu->cb.io, oper1);
                break;

            case 0xEE: /* EE OUT regdx CPU_AL */
                oper1 = CPU_DX;
                cpu->cb.io_write8(cpu->cb.io, oper1, CPU_AL);
                break;

            case 0xEF: /* EF OUT regdx eAX */
                oper1 = CPU_DX;
                cpu->cb.io_write16(cpu->cb.io, oper1, CPU_AX);
                break;

            case 0xF0: /* F0 LOCK */
                break;

            case 0xF4: /* F4 HLT */
                /// TODO:
                //hltstate = 1;
                break;

            case 0xF5: /* F5 CMC */
                if (!cf) {
                    cf = 1;
                } else {
                    cf = 0;
                }
                break;

            case 0xF6: /* F6 GRP3a Eb */
                modregrm(cpu);
                oper1b = readrm8(cpu, rm);
                oper1 = signext(oper1b);
                switch (reg) {
                    case 0:
                    case 1: /* TEST */
                        flag_log8(cpu, oper1b & getmem8(CPU_CS, CPU_IP));
                        StepIP(1);
                        break;

                    case 2: /* NOT */
                        res8 = ~oper1b;
                        break;

                    case 3: /* NEG */
                        res8 = (~oper1b) + 1;
                        flag_sub8(cpu, 0, oper1b);
                        if (res8 == 0) {
                            cf = 0;
                        } else {
                            cf = 1;
                        }
                        break;

                    case 4: {
                        /* MUL */
                        register uint32_t temp1 = (uint32_t) oper1b * (uint32_t) CPU_AL;
                        CPU_AX = temp1 & 0xFFFF;
                        flag_szp8(cpu, (uint8_t) temp1);
                        if (CPU_AH) {
                            cpu->flags.value |= FLAG_CF_OF_MASK;
                        } else {
                            cpu->flags.value &= ~FLAG_CF_OF_MASK;
                        }
#ifdef CPU_CLEAR_ZF_ON_MUL
                        zf = 0;
#endif
                        break;
                    }
                    case 5: {
                        /* IMUL */
                        oper1 = signext(oper1b);
                        register int32_t temp1 = (int32_t)(int8_t)signext(CPU_AL);
                        register int32_t temp2 = (int32_t)(int8_t)oper1;
						temp1 *= temp2;
						int16_t result = (int16_t)temp1;
						int8_t truncated = (int8_t)result;
						if (result != (int16_t)truncated) {
							cpu->flags.value |= FLAG_CF_OF_MASK; // CF=OF=1
						} else {
							cpu->flags.value &= ~FLAG_CF_OF_MASK; // CF=OF=0
						}
						CPU_AL = truncated;
						CPU_AH = (uint8_t)(result >> 8);
#ifdef CPU_CLEAR_ZF_ON_MUL
                        zf = 0;
#endif
                        break;
                    }
                    case 6: /* DIV */
                        op_div8(cpu, CPU_AX, oper1b);
                        break;

                    case 7: /* IDIV */
                        op_idiv8(cpu, CPU_AX, oper1b);
                        break;
                }

                if ((reg > 1) && (reg < 4)) {
                    writerm8(cpu, rm, res8
                    );
                }
                break;

            case 0xF7: /* F7 GRP3b Ev */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                op_grp3_16(cpu);
                if ((reg > 1) && (reg < 4)) {
                    writerm16(cpu, rm, res16
                    );
                }
                break;

            case 0xF8: /* F8 CLC */
                cf = 0;
                break;

            case 0xF9: /* F9 STC */
                cf = 1;
                break;

            case 0xFA: /* FA CLI */
                ifl = 0;
                break;

            case 0xFB: /* FB STI */
                ifl = 1;
                break;

            case 0xFC: /* FC CLD */
                df = 0;
                break;

            case 0xFD: /* FD STD */
                df = 1;
                break;

            case 0xFE: /* FE GRP4 Eb */
                modregrm(cpu);
                oper1b = readrm8(cpu, rm);
                oper2b = 1;
                if (!reg) {
                    tempcf = cf;
                    op_add8();
                    cf = tempcf;
                    writerm8(cpu, rm, res8);
                } else {
                    tempcf = cf;
                    res8 = oper1b - oper2b;
                    flag_sub8(cpu, oper1b, oper2b);
                    cf = tempcf;
                    writerm8(cpu, rm, res8);
                }
                break;

            case 0xFF: /* FF GRP5 Ev */
                modregrm(cpu);

                oper1 = readrm16(cpu, rm);
                op_grp5(cpu);
                break;

            default:
#ifdef CPU_ALLOW_ILLEGAL_OP_EXCEPTION
                intcall86(cpu, 6); /* trip invalid opcode exception. this occurs on the 80186+, 8086/8088 CPUs treat them as NOPs. */
                /* technically they aren't exactly like NOPs in most cases, but for our pursoses, that's accurate enough. */
                printf("[CPU] Invalid opcode 0x%02x exception at %04X:%04X\r\n", opcode, CPU_CS, firstip);
#endif
                break;
        }
        if (pending_ss_trap) {
            pending_ss_trap = false;
            intcall86(cpu, 1);
        }
        if (tf) {
            pending_ss_trap = true;
        }
    }
}

#ifndef I386_MODE
void __not_in_flash() cpu_save_regs(const CPU* cpu, CPU_regs* regs) {
	regs->es = CPU_ES;
	regs->ds = CPU_DS;
	regs->fs = CPU_FS;
	regs->gs = CPU_GS;
	for(int i = 0; i < 8; ++i)
        regs->gprx[i] = cpu->gprx[i];
    regs->flags = cpu->flags;
}
void __not_in_flash() cpu_restore_regs(CPU* cpu, const CPU_regs* regs) {
	CPU_ES = regs->es;
	CPU_DS = regs->ds;
	CPU_FS = regs->fs;
	CPU_GS = regs->gs;
	for(int i = 0; i < 8; ++i)
        cpu->gprx[i] = regs->gprx[i];
    cpu->flags = regs->flags;
}
void __not_in_flash() cpu_intcall(CPU* cpu, uint8_t intnum) {
    intcall86(cpu, intnum);
}
#endif