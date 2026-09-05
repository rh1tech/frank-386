#include "i386.h"
#include "audiodiag.h"
#include "remote_mem.h"
#include "codeprofile.h"
#include "bbprofile.h"
#include <pico.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#ifdef BUILD_ESP32
#include "esp_attr.h"
#else
#define IRAM_ATTR __not_in_flash()
#define IRAM_ATTR_CPU_EXEC1 __not_in_flash()
#endif

#define I386_OPT1
#ifndef __wasm__
#define I386_OPT2
#endif

#define I386_ENABLE_FPU 1

#if BLOCK_JIT
/* FRANK_BLOCK_JIT_V1: forward declarations for write/TLB invalidation. */
static inline void bj_note_write(CPUI386 *cpu, uword addr, unsigned len);
static void bj_flush(void);
#endif

#if NATIVE_JIT
/* FRANK_NATIVE_JIT_V3: native Thumb-2 hot-block cache. */
/* FRANK_NATIVE_JIT_V5_CONSOLIDATED: productive-loop first, low-tax dispatch. */
/* FRANK_NATIVE_JIT_V6_HYBRID_TRACE: general native-prefix trace JIT with interpreter fallback. */
/* FRANK_NATIVE_JIT_V8_4_1_BRANCHFIX: corrected fall-through state for terminal backward Jcc/LOOP; 2-way low-RAM cache retained. */
/* FRANK_NATIVE_JIT_V8_5_PAGED_BPSTACK: Symantec exact BP-stack loop safely enabled under EMM386 paging. */
/* FRANK_NATIVE_JIT_V8_5_1_IRAMFIX: keep paged BP-stack validation out of always-inline IRAM lookup expansion. */
/* FRANK_NATIVE_JIT_V8_6_PAGED_WALK: validate exact BP-stack mappings from current page tables, independent of direct-mapped TLB residency. */
/* FRANK_NATIVE_JIT_V8_7_3_V86_MICROLOOP: v8.7.2 plus VM86+paging-only page-bounded byte micro-loop. */
static inline void nj_note_write(CPUI386 *cpu, uword addr, unsigned len);
static void nj_flush(void);
#endif
#ifdef I386_ENABLE_FPU
#include "fpu.h"
#else
#define fpu_new(...) NULL
#define fpu_exec1(...) false
#define fpu_exec2(...) false
#define fpu_delete(...)
typedef void FPU;
#endif

/* Prefetch buffer: holds 4 bytes fetched as one 32-bit aligned read.
 * cpu->prefetch_base is the physical address of the aligned 4-byte slot currently
 * in the buffer (always a multiple of 4).  (u32)-1 means "invalid / empty".
 * Invalidated automatically when the physical address of next_ip falls outside
 * the current 4-byte slot */
//static u32 cpu->prefetch_base = (u32)-1;
//static u8  cpu->prefetch[16] __attribute__((aligned(4))) = {0};

// #define DEBUG_CPU 1
#ifdef DEBUG_CPU
#include <stdarg.h>
#include "ff.h"
static u8 opcode;
void dolog(const char *fmt, ...)
{
	static FIL _tf;
	static int _tf_open = 0;
    if (!_tf_open) _tf_open = (f_open(&_tf, "386/cpu.txt", FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK);
    if (!_tf_open) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    UINT bw;
    f_write(&_tf, buf, len, &bw);
    f_sync(&_tf);
}
#else
#define dolog(...) (void)0
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define wordmask ((uword) ((sword) -1))
#define TRY(f) if(!(f)) { return false; }
#define TRYL(f) if(unlikely(!(f))) { return false; }
#define TRY1(f) if(unlikely(!(f))) { dolog("TRY1 @ %s %d\n", __func__, __LINE__); cpu_abort(cpu, -1); }
#define THROW(ex, err) do { \
    dolog("THROW ex=%d err=%x eip=%08x cs=%04x %s:%d\n", \
          (ex), (unsigned)(err), cpu->ip, cpu->seg[SEG_CS].sel,  __func__, __LINE__); \
    frank_diag_exc(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)(ex), \
                   (uint32_t)(err), (uint32_t)cpu->flags); \
    cpu->excno = (ex); cpu->excerr = (err); \
	return false; \
} while(0)
#define THROW0(ex) do { \
    dolog("THROW0 ex=%d eip=%08x cs=%04x op=%02x %s:%d\n", (ex), cpu->ip, cpu->seg[SEG_CS].sel, \
          opcode, __func__, __LINE__); \
	frank_diag_exc(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)(ex), \
	               0u /* opcode is not in scope at every THROW0 site */, (uint32_t)cpu->flags); \
	cpu->excno = (ex); \
	return false; \
} while(0)

// the second branchless version works better on gcc
//#define SET_BIT(w, f, m) ((w) ^= ((-(uword)(f)) ^ (w)) & (m))
#define SET_BIT(w, f, m) ((w) = ((w) & ~((uword)(m))) | ((-(uword)(f)) & (m)))
//#define SET_BIT(w, f, m) do { if (f) (w) |= (m); else (w) &= ~(m); } while (0)

enum {
	EX_DE,
	EX_DB,
	EX_NMI,
	EX_BP,
	EX_OF,
	EX_BR,
	EX_UD,
	EX_NM,
	EX_DF,
	EX_INT9,
	EX_TS,
	EX_NP,
	EX_SS,
	EX_GP,
	EX_PF,
};

enum {
	CF = 0x1,
	/* 1 0x2 */
	PF = 0x4,
	/* 0 0x8 */
	AF = 0x10,
	/* 0 0x20 */
	ZF = 0x40,
	SF = 0x80,
	TF = 0x100,
	IF = 0x200,
	DF = 0x400,
	OF = 0x800,
	IOPL = 0x3000,
	NT = 0x4000,
	/* 0 0x8000 */
	RF = 0x10000,
	VM = 0x20000,
};

enum {
	SEG_ES = 0,
	SEG_CS,
	SEG_SS,
	SEG_DS,
	SEG_FS,
	SEG_GS,
	SEG_LDT,
	SEG_TR,
};

enum {
	SEG_D_BIT = 1 << 14,
	SEG_B_BIT = 1 << 14,
};

#ifdef I386_OPT1
#define REGi(x) (cpu->gprx[x].r32)
#else
#define REGi(x) (cpu->gpr[x])
#endif
#define SEGi(x) (cpu->seg[x].sel)

static void cpu_debug(CPUI386 *cpu);

void cpu_abort(CPUI386 *cpu, int code)
{
	dolog("abort: %d %x cycle %ld\n", code, code, cpu->cycle);
	cpu_debug(cpu);
	abort();
}

static inline uword sext8(u8 a)
{
	return (sword) (s8) a;
}

static inline uword sext16(u16 a)
{
	return (sword) (s16) a;
}

static inline uword sext32(u32 a)
{
	return (sword) (s32) a;
}

#ifdef I386_OPT1
/* only works on hosts that are little-endian and support unaligned access */
static inline u8 pload8_local(CPUI386 *cpu, uword addr)
{
	return cpu->phys_mem[addr];
}

static inline u16 pload16_local(CPUI386 *cpu, uword addr)
{
	return *(u16 *)&(cpu->phys_mem[addr]);
}

static inline u32 pload32_local(CPUI386 *cpu, uword addr)
{
	return *(u32 *)&(cpu->phys_mem[addr]);
}

static inline void pstore8_local(CPUI386 *cpu, uword addr, u8 val)
{
	cpu->phys_mem[addr] = val;
}

static inline void pstore16_local(CPUI386 *cpu, uword addr, u16 val)
{
	*(u16 *)&(cpu->phys_mem[addr]) = val;
}

static inline void pstore32_local(CPUI386 *cpu, uword addr, u32 val)
{
	*(u32 *)&(cpu->phys_mem[addr]) = val;
}
#else
static inline u8 pload8_local(CPUI386 *cpu, uword addr)
{
	return cpu->phys_mem[addr];
}

static inline u16 pload16_local(CPUI386 *cpu, uword addr)
{
	u8 *mem = (u8 *) cpu->phys_mem;
	return mem[addr] | (mem[addr + 1] << 8);
}

static inline u32 pload32_local(CPUI386 *cpu, uword addr)
{
	u8 *mem = (u8 *) cpu->phys_mem;
	return mem[addr] | (mem[addr + 1] << 8) |
		(mem[addr + 2] << 16) | (mem[addr + 3] << 24);
}

static inline void pstore8_local(CPUI386 *cpu, uword addr, u8 val)
{
	cpu->phys_mem[addr] = val;
}

static inline void pstore16_local(CPUI386 *cpu, uword addr, u16 val)
{
	cpu->phys_mem[addr] = val;
	cpu->phys_mem[addr + 1] = val >> 8;
}

static inline void pstore32_local(CPUI386 *cpu, uword addr, u32 val)
{
	cpu->phys_mem[addr] = val;
	cpu->phys_mem[addr + 1] = val >> 8;
	cpu->phys_mem[addr + 2] = val >> 16;
	cpu->phys_mem[addr + 3] = val >> 24;
}
#endif

/*
 * Remote window dispatch.
 *
 * A slice of guest physical memory can be served out of the slave
 * RP2350's SRAM, which is measured at 89 core cycles per access against
 * 182 for the master's own PSRAM — twice as fast. See remote_mem.h.
 *
 * The check sits here rather than in load8()/store8() on purpose. Those
 * already range-check every access, so hooking there would have cost
 * nothing — but page-table walks and instruction prefetch call pload32()
 * directly, and a window that quietly mishandled either would fail in
 * ways that look nothing like a memory fault.
 *
 * When REMOTE_MEM is not compiled in, is_remote() is a constant false
 * and every one of these folds back into the bare local access.
 */
static inline u8 pload8(CPUI386 *cpu, uword addr)
{
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) return remote_read8(addr);
#endif
	return pload8_local(cpu, addr);
}

static inline u16 pload16(CPUI386 *cpu, uword addr)
{
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) return remote_read16(addr);
#endif
	return pload16_local(cpu, addr);
}

static inline u32 pload32(CPUI386 *cpu, uword addr)
{
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) return remote_read32(addr);
#endif
	return pload32_local(cpu, addr);
}

/*
 * The system BIOS is ROM on a PC, and a store to it has to be dropped rather
 * than land in RAM.
 *
 * Prehistorik 2 looks for a debugger by reading the INT 3 vector, poking the
 * byte it points at, and folding the result into an opcode of its own:
 *
 *      lds  bx, [000c]        ; the INT 3 vector - F000:06F4 under SeaBIOS
 *      mov  al, [bx]
 *      xor  byte [bx], 55     ; a no-op on a real machine: that is ROM
 *      sub  al, [bx]          ; so al comes out zero
 *      add  cs:[62b8], al     ; and the opcode is left alone
 *
 * With the BIOS sitting in plain writable RAM the xor sticks instead, al comes
 * out 0x00 - 0x55 = 0xab, and the game's own `push ax` at 10BB:62B8 becomes
 * `sti`.  That routine then pops four registers against three pushes, returns
 * two bytes off, and runs away into the interrupt vector table until it hits
 * an invalid opcode.  The corruption happens once, while the level loads; the
 * game only dies later, when it first calls the routine - which is why this
 * looked for two sessions like an intermittent fault caused by moving or by
 * touching an enemy.
 *
 * Only the last 64 KB is protected.  A 256 KB BIOS image also occupies
 * 0xc0000-0xeffff, but so do the upper memory blocks EMM386 hands out and DOS
 * loads drivers into, and blocking writes there would break far more than it
 * fixes.  0xf0000-0xfffff is ROM on every PC and nothing legitimately writes
 * to it - the BIOS image itself is put there by load_rom(), which memcpy()s
 * into phys_mem and never comes through here.
 */
/* GUEST_ prefixed: the SDK already defines ROM_BASE for the RP2350. */
#define GUEST_ROM_BASE 0xf0000u
#define GUEST_ROM_SIZE 0x10000u

static inline bool __attribute__((always_inline)) in_rom(uword addr)
{
	return (addr - GUEST_ROM_BASE) < GUEST_ROM_SIZE;
}

/*
 * Blocking the region outright stops the machine booting, because SeaBIOS
 * keeps mutable globals in the F segment and writes them during POST - just
 * as a real chipset allows while the PAM registers hold the window open.
 * Every one of the nine writes a boot makes was measured, and they agree:
 *
 *   0xf7f28 <- 1          from cs_base 0, ip 0x0f365d
 *   0xf30c8 <- 0x800000   from cs_base 0, ip 0x0ef0cd
 *   0xf7304, 0xf7320..0xf7330                cs_base 0, ip 0x0e974a..0x0e9777
 *
 * All of them come from a flat 32-bit code segment with an EIP far above
 * 0xffff, which is only reachable in protected mode: this is POST, before the
 * BIOS would have closed PAM again.  Everything from DOS onwards runs in real
 * or V86 mode, where a real machine has ROM there and nothing can write to it.
 *
 * Emulating PAM properly would be the exact answer, but this emulator never
 * registers the i440FX host bridge at all (see the commented-out
 * pci_register_device() in i440fx_init), so the guest's PAM writes reach no
 * config space and there is nothing to read back.  The mode test costs one
 * compare on a path that is already off the hot road - only addresses inside
 * the 64 KB reach it - and separates the two cases exactly as observed.
 */
static inline bool __attribute__((always_inline)) rom_write_allowed(CPUI386 *cpu)
{
	return (cpu->cr0 & 1) && !(cpu->flags & VM);
}

/*
 * The writes that were dropped, so a regression is visible rather than
 * silent: 32 entries of {address, value, CS base, IP} at guest 0xb4800, in
 * the gap between the shadow copy and the port histograms, with the running
 * total just past the end of the ring.
 */
#define ROMLOG_RING ((volatile uint32_t *)(0x11000000u + 0x000b4800u))
#define ROMLOG_N    32u
static uint32_t romlog_head;

static inline void __attribute__((always_inline))
rom_write_log(CPUI386 *cpu, uword addr, uint32_t val)
{
	uint32_t n = romlog_head++;
	if (n < ROMLOG_N) {
		volatile uint32_t *e = ROMLOG_RING + n * 4u;
		e[0] = addr;
		e[1] = val;
		e[2] = cpu->seg[SEG_CS].base;
		e[3] = cpu->ip;
	}
	/* The count keeps going past the ring so the total is readable too. */
	ROMLOG_RING[ROMLOG_N * 4u] = romlog_head;
}

static inline void pstore8(CPUI386 *cpu, uword addr, u8 val)
{
	if (unlikely(in_rom(addr)) && !rom_write_allowed(cpu)) {
		rom_write_log(cpu, addr, val);
		return;
	}
	frank_diag_wp(addr, val, cpu->seg[SEG_CS].base, cpu->ip);
#if NATIVE_JIT
	nj_note_write(cpu, addr, 1);
#endif
#if BLOCK_JIT
	bj_note_write(cpu, addr, 1);
#endif
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) { remote_write8(addr, val); return; }
#endif
	pstore8_local(cpu, addr, val);
}

static inline void pstore16(CPUI386 *cpu, uword addr, u16 val)
{
	if (unlikely(in_rom(addr)) && !rom_write_allowed(cpu)) {
		rom_write_log(cpu, addr, val);
		return;
	}
	frank_diag_wp(addr, val, cpu->seg[SEG_CS].base, cpu->ip);
#if NATIVE_JIT
	nj_note_write(cpu, addr, 2);
#endif
#if BLOCK_JIT
	bj_note_write(cpu, addr, 2);
#endif
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) { remote_write16(addr, val); return; }
#endif
	pstore16_local(cpu, addr, val);
}

static inline void pstore32(CPUI386 *cpu, uword addr, u32 val)
{
	if (unlikely(in_rom(addr)) && !rom_write_allowed(cpu)) {
		rom_write_log(cpu, addr, val);
		return;
	}
	frank_diag_wp(addr, val, cpu->seg[SEG_CS].base, cpu->ip);
#if NATIVE_JIT
	nj_note_write(cpu, addr, 4);
#endif
#if BLOCK_JIT
	bj_note_write(cpu, addr, 4);
#endif
#if REMOTE_MEM
	if (unlikely(is_remote(addr))) { remote_write32(addr, val); return; }
#endif
	pstore32_local(cpu, addr, val);
}

/* lazy flags */
enum {
	CC_ADC, CC_ADD,	CC_SBB, CC_SUB,
	CC_NEG8, CC_NEG16, CC_NEG32,
	CC_DEC8, CC_DEC16, CC_DEC32,
	CC_INC8, CC_INC16, CC_INC32,
	CC_IMUL8, CC_IMUL16, CC_IMUL32,	CC_MUL8, CC_MUL16, CC_MUL32,
	CC_SAR, CC_SHL, CC_SHR,
	CC_SHLD, CC_SHRD, CC_BSF, CC_BSR,
	CC_AND, CC_OR, CC_XOR,
};

static int __not_in_flash_func( get_CF )(CPUI386 *cpu)
{
	if (cpu->cc.mask & CF) {
		switch(cpu->cc.op) {
		case CC_ADC:
			return cpu->cc.dst <= cpu->cc.src2;
		case CC_ADD:
			return cpu->cc.dst < cpu->cc.src2;
		case CC_SBB:
			return cpu->cc.src1 <= cpu->cc.src2;
		case CC_SUB:
			return cpu->cc.src1 < cpu->cc.src2;
		case CC_NEG8: case CC_NEG16: case CC_NEG32:
			return cpu->cc.dst != 0;
		case CC_DEC8: case CC_DEC16: case CC_DEC32:
		case CC_INC8: case CC_INC16: case CC_INC32:
			assert(false); // should not happen
		case CC_IMUL8:
			return sext8(cpu->cc.dst) != cpu->cc.dst;
		case CC_IMUL16:
			return sext16(cpu->cc.dst) != cpu->cc.dst;
		case CC_IMUL32:
			return (((s32) cpu->cc.dst) >> 31) != cpu->cc.dst2;
		case CC_MUL8:
			return (cpu->cc.dst >> 8) != 0;
		case CC_MUL16:
			return (cpu->cc.dst >> 16) != 0;
		case CC_MUL32:
			return (cpu->cc.dst2) != 0;
		case CC_SHL:
		case CC_SHR:
		case CC_SAR:
			return cpu->cc.dst2 & 1;
		case CC_SHLD:
			return cpu->cc.dst2 >> 31;
		case CC_SHRD:
			return cpu->cc.dst2 & 1;
		case CC_BSF:
		case CC_BSR:
			return 0;
		case CC_AND:
		case CC_OR:
		case CC_XOR:
			return 0;
		}
	} else {
		return !!(cpu->flags & CF);
	}
	assert(false);
}

const static u8 parity_tab[256] __not_in_flash("parity_tab") = {
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
  1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

static inline int get_PF(CPUI386 *cpu)
{
	if (cpu->cc.mask & PF) {
		return parity_tab[cpu->cc.dst & 0xff];
	} else {
		return !!(cpu->flags & PF);
	}
}

static inline int get_AF(CPUI386 *cpu)
{
	if (cpu->cc.mask & AF) {
		switch(cpu->cc.op) {
		case CC_ADC:
		case CC_ADD:
		case CC_SBB:
		case CC_SUB:
			return ((cpu->cc.src1 ^ cpu->cc.src2 ^ cpu->cc.dst) >> 4) & 1;
		case CC_NEG8: case CC_NEG16: case CC_NEG32:
			return (cpu->cc.dst & 0xf) != 0;
		case CC_DEC8: case CC_DEC16: case CC_DEC32:
			return (cpu->cc.dst & 0xf) == 0xf;
		case CC_INC8: case CC_INC16: case CC_INC32:
			return (cpu->cc.dst & 0xf) == 0;
		case CC_IMUL8: case CC_IMUL16: case CC_IMUL32:
		case CC_MUL8: case CC_MUL16: case CC_MUL32:
			return 0;
		case CC_SAR:
		case CC_SHL:
		case CC_SHR:
		case CC_SHLD:
		case CC_SHRD:
		case CC_BSF:
		case CC_BSR:
		case CC_AND:
		case CC_OR:
		case CC_XOR:
			return 0;
		}
	} else {
		return !!(cpu->flags & AF);
	}
	assert(false);
}

static int IRAM_ATTR get_ZF(CPUI386 *cpu)
{
	if (cpu->cc.mask & ZF) {
		return cpu->cc.dst == 0;
	} else {
		return !!(cpu->flags & ZF);
	}
}

static int IRAM_ATTR get_SF(CPUI386 *cpu)
{
	if (cpu->cc.mask & SF) {
		return cpu->cc.dst >> (sizeof(uword) * 8 - 1);
	} else {
		return !!(cpu->flags & SF);
	}
}

static int IRAM_ATTR get_OF(CPUI386 *cpu)
{
	if (cpu->cc.mask & OF) {
		switch(cpu->cc.op) {
		case CC_ADC:
		case CC_ADD:
			return (~(cpu->cc.src1 ^ cpu->cc.src2) & (cpu->cc.dst ^ cpu->cc.src2)) >> (sizeof(uword) * 8 - 1);
		case CC_SBB:
		case CC_SUB:
			return ((cpu->cc.src1 ^ cpu->cc.src2) & (cpu->cc.dst ^ cpu->cc.src1)) >> (sizeof(uword) * 8 - 1);
		case CC_DEC8:
			return cpu->cc.dst == sext8((u8) ~(1u << 7));
		case CC_DEC16:
			return cpu->cc.dst == sext16((u16) ~(1u << 15));
		case CC_DEC32:
			return cpu->cc.dst == sext32((u32) ~(1u << 31));
		case CC_INC8: case CC_NEG8:
			return cpu->cc.dst == sext8(1u << 7);
		case CC_INC16: case CC_NEG16:
			return cpu->cc.dst == sext16(1u << 15);
		case CC_INC32: case CC_NEG32:
			return cpu->cc.dst == sext32(1u << 31);
		case CC_IMUL8: case CC_IMUL16: case CC_IMUL32:
		case CC_MUL8: case CC_MUL16: case CC_MUL32:
			return get_CF(cpu);
		case CC_SAR:
			return 0;
		case CC_SHL:
			return (cpu->cc.dst >> (sizeof(uword) * 8 - 1)) ^ (cpu->cc.dst2 & 1);
		case CC_SHR:
			return (cpu->cc.src1 >> (sizeof(uword) * 8 - 1));
		case CC_SHLD:
		case CC_SHRD:
			return (cpu->cc.src1 ^ cpu->cc.dst) >> (sizeof(uword) * 8 - 1);
		case CC_BSF:
		case CC_BSR:
			return 0;
		case CC_AND:
		case CC_OR:
		case CC_XOR:
			return 0;
		}
		assert(false);
	} else {
		return !!(cpu->flags & OF);
	}
	assert(false);
}

static void IRAM_ATTR refresh_flags(CPUI386 *cpu)
{
	/*
	 * Only materialise the flags that are actually pending.
	 *
	 * cc.mask says which flags are still held lazily in cc.op/dst/src.
	 * For every other flag the getters simply return the bit already in
	 * cpu->flags, so calling them writes back what is already there —
	 * six out-of-line calls (get_CF and friends are real functions, not
	 * inlined) and six big switches to compute nothing.
	 *
	 * Measured at 12.4% of core-0 time in a Wolf3D profile, which is
	 * what makes a guard this simple worth having: PUSHF, LAHF and every
	 * interrupt entry land here, and real-mode DOS code does all three
	 * constantly.
	 *
	 * Equivalent by construction: when a mask bit is clear the getter is
	 * defined to return the current cpu->flags bit, so skipping it
	 * cannot change the result.
	 */
	uword mask = cpu->cc.mask;
	if (mask & CF) SET_BIT(cpu->flags, get_CF(cpu), CF);
	if (mask & PF) SET_BIT(cpu->flags, get_PF(cpu), PF);
	if (mask & AF) SET_BIT(cpu->flags, get_AF(cpu), AF);
	if (mask & ZF) SET_BIT(cpu->flags, get_ZF(cpu), ZF);
	if (mask & SF) SET_BIT(cpu->flags, get_SF(cpu), SF);
	if (mask & OF) SET_BIT(cpu->flags, get_OF(cpu), OF);
}

static inline int get_IOPL(CPUI386 *cpu)
{
	return (cpu->flags & IOPL) >> 12;
}

/*
 * FRANK_WORKLOAD_PROFILE_V88
 *
 * Window-scoped workload counters for the Win+F7 .. Win+F8 capture.
 *
 * v8.7.3 compiled the single hottest entry of the v8.7.2 JIT reject list
 * (the F000:8E15 byte-clear micro-loop) and the Symantec-under-EMM386 score
 * did not move at all, while v8.5.1 -> v8.6 produced byte-identical counters.
 * Two consecutive A/Bs therefore say the reject list is not where the EMM386
 * time is.  Choosing the next optimisation needs numbers no previous capture
 * contained:
 *
 *   - guest instructions retired inside the window, so native_guest_insns
 *     becomes a real coverage fraction instead of a guess;
 *   - how much of the window is VM86+paging at all;
 *   - what the paging path itself costs: TLB refills and full TLB flushes are
 *     page-table traffic against PSRAM, measured by this project at ~182 core
 *     cycles per uncached access (see the remote-memory note above pload8());
 *   - how much of the window is service work (software INT, hardware IRQ,
 *     faults) rather than benchmark compute.
 *
 * Every counter below is incremented only on a path that is already far more
 * expensive than an SRAM increment - TLB refill, TLB flush, exception entry,
 * software interrupt - or once per taken backward branch, which already calls
 * the JIT dispatcher.  None of them sits on the per-instruction decode path,
 * and none of them changes emulated behaviour.
 */
volatile u32 g_wl_tlb_refills __attribute__((used));
volatile u32 g_wl_tlb_clears __attribute__((used));
volatile u32 g_wl_exc_pf __attribute__((used));
volatile u32 g_wl_exc_gp __attribute__((used));
volatile u32 g_wl_exc_other __attribute__((used));
volatile u32 g_wl_hw_irq __attribute__((used));
/*
 * FRANK_WL_RAM_BUDGET_V89_1
 *
 * Counters that land inside a RAM-resident function are far more
 * expensive than they look.  cpu_exec1(), nj_exec_loop(), tlb_refill()
 * and translate*() are all __not_in_flash(), so their code sits in
 * .data, and at v8.7.3 .data ended exactly 8 bytes below the 4 KB
 * boundary that .bss is aligned to.  Any growth there therefore costs a
 * further 4096 bytes of link padding on top of the code itself, taken
 * straight out of the malloc heap that pc_new() draws on.
 *
 * The v8.9 build did exactly that: three counters inlined at the 13
 * NJ_HOT_BACKEDGE sites pushed RAM from 91.66% to 92.50%, and the board
 * came up with no HDMI signal.  codeprofile.h already records the same
 * failure mode from the other direction - .bss growth leaving pc_new()
 * with nothing, the emulator reaching vga_initialized and never
 * finishing init.
 *
 * So the backedge counters are gone.  They did their job: they are what
 * proved 99.5% of backedges were being dropped at the TF guard.  What is
 * kept below costs nothing in RAM-resident code, or is small enough to
 * measure and stay under the boundary.
 */

/*
 * FRANK_TF_V88
 *
 * Single-step debug exceptions are not implemented by this emulator.
 * EX_DB is declared in the exception enum and never thrown; cpu->dr[] is
 * zeroed at reset and otherwise only moved in and out by MOV DR.  TF is
 * therefore stored, pushed, popped and cleared on interrupt entry, but it
 * never causes a trap, and no guest single-step handler ever runs to
 * clear it again.  Once some guest sets it, it stays set for the rest of
 * the session.
 *
 * That is exactly what happens under HIMEM + EMM386.  In the
 * jitstats001 capture (Symantec under M602 + EMM386, score 8.7) the CPU
 * mode at dump time is flags=00023347, i.e. VM | IOPL=3 | IF | TF, and
 * the counters say the same thing independently:
 *
 *   window_backedges                       = 2338872
 *   hits                                   =    6842
 *   misses * 256 (sampled discovery)       =    4864
 *   ------------------------------------------------
 *   backedges that got past the TF test    =   11706  (0.5%)
 *
 * so 99.5% of all taken backward branches returned at the TF guard and
 * the JIT was switched off for the whole run: native_coverage_ppm=4070,
 * against 745770 for the identical benchmark without EMM386, which
 * scores 345 instead of 8.7.
 *
 * The guard exists to preserve single-step semantics that the
 * interpreter does not implement, so it protects nothing while costing
 * the entire JIT.  A JIT block executes exactly what the interpreter
 * would have executed, so running one with TF set is indistinguishable
 * from interpreting with TF set - which is what already happens today.
 *
 * If TF -> #DB is ever implemented, set this back to 1 and both guards
 * below become live again.
 */
#define NJ_SINGLE_STEP_IMPLEMENTED 0

/*
 * Runtime form of the guard rather than #if.
 *
 * Deleting the test outright cost 408 bytes of .data: nj_try_execute()
 * is always_inline at 13 NJ_HOT_BACKEDGE sites inside cpu_exec1(), which
 * is __not_in_flash(), and losing the early return let GCC materialise
 * more of the body at each one.  .data ended 8 bytes below the 4 KB
 * boundary .bss is aligned to, so those 408 bytes pulled in a further
 * 4096 of link padding and the board came up with no HDMI signal - the
 * failure mode codeprofile.h already records, where pc_new() cannot get
 * its heap and vga_hw_set_vga_state() is therefore never reached.
 *
 * Keeping the branch and testing a variable the compiler cannot fold
 * preserves the generated shape, so the size stays where it was.  The
 * flag is 0, so the guard never fires; it also makes the whole change
 * A/B-testable on one firmware image if that is ever wanted.
 */

/* MMU */
#define CR0_PG (1<<31)
#define CR0_WP (0x10000)
#ifdef BUILD_ESP32
#define tlb_size 256
#else
#define tlb_size 512
#endif
typedef struct {
	enum {
		ADDR_OK1,
		ADDR_OK2,
	} res;
	uword addr1;
	uword addr2;
} OptAddr;

static void tlb_clear(CPUI386 *cpu)
{
	g_wl_tlb_clears++;
	for (int i = 0; i < tlb_size; i++) {
		cpu->tlb.tab[i].lpgno = -1;
	}
	cpu->ifetch.laddr = -1;
	cpu->prefetch_base = (u32)-1;
#if NATIVE_JIT
	/*
	 * FRANK_NATIVE_JIT_V8_SUPERTRACE:
	 * TLB invalidation is not native-code invalidation.  A paged block is
	 * guarded against the current linear->physical code mapping before it can
	 * execute, and dynamic memory operations use the live guest TLB at runtime.
	 * Keeping translated ARM code across CR3 reloads/INVLPG avoids the massive
	 * DPMI/Doom recompile churn seen in v6.
	 */
#endif
#if BLOCK_JIT
	bj_flush();
#endif
}

static int pte_lookup[2][4][2][2] = { //[wp != 0][(pte >> 1) & 3][cpl > 0][rwm > 1]
	{ // wp == 0
		{ {0, 0}, {1, 1} }, // s,r
		{ {0, 0}, {1, 1} }, // s,w
		{ {0, 0}, {0, 1} }, // u,r
		{ {0, 0}, {0, 0} }, // u,w
	},
	{ // wp == 1
		{ {0, 1}, {1, 1} }, // s,r
		{ {0, 0}, {1, 1} }, // s,w
		{ {0, 1}, {0, 1} }, // u,r
		{ {0, 0}, {0, 0} }, // u,w
	}
};

static bool IRAM_ATTR tlb_refill(CPUI386 *cpu, struct tlb_entry *ent, uword lpgno)
{
	g_wl_tlb_refills++;

	uword base_addr = cpu->cr3 & ~0xfff;
	uword i = lpgno >> 10;
	uword j = lpgno & 1023;

	u8 *mem = (u8 *) cpu->phys_mem;
	uword pde = pload32(cpu, base_addr + i * 4);
	if (!(pde & 1))
		return false;
	mem[base_addr + i * 4] |= 1 << 5; // accessed

	uword base_addr2 = pde & ~0xfff;
	uword pte = pload32(cpu, base_addr2 + j * 4);
	if (!(pte & 1))
		return false;

	mem[base_addr2 + j * 4] |= 1 << 5; // accessed
//	mem[base_addr2 + j * 4] |= 1 << 6; // dirty

	ent->lpgno = lpgno;
	ent->xaddr = (pte & ~0xfff) ^ (lpgno << 12);
	pte = pte & ((pde & 7) | 0xfffffff8);
	ent->pte_lookup = pte_lookup[!!(cpu->cr0 & CR0_WP)][(pte >> 1) & 3];
	ent->ppte = &(mem[base_addr2 + j * 4]);
	return true;
}

static bool IRAM_ATTR translate_lpgno(CPUI386 *cpu, int rwm, uword lpgno, uword laddr, int cpl, uword *paddr)
{
	struct tlb_entry *ent = &(cpu->tlb.tab[lpgno % tlb_size]);
	if (ent->lpgno != lpgno) {
		if (!tlb_refill(cpu, ent, lpgno)) {
			cpu->cr2 = laddr;
			cpu->excno = EX_PF;
			cpu->excerr = 0;
			if (rwm & 2)
				cpu->excerr |= 2;
			if (cpl)
				cpu->excerr |= 4;
			return false;
		}
	}
	if (ent->pte_lookup[cpl > 0][rwm > 1]) {
		cpu->cr2 = laddr;
		cpu->excno = EX_PF;
		cpu->excerr = 1;
		if (rwm & 2)
			cpu->excerr |= 2;
		if (cpl)
			cpu->excerr |= 4;
		ent->lpgno = -1;
		return false;
	}
	*paddr = ent->xaddr ^ laddr;
	if (rwm & 2) {
		/*
		 * Set the PTE dirty bit only when it is actually clear.
		 *
		 * This is a read-modify-write of guest memory, and guest memory is
		 * PSRAM: the unconditional OR turned every guest write into an extra
		 * store, on a page that is almost always dirty already after the
		 * first write to it.  Testing first keeps the load - which the cache
		 * serves - and drops the store.
		 */
		u8 *ppte = ent->ppte;
		if (unlikely(!(*ppte & (1 << 6))))
			*ppte |= 1 << 6;
	}
	return true;
}

static bool IRAM_ATTR translate_laddr(CPUI386 *cpu, OptAddr *res, int rwm, uword laddr, int size, int cpl)
{
	if (cpu->cr0 & CR0_PG) {
		uword lpgno = laddr >> 12;
		uword paddr;
		TRY(translate_lpgno(cpu, rwm, lpgno, laddr, cpl, &paddr));
		res->res = ADDR_OK1;
		res->addr1 = paddr;
		if ((laddr & 0xfff) > 0x1000 - size) {
			lpgno++;
			TRY(translate_lpgno(cpu, rwm, lpgno, lpgno << 12, cpl, &paddr));
			res->res = ADDR_OK2;
			res->addr2 = paddr;
		}
	} else {
		res->res = ADDR_OK1;
		res->addr1 = laddr;
	}
	return true;
}

static bool IRAM_ATTR segcheck(CPUI386 *cpu, int rwm, int seg, uword addr, int size)
{
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) {
		/* null selector check */
		if (cpu->seg[seg].limit == 0 && (cpu->seg[seg].sel & ~0x3) == 0) {
			dolog("segcheck null: seg=%d sel=%04x addr=%08x\n", seg, cpu->seg[seg].sel, addr);
			THROW(seg == SEG_SS ? EX_SS : EX_GP, 0);
		}
		/* limit check + write check disabled to match tiny386 upstream */
#if 0
		/* todo: limit check, readonly check */
#endif
	}
	return true;
}

/*
 * Everything translate() does not want inline: the segment-limit rejection,
 * a TLB miss and its refill, a page fault, and an access that straddles two
 * pages and therefore needs two translations.
 */
static bool __attribute__((noinline)) IRAM_ATTR
translate_out_of_line(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr,
		      int size, int cpl)
{
	uword laddr = cpu->seg[seg].base + addr;

	TRYL(segcheck(cpu, rwm, seg, addr, size));

	return translate_laddr(cpu, res, rwm, laddr, size, cpl);
}

/*
 * Address translation, with the case that actually happens kept in one
 * function.
 *
 * This used to be four nested out-of-line calls for a plain TLB hit -
 * translate() called segcheck(), then translate_laddr(), which called
 * translate_lpgno() - and every instruction with a memory operand pays for
 * all of them.  None of the four is expensive; the call sequences are, and on
 * a machine spending 252 host cycles per guest instruction they are a
 * measurable share of the whole interpreter.  `translate` plus
 * `translate_laddr` alone were 9.4% of core 0 in the DRACIHIS profile, and the
 * register spills the calls force on cpu_exec1() land in *its* 41.4%.
 *
 * So the hit path - segment check passes, paging on, TLB entry present and
 * permitted, access inside one page - is straight-line code here, and
 * everything else is one call to translate_out_of_line().  This is the same
 * shape as the instruction-fetch change that was worth 12.9%: a small inline
 * test, a noinline miss.  Crucially it does *not* inline into the ~200 call
 * sites, so it costs almost nothing in SRAM, which is 90.85% full.
 *
 * The behaviour is unchanged, including the order in which exceptions are
 * raised: a segment rejection is tested before anything paging-related, and
 * every case the fast path declines is re-done from scratch by the slow one.
 */
static bool IRAM_ATTR translate(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr, int size, int cpl)
{
	assert(seg != -1);
	uword laddr = cpu->seg[seg].base + addr;

	/* segcheck()'s only rejection, tested here so its call disappears. */
	if (unlikely((cpu->cr0 & 1) && !(cpu->flags & VM) &&
		     cpu->seg[seg].limit == 0 && (cpu->seg[seg].sel & ~0x3) == 0))
		return translate_out_of_line(cpu, res, rwm, seg, addr, size, cpl);

	if (likely(cpu->cr0 & CR0_PG)) {
		uword lpgno = laddr >> 12;
		struct tlb_entry *ent = &(cpu->tlb.tab[lpgno % tlb_size]);

		if (unlikely(ent->lpgno != lpgno) ||
		    unlikely(ent->pte_lookup[cpl > 0][rwm > 1]) ||
		    unlikely((laddr & 0xfff) > 0x1000 - size))
			return translate_out_of_line(cpu, res, rwm, seg, addr, size, cpl);

		if (rwm & 2) {
			u8 *ppte = ent->ppte;
			if (unlikely(!(*ppte & (1 << 6))))
				*ppte |= 1 << 6;
		}
		res->res = ADDR_OK1;
		res->addr1 = ent->xaddr ^ laddr;
		return true;
	}

	res->res = ADDR_OK1;
	res->addr1 = laddr;
	return true;
}

static bool IRAM_ATTR translate8r(CPUI386 *cpu, OptAddr *res, int seg, uword addr)
{
	assert(seg != -1);
	uword laddr = cpu->seg[seg].base + addr;

	TRYL(segcheck(cpu, 1, seg, addr, 1));

	if (cpu->cr0 & CR0_PG) {
		uword lpgno = laddr >> 12;
		struct tlb_entry *ent = &(cpu->tlb.tab[lpgno % tlb_size]);
		if (ent->lpgno != lpgno) {
			if (!tlb_refill(cpu, ent, lpgno)) {
				cpu->cr2 = laddr;
				cpu->excno = EX_PF;
				cpu->excerr = 0;
				if (cpu->cpl)
					cpu->excerr |= 4;
				return false;
			}
		}
		if (ent->pte_lookup[cpu->cpl > 0][0]) {
			cpu->cr2 = laddr;
			cpu->excno = EX_PF;
			cpu->excerr = 1;
			if (cpu->cpl)
				cpu->excerr |= 4;
			ent->lpgno = -1;
			return false;
		}
		res->res = ADDR_OK1;
		res->addr1 = ent->xaddr ^ laddr;
	} else {
		res->res = ADDR_OK1;
		res->addr1 = laddr;
	}

	return true;
}

static inline bool translate8(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr)
{
	return translate(cpu, res, rwm, seg, addr, 1, cpu->cpl);
}

static inline bool translate16(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr)
{
	return translate(cpu, res, rwm, seg, addr, 2, cpu->cpl);
}

static inline bool translate32(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr)
{
	return translate(cpu, res, rwm, seg, addr, 4, cpu->cpl);
}

static inline bool __attribute__((always_inline)) in_iomem(uword addr)
{
	/*
	 * Almost every DOS RAM access is below the VGA aperture.  Reject that
	 * common case with one ordered comparison; only the uncommon high address
	 * needs the second half of the aperture/device test.
	 */
	if (likely(addr < 0xa0000u))
		return false;
	return addr < 0xc0000u || addr >= 0xe0000000u;
}

static u8 IRAM_ATTR load8(CPUI386 *cpu, OptAddr *res)
{
	uword addr = res->addr1;
	if (in_iomem(addr) && cpu->cb.iomem_read8)
		return cpu->cb.iomem_read8(cpu->cb.iomem, addr);
	if (unlikely(addr >= cpu->phys_mem_size)) {
		return 0;
	}
	return pload8(cpu, addr);
}

static u16 IRAM_ATTR load16(CPUI386 *cpu, OptAddr *res)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_read16)
		return cpu->cb.iomem_read16(cpu->cb.iomem, res->addr1);
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return 0;
	}
	if (likely(res->res == ADDR_OK1))
		return pload16(cpu, res->addr1);
	else
		return pload8(cpu, res->addr1) | (pload8(cpu, res->addr2) << 8);
}

static u32 IRAM_ATTR load32(CPUI386 *cpu, OptAddr *res)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_read32)
		return cpu->cb.iomem_read32(cpu->cb.iomem, res->addr1);
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return 0;
	}
	if (likely(res->res == ADDR_OK1)) {
		return pload32(cpu, res->addr1);
	} else {
		switch(res->addr1 & 0xf) {
		case 0xf:
			return pload8(cpu, res->addr1) | (pload16(cpu, res->addr2) << 8) |
				(pload8(cpu, res->addr2 + 2) << 24);
		case 0xe:
			return pload16(cpu, res->addr1) | (pload16(cpu, res->addr2) << 16);
		case 0xd:
			return pload8(cpu, res->addr1) | (pload16(cpu, res->addr1 + 1) << 8) |
				(pload8(cpu, res->addr2) << 24);
		}
	}
	assert(false);
}

static void IRAM_ATTR store8(CPUI386 *cpu, OptAddr *res, u8 val)
{
	uword addr = res->addr1;
	if (in_iomem(addr) && cpu->cb.iomem_write8) {
		cpu->cb.iomem_write8(cpu->cb.iomem, addr, val);
		return;
	}
	if (unlikely(addr >= cpu->phys_mem_size)) {
		return;
	}
	pstore8(cpu, addr, val);
}

static void IRAM_ATTR store16(CPUI386 *cpu, OptAddr *res, u16 val)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_write16) {
		cpu->cb.iomem_write16(cpu->cb.iomem, res->addr1, val);
		return;
	}
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return;
	}
	if (likely(res->res == ADDR_OK1)) {
		pstore16(cpu, res->addr1, val);
	} else {
		pstore8(cpu, res->addr1, val);
		pstore8(cpu, res->addr2, val >> 8);
	}
}

static void IRAM_ATTR store32(CPUI386 *cpu, OptAddr *res, u32 val)
{
	if (in_iomem(res->addr1) && cpu->cb.iomem_write32) {
		cpu->cb.iomem_write32(cpu->cb.iomem, res->addr1, val);
		return;
	}
	if (unlikely(res->addr1 >= cpu->phys_mem_size)) {
		return;
	}
	if (likely(res->res == ADDR_OK1)) {
		pstore32(cpu, res->addr1, val);
	} else {
		switch(res->addr1 & 0xf) {
		case 0xf:
			pstore8(cpu, res->addr1, val);
			pstore16(cpu, res->addr2, val >> 8);
			pstore8(cpu, res->addr2 + 2, val >> 24);
			break;
		case 0xe:
			pstore16(cpu, res->addr1, val);
			pstore16(cpu, res->addr2, val >> 16);
			break;
		case 0xd:
			pstore8(cpu, res->addr1, val);
			pstore16(cpu, res->addr1 + 1, val >> 8);
			pstore8(cpu, res->addr2, val >> 24);
			break;
		}
	}
}

#define LOADSTORE(BIT) \
bool cpu_load ## BIT(CPUI386 *cpu, int seg, uword addr, u ## BIT *res) \
{ \
	OptAddr o; \
	TRY(translate ## BIT(cpu, &o, 1, seg, addr)); \
	*res = load ## BIT(cpu, &o); \
	return true; \
} \
\
bool cpu_store ## BIT(CPUI386 *cpu, int seg, uword addr, u ## BIT val) \
{ \
	OptAddr o; \
	TRY(translate ## BIT(cpu, &o, 2, seg, addr)); \
	store ## BIT(cpu, &o, val); \
	return true; \
} \

LOADSTORE(8)
LOADSTORE(16)
LOADSTORE(32)

/*
 * Instruction prefetch buffer: 32 bytes loaded as eight 32-bit reads from a
 * 32-byte-aligned physical address.  cpu->prefetch_base holds that physical base
 * address (always a multiple of 32), or (u32)-1 when invalid.
 *
 * The width is the XIP cache's line size deliberately.  A 16-byte fill took
 * one PSRAM miss and used half of what that miss brought in, so the next
 * refill sixteen bytes later was free anyway - the cost was the refill path
 * itself, and peek8_slow() was 8.9% of core 0 in the DRACIHIS profile.  A
 * 32-byte line never straddles a 4 KB page either, since 4096 divides evenly
 * by 32, so nothing about the paging checks changes.
 *
 * Invalidation is implicit: any jump/call/ret changes next_ip so that the
 * resulting paddr falls outside [cpu->prefetch_base, cpu->prefetch_base+16), causing
 * an automatic refill on the very next fetch.  No explicit flush is needed
 * at branch sites.
 *
 * cpu->prefetch[] is aligned to 4 bytes so the four pload32 calls are natural.
 */

/* Refill: load 32 bytes (8 x u32) from the 32-byte-aligned block that
 * contains paddr.  Caller guarantees paddr is in plain RAM and within the
 * current ifetch page. */
static inline void __attribute__((always_inline))
prefetch_fill(CPUI386 *cpu, uword laddr, uword paddr)
{
	u32 pbase = paddr & ~(u32)31;
	cp_note(pbase);

	/*
	 * Tag the 32-byte instruction-prefetch line by linear address.
	 * Paging preserves the low 12 address bits, so laddr and paddr have
	 * the same offset within a 32-byte line.  The physical address is
	 * still used for the actual PSRAM reads below.
	 *
	 * This removes the ifetch page test + physical-address XOR from the
	 * FAST_FETCH hit path, which is one of the hottest paths on Z2.
	 */
	cpu->prefetch_base = laddr & ~(u32)31;

	u32* prefetch = (u32*)cpu->prefetch;
	*prefetch++ = pload32(cpu, pbase);
	*prefetch++ = pload32(cpu, pbase + 4);
	*prefetch++ = pload32(cpu, pbase + 8);
	*prefetch++ = pload32(cpu, pbase + 12);
	*prefetch++ = pload32(cpu, pbase + 16);
	*prefetch++ = pload32(cpu, pbase + 20);
	*prefetch++ = pload32(cpu, pbase + 24);
	*prefetch++ = pload32(cpu, pbase + 28);
}

/* True if laddr is covered by the current prefetch buffer. */
#define PREFETCH_HIT(laddr) \
	(likely(((uword)(laddr) - cpu->prefetch_base) < 32u))

/*
 * Instruction fetch is 21% of core-0 time, and peek8 was being called
 * out of line from 473 sites — one call, prologue and return per
 * instruction *byte*. IRAM_ATTR is __not_in_flash(), an explicit
 * section attribute, and GCC will not inline a function that carries
 * one, so the `static` here was never enough.
 *
 * The split below inlines only the hit path: two compares and a byte
 * load, ~20 bytes per site. Everything else — prefetch refill, page
 * miss, the full TLB walk — stays out of line.
 */
static bool __attribute__((noinline)) IRAM_ATTR
peek8_miss(CPUI386 *cpu, u8 *val);

static bool IRAM_ATTR peek8_slow(CPUI386 *cpu, u8 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;

	/*
	 * The prefetch line is tagged by linear address, so a hit is valid without
	 * consulting the cached translation first.  Test it before ifetch.laddr:
	 * this is the common path and avoids a page-tag load/XOR/compare for every
	 * interpreted instruction byte.  FAST_FETCH already relies on the same
	 * invariant; keeping it here gives Z2 most of that hot-path win without
	 * duplicating the test at hundreds of call sites.
	 */
	if (PREFETCH_HIT(laddr)) {
		*val = cpu->prefetch[laddr & 31];
		return true;
	}

	/* Keep the overwhelmingly common hit path small; the noinline miss helper
	 * also prevents its large register-save set from infecting every hit. */
	return peek8_miss(cpu, val);
}

static bool __attribute__((noinline)) IRAM_ATTR
peek8_miss(CPUI386 *cpu, u8 *val)
{
	/* One recomputation per 32-byte refill is cheaper than carrying laddr into
	 * the helper and forcing another callee-saved register on every hit. */
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;

	if (likely((laddr ^ cpu->ifetch.laddr) < 4096)) {
		uword paddr = cpu->ifetch.xaddr ^ laddr;
		prefetch_fill(cpu, laddr, paddr);
		*val = cpu->prefetch[laddr & 31];
		return true;
	}
	/* ifetch page miss: full TLB translate */
	OptAddr res;
	TRY(translate8r(cpu, &res, SEG_CS, cpu->next_ip));
	cpu->ifetch.laddr = laddr & (~4095ul);
	cpu->ifetch.xaddr = res.addr1 ^ laddr;
	if (!in_iomem(res.addr1) && res.addr1 + 31 < cpu->phys_mem_size) {
		prefetch_fill(cpu, laddr, res.addr1);
		*val = cpu->prefetch[laddr & 31];
	} else {
		cpu->prefetch_base = (u32)-1;
		*val = load8(cpu, &res);
	}
	return true;
}

/*
 * Gated because it costs ~12 KB of RAM.
 *
 * The win is board-independent, but master SRAM is not: C2 sits at 88.9%
 * with it and the emulator stops booting around 91% when pc_new() can no
 * longer allocate. M1/M2/PC/Z2 were at 88.3% before this change and 90.7%
 * after — still building, but close enough to a threshold I have actually
 * hit that enabling it on boards I cannot boot-test would be careless.
 *
 * ON for C2, where it is measured. Other boards keep their previous
 * footprint byte for byte; enable FAST_FETCH per board once tested.
 */
#if FAST_FETCH
static inline __attribute__((always_inline))
bool peek8(CPUI386 *cpu, u8 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;
	if (likely(PREFETCH_HIT(laddr))) {
		*val = cpu->prefetch[laddr & 31];
		return true;
	}
	return peek8_slow(cpu, val);
}
#else
#define peek8 peek8_slow
#endif

static bool IRAM_ATTR fetch8(CPUI386 *cpu, u8 *val)
{
	TRY(peek8(cpu, val));
	cpu->next_ip++;
	return true;
}

static bool IRAM_ATTR fetch16(CPUI386 *cpu, u16 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;
	if (likely((laddr ^ cpu->ifetch.laddr) < 4095)) {
		uword paddr = cpu->ifetch.xaddr ^ laddr;
		if (!PREFETCH_HIT(laddr))
			prefetch_fill(cpu, laddr, paddr);
		unsigned off = laddr & 31;
		if (likely(off <= 30)) {
			/* Both bytes inside the buffer */
			*val = cpu->prefetch[off] | ((u16)cpu->prefetch[off + 1] << 8);
		} else {
			/* Byte 1 is last byte of current block; byte 0 already in buffer.
			 * Read second byte via pload (still within ifetch page). */
			u8 lo = cpu->prefetch[31];
			u8 hi = pload8(cpu, paddr + 1);
			*val = lo | ((u16)hi << 8);
		}
	} else {
		OptAddr res;
		TRY(translate16(cpu, &res, 1, SEG_CS, cpu->next_ip));
		*val = load16(cpu, &res);
		cpu->prefetch_base = (u32)-1;
	}
	cpu->next_ip += 2;
	return true;
}

static bool IRAM_ATTR fetch32(CPUI386 *cpu, u32 *val)
{
	uword laddr = cpu->seg[SEG_CS].base + cpu->next_ip;
	if (likely((laddr ^ cpu->ifetch.laddr) < 4093)) {
		uword paddr = cpu->ifetch.xaddr ^ laddr;
		if (!PREFETCH_HIT(laddr))
			prefetch_fill(cpu, laddr, paddr);
		unsigned off = laddr & 31;
		if (likely(off <= 28)) {
			/* All 4 bytes inside the buffer */
			*val = cpu->prefetch[off]
			     | ((u32)cpu->prefetch[off + 1] << 8)
			     | ((u32)cpu->prefetch[off + 2] << 16)
			     | ((u32)cpu->prefetch[off + 3] << 24);
		} else {
			/* Spans two prefetch lines: read the remaining bytes
			 * directly and let the next fetch refill. */
			*val = pload32(cpu, paddr);
			cpu->prefetch_base = (u32)-1;
		}
	} else {
		OptAddr res;
		TRY(translate32(cpu, &res, 1, SEG_CS, cpu->next_ip));
		*val = load32(cpu, &res);
		cpu->prefetch_base = (u32)-1;
	}
	cpu->next_ip += 4;
	return true;
}

/* insts decode && execute */
static inline bool modsib32(CPUI386 *cpu, int mod, int rm, uword *addr, int *seg)
{
	if (rm == 4) {
		u8 sib;
		TRY(fetch8(cpu, &sib));
		int b = sib & 7;
		if (b == 5 && mod == 0) {
			TRY(fetch32(cpu, addr));
		} else {
			*addr = REGi(b);
			// sp bp as base register
			if ((b == 4 || b == 5) && *seg == -1)
				*seg = SEG_SS;
		}
		int i = (sib >> 3) & 7;
		if (i != 4)
			*addr += REGi(i) << (sib >> 6);
	} else if (rm == 5 && mod == 0) {
		TRY(fetch32(cpu, addr));
	} else {
		*addr = REGi(rm);
		// bp as base register
		if (rm == 5 && *seg == -1)
			*seg = SEG_SS;
	}
	if (mod == 1) {
		u8 imm8;
		TRY(fetch8(cpu, &imm8));
		*addr += (s8) imm8;
	} else if (mod == 2) {
		u32 imm32;
		TRY(fetch32(cpu, &imm32));
		*addr += (s32) imm32;
	}
	if (*seg == -1)
		*seg = SEG_DS;
	return true;
}

static inline bool modsib16(CPUI386 *cpu, int mod, int rm, uword *addr, int *seg)
{
	if (rm == 6 && mod == 0) {
		u16 imm16;
		TRY(fetch16(cpu, &imm16));
		*addr = imm16;
	} else {
		switch(rm) {
		case 0: *addr = REGi(3) + REGi(6); break;
		case 1: *addr = REGi(3) + REGi(7); break;
		case 2: *addr = REGi(5) + REGi(6); break;
		case 3: *addr = REGi(5) + REGi(7); break;
		case 4: *addr = REGi(6); break;
		case 5: *addr = REGi(7); break;
		case 6: *addr = REGi(5); break;
		case 7: *addr = REGi(3); break;
		}
		if (mod == 1) {
			u8 imm8;
			TRY(fetch8(cpu, &imm8));
			*addr += (s8) imm8;
		} else if (mod == 2) {
			u16 imm16;
			TRY(fetch16(cpu, &imm16));
			*addr += imm16;
		}
		*addr &= 0xffff;
	}
	if (*seg == -1) {
		if (rm == 2 || rm == 3)
			*seg = SEG_SS;
		else if (mod != 0 && rm == 6)
			*seg = SEG_SS;
		else
			*seg = SEG_DS;
	}
	return true;
}

static bool IRAM_ATTR modsib(CPUI386 *cpu, int adsz16, int mod, int rm, uword *addr, int *seg)
{
	if (adsz16) return modsib16(cpu, mod, rm, addr, seg);
	else return modsib32(cpu, mod, rm, addr, seg);
}

static bool read_desc(CPUI386 *cpu, int sel, uword *w1, uword *w2)
{
	OptAddr meml;
	sel = sel & 0xffff;
	uword off = sel & ~0x7;
	uword base;
	uword limit;
	if (sel & 0x4) {
		base = cpu->seg[SEG_LDT].base;
		limit = cpu->seg[SEG_LDT].limit;
	} else {
		base = cpu->gdt.base;
		limit = cpu->gdt.limit;
	}

	if (off + 7 > limit) {
		dolog("read_desc: sel %04x base %x limit %x off %x\n", sel, base, limit, off);
		THROW(EX_GP, sel & ~0x3);
	}
	if (w1) {
		TRY(translate_laddr(cpu, &meml, 1, base + off, 4, 0));
		*w1 = load32(cpu, &meml);
	}
	TRY(translate_laddr(cpu, &meml, 1, base + off + 4, 4, 0));
	*w2 = load32(cpu, &meml);
	return true;
}

static bool __not_in_flash_func(set_seg)(CPUI386 *cpu, int seg, int sel)
{
	if (seg == SEG_CS) {
		/* The stack is read straight out of physical memory: every guest
		 * that gets here runs the low megabyte identity-mapped, and a
		 * diagnostic read must never fault or walk page tables itself. */
		uint32_t ssp = cpu->seg[SEG_SS].base +
			(REGi(4) & (uint32_t)cpu->sp_mask);
		const uint8_t *stk = (ssp + 8 <= (uint32_t)cpu->phys_mem_size) ?
			cpu->phys_mem + ssp : 0;
		frank_diag_cs(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)sel,
			      ssp, (uint32_t)cpu->flags, stk);
	}
	sel = sel & 0xffff;
	if (!(cpu->cr0 & 1) || (cpu->flags & VM)) {
		cpu->seg[seg].sel = sel;
		cpu->seg[seg].base = sel << 4;
		cpu->seg[seg].limit = 0xffff;
		cpu->seg[seg].flags = 0; // D_BIT is not set
		if (seg == SEG_CS) {
			cpu->cpl = cpu->flags & VM ? 3 : 0;
			cpu->code16 = true;
		}
		if (seg == SEG_SS) {
			cpu->sp_mask = 0xffff;
		}
		return true;
	}

	/* Protected mode */
	if ((sel & ~0x3) == 0) {
		switch(seg) {
		case SEG_DS:
		case SEG_ES:
		case SEG_FS:
		case SEG_GS:
			/* Null selector is allowed; mark segment unusable. */
			cpu->seg[seg].sel = sel;
			cpu->seg[seg].base = 0;
			cpu->seg[seg].limit = 0;
			cpu->seg[seg].flags = 0;
			return true;
		case SEG_LDT:
			/* LLDT with null selector invalidates LDTR. */
			cpu->seg[seg].sel = 0;
			cpu->seg[seg].base = 0;
			cpu->seg[seg].limit = 0;
			cpu->seg[seg].flags = 0;
			return true;
		case SEG_SS:
		case SEG_CS:
		case SEG_TR:
			THROW(EX_GP, 0);
		default:
			THROW(EX_GP, 0);
		}
	}

	uword w1, w2;
	TRY(read_desc(cpu, sel, &w1, &w2));

	// TODO: various permission checks
	bool s = (w2 >> 12) & 1;
	bool p = (w2 >> 15) & 1;
	if (sel & ~0x3) {
		switch(seg) {
		case SEG_DS: case SEG_ES: case SEG_FS: case SEG_GS:
			if (!s) {
				THROW(EX_GP, sel & ~0x3);
			}
		}
		if (!p) THROW((seg == SEG_SS ? EX_SS : EX_NP), sel & ~0x3);
	}

	cpu->seg[seg].sel = sel;
	cpu->seg[seg].base = (w1 >> 16) | ((w2 & 0xff) << 16) | (w2 & 0xff000000);
	cpu->seg[seg].limit = (w2 & 0xf0000) | (w1 & 0xffff);
	if (w2 & 0x00800000)
		cpu->seg[seg].limit = (cpu->seg[seg].limit << 12) | 0xfff;
	cpu->seg[seg].flags = (w2 >> 8) & 0xffff;
	if (seg == SEG_CS) {
		cpu->cpl = sel & 3;
		cpu->code16 = !(cpu->seg[SEG_CS].flags & SEG_D_BIT);
	}
	if (seg == SEG_SS) {
		cpu->sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
	}
	return true;
}

static inline void clear_segs(CPUI386 *cpu)
{
	int segs[] = { SEG_DS, SEG_ES, SEG_FS, SEG_GS };
	for (int i = 0; i < 4; i++) {
		uword w2 = cpu->seg[segs[i]].flags << 8;
		bool is_dataseg = !((w2 >> 11) & 1);
		int dpl = (w2 >> 13) & 0x3;
		bool conforming = (w2 >> 8) & 0x4;
		if (is_dataseg || !conforming) {
			if (dpl < cpu->cpl) {
				cpu->seg[segs[i]].sel = 0;
				cpu->seg[segs[i]].base = 0;
				cpu->seg[segs[i]].limit = 0;
				cpu->seg[segs[i]].flags = 0;
			}
		}
	}
}

/*
 * addressing modes
 */
#define _(rwm, inst) inst()

#define E_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, laddr ## BIT, saddr ## BIT) \
	}

#define Eb(...) E_helper(8, , __VA_ARGS__)
#define Ev(...) if (opsz16) { E_helper(16, w, __VA_ARGS__) } else { E_helper(32, d, __VA_ARGS__) }

#define EG_helper(PM, BT, BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (PM && (!(cpu->cr0 & 1) || (cpu->flags & VM))) THROW0(EX_UD); \
	if (mod == 3) { \
		INST ## SUFFIX(rm, reg, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		if (BT) addr += lreg ## BIT(reg) / BIT * (BIT / 8); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, reg, laddr ## BIT, saddr ## BIT, lreg ## BIT, sreg ## BIT) \
	}

#define EbGb(...) EG_helper(false, false, 8, , __VA_ARGS__)
#define EwGw(...) EG_helper(false, false, 16, , __VA_ARGS__)
#define PMEwGw(...) EG_helper(true, false, 16, , __VA_ARGS__)
#define EvGv(...) if (opsz16) { EG_helper(false, false, 16, w, __VA_ARGS__) } else { EG_helper(false, false, 32, d, __VA_ARGS__) }
#define BTEvGv(...) if (opsz16) { EG_helper(false, true, 16, w, __VA_ARGS__) } else { EG_helper(false, true, 32, d, __VA_ARGS__) }

#define EGIb_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	u8 imm8; \
	if (mod == 3) { \
		TRY(fetch8(cpu, &imm8)); \
		INST ## SUFFIX(rm, reg, imm8, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(fetch8(cpu, &imm8)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, reg, imm8, laddr ## BIT, saddr ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	}

#define EvGvIb(...) if (opsz16) { EGIb_helper(16, w, __VA_ARGS__) } else { EGIb_helper(32, d, __VA_ARGS__) }

#define EGCL_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, reg, 1, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT, lreg8, sreg8) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, reg, 1, laddr ## BIT, saddr ## BIT, lreg ## BIT, sreg ## BIT, lreg8, sreg8) \
	}

#define EvGvCL(...) if (opsz16) { EGCL_helper(16, w, __VA_ARGS__) } else { EGCL_helper(32, d, __VA_ARGS__) }

#define EI_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	u ## BIT imm ## BIT; \
	if (mod == 3) { \
		TRY(fetch ## BIT(cpu, &imm ## BIT)); \
		INST ## SUFFIX(rm, imm ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(fetch ## BIT(cpu, &imm ## BIT)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, imm ## BIT, laddr ## BIT, saddr ## BIT, limm, 0) \
	}

#define EbIb(...) EI_helper(8, , __VA_ARGS__)
#define EvIv(...) if (opsz16) { EI_helper(16, w, __VA_ARGS__) } else { EI_helper(32, d, __VA_ARGS__) }

#define EIb_helper(BT, BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	u8 imm8; \
	u ## BIT imm ## BIT; \
	if (mod == 3) { \
		TRY(fetch8(cpu, &imm8)); \
		imm ## BIT = (s ## BIT) ((s8) imm8); \
		INST ## SUFFIX(rm, imm ## BIT, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(fetch8(cpu, &imm8)); \
		imm ## BIT = (s ## BIT) ((s8) imm8); \
		if (BT) addr += imm ## BIT / BIT * (BIT / 8); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, imm ## BIT, laddr ## BIT, saddr ## BIT, limm, 0) \
	}

#define EvIb(...) if (opsz16) { EIb_helper(false, 16, w, __VA_ARGS__) } else { EIb_helper(false, 32, d, __VA_ARGS__) }
#define BTEvIb(...) if (opsz16) { EIb_helper(true, 16, w, __VA_ARGS__) } else { EIb_helper(true, 32, d, __VA_ARGS__) }

#define E1_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, 1, lreg ## BIT, sreg ## BIT, limm, 0) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, 1, laddr ## BIT, saddr ## BIT, limm, 0) \
	}

#define Eb1(...) E1_helper(8, , __VA_ARGS__)
#define Ev1(...) if (opsz16) { E1_helper(16, w, __VA_ARGS__) } else { E1_helper(32, d, __VA_ARGS__) }

#define ECL_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(rm, 1, lreg ## BIT, sreg ## BIT, lreg8, sreg8) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(&meml, 1, laddr ## BIT, saddr ## BIT, lreg8, sreg8) \
	}

#define EbCL(...) ECL_helper(8, , __VA_ARGS__)
#define EvCL(...) if (opsz16) { ECL_helper(16, w, __VA_ARGS__) } else { ECL_helper(32, d, __VA_ARGS__) }

#define GE_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(reg, rm, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX(reg, &meml, lreg ## BIT, sreg ## BIT, laddr ## BIT, saddr ## BIT) \
	}

#define GbEb(...) GE_helper(8, , __VA_ARGS__)
#define GvEv(...) if (opsz16) { GE_helper(16, w, __VA_ARGS__) } else { GE_helper(32, d, __VA_ARGS__) }

#define GvM_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX(reg, rm, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		INST ## SUFFIX(reg, addr, lreg ## BIT, sreg ## BIT, limm, 0) \
	}
#define GvM(...) if (opsz16) { GvM_helper(16, w, __VA_ARGS__) } else { GvM_helper(32, d, __VA_ARGS__) }

#define GvMp_helper(BIT, SUFFIX, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) THROW0(EX_UD); \
	else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		INST ## SUFFIX(reg, addr, lreg ## BIT, sreg ## BIT, limm, 0) \
	}
#define GvMp(...) if (opsz16) { GvMp_helper(16, w, __VA_ARGS__) } else { GvMp_helper(32, d, __VA_ARGS__) }

#define GE_helper2(BIT, SUFFIX, BIT2, SUFFIX2, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST ## SUFFIX ## SUFFIX2(reg, rm, lreg ## BIT, sreg ## BIT, lreg ## BIT2, sreg ## BIT2) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate ## BIT2(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX ## SUFFIX2(reg, &meml, lreg ## BIT, sreg ## BIT, laddr ## BIT2, saddr ## BIT2) \
	}

#define GvEb(...) if (opsz16) { GE_helper2(16, w, 8, b, __VA_ARGS__) } else { GE_helper2(32, d, 8, b, __VA_ARGS__) }
#define GvEw(...) if (opsz16) { GE_helper2(16, w, 16, w, __VA_ARGS__) } else { GE_helper2(32, d, 16, w, __VA_ARGS__) }

#define GEI_helperI2(BIT, SUFFIX, BIT2, SUFFIX2, rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		u ## BIT2 imm ## BIT2; \
		TRY(fetch ## BIT2(cpu, &imm ## BIT2)); \
		INST ## SUFFIX ## I ## SUFFIX2(reg, rm, imm ## BIT2, lreg ## BIT, sreg ## BIT, lreg ## BIT, sreg ## BIT) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		u ## BIT2 imm ## BIT2; \
		TRY(fetch ## BIT2(cpu, &imm ## BIT2)); \
		TRY(translate ## BIT(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## SUFFIX ## I ## SUFFIX2(reg, &meml, imm ## BIT2, lreg ## BIT, sreg ## BIT, laddr ## BIT, saddr ## BIT) \
	}

#define GvEvIb(...) if (opsz16) { GEI_helperI2(16, w, 8, b, __VA_ARGS__) } else { GEI_helperI2(32, d, 8, b, __VA_ARGS__) }
#define GvEvIv(...) if (opsz16) { GEI_helperI2(16, w, 16, w, __VA_ARGS__) } else { GEI_helperI2(32, d, 32, d, __VA_ARGS__) }

#define ALIb(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(0, imm8, lreg8, sreg8, limm, 0)

#define AXIb(rwm, INST) \
	if (opsz16) { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## w(0, imm8, lreg16, sreg16, limm, 0) \
	} else { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## d(0, imm8, lreg32, sreg32, limm, 0) \
	}

#define IbAL(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm8, 0, limm, 0, lreg8, sreg8)

#define IbAX(rwm, INST) \
	if (opsz16) { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## w(imm8, 0, limm, 0, lreg16, sreg16) \
	} else { \
		u8 imm8; \
		TRY(fetch8(cpu, &imm8)); \
		INST ## d(imm8, 0, limm, 0, lreg32, sreg32) \
	}

#define DXAL(rwm, INST) \
	INST(2, 0, lreg16, sreg16, lreg8, sreg8)

#define DXAX(rwm, INST) \
	if (opsz16) { \
		INST ## w(2, 0, lreg16, sreg16, lreg16, sreg16) \
	} else { \
		INST ## d(2, 0, lreg16, sreg16, lreg32, sreg32) \
	}

#define ALDX(rwm, INST) \
	INST(0, 2, lreg8, sreg8, lreg16, sreg16)

#define AXDX(rwm, INST) \
	if (opsz16) { \
		INST ## w(0, 2, lreg16, sreg16, lreg16, sreg16) \
	} else { \
		INST ## d(0, 2, lreg32, sreg32, lreg16, sreg16) \
	}

#define AXIv(rwm, INST) \
	if (opsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w(0, imm16, lreg16, sreg16, limm, 0) \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d(0, imm32, lreg32, sreg32, limm, 0) \
	}

#define ALOb(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	TRY(translate8(cpu, &meml, rwm, curr_seg, addr)); \
	INST(0, &meml, lreg8, sreg8, laddr8, saddr8)

#define AXOv(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	if (opsz16) { \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## w(0, &meml, lreg16, sreg16, laddr16, saddr16) \
	} else { \
		TRY(translate32(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## d(0, &meml, lreg32, sreg32, laddr32, saddr32) \
	}

#define ObAL(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	TRY(translate8(cpu, &meml, rwm, curr_seg, addr)); \
	INST(&meml, 0, laddr8, saddr8, lreg8, sreg8)

#define OvAX(rwm, INST) \
	if (adsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	if (opsz16) { \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## w(&meml, 0, laddr16, saddr16, lreg16, sreg16) \
	} else { \
		TRY(translate32(cpu, &meml, rwm, curr_seg, addr)); \
		INST ## d(&meml, 0, laddr32, saddr32, lreg32, sreg32) \
	}

#define PlusRegv(rwm, INST) \
	if (opsz16) { \
		INST ## w((b1 & 7), lreg16, sreg16) \
	} else { \
		INST ## d((b1 & 7), lreg32, sreg32) \
	}

#define PlusRegIb(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST((b1 & 7), imm8, lreg8, sreg8, limm, 0)

#define PlusRegIv(rwm, INST) \
	if (opsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w((b1 & 7), imm16, lreg16, sreg16, limm, 0) \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d((b1 & 7), imm32, lreg32, sreg32, limm, 0) \
	}

#define Ib(rwm, INST) \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm8, limm, 0)
#define Jb Ib

#define Iw(rwm, INST) \
	u16 imm16; \
	TRY(fetch16(cpu, &imm16)); \
	INST(imm16, limm, 0)

#define IwIb(rwm, INST) \
	u16 imm16; \
	TRY(fetch16(cpu, &imm16)); \
	u8 imm8; \
	TRY(fetch8(cpu, &imm8)); \
	INST(imm16, imm8, limm, 0, limm, 0)

#define Iv(rwm, INST) \
	if (opsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w(imm16, limm, 0) \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d(imm32, limm, 0) \
	}

#define Jv(rwm, INST) \
	if (adsz16) { \
		u16 imm16; \
		TRY(fetch16(cpu, &imm16)); \
		INST ## w(imm16, limm, 0); \
	} else { \
		u32 imm32; \
		TRY(fetch32(cpu, &imm32)); \
		INST ## d(imm32, limm, 0); \
	}
#define Av Iv

#define Ap(rwm, INST) \
	u16 seg; \
	if (opsz16) { \
		u16 addr16; \
		TRY(fetch16(cpu, &addr16)); \
		addr = addr16; \
	} else { \
		TRY(fetch32(cpu, &addr)); \
	} \
	TRY(fetch16(cpu, &seg)); \
	INST(addr, seg)

#define Ep(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) THROW0(EX_UD); \
	else { \
		u16 seg; \
		u32 off; \
		OptAddr moff, mseg; \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		if (opsz16) { \
			TRY(translate16(cpu, &moff, rwm, curr_seg, addr)); \
			TRY(translate16(cpu, &mseg, rwm, curr_seg, addr + 2)); \
			off = laddr16(&moff); \
			seg = laddr16(&mseg); \
		} else { \
			TRY(translate32(cpu, &moff, rwm, curr_seg, addr)); \
			TRY(translate16(cpu, &mseg, rwm, curr_seg, addr + 4)); \
			off = laddr32(&moff); \
			seg = laddr16(&mseg); \
		} \
		INST(off, seg) \
	}

#define Ms(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) THROW0(EX_UD); \
	else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		INST(addr) \
	}

#define Ew(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		if (opsz16) { \
			INST(rm, lreg16, sreg16) \
		} else { \
			INST(rm, lreg32, sreg32) \
		} \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST(&meml, laddr16, saddr16) \
	}

#define EwSw(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		if (opsz16) { \
			INST(rm, reg, lreg16, sreg16, lseg, 0) \
		} else { \
			INST(rm, reg, lreg32, sreg32, lseg, 0) \
		} \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST(&meml, reg, laddr16, saddr16, lseg, 0) \
	}

#define SwEw(rwm, INST) \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	if (mod == 3) { \
		INST(reg, rm, lseg, 0, lreg16, sreg16) \
	} else { \
		TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
		TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); \
		INST(reg, &meml,lseg, 0, laddr16, saddr16) \
	}

#define limm(i) i
#ifdef I386_OPT1
#define lreg8(i) ((i) > 3 ? cpu->gprx[i - 4].r8[1] : cpu->gprx[i].r8[0])
#define sreg8(i, v) ((i) > 3 ? (cpu->gprx[i - 4].r8[1] = (v)) : (cpu->gprx[i].r8[0] = (v)))
#define lreg16(i) (cpu->gprx[i].r16)
#define sreg16(i, v) (cpu->gprx[i].r16 = (v))
#define lreg32(i) (REGi(i))
#define sreg32(i, v) ((REGi(i)) = (v))
#else
#define lreg8(i) ((u8) ((i) > 3 ? REGi((i) - 4) >> 8 : REGi((i))))
#define sreg8(i, v) ((i) > 3 ? \
		     (REGi((i) - 4) = (REGi((i) - 4) & (wordmask ^ 0xff00)) | (((v) & 0xff) << 8)) : \
		     (REGi((i)) = (REGi((i)) & (wordmask ^ 0xff)) | ((v) & 0xff)))
#define lreg16(i) ((u16) REGi((i)))
#define sreg16(i, v) (REGi((i)) = (REGi((i)) & (wordmask ^ 0xffff)) | ((v) & 0xffff))
#define lreg32(i) ((u32) REGi((i)))
#define sreg32(i, v) (REGi((i)) = (REGi((i)) & (wordmask ^ 0xffffffff)) | ((v) & 0xffffffff))
#endif
#define laddr8(addr) load8(cpu, addr)
#define saddr8(addr, v) store8(cpu, addr, v)
#define laddr16(addr) load16(cpu, addr)
#define saddr16(addr, v) store16(cpu, addr, v)
#define laddr32(addr) load32(cpu, addr)
#define saddr32(addr, v) store32(cpu, addr, v)
#define lseg(i) ((u16) SEGi((i)))
#define set_sp(v, mask) (sreg32(4, ((v) & mask) | (lreg32(4) & ~mask)))

/*
 * instructions
 */
#define ACOP_helper(NAME1, NAME2, BIT, OP, a, b, la, sa, lb, sb) \
	int cf = get_CF(cpu); \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lb(b)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 OP cpu->cc.src2 OP cf); \
	cpu->cc.op = cf ? CC_ ## NAME1 : CC_ ## NAME2; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define AOP0_helper(NAME, BIT, OP, a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lb(b)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 OP cpu->cc.src2); \
	cpu->cc.op = CC_ ## NAME; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF;

#define LOP0_helper(NAME, BIT, OP, a, b, la, sa, lb, sb) \
	cpu->cc.dst = sext ## BIT(la(a) OP lb(b)); \
	cpu->cc.op = CC_ ## NAME; \
	cpu->cc.mask = CF | PF | ZF | SF | OF;

#define AOP_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	AOP0_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	sa(a, cpu->cc.dst);

#define LOP_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	LOP0_helper(NAME1, BIT, OP, a, b, la, sa, lb, sb) \
	sa(a, cpu->cc.dst);

#define INCDEC_helper(NAME, BIT, OP, a, la, sa) \
	int cf = get_CF(cpu); \
	cpu->cc.dst = sext ## BIT(sext ## BIT(la(a)) OP 1); \
	cpu->cc.op = CC_ ## NAME ## BIT; \
	SET_BIT(cpu->flags, cf, CF); \
	cpu->cc.mask = PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define NEG_helper(BIT, a, la, sa) \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.dst = sext ## BIT(-cpu->cc.src1); \
	cpu->cc.op = CC_NEG ## BIT; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define ADCb(...) ACOP_helper(ADC, ADD,  8, +, __VA_ARGS__)
#define ADCw(...) ACOP_helper(ADC, ADD, 16, +, __VA_ARGS__)
#define ADCd(...) ACOP_helper(ADC, ADD, 32, +, __VA_ARGS__)
#define SBBb(...) ACOP_helper(SBB, SUB,  8, -, __VA_ARGS__)
#define SBBw(...) ACOP_helper(SBB, SUB, 16, -, __VA_ARGS__)
#define SBBd(...) ACOP_helper(SBB, SUB, 32, -, __VA_ARGS__)
#define ADDb(...) AOP_helper(ADD,  8, +, __VA_ARGS__)
#define ADDw(...) AOP_helper(ADD, 16, +, __VA_ARGS__)
#define ADDd(...) AOP_helper(ADD, 32, +, __VA_ARGS__)
#define SUBb(...) AOP_helper(SUB,  8, -, __VA_ARGS__)
#define SUBw(...) AOP_helper(SUB, 16, -, __VA_ARGS__)
#define SUBd(...) AOP_helper(SUB, 32, -, __VA_ARGS__)
#define ORb(...)  LOP_helper(OR,   8, |, __VA_ARGS__)
#define ORw(...)  LOP_helper(OR,  16, |, __VA_ARGS__)
#define ORd(...)  LOP_helper(OR,  32, |, __VA_ARGS__)
#define ANDb(...) LOP_helper(AND,  8, &, __VA_ARGS__)
#define ANDw(...) LOP_helper(AND, 16, &, __VA_ARGS__)
#define ANDd(...) LOP_helper(AND, 32, &, __VA_ARGS__)
#define XORb(...) LOP_helper(XOR,  8, ^, __VA_ARGS__)
#define XORw(...) LOP_helper(XOR, 16, ^, __VA_ARGS__)
#define XORd(...) LOP_helper(XOR, 32, ^, __VA_ARGS__)
#define CMPb(...)  AOP0_helper(SUB,  8, -, __VA_ARGS__)
#define CMPw(...)  AOP0_helper(SUB, 16, -, __VA_ARGS__)
#define CMPd(...)  AOP0_helper(SUB, 32, -, __VA_ARGS__)
#define TESTb(...) LOP0_helper(AND,  8, &, __VA_ARGS__)
#define TESTw(...) LOP0_helper(AND, 16, &, __VA_ARGS__)
#define TESTd(...) LOP0_helper(AND, 32, &, __VA_ARGS__)
#define INCb(...) INCDEC_helper(INC,  8, +, __VA_ARGS__)
#define INCw(...) INCDEC_helper(INC, 16, +, __VA_ARGS__)
#define INCd(...) INCDEC_helper(INC, 32, +, __VA_ARGS__)
#define DECb(...) INCDEC_helper(DEC,  8, -, __VA_ARGS__)
#define DECw(...) INCDEC_helper(DEC, 16, -, __VA_ARGS__)
#define DECd(...) INCDEC_helper(DEC, 32, -, __VA_ARGS__)
#define NOTb(a, la, sa) sa(a, ~la(a));
#define NOTw(a, la, sa) sa(a, ~la(a));
#define NOTd(a, la, sa) sa(a, ~la(a));
#define NEGb(...) NEG_helper(8,  __VA_ARGS__)
#define NEGw(...) NEG_helper(16, __VA_ARGS__)
#define NEGd(...) NEG_helper(32, __VA_ARGS__)

#define SHL_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = (lb(b)) & 0x1f; \
	if (y) { \
		cpu->cc.dst = sext ## BIT(x << y); \
		cpu->cc.dst2 = ((x >> (BIT - y)) & 1); \
		cpu->cc.op = CC_SHL; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHLb(...) SHL_helper(8, __VA_ARGS__)
#define SHLw(...) SHL_helper(16, __VA_ARGS__)
#define SHLd(...) SHL_helper(32, __VA_ARGS__)

#define ROL_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y0 = lb(b); \
	uword y = y0 & (BIT - 1); \
	uword res = x; \
	if (y) { \
		res = sext ## BIT((x << y) | (x >> (BIT - y))); \
		sa(a, res); \
	} \
	if (y0) { \
		int cf1 = res & 1; \
		int of1 = (res >> (sizeof(uword) * 8 - 1)) ^ cf1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
	}

#define ROLb(...) ROL_helper(8, __VA_ARGS__)
#define ROLw(...) ROL_helper(16, __VA_ARGS__)
#define ROLd(...) ROL_helper(32, __VA_ARGS__)

#define RCL_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = ((lb(b)) & 0x1f) % (BIT + 1); \
	if (y) { \
		uword cf = get_CF(cpu); \
		uword res = sext ## BIT((x << y) | (cf << (y - 1)) | (y != 1 ? (x >> (BIT + 1 - y)) : 0)); \
		int cf1 = (x >> (BIT - y)) & 1; \
		int of1 = (res >> (sizeof(uword) * 8 - 1)) ^ cf1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
		sa(a, res); \
	}

#define RCLb(...) RCL_helper(8, __VA_ARGS__)
#define RCLw(...) RCL_helper(16, __VA_ARGS__)
#define RCLd(...) RCL_helper(32, __VA_ARGS__)

#define RCR_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = ((lb(b)) & 0x1f) % (BIT + 1); \
	if (y) { \
		uword cf = get_CF(cpu); \
		uword res = sext ## BIT((x >> y) | (cf << (BIT - y)) | (y != 1 ? (x << (BIT + 1 - y)) : 0)); \
		int cf1 = (sext ## BIT(x << (BIT - y)) >> (BIT - 1)) & 1; \
		int of1 = ((res ^ (res << 1)) >> (BIT - 1)) & 1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
		sa(a, res); \
	}

#define RCRb(...) RCR_helper(8, __VA_ARGS__)
#define RCRw(...) RCR_helper(16, __VA_ARGS__)
#define RCRd(...) RCR_helper(32, __VA_ARGS__)

#define ROR_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y0 = lb(b); \
	uword y = y0 & (BIT - 1); \
	uword res = x; \
	if (y) { \
		res = sext ## BIT((x >> y) | (x << (BIT - y))); \
		sa(a, res); \
	} \
	if (y0) { \
		int cf1 = (res >> (BIT - 1)) & 1; \
		int of1 = ((res ^ (res << 1)) >> (BIT - 1)) & 1; \
		SET_BIT(cpu->flags, cf1, CF); \
		SET_BIT(cpu->flags, of1, OF); \
		cpu->cc.mask &= ~(CF | OF); \
	}

#define RORb(...) ROR_helper(8, __VA_ARGS__)
#define RORw(...) ROR_helper(16, __VA_ARGS__)
#define RORd(...) ROR_helper(32, __VA_ARGS__)

#define SHR_helper(BIT, a, b, la, sa, lb, sb) \
	uword x = la(a); \
	uword y = (lb(b)) & 0x1f; \
	if (y) { \
		cpu->cc.src1 = sext ## BIT(x); \
		cpu->cc.dst = sext ## BIT(x >> y); \
		cpu->cc.dst2 = (x >> (y - 1)) & 1; \
		cpu->cc.op = CC_SHR; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHRb(...) SHR_helper(8, __VA_ARGS__)
#define SHRw(...) SHR_helper(16, __VA_ARGS__)
#define SHRd(...) SHR_helper(32, __VA_ARGS__)

#define SHLD_helper(BIT, a, b, c, la, sa, lb, sb, lc, sc) \
	int count = (lc(c)) & 0x1f; \
	uword x = la(a); \
	uword y = lb(b); \
	if (count) { \
		cpu->cc.src1 = sext ## BIT(x); \
		if (BIT < count) {  /* undocumented */ \
			uword z = x; x = y; y = z; \
			count -= BIT; \
		} \
		cpu->cc.dst = sext ## BIT((x << count) | (y >> (BIT - count))); \
		if (count == 1) { \
			cpu->cc.dst2 = sext ## BIT(x); \
		} else { \
			cpu->cc.dst2 = sext ## BIT((x << (count - 1)) | (count == 1 ? 0 : (y >> (BIT - (count - 1))))); \
		} \
		cpu->cc.op = CC_SHLD; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHLDw(...) SHLD_helper(16, __VA_ARGS__)
#define SHLDd(...) SHLD_helper(32, __VA_ARGS__)

#define SHRD_helper(BIT, a, b, c, la, sa, lb, sb, lc, sc) \
	int count = (lc(c)) & 0x1f; \
	uword x = la(a); \
	uword y = lb(b); \
	if (count) { \
		if (BIT < count) {  /* undocumented */ \
			uword z = x; x = y; y = z; \
			count -= BIT; \
		} \
		cpu->cc.src1 = sext ## BIT(x); \
		cpu->cc.dst = sext ## BIT((x >> count) | (y << (BIT - count))); \
		if (count == 1) { \
			cpu->cc.dst2 = sext ## BIT(x); \
		} else { \
			cpu->cc.dst2 = sext ## BIT((x >> (count - 1)) | (y << (BIT - (count - 1)))); \
		} \
		cpu->cc.op = CC_SHRD; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SHRDw(...) SHRD_helper(16, __VA_ARGS__)
#define SHRDd(...) SHRD_helper(32, __VA_ARGS__)

// ">>"
#define SAR_helper(BIT, a, b, la, sa, lb, sb) \
	sword x = sext ## BIT(la(a)); \
	sword y = (lb(b)) & 0x1f; \
	if (y) { \
		cpu->cc.dst = x >> y; \
		cpu->cc.dst2 = (x >> (y - 1)) & 1; \
		cpu->cc.op = CC_SAR; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		sa(a, cpu->cc.dst); \
	}

#define SARb(...) SAR_helper(8, __VA_ARGS__)
#define SARw(...) SAR_helper(16, __VA_ARGS__)
#define SARd(...) SAR_helper(32, __VA_ARGS__)

#define IMUL2w(a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext16(la(a)); \
	cpu->cc.src2 = sext16(lb(b)); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL16; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2d(a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext32(la(a)); \
	cpu->cc.src2 = sext32(lb(b)); \
	int64_t res = (int64_t) (s32) cpu->cc.src1 * (int64_t) (s32) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_IMUL32; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2wI_helper(BIT, BITI, a, b, c, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(lb(b)); \
	cpu->cc.src2 = sext ## BITI(c); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL ## BIT; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2dI_helper(BIT, BITI, a, b, c, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(lb(b)); \
	cpu->cc.src2 = sext ## BITI(c); \
	int64_t res = (int64_t) (s32) cpu->cc.src1 * (int64_t) (s32) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_IMUL ## BIT; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sa(a, cpu->cc.dst);

#define IMUL2wIb(...) IMUL2wI_helper(16, 8, __VA_ARGS__)
#define IMUL2wIw(...) IMUL2wI_helper(16, 16, __VA_ARGS__)
#define IMUL2dIb(...) IMUL2dI_helper(32, 8, __VA_ARGS__)
#define IMUL2dId(...) IMUL2dI_helper(32, 32, __VA_ARGS__)

#define IMULb(a, la, sa) \
	cpu->cc.src1 = sext8(lreg8(0)); \
	cpu->cc.src2 = sext8(la(a)); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL8; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst);

#define IMULw(a, la, sa) \
	cpu->cc.src1 = sext16(lreg16(0)); \
	cpu->cc.src2 = sext16(la(a)); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_IMUL16; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst); \
	sreg16(2, (cpu->cc.dst >> 16));

#define IMULd(a, la, sa) \
	cpu->cc.src1 = sext32(lreg32(0)); \
	cpu->cc.src2 = sext32(la(a)); \
	int64_t res = (int64_t) (s32) cpu->cc.src1 * (int64_t) (s32) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_IMUL32; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg32(0, cpu->cc.dst); \
	sreg32(2, cpu->cc.dst2);

#define MULb(a, la, sa) \
	cpu->cc.src1 = lreg8(0); \
	cpu->cc.src2 = la(a); \
	cpu->cc.dst = sext16(cpu->cc.src1 * cpu->cc.src2); \
	cpu->cc.op = CC_MUL8; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst);

#define MULw(a, la, sa) \
	cpu->cc.src1 = lreg16(0); \
	cpu->cc.src2 = la(a); \
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2; \
	cpu->cc.op = CC_MUL16; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg16(0, cpu->cc.dst); \
	sreg16(2, (cpu->cc.dst >> 16));

#define MULd(a, la, sa) \
	cpu->cc.src1 = lreg32(0); \
	cpu->cc.src2 = la(a); \
	uint64_t res = (uint64_t) cpu->cc.src1 * (uint64_t) cpu->cc.src2; \
	cpu->cc.dst = res; \
	cpu->cc.dst2 = res >> 32; \
	cpu->cc.op = CC_MUL32; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sreg32(0, cpu->cc.dst); \
	sreg32(2, cpu->cc.dst2);

#define IDIVb(a, la, sa) \
	sword src1 = sext16(lreg16(0)); \
	sword src2 = sext8(la(a)); \
	if (src2 == 0) THROW0(EX_DE); \
	sword res = src1 / src2; \
	if (res > 127 || res < -128) THROW0(EX_DE); \
	sreg8(0, res); \
	sreg8(4, src1 % src2);

#define IDIVw(a, la, sa) \
	sword src1 = sext32(lreg16(0) | (lreg16(2)<< 16)); \
	sword src2 = sext16(la(a)); \
	if (src2 == 0) THROW0(EX_DE); \
	sword res = src1 / src2; \
	if (res > 32767 || res < -32768) THROW0(EX_DE); \
	sreg16(0, res); \
	sreg16(2, src1 % src2);

#define IDIVd(a, la, sa) \
	int64_t src1 = (((uint64_t) lreg32(2)) << 32) | lreg32(0); \
	int64_t src2 = (sword) (la(a));	\
	if (src2 == 0) THROW0(EX_DE); \
	int64_t res = src1 / src2; \
	if (res > 2147483647 || res < -2147483648) THROW0(EX_DE); \
	sreg32(0, res); \
	sreg32(2, src1 % src2);

#define DIVb(a, la, sa) \
	uword src1 = lreg16(0); \
	uword src2 = la(a); \
	if (src2 == 0) THROW0(EX_DE); \
	uword res = src1 / src2; \
	if (res > 0xff) THROW0(EX_DE); \
	/* bypass the Cyrix 5/2 test */ \
	if (src1 == 0x5 && src2 == 0x2) { cpu->cc.mask &= ~ZF; cpu->flags |= ZF; } \
	sreg8(0, res); \
	sreg8(4, src1 % src2);

#define DIVw(a, la, sa) \
	uword src1 = lreg16(0) | (lreg16(2)<< 16); \
	uword src2 = la(a); \
	if (src2 == 0) THROW0(EX_DE); \
	uword res = src1 / src2; \
	if (res > 0xffff) THROW0(EX_DE); \
	/* bypass the NexGen 0x5555/2 test */ \
	if (src1 == 0x5555 && src2 == 0x2) { cpu->cc.mask &= ~ZF; cpu->flags &= ~ZF; } \
	sreg16(0, res); \
	sreg16(2, src1 % src2);

#define DIVd(a, la, sa) \
	uint64_t src1 = (((uint64_t) lreg32(2)) << 32) | lreg32(0); \
	uint64_t src2 = la(a); \
	if (src2 == 0) THROW0(EX_DE); \
	uint64_t res = src1 / src2; \
	if (res > 0xffffffff) THROW0(EX_DE); \
	sreg32(0, res); \
	sreg32(2, src1 % src2);

#define BT_helper(BIT, a, b, la, sa, lb, sb) \
	int bb = lb(b) % BIT; \
	bool bit = (la(a) >> bb) & 1; \
	cpu->cc.mask &= ~CF; \
	SET_BIT(cpu->flags, bit, CF);

#define BTw(...) BT_helper(16, __VA_ARGS__)
#define BTd(...) BT_helper(32, __VA_ARGS__)

#define BTX_helper(BIT, OP, a, b, la, sa, lb, sb) \
	int bb = lb(b) % BIT; \
	bool bit = (la(a) >> bb) & 1; \
	sa(a, la(a) OP (1 << bb)); \
	cpu->cc.mask &= ~CF; \
	SET_BIT(cpu->flags, bit, CF);

#define BTSw(...) BTX_helper(16, |, __VA_ARGS__)
#define BTSd(...) BTX_helper(32, |, __VA_ARGS__)
#define BTRw(...) BTX_helper(16, & ~, __VA_ARGS__)
#define BTRd(...) BTX_helper(32, & ~, __VA_ARGS__)
#define BTCw(...) BTX_helper(16, ^, __VA_ARGS__)
#define BTCd(...) BTX_helper(32, ^, __VA_ARGS__)

#define BSF_helper(BIT, a, b, la, sa, lb, sb) \
	u ## BIT src = lb(b); \
	u ## BIT temp = 0; \
	cpu->cc.mask = 0; \
	if (src == 0) { \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
		while ((src & 1) == 0) { \
			temp++; \
			src >>= 1; \
		} \
		sa(a, temp); \
	}

#define BSFw(...) BSF_helper(16, __VA_ARGS__)
#define BSFd(...) BSF_helper(32, __VA_ARGS__)

#define BSR_helper(BIT, a, b, la, sa, lb, sb) \
	s ## BIT src = lb(b); \
	u ## BIT temp = BIT - 1; \
	cpu->cc.mask = 0; \
	if (src == 0) { \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
		while (src >= 0) { \
			temp--; \
			src <<= 1; \
		} \
		sa(a, temp); \
	}

#define BSRw(...) BSR_helper(16, __VA_ARGS__)
#define BSRd(...) BSR_helper(32, __VA_ARGS__)

#define MOVb(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVw(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVd(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVSeg(a, b, la, sa, lb, sb) \
	if (a == SEG_CS) THROW0(EX_UD); \
	if (a == SEG_SS) stepcount++; \
	TRY(set_seg(cpu, a, lb(b)));
#define MOVZXdb(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVZXwb(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVZXww(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVZXdw(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVSXdb(a, b, la, sa, lb, sb) sa(a, sext8(lb(b)));
#define MOVSXwb(a, b, la, sa, lb, sb) sa(a, sext8(lb(b)));
#define MOVSXww(a, b, la, sa, lb, sb) sa(a, lb(b));
#define MOVSXdw(a, b, la, sa, lb, sb) sa(a, sext16(lb(b)));

#define XCHG(a, b, la, sa, lb, sb) \
	uword tmp = lb(b); \
	sb(b, la(a)); \
	sa(a, tmp);
#define XCHGb(...) XCHG(__VA_ARGS__)
#define XCHGw(...) XCHG(__VA_ARGS__)
#define XCHGd(...) XCHG(__VA_ARGS__)

#define XCHGAX() \
	if (opsz16) { \
		int reg = b1 & 7; \
		uword tmp = lreg16(reg); \
		sreg16(reg, lreg16(0)); \
		sreg16(0, tmp); \
	} else { \
		int reg = b1 & 7; \
		uword tmp = lreg32(reg); \
		sreg32(reg, lreg32(0)); \
		sreg32(0, tmp); \
	}

#define LEAd(a, b, la, sa, lb, sb) \
	if (mod == 3) THROW0(EX_UD); \
	sa(a, lb(b));
#define LEAw LEAd

#define CBW_CWDE() \
	if (opsz16) sreg16(0, sext8(lreg8(0))); \
	else sreg32(0, sext16(lreg16(0)));

#define CWD_CDQ() \
	if (opsz16) sreg16(2, sext16(-(sext16(lreg16(0)) >> 31))); \
	else sreg32(2, sext32(-(sext32(lreg32(0)) >> 31)));

#define MOVFC() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	if (reg == 0) { \
		sreg32(rm, cpu->cr0); \
	} else if (reg == 2) { \
		sreg32(rm, cpu->cr2); \
	} else if (reg == 3) { \
		sreg32(rm, cpu->cr3); \
	} else if (reg == 4) { \
		sreg32(rm, 0); \
	} else THROW0(EX_UD);

#define MOVTC() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	if (reg == 0) { \
		u32 new_cr0 = lreg32(rm); \
		if ((new_cr0 ^ cpu->cr0) & (CR0_PG | CR0_WP | 1)) \
			tlb_clear(cpu); \
		if (cpu->fpu) new_cr0 |= 0x10; \
		cpu->cr0 = new_cr0; \
	} else if (reg == 2) { \
		cpu->cr2 = lreg32(rm); \
	} else if (reg == 3) { \
		cpu->cr3 = lreg32(rm); \
		tlb_clear(cpu); \
	} else if (reg == 4) { \
	} else THROW0(EX_UD);

/*
 * INT3 has to take the same route as INT n in virtual-8086 mode.
 *
 * Throwing #BP straight away bypassed the IOPL check that INT() does, so a
 * guest running under EMM386 got a breakpoint exception delivered to the
 * monitor instead of a software interrupt to reflect.  EMM386 does not
 * reflect an unexpected #BP - it halts with "has detected error #83 in an
 * application" - which is why several games (Summer Challenge among them)
 * died the moment EMM386 was loaded and were fine without it: in real mode
 * IVT[3] points at an IRET and the call is harmless.  They call INT 3 on
 * purpose, with arguments in SI/DI, as a hook for a resident program.
 *
 * The #GP is a fault, so cpu->ip must still point at the INT3 itself; only
 * the #BP path, a trap, advances to the next instruction.
 */
#define INT3() \
	if (cpu->flags & VM) { \
		if (get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
		uword oldip = cpu->ip; \
		cpu->ip = cpu->next_ip; \
		if (!call_isr(cpu, 3, false, 0)) { \
			cpu->ip = oldip; \
			return false; \
		} \
	} else { \
		cpu->ip = cpu->next_ip; \
		THROW0(EX_BP); \
	}

#define INTO() \
	if (get_OF(cpu)) { \
		cpu->ip = cpu->next_ip; \
		THROW0(EX_OF); \
	}

static bool call_isr(CPUI386 *cpu, int no, bool pusherr, int ext);

#define INT(i, li, _) \
	/*dolog("int %02x %08x %04x:%08x\n", li(i), REGi[0], SEGi(SEG_CS), cpu->ip);*/ \
	if ((cpu->flags & VM)) { \
		if(get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
	} \
	uword oldip = cpu->ip; \
	cpu->ip = cpu->next_ip; \
	if (!call_isr(cpu, li(i), false, 0)) { \
		cpu->ip = oldip; \
		return false; \
	}

#define IRET() \
	if ((cpu->cr0 & 1) && (!(cpu->flags & VM) || get_IOPL(cpu) < 3)) { \
		TRY(pmret(cpu, opsz16, 0, true)); \
        cpu->prefetch_base = (u32)-1; \
	} else { \
		OptAddr meml1, meml2, meml3; \
		uword sp = lreg32(4); \
		register uword newip; \
		if (opsz16) { \
			/* ip */ TRY(translate16(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr16(&meml1); \
			/* cs */ TRY(translate16(cpu, &meml2, 1, SEG_SS, (sp + 2) & sp_mask)); \
			int newcs = laddr16(&meml2); \
			/* flags */ TRY(translate16(cpu, &meml3, 1, SEG_SS, (sp + 4) & sp_mask)); \
			uword oldflags = cpu->flags; \
			if (cpu->flags & VM) cpu->flags = (cpu->flags & (0xffff0000 | IOPL)) | (laddr16(&meml3) & ~IOPL); \
			else cpu->flags = (cpu->flags & 0xffff0000) | laddr16(&meml3); \
			cpu->flags &= EFLAGS_MASK; \
			cpu->flags |= 0x2; \
			if (!set_seg(cpu, SEG_CS, newcs)) { cpu->flags = oldflags; return false; } \
			cpu->cc.mask = 0; \
			set_sp(sp + 6, sp_mask); \
		} else { \
			/* eip */ TRY(translate32(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr32(&meml1); \
			/* cs (pop as dword, selector is low 16 bits) */ \
			TRY(translate32(cpu, &meml2, 1, SEG_SS, (sp + 4) & sp_mask)); \
			int newcs = laddr32(&meml2); \
			/* eflags */ TRY(translate32(cpu, &meml3, 1, SEG_SS, (sp + 8) & sp_mask)); \
			uword oldflags = cpu->flags; \
			if (cpu->flags & VM) cpu->flags = (cpu->flags & IOPL) | (laddr32(&meml3) & ~IOPL); \
			else cpu->flags = laddr32(&meml3); \
			cpu->flags &= EFLAGS_MASK; \
			cpu->flags |= 0x2; \
			if (!set_seg(cpu, SEG_CS, newcs)) { cpu->flags = oldflags; return false; } \
			cpu->cc.mask = 0; \
			set_sp(sp + 12, sp_mask); \
		} \
		cpu->next_ip = newip; \
        cpu->prefetch_base = (u32)-1; \
	} \
	if (cpu->intr && (cpu->flags & IF)) return true;

#define RETFARw(i, li, _) \
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) { \
		TRY(pmret(cpu, opsz16, li(i), false)); \
	} else { \
		uword sp = lreg32(4); \
		OptAddr meml1, meml2; \
		register uword newip; \
		if (opsz16) { \
			/* ip */ TRY(translate16(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr16(&meml1); \
			/* cs */ TRY(translate16(cpu, &meml2, 1, SEG_SS, (sp + 2) & sp_mask)); \
			int newcs = laddr16(&meml2); \
			TRY(set_seg(cpu, SEG_CS, newcs)); \
			set_sp(sp + 4 + li(i), sp_mask); \
		} else { \
			/* ip */ TRY(translate32(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
			newip = laddr32(&meml1); \
			/* cs */ TRY(translate32(cpu, &meml2, 1, SEG_SS, (sp + 4) & sp_mask)); \
			int newcs = laddr32(&meml2); \
			TRY(set_seg(cpu, SEG_CS, newcs)); \
			set_sp(sp + 8 + li(i), sp_mask); \
		} \
		cpu->next_ip = newip; \
		cpu->prefetch_base = (u32)-1; \
	}

#define RETFAR() RETFARw(0, limm, 0)

#define HLT() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	cpu->halt = true; return true;
#define NOP()

#define LAHF() \
	refresh_flags(cpu); \
	cpu->cc.mask = 0; \
	sreg8(4, cpu->flags);

#define SAHF() \
	cpu->cc.mask &= OF; \
	cpu->flags = (cpu->flags & (wordmask ^ 0xff)) | lreg8(4); \
	cpu->flags &= EFLAGS_MASK; \
	cpu->flags |= 0x2;

#define CMC() \
	int cf = get_CF(cpu); \
	cpu->cc.mask &= ~CF; \
	SET_BIT(cpu->flags, !cf, CF);

#define CLC() \
	cpu->cc.mask &= ~CF; \
	cpu->flags &= ~CF;

#define STC() \
	cpu->cc.mask &= ~CF; \
	cpu->flags |= CF;

#define CLI() \
	if (get_IOPL(cpu) < cpu->cpl) THROW(EX_GP, 0); \
	cpu->flags &= ~IF;

/* STI: interrupts enabled at the end of the **next** instruction */
#define STI() \
	if (get_IOPL(cpu) < cpu->cpl) THROW(EX_GP, 0); \
	cpu->flags |= IF; \
	if (cpu->intr || stepcount < 2) stepcount = 2;

#define CLD() \
	cpu->flags &= ~DF;

#define STD() \
	cpu->flags |= DF;

#define PUSHb(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	uword val = sext8(la(a)); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
		set_sp(sp - 2, sp_mask); \
		saddr16(&meml1, val); \
	} else { \
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4, sp_mask); \
		saddr32(&meml1, val); \
	}

#define PUSHw(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	uword val = sext16(la(a)); \
	TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
	set_sp(sp - 2, sp_mask); \
	saddr16(&meml1, val);

#define PUSHd(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	uword val = sext32(la(a)); \
	TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
	set_sp(sp - 4, sp_mask); \
	saddr32(&meml1, val);

#define POPRegw(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	TRY(translate16(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
	u16 src = laddr16(&meml1); \
	set_sp(sp + 2, sp_mask); \
	sa(a, src);

#define POPRegd(a, la, sa) \
	OptAddr meml1; \
	uword sp = lreg32(4); \
	TRY(translate32(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
	u32 src = laddr32(&meml1); \
	set_sp(sp + 4, sp_mask); \
	sa(a, src);

#define POP_helper(BIT) \
	OptAddr meml1; \
	TRY(fetch8(cpu, &modrm)); \
	int mod = modrm >> 6; \
	int rm = modrm & 7; \
	uword sp = lreg32(4); \
	TRY(translate ## BIT(cpu, &meml1, 1, SEG_SS, sp & sp_mask)); \
	u ## BIT src = laddr ## BIT(&meml1); \
	set_sp(sp + BIT / 8, sp_mask); \
	if (mod == 3) { \
		sreg ## BIT(rm, src); \
	} else { \
		if (!modsib(cpu, adsz16, mod, rm, &addr, &curr_seg) || \
		    !translate ## BIT(cpu, &meml, 2, curr_seg, addr)) { \
			set_sp(sp, sp_mask); \
			return false; \
		} \
		saddr ## BIT(&meml, src); \
	}

#define POP() if (opsz16) { POP_helper(16) } else { POP_helper(32) }

#define PUSHF() \
	if ((cpu->flags & VM) && get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
		refresh_flags(cpu); \
		cpu->cc.mask = 0; \
		set_sp(sp - 2, sp_mask); \
		saddr16(&meml, cpu->flags); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
		refresh_flags(cpu); \
		cpu->cc.mask = 0; \
		set_sp(sp - 4, sp_mask); \
		saddr32(&meml, cpu->flags & ~(RF | VM)); \
	}

#define EFLAGS_MASK_386 0x37fd7
#define EFLAGS_MASK_486 0x77fd7
#define EFLAGS_MASK_586 0x277fd7
#define EFLAGS_MASK (cpu->flags_mask)

#define POPF() \
	if ((cpu->flags & VM) && get_IOPL(cpu) < 3) THROW(EX_GP, 0); \
	uword mask = VM; \
	if (cpu->cr0 & 1) { \
		if (cpu->cpl > 0) mask |= IOPL; \
		if (get_IOPL(cpu) < cpu->cpl) mask |= IF; \
	} \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 2, sp_mask); \
		cpu->flags = (cpu->flags & (0xffff0000 | mask)) | (laddr16(&meml) & ~mask); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 4, sp_mask); \
		cpu->flags = (cpu->flags & mask) | (laddr32(&meml) & ~mask); \
	} \
	cpu->flags &= EFLAGS_MASK; \
	cpu->flags |= 0x2; \
	cpu->cc.mask = 0; \
	if (cpu->intr && (cpu->flags & IF)) return true;

#define PUSHSeg(seg) \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
		set_sp(sp - 2, sp_mask); \
		saddr16(&meml, lseg(seg)); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4, sp_mask); \
		saddr32(&meml, lseg(seg)); \
	}
#define PUSH_ES() PUSHSeg(SEG_ES)
#define PUSH_CS() PUSHSeg(SEG_CS)
#define PUSH_SS() PUSHSeg(SEG_SS)
#define PUSH_DS() PUSHSeg(SEG_DS)
#define PUSH_FS() PUSHSeg(SEG_FS)
#define PUSH_GS() PUSHSeg(SEG_GS)

#define POPSeg(seg) \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		TRY(set_seg(cpu, seg, laddr16(&meml))); \
		set_sp(sp + 2, sp_mask); \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		TRY(set_seg(cpu, seg, laddr32(&meml))); \
		set_sp(sp + 4, sp_mask); \
	}
#define POP_ES() POPSeg(SEG_ES)
#define POP_SS() POPSeg(SEG_SS) stepcount++;
#define POP_DS() POPSeg(SEG_DS)
#define POP_FS() POPSeg(SEG_FS)
#define POP_GS() POPSeg(SEG_GS)

#define PUSHA_helper(BIT, BYTE) \
	uword sp = lreg32(4); \
	OptAddr meml1, meml2, meml3, meml4; \
	OptAddr meml5, meml6, meml7, meml8; \
	TRY(translate ## BIT(cpu, &meml1, 2, SEG_SS, (sp - BYTE * 1) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml2, 2, SEG_SS, (sp - BYTE * 2) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml3, 2, SEG_SS, (sp - BYTE * 3) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml4, 2, SEG_SS, (sp - BYTE * 4) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml5, 2, SEG_SS, (sp - BYTE * 5) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml6, 2, SEG_SS, (sp - BYTE * 6) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml7, 2, SEG_SS, (sp - BYTE * 7) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml8, 2, SEG_SS, (sp - BYTE * 8) & sp_mask)); \
	saddr ## BIT(&meml1, lreg ## BIT(0)); \
	saddr ## BIT(&meml2, lreg ## BIT(1)); \
	saddr ## BIT(&meml3, lreg ## BIT(2)); \
	saddr ## BIT(&meml4, lreg ## BIT(3)); \
	saddr ## BIT(&meml5, sp); \
	saddr ## BIT(&meml6, lreg ## BIT(5)); \
	saddr ## BIT(&meml7, lreg ## BIT(6)); \
	saddr ## BIT(&meml8, lreg ## BIT(7)); \
	set_sp(sp - BYTE * 8, sp_mask);
#define PUSHA() if (opsz16) { PUSHA_helper(16, 2) } else { PUSHA_helper(32, 4) }

#define POPA_helper(BIT, BYTE) \
	uword sp = lreg32(4); \
	OptAddr meml1, meml2, meml3, meml4; \
	OptAddr meml5, meml6, meml7; \
	TRY(translate ## BIT(cpu, &meml1, 1, SEG_SS, (sp + BYTE * 0) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml2, 1, SEG_SS, (sp + BYTE * 1) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml3, 1, SEG_SS, (sp + BYTE * 2) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml4, 1, SEG_SS, (sp + BYTE * 4) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml5, 1, SEG_SS, (sp + BYTE * 5) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml6, 1, SEG_SS, (sp + BYTE * 6) & sp_mask)); \
	TRY(translate ## BIT(cpu, &meml7, 1, SEG_SS, (sp + BYTE * 7) & sp_mask)); \
	sreg ## BIT(7, laddr ## BIT(&meml1)); \
	sreg ## BIT(6, laddr ## BIT(&meml2)); \
	sreg ## BIT(5, laddr ## BIT(&meml3)); \
	sreg ## BIT(3, laddr ## BIT(&meml4)); \
	sreg ## BIT(2, laddr ## BIT(&meml5)); \
	sreg ## BIT(1, laddr ## BIT(&meml6)); \
	sreg ## BIT(0, laddr ## BIT(&meml7)); \
	set_sp(sp + BYTE * 8, sp_mask);
#define POPA() if (opsz16) { POPA_helper(16, 2) } else { POPA_helper(32, 4) }

// string operations
#define stdi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 2, SEG_ES, lreg ## ABIT(7))); \
	saddr ## BIT(&meml, ax); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define ldsi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax = laddr ## BIT(&meml); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir);

#define lddi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, SEG_ES, lreg ## ABIT(7))); \
	ax = laddr ## BIT(&meml); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define ldsistdi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax = laddr ## BIT(&meml); \
	TRY(translate ## BIT(cpu, &meml, 2, SEG_ES, lreg ## ABIT(7))); \
	saddr ## BIT(&meml, ax); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define ldsilddi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax0 = laddr ## BIT(&meml); \
	TRY(translate ## BIT(cpu, &meml, 1, SEG_ES, lreg ## ABIT(7))); \
	ax = laddr ## BIT(&meml); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define xdir8 int dir = (cpu->flags & DF) ? -1 : 1;
#define xdir16 int dir = (cpu->flags & DF) ? -2 : 2;
#define xdir32 int dir = (cpu->flags & DF) ? -4 : 4;

#define STOS_helper2(BIT, ABIT) \
	OptAddr memld; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memld, 2, SEG_ES, lreg ## ABIT(7))); \
		if (memld.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				stdi(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int countd; \
		if (dir > 0) countd = (4096 - (memld.addr1 & 4095)) / (BIT / 8); \
		else countd = 1 + (memld.addr1 & 4095) / (BIT / 8); \
		if (countd < count) \
			count = countd; \
		for (uword i = 0; i <= count - 1; i++) { \
			saddr ## BIT(&memld, ax); \
			memld.addr1 += dir; \
		} \
		sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define STOS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax = REGi(0); \
	if (rep == 0) { \
		if (adsz16) { stdi(BIT, 16) } else { stdi(BIT, 32) } \
	} else { \
		if (adsz16) { STOS_helper2(BIT, 16) } else { STOS_helper2(BIT, 32) } \
	}

#define LODS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { ldsi(BIT, 16) } else { ldsi(BIT, 32) } \
		sreg ## BIT(0, ax); \
	} else { \
		if (adsz16) { \
			while (lreg16(1)) { \
				ldsi(BIT, 16) \
				sreg ## BIT(0, ax); \
				sreg16(1, lreg16(1) - 1); \
			} \
		} else { \
			while (lreg32(1)) { \
				ldsi(BIT, 32) \
				sreg ## BIT(0, ax); \
				sreg32(1, lreg32(1) - 1); \
			} \
		} \
	}

#define SCAS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax0 = REGi(0); \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { lddi(BIT, 16) } else { lddi(BIT, 32) } \
		cpu->cc.src1 = sext ## BIT(ax0); \
		cpu->cc.src2 = sext ## BIT(ax); \
		cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
		cpu->cc.op = CC_SUB; \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	} else { \
		if (adsz16) { \
			while (lreg16(1)) { \
				lddi(BIT, 16) \
				sreg16(1, lreg16(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} else { \
			while (lreg32(1)) { \
				lddi(BIT, 32) \
				sreg32(1, lreg32(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} \
	}

#define MOVS_helper2(BIT, ABIT) \
	OptAddr memls, memld; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memls, 1, curr_seg, lreg ## ABIT(6))); \
		TRY(translate ## BIT(cpu, &memld, 2, SEG_ES, lreg ## ABIT(7))); \
		if (memls.addr1 % (BIT / 8) || memld.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				ldsistdi(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int counts, countd; \
		if (dir > 0) { \
			counts = (4096 - (memls.addr1 & 4095)) / (BIT / 8); \
			countd = (4096 - (memld.addr1 & 4095)) / (BIT / 8); \
		} else { \
			counts = 1 + (memls.addr1 & 4095) / (BIT / 8); \
			countd = 1 + (memld.addr1 & 4095) / (BIT / 8); \
		} \
		if (counts < count) \
			count = counts; \
		if (countd < count) \
			count = countd; \
		if (cpu->cb.iomem_write_string && in_iomem(memld.addr1) && \
		    dir > 0  && in_iomem(memld.addr1 + count - 1) && \
		    (memls.addr1 | 4095) < cpu->phys_mem_size && \
		    !in_iomem(memls.addr1) && !in_iomem(memls.addr1 | 4095)) { \
			if (cpu->cb.iomem_write_string( \
				    cpu->cb.iomem, memld.addr1, \
				    cpu->phys_mem + memls.addr1, count * dir)) { \
				sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
				sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
				sreg ## ABIT(1, cx - count); \
				cx = lreg ## ABIT(1); \
				continue; \
			} \
		} \
		for (uword i = 0; i <= count - 1; i++) { \
			store ## BIT(cpu, &memld, load ## BIT(cpu, &memls)); \
			memld.addr1 += dir; \
			memls.addr1 += dir; \
		} \
		sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
		sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define MOVS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { ldsistdi(BIT, 16) } else { ldsistdi(BIT, 32) } \
	} else { \
		if (adsz16) { MOVS_helper2(BIT, 16) } else { MOVS_helper2(BIT, 32) } \
	}

#define CMPS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	xdir ## BIT \
	u ## BIT ax0, ax; \
	if (rep == 0) { \
		if (adsz16) { ldsilddi(BIT, 16) } else { ldsilddi(BIT, 32) } \
		cpu->cc.src1 = sext ## BIT(ax0); \
		cpu->cc.src2 = sext ## BIT(ax); \
		cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
		cpu->cc.op = CC_SUB; \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	} else { \
		if (adsz16) { \
			while (lreg16(1)) { \
				ldsilddi(BIT, 16) \
				sreg16(1, lreg16(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} else { \
			while (lreg32(1)) { \
				ldsilddi(BIT, 32) \
				sreg32(1, lreg32(1) - 1); \
				cpu->cc.src1 = sext ## BIT(ax0); \
				cpu->cc.src2 = sext ## BIT(ax); \
				cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
				cpu->cc.op = CC_SUB; \
				cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
				bool zf = get_ZF(cpu); \
				if ((zf && rep == 2) || (!zf && rep == 1)) break; \
			} \
		} \
	}

#define STOSb() STOS_helper(8)
#define LODSb() LODS_helper(8)
#define SCASb() SCAS_helper(8)
#define MOVSb() MOVS_helper(8)
#define CMPSb() CMPS_helper(8)
#define STOS() if (opsz16) { STOS_helper(16) } else { STOS_helper(32) }
#define LODS() if (opsz16) { LODS_helper(16) } else { LODS_helper(32) }
#define SCAS() if (opsz16) { SCAS_helper(16) } else { SCAS_helper(32) }
#define MOVS() if (opsz16) { MOVS_helper(16) } else { MOVS_helper(32) }
#define CMPS() if (opsz16) { CMPS_helper(16) } else { CMPS_helper(32) }

#define indxstdi(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 2, SEG_ES, lreg ## ABIT(7))); \
	ax = cpu->cb.io_read ## BIT(cpu->cb.io, lreg16(2)); \
	saddr ## BIT(&meml, ax); \
	sreg ## ABIT(7, lreg ## ABIT(7) + dir);

#define INS_helper2(BIT, ABIT) \
	OptAddr memld; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memld, 2, SEG_ES, lreg ## ABIT(7))); \
		if (memld.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				indxstdi(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int countd; \
		if (dir > 0) countd = (4096 - (memld.addr1 & 4095)) / (BIT / 8); \
		else countd = 1 + (memld.addr1 & 4095) / (BIT / 8); \
		if (countd < count) \
			count = countd; \
		if (cpu->cb.io_read_string && dir > 0 && \
		    (memld.addr1 | 4095) < cpu->phys_mem_size && \
		    !in_iomem(memld.addr1) && !in_iomem(memld.addr1 | 4095)) { \
			int count1 = cpu->cb.io_read_string( \
				cpu->cb.io, lreg16(2), \
				cpu->phys_mem + memld.addr1, dir, count); \
			if (count1 > 0) { \
				frank_diag_wp_range(memld.addr1, \
					count1 * (BIT / 8), 0x1451u, lreg16(2)); \
				count = count1; \
				sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
				sreg ## ABIT(1, cx - count); \
				cx = lreg ## ABIT(1); \
				continue; \
			} \
		} \
		for (uword i = 0; i <= count - 1; i++) { \
			ax = cpu->cb.io_read ## BIT(cpu->cb.io, lreg16(2)); \
			saddr ## BIT(&memld, ax); \
			memld.addr1 += dir; \
		} \
		sreg ## ABIT(7, lreg ## ABIT(7) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define INS_helper(BIT) \
	TRY(check_ioperm(cpu, lreg16(2), BIT)); \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { indxstdi(BIT, 16) } else { indxstdi(BIT, 32) } \
	} else { \
		if (rep != 1 && rep != 2) THROW0(EX_UD); \
		if (adsz16) { INS_helper2(BIT, 16) } else { INS_helper2(BIT, 32) } \
	}

#define INSb() INS_helper(8)
#define INS() if (opsz16) { INS_helper(16) } else { INS_helper(32) }

#define ldsioutdx(BIT, ABIT) \
	TRY(translate ## BIT(cpu, &meml, 1, curr_seg, lreg ## ABIT(6))); \
	ax = laddr ## BIT(&meml); \
	cpu->cb.io_write ## BIT(cpu->cb.io, lreg16(2), ax); \
	sreg ## ABIT(6, lreg ## ABIT(6) + dir);

#define OUTS_helper2(BIT, ABIT) \
	OptAddr memls; \
	uword cx = lreg ## ABIT(1); \
	while (cx) { \
		TRY(translate ## BIT(cpu, &memls, 1, curr_seg, lreg ## ABIT(6))); \
		if (memls.addr1 % (BIT / 8)) { \
			/* slow path */ \
			while (lreg ## ABIT(1)) { \
				ldsioutdx(BIT, ABIT) \
				sreg ## ABIT(1, lreg ## ABIT(1) - 1); \
			} \
			break; \
		} \
		uword count = cx; \
		int counts; \
		if (dir > 0) counts = (4096 - (memls.addr1 & 4095)) / (BIT / 8); \
		else counts = 1 + (memls.addr1 & 4095) / (BIT / 8); \
		if (counts < count) \
			count = counts; \
		if (cpu->cb.io_write_string && dir > 0 && \
		    (memls.addr1 | 4095) < cpu->phys_mem_size && \
		    !in_iomem(memls.addr1) && !in_iomem(memls.addr1 | 4095)) { \
			int count1 = cpu->cb.io_write_string( \
				cpu->cb.io, lreg16(2), \
				cpu->phys_mem + memls.addr1, dir, count); \
			if (count1 > 0) { \
				count = count1; \
				sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
				sreg ## ABIT(1, cx - count); \
				cx = lreg ## ABIT(1); \
				continue; \
			} \
		} \
		for (uword i = 0; i <= count - 1; i++) { \
			ax = laddr ## BIT(&memls); \
			cpu->cb.io_write ## BIT(cpu->cb.io, lreg16(2), ax); \
			memls.addr1 += dir; \
		} \
		sreg ## ABIT(6, lreg ## ABIT(6) + count * dir); \
		sreg ## ABIT(1, cx - count); \
		cx = lreg ## ABIT(1); \
	}

#define OUTS_helper(BIT) \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	TRY(check_ioperm(cpu, lreg16(2), BIT)); \
	xdir ## BIT \
	u ## BIT ax; \
	if (rep == 0) { \
		if (adsz16) { ldsioutdx(BIT, 16) } else { ldsioutdx(BIT, 32) } \
	} else { \
		if (rep != 1 && rep != 2) THROW0(EX_UD); \
		if (adsz16) { \
			OUTS_helper2(BIT, 16) \
		} else { \
			OUTS_helper2(BIT, 32) \
		} \
	}

#define OUTSb() OUTS_helper(8)
#define OUTS() if (opsz16) { OUTS_helper(16) } else { OUTS_helper(32) }

/*
 * Native JIT dispatch is attached to actual taken backward branches instead
 * of the cpu_exec1 loop head.  This is the key difference from v3:
 * sequential instructions pay exactly zero JIT-dispatch instructions.
 *
 * stepcount is cpu_exec1's remaining instruction budget and is intentionally
 * referenced by this macro at its expansion sites.
 */
#if NATIVE_JIT
#define NJ_HOT_BACKEDGE(d) do { \
	if ((sword)(d) < 0 && stepcount > 0) { \
		int _nj_done = nj_try_execute(cpu, stepcount); \
		if (_nj_done > 0) stepcount -= _nj_done; \
	} \
} while (0)
#else
#define NJ_HOT_BACKEDGE(d) do { (void)(d); } while (0)
#endif

#define JCXZb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		if (lreg16(1) == 0) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		if (lreg32(1) == 0) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define LOOPb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		sreg16(1, lreg16(1) - 1); \
		if (lreg16(1)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		sreg32(1, lreg32(1) - 1); \
		if (lreg32(1)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define LOOPEb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		sreg16(1, lreg16(1) - 1); \
		if (lreg16(1) && get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		sreg32(1, lreg32(1) - 1); \
		if (lreg32(1) && get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define LOOPNEb(i, li, _) \
	sword d = sext8(li(i)); \
	if (adsz16) { \
		sreg16(1, lreg16(1) - 1); \
		if (lreg16(1) && !get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	} else { \
		sreg32(1, lreg32(1) - 1); \
		if (lreg32(1) && !get_ZF(cpu)) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); } \
	}

#define COND() \
	int cond; \
	switch(b1 & 0xf) { \
	case 0x0: cond =  get_OF(cpu); break; \
	case 0x1: cond = !get_OF(cpu); break; \
	case 0x2: cond =  get_CF(cpu); break; \
	case 0x3: cond = !get_CF(cpu); break; \
	case 0x4: cond =  get_ZF(cpu); break; \
	case 0x5: cond = !get_ZF(cpu); break; \
	case 0x6: cond =  get_ZF(cpu) ||  get_CF(cpu); break; \
	case 0x7: cond = !get_ZF(cpu) && !get_CF(cpu); break; \
	case 0x8: cond =  get_SF(cpu); break; \
	case 0x9: cond = !get_SF(cpu); break; \
	case 0xa: cond =  get_PF(cpu); break; \
	case 0xb: cond = !get_PF(cpu); break; \
	case 0xc: cond =  get_SF(cpu) != get_OF(cpu); break; \
	case 0xd: cond =  get_SF(cpu) == get_OF(cpu); break; \
	case 0xe: cond =  get_ZF(cpu) || get_SF(cpu) != get_OF(cpu); break; \
	case 0xf: cond = !get_ZF(cpu) && get_SF(cpu) == get_OF(cpu); break; \
	}

#define JCC_common(d) \
	COND() \
	if (cond) { cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d); }

#define SETCCb(a, la, sa) \
	COND() \
	sa(a, cond);

#define JCCb(i, li, _) \
	sword d = sext8(li(i)); \
	JCC_common(d)

#define JCCw(i, li, _) \
	sword d = sext16(li(i)); \
	JCC_common(d)

#define JCCd(i, li, _) \
	sword d = sext32(li(i)); \
	JCC_common(d)

#define JMPb(i, li, _) \
	sword d = sext8(li(i)); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d);

#define JMPw(i, li, _) \
	sword d = sext16(li(i)); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d);

#define JMPd(i, li, _) \
	sword d = sext32(li(i)); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1; NJ_HOT_BACKEDGE(d);

#define JMPABSw(i, li, _) \
	cpu->next_ip = li(i); cpu->prefetch_base = (u32)-1;

#define JMPABSd(i, li, _) \
	cpu->next_ip = li(i); cpu->prefetch_base = (u32)-1;

#define JMPFAR(addr, seg) \
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) { \
		TRY(pmcall(cpu, opsz16, addr, seg, true)); \
	} else { \
	    TRY(set_seg(cpu, SEG_CS, seg)); \
	    cpu->next_ip = addr; \
	} \
	cpu->prefetch_base = (u32)-1;

#define CALLFAR(addr, seg) \
	if ((cpu->cr0 & 1) && !(cpu->flags & VM)) { \
		TRY(pmcall(cpu, opsz16, addr, seg, false)); \
	} else { \
	OptAddr meml1, meml2; \
	uword sp = lreg32(4); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
		TRY(translate16(cpu, &meml2, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4, sp_mask); \
		saddr16(&meml1, cpu->seg[SEG_CS].sel); \
		saddr16(&meml2, cpu->next_ip); \
	} else { \
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
		TRY(translate32(cpu, &meml2, 2, SEG_SS, (sp - 8) & sp_mask)); \
		set_sp(sp - 8, sp_mask); \
		saddr32(&meml1, cpu->seg[SEG_CS].sel); \
		saddr32(&meml2, cpu->next_ip); \
	} \
	TRY(set_seg(cpu, SEG_CS, seg)); \
	cpu->next_ip = addr; \
	} \
	cpu->prefetch_base = (u32)-1;

#define CALLw(i, li, _) \
	sword d = sext16(li(i)); \
	uword sp = lreg32(4); \
	TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
	set_sp(sp - 2, sp_mask); \
	saddr16(&meml, cpu->next_ip); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1;

#define CALLd(i, li, _) \
	sword d = sext32(li(i)); \
	uword sp = lreg32(4); \
	TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
	set_sp(sp - 4, sp_mask); \
	saddr32(&meml, cpu->next_ip); \
	cpu->next_ip += d; cpu->prefetch_base = (u32)-1;

#define CALLABSw(i, li, _) \
	uword nip = li(i); \
	uword sp = lreg32(4); \
	TRY(translate16(cpu, &meml, 2, SEG_SS, (sp - 2) & sp_mask)); \
	set_sp(sp - 2, sp_mask); \
	saddr16(&meml, cpu->next_ip); \
	cpu->next_ip = nip; cpu->prefetch_base = (u32)-1;

#define CALLABSd(i, li, _) \
	uword nip = li(i); \
	uword sp = lreg32(4); \
	TRY(translate32(cpu, &meml, 2, SEG_SS, (sp - 4) & sp_mask)); \
	set_sp(sp - 4, sp_mask); \
	saddr32(&meml, cpu->next_ip); \
	cpu->next_ip = nip; cpu->prefetch_base = (u32)-1;

#define FRANK_RET_WATCH() \
	{ \
		uint32_t dssp = cpu->seg[SEG_SS].base + (sp & sp_mask); \
		const uint8_t *dstk = (dssp >= 32 && \
			dssp + 32 <= (uint32_t)cpu->phys_mem_size) ? \
			cpu->phys_mem + dssp - 32 : 0; \
		frank_diag_ret(cpu->next_ip, cpu->seg[SEG_CS].base, cpu->ip, \
			       dssp, dstk); \
	}

#define RETw(i, li, _) \
	if (opsz16) { \
		uword sp = lreg32(4); \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 2 + li(i), sp_mask); \
		cpu->next_ip = laddr16(&meml); \
		FRANK_RET_WATCH() \
	} else { \
		uword sp = lreg32(4); \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 4 + li(i), sp_mask); \
		cpu->next_ip = laddr32(&meml); \
		FRANK_RET_WATCH() \
	} \
	cpu->prefetch_base = (u32)-1;

#define RET() RETw(0, limm, 0)

static bool __not_in_flash_func(enter_helper)(
	CPUI386 *cpu,
	bool opsz16,
	uword sp_mask,
	int level,
	int allocsz
) {
	assert(level != 0);
	uword temp;
	OptAddr meml1, memsrc;

	uword sp = lreg32(4);
	if (opsz16) {
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask));
		sp = (sp - 2) & sp_mask;
		set_sp(sp, sp_mask);
		saddr16(&meml1, lreg16(5));
		temp = lreg16(4);
	} else {
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask));
		sp = (sp - 4) & sp_mask;
		set_sp(sp, sp_mask);
		saddr32(&meml1, lreg32(5));
		temp = lreg32(4);
	}

	for (int i = 0; i < level - 1; i++) {
		if (opsz16) {
			if (sp_mask == 0xffff) {
				sreg16(5, lreg16(5) - 2);
			} else {
				sreg32(5, lreg32(5) - 2);
			}
			/* push word ptr [SS:BP] */
			TRY(translate16(cpu, &memsrc, 1, SEG_SS, lreg16(5) & sp_mask));
			sp = (sp - 2) & sp_mask;
			TRY(translate16(cpu, &meml1, 2, SEG_SS, sp));
			set_sp(sp, sp_mask);
			saddr16(&meml1, laddr16(&memsrc));
		} else {
			if (sp_mask == 0xffff) {
				sreg16(5, lreg16(5) - 4);
			} else {
				sreg32(5, lreg32(5) - 4);
			}
			/* push dword ptr [SS:EBP] */
			TRY(translate32(cpu, &memsrc, 1, SEG_SS, lreg32(5) & sp_mask));
			sp = (sp - 4) & sp_mask;
			TRY(translate32(cpu, &meml1, 2, SEG_SS, sp));
			set_sp(sp, sp_mask);
			saddr32(&meml1, laddr32(&memsrc));
		}
	}

	if (opsz16) {
		sp = (sp - 2) & sp_mask;
		TRY(translate16(cpu, &meml1, 2, SEG_SS, sp));
		set_sp((sp - allocsz) & sp_mask, sp_mask);
		saddr16(&meml1, temp);
		sreg16(5, temp);
	} else {
		sp = (sp - 4) & sp_mask;
		TRY(translate32(cpu, &meml1, 2, SEG_SS, sp));
		set_sp((sp - allocsz) & sp_mask, sp_mask);
		saddr32(&meml1, temp);
		sreg32(5, temp);
	}
	return true;
}

#define ENTER(i16, i8, l16, s16, l8, s8) \
	OptAddr meml1; \
	int level = l8(i8) % 32; \
	if (level == 0) { \
	uword sp = lreg32(4); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask)); \
		set_sp(sp - 2 - l16(i16), sp_mask); \
		saddr16(&meml1, lreg16(5)); \
		sreg16(5, (sp - 2) & sp_mask); \
	} else { \
		TRY(translate32(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask)); \
		set_sp(sp - 4 - l16(i16), sp_mask); \
		saddr32(&meml1, lreg32(5)); \
		sreg32(5, (sp - 4) & sp_mask); \
	} \
	} else { \
		TRY(enter_helper(cpu, opsz16, sp_mask, level, l16(i16))); \
	}

#define LEAVE() \
	uword sp = lreg32(5); \
	if (opsz16) { \
		TRY(translate16(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 2, sp_mask); \
		sreg16(5, laddr16(&meml)); \
	} else { \
		TRY(translate32(cpu, &meml, 1, SEG_SS, sp & sp_mask)); \
		set_sp(sp + 4, sp_mask); \
		sreg32(5, laddr32(&meml)); \
	}

#define SXXX(addr) \
	OptAddr meml1, meml2; \
	TRY(translate16(cpu, &meml1, 2, curr_seg, addr)); \
	TRY(translate32(cpu, &meml2, 2, curr_seg, addr + 2)); \

#define SGDT(addr) \
	SXXX(addr) \
	store16(cpu, &meml1, cpu->gdt.limit); \
	store32(cpu, &meml2, cpu->gdt.base);

#define SIDT(addr) \
	SXXX(addr) \
	store16(cpu, &meml1, cpu->idt.limit); \
	store32(cpu, &meml2, cpu->idt.base);

#define LXXX(addr) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	OptAddr meml1, meml2; \
	TRY(translate16(cpu, &meml1, 1, curr_seg, addr)); \
	TRY(translate32(cpu, &meml2, 1, curr_seg, addr + 2)); \
	u16 limit = load16(cpu, &meml1); \
	u32 base = load32(cpu, &meml2); \
	if (opsz16) base &= 0xffffff;

#define LGDT(addr) \
	LXXX(addr) \
	cpu->gdt.base = base; \
	cpu->gdt.limit = limit;

#define LIDT(addr) \
	LXXX(addr) \
	cpu->idt.base = base; \
	cpu->idt.limit = limit;

#define LLDT(a, la, sa) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(set_seg(cpu, SEG_LDT, la(a)));

#define SLDT(a, la, sa) \
	sa(a, cpu->seg[SEG_LDT].sel);

#define LTR(a, la, sa) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	TRY(set_seg(cpu, SEG_TR, la(a)));

#define STR(a, la, sa) \
	sa(a, cpu->seg[SEG_TR].sel);

#define MOVFD() \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	sreg32(rm, cpu->dr[reg]);

#define MOVTD() \
	TRY(fetch8(cpu, &modrm)); \
	int reg = (modrm >> 3) & 7; \
	int rm = modrm & 7; \
	cpu->dr[reg] = lreg32(rm);

#define MOVFT() \
	TRY(fetch8(cpu, &modrm));
#define MOVTT() \
	TRY(fetch8(cpu, &modrm));

#define SMSW(addr, laddr, saddr) \
	saddr(addr, cpu->cr0 & 0xffff);

#define LMSW(addr, laddr, saddr) \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	cpu->cr0 = (cpu->cr0 & ((~0xf) | 1)) | (laddr(addr) & 0xf);

#define LSEGd(NAME, reg, addr, lreg32, sreg32, laddr32, saddr32) \
	OptAddr meml1, meml2; \
	if (adsz16) addr = addr & 0xffff; \
	TRY(translate32(cpu, &meml1, 1, curr_seg, addr)); \
	TRY(translate16(cpu, &meml2, 1, curr_seg, addr + 4)); \
	u32 r = load32(cpu, &meml1); \
	u32 s = load16(cpu, &meml2); \
	TRY(set_seg(cpu, SEG_ ## NAME, s)); \
	sreg32(reg, r);

#define LSEGw(NAME, reg, addr, lreg16, sreg16, laddr16, saddr16) \
	OptAddr meml1, meml2; \
	if (adsz16) addr = addr & 0xffff; \
	TRY(translate16(cpu, &meml1, 1, curr_seg, addr)); \
	TRY(translate16(cpu, &meml2, 1, curr_seg, addr + 2)); \
	u32 r = load16(cpu, &meml1); \
	u32 s = load16(cpu, &meml2); \
	TRY(set_seg(cpu, SEG_ ## NAME, s)); \
	sreg16(reg, r);

#define LESd(...) LSEGd(ES, __VA_ARGS__)
#define LSSd(...) LSEGd(SS, __VA_ARGS__)
#define LDSd(...) LSEGd(DS, __VA_ARGS__)
#define LFSd(...) LSEGd(FS, __VA_ARGS__)
#define LGSd(...) LSEGd(GS, __VA_ARGS__)
#define LESw(...) LSEGw(ES, __VA_ARGS__)
#define LSSw(...) LSEGw(SS, __VA_ARGS__)
#define LDSw(...) LSEGw(DS, __VA_ARGS__)
#define LFSw(...) LSEGw(FS, __VA_ARGS__)
#define LGSw(...) LSEGw(GS, __VA_ARGS__)

static bool __not_in_flash_func( check_ioperm )(CPUI386 *cpu, int port, int bit)
{
	bool allow = true;
	if ((cpu->cr0 & 1) && (cpu->cpl > get_IOPL(cpu) || (cpu->flags & VM))) {
		allow = false;
		if (cpu->seg[SEG_TR].limit >= 103) {
			OptAddr meml;
			TRY(translate(cpu, &meml, 1, SEG_TR, 102, 2, 0));
			u32 iobase = load16(cpu, &meml);
			if (iobase + port / 8 < cpu->seg[SEG_TR].limit) {
				TRY(translate(cpu, &meml, 1, SEG_TR, iobase + port / 8, 2, 0));
				u16 perm = load16(cpu, &meml);
				int len = bit / 8;
				unsigned bit_index = port & 0x7;
				unsigned mask = (1 << len) - 1;
				if (!((perm >> bit_index) & mask))
					allow = true;
			}
		}
	}

	if (!allow) THROW(EX_GP, 0);
	return true;
}

#define INb(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	TRY(check_ioperm(cpu, port, 8)); \
	sa(a, cpu->cb.io_read8(cpu->cb.io, port));

#define INw(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	TRY(check_ioperm(cpu, port, 16)); \
	sa(a, cpu->cb.io_read16(cpu->cb.io, port));

#define INd(a, b, la, sa, lb, sb) \
	int port = lb(b); \
	TRY(check_ioperm(cpu, port, 32)); \
	sa(a, cpu->cb.io_read32(cpu->cb.io, port));

#define OUTb(a, b, la, sa, lb, sb) \
	int port = la(a); \
	TRY(check_ioperm(cpu, port, 8)); \
	cpu->cb.io_write8(cpu->cb.io, port, lb(b));

#define OUTw(a, b, la, sa, lb, sb) \
	int port = la(a); \
	TRY(check_ioperm(cpu, port, 16)); \
	cpu->cb.io_write16(cpu->cb.io, port, lb(b));

#define OUTd(a, b, la, sa, lb, sb) \
	int port = la(a); \
	TRY(check_ioperm(cpu, port, 32)); \
	cpu->cb.io_write32(cpu->cb.io, port, lb(b));

#define CLTS() \
	cpu->cr0 &= ~(1 << 3);

#define ESC() \
	if (cpu->cr0 & 0xc) THROW0(EX_NM); \
	else { \
		TRY(fetch8(cpu, &modrm)); \
		int mod = modrm >> 6; \
		int rm = modrm & 7; \
		int op = b1 - 0xd8; \
		int group = (modrm >> 3) & 7; \
		if (mod != 3) { \
			TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg)); \
			if (cpu->fpu) { \
				TRY(fpu_exec2(cpu->fpu, cpu, opsz16, op, group, curr_seg, addr)); \
			} \
		} else { \
			int reg = modrm & 7; \
			if (cpu->fpu) { \
				TRY(fpu_exec1(cpu->fpu, cpu, op, group, reg)); \
			} \
		} \
	}

#define WAIT() \
	if ((cpu->cr0 & 0xa) == 0xa) THROW0(EX_NM);

// ...
#define AAD(i, li, _) \
	u8 al = lreg8(0); \
	u8 ah = lreg8(4); \
	u8 imm = li(i); \
	u8 res = al + ah * imm; \
	sreg8(0, res); \
	sreg8(4, 0); \
	cpu->flags &= ~(OF | AF | CF); /* undocumented */ \
	cpu->cc.dst = sext8(res); \
	cpu->cc.mask = ZF | SF | PF;

#define AAM(i, li, _) \
	u8 al = lreg8(0); \
	u8 imm = li(i); \
	u8 res = al % imm; \
	sreg8(4, al / imm); \
	sreg8(0, res); \
	cpu->flags &= ~(OF | AF | CF); /* undocumented */ \
	cpu->cc.dst = sext8(res); \
	cpu->cc.mask = ZF | SF | PF;

#define SALC() \
	if (get_CF(cpu)) sreg8(0, 0xff); else sreg8(0, 0x00);

#define XLAT() \
	if (curr_seg == -1) curr_seg = SEG_DS; \
	if (adsz16) { \
		addr = lreg16(3) + lreg8(0); \
		addr &= 0xffff; \
		TRY(translate8(cpu, &meml, 1, curr_seg, addr)); \
		sreg8(0, laddr8(&meml)); \
	} else { \
		addr = lreg32(3) + lreg8(0); \
		TRY(translate8(cpu, &meml, 1, curr_seg, addr)); \
		sreg8(0, laddr8(&meml)); \
	}

#define DAA() \
	u8 al = lreg8(0); \
	int cf = get_CF(cpu); \
	cpu->flags &= ~CF; \
	if ((al & 0xf) > 9 || get_AF(cpu)) { \
		sreg8(0, al + 6); \
		if (cf || al > 0xff - 6) cpu->flags |= CF; \
		cpu->flags |= AF; \
	} else { \
		cpu->flags &= ~AF; \
	} \
	if (al > 0x99 || cf) { \
		sreg8(0, lreg8(0) + 0x60); \
		cpu->flags |= CF; \
	} \
	cpu->cc.dst = sext8(lreg8(0)); \
	cpu->cc.mask = ZF | SF | PF;

#define DAS() \
	u8 al = lreg8(0); \
	int cf = get_CF(cpu); \
	cpu->flags &= ~CF; \
	if ((al & 0xf) > 9 || get_AF(cpu)) { \
		sreg8(0, al - 6); \
		if (cf || al < 6) cpu->flags |= CF; \
		cpu->flags |= AF; \
	} else { \
		cpu->flags &= ~AF; \
	} \
	if (al > 0x99 || cf) { \
		sreg8(0, lreg8(0) - 0x60); \
		cpu->flags |= CF; \
	} \
	cpu->cc.dst = sext8(lreg8(0)); \
	cpu->cc.mask = ZF | SF | PF;

#define AAA() \
	if ((lreg8(0) & 0xf) > 9 || get_AF(cpu)) { \
		sreg16(0, lreg16(0) + 0x106); \
		cpu->flags |= AF | CF; \
	} else { \
		cpu->flags &= ~(AF | CF); \
	} \
	cpu->cc.mask = ZF | SF | PF; \
	sreg8(0, lreg8(0) & 0xf);

#define AAS() \
	if ((lreg8(0) & 0xf) > 9 || get_AF(cpu)) { \
		sreg16(0, lreg16(0) - 6); \
		sreg8(4, lreg8(4) - 1); \
		cpu->flags |= AF | CF; \
	} else { \
		cpu->flags &= ~(AF | CF); \
	} \
	cpu->cc.mask = ZF | SF | PF; \
	sreg8(0, lreg8(0) & 0xf);

static bool larsl_helper(CPUI386 *cpu, int sel, uword *ar, uword *sl, int *zf)
{
	sel = sel & 0xffff;

	if (!(cpu->cr0 & 1) || (cpu->flags & VM))
		THROW0(EX_UD);

	if ((sel & ~0x3) == 0) {
		*zf = 0;
		return true;
	}

	uword w1, w2;
	if (!read_desc(cpu, sel, &w1, &w2)) {
		*zf = 0;
		return true;
	}

	if ((w2 >> 12) & 1) {
		int dpl = (w2 >> 13) & 0x3;
		if (((w2 >> 10) & 0x3) != 0x3 && (cpu->cpl > dpl || (sel & 0x3) > dpl)) {
			*zf = 0;
			return true;
		}
	} else {
		int type = (w2 >> 8) & 0xf;
		if (ar) {
			switch (type) {
			case 0: case 6: case 7: case 8: case 10:
			case 13: case 14: case 15:
				*zf = 0;
				return true;
			}
		}
		if (sl) {
			switch (type) {
			case 0: case 4: case 5: case 6: case 7: case 8:
			case 10: case 12: case 13: case 14: case 15:
				*zf = 0;
				return true;
			}
		}
	}

	if (ar)
		*ar = w2 & 0x00ffff00;
	if (sl) {
		*sl = (w2 & 0xf0000) | (w1 & 0xffff);
		if (w2 & 0x00800000)
			*sl = (*sl << 12) | 0xfff;
	}

	*zf = 1;
	return true;
}

static bool verrw_helper(CPUI386 *cpu, int sel, int wr, int *zf)
{
	sel = sel & 0xffff;

	if (!(cpu->cr0 & 1) || (cpu->flags & VM))
		THROW0(EX_UD);

	if ((sel & ~0x3) == 0) {
		*zf = 0;
		return true;
	}

	uword w1, w2;
	if (!read_desc(cpu, sel, &w1, &w2)) {
		*zf = 0;
		return true;
	}

	if (((w2 >> 12) & 0x1) == 0) {
		*zf = 0;
		return true;
	}

	int dpl = (w2 >> 13) & 0x3;
	if (((w2 >> 10) & 0x3) != 0x3 && (cpu->cpl > dpl || (sel & 0x3) > dpl)) {
		*zf = 0;
		return true;
	}

	if (((w2 >> 11) & 0x1) == 0) {
		/* data */
		if (wr && ((w2 >> 9) & 0x1) == 0) {
			*zf = 0;
			return true;
		}
	} else {
		/* code */
		if (!wr && ((w2 >> 9) & 0x1) == 0) {
			*zf = 0;
			return true;
		}
	}

	*zf = 1;
	return true;
}

#define LARdw(a, b, la, sa, lb, sb) \
	uword res; \
	int zf; \
	TRY(larsl_helper(cpu, lb(b), &res, NULL, &zf)); \
	if (zf) { \
		sa(a, res); \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
	} \
	cpu->cc.mask &= ~ZF;
#define LARww LARdw

#define LSLdw(a, b, la, sa, lb, sb) \
	uword res; \
	int zf; \
	TRY(larsl_helper(cpu, lb(b), NULL, &res, &zf)); \
	if (zf) { \
		sa(a, res); \
		cpu->flags |= ZF; \
	} else { \
		cpu->flags &= ~ZF; \
	} \
	cpu->cc.mask &= ~ZF;
#define LSLww LSLdw

#define VERR(a, la, sa) \
	int zf; \
	TRY(verrw_helper(cpu, la(a), 0, &zf)); \
	cpu->cc.mask &= ~ZF; \
	SET_BIT(cpu->flags, zf, ZF);

#define VERW(a, la, sa) \
	int zf; \
	TRY(verrw_helper(cpu, la(a), 1, &zf)); \
	cpu->cc.mask &= ~ZF; \
	SET_BIT(cpu->flags, zf, ZF);

#define ARPL(a, b, la, sa, lb, sb) \
	if (!(cpu->cr0 & 1) || (cpu->flags & VM)) THROW0(EX_UD); \
	u16 dst = la(a); \
	u16 src = lb(b); \
	if ((dst & 3) < (src & 3)) { \
		cpu->flags |= ZF; \
		sa(a, ((dst & ~3) | (src & 3))); \
	} else { \
		cpu->flags &= ~ZF; \
	} \
	cpu->cc.mask &= ~ZF;

#define GvMa GvM
#define BOUND_helper(BIT, a, b, la, sa, lb, sb) \
	OptAddr meml1, meml2; \
	s ## BIT idx = la(a); \
	uword addr1 = lb(b); \
	TRY(translate ## BIT(cpu, &meml1, 3, curr_seg, addr1)); \
	TRY(translate ## BIT(cpu, &meml2, 3, curr_seg, addr1 + BIT / 8)); \
	s ## BIT lo = load ## BIT(cpu, &meml1); \
	s ## BIT hi = load ## BIT(cpu, &meml2); \
	if (idx < lo || idx > hi) { \
		if (cpu->cr0 & 1) THROW0(EX_BR); \
	}
#define BOUNDw(...) BOUND_helper(16, __VA_ARGS__)
#define BOUNDd(...) BOUND_helper(32, __VA_ARGS__)

// 486...
#define CMPXCH_helper(BIT, a, b, la, sa, lb, sb) \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lreg ## BIT(0)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2); \
	cpu->cc.op = CC_SUB; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	if (cpu->cc.dst == 0) sa(a, lb(b)); else sreg ## BIT(0, cpu->cc.src1); 

#define XADD_helper(BIT, a, b, la, sa, lb, sb) \
	u ## BIT dst = la(a); \
	cpu->cc.src1 = sext ## BIT(la(a)); \
	cpu->cc.src2 = sext ## BIT(lb(b)); \
	cpu->cc.dst = sext ## BIT(cpu->cc.src1 + cpu->cc.src2); \
	cpu->cc.op = CC_ADD; \
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	sb(b, dst); \
	sa(a, cpu->cc.dst);

#define CMPXCHb(...) CMPXCH_helper(8, __VA_ARGS__)
#define CMPXCHw(...) CMPXCH_helper(16, __VA_ARGS__)
#define CMPXCHd(...) CMPXCH_helper(32, __VA_ARGS__)
#define XADDb(...) XADD_helper(8, __VA_ARGS__)
#define XADDw(...) XADD_helper(16, __VA_ARGS__)
#define XADDd(...) XADD_helper(32, __VA_ARGS__)

#define INVLPG(addr) tlb_clear(cpu);

#define BSWAPw(a, la, sa) THROW0(EX_UD);

#define BSWAPd(a, la, sa) \
	u32 src = la(a); \
	u32 dst = ((src & 0xff) << 24) | (((src >> 8) & 0xff) << 16) | (((src >> 16) & 0xff) << 8) | ((src >> 24) & 0xff); \
	sa(a, dst);

#define WBINVD()

// 586 and later...
#define UD0() THROW0(EX_UD);

#if defined(I386_ENABLE_SSE3)
#define CPUID_SIMD_FEATURE2 0x1
#else
#define CPUID_SIMD_FEATURE2 0x0
#endif
#if defined(I386_ENABLE_SSE2)
#define CPUID_SIMD_FEATURE 0x7800000
#elif defined(I386_ENABLE_SSE)
#define CPUID_SIMD_FEATURE 0x3800000
#elif defined(I386_ENABLE_MMX)
#define CPUID_SIMD_FEATURE 0x800000
#else
#define CPUID_SIMD_FEATURE 0x0
#endif

#define CPUID() \
	switch (REGi(0)) { \
	case 0: \
		REGi(0) = 1; \
		REGi(3) = 0x594e4954; \
		REGi(2) = 0x20363833; \
		REGi(1) = 0x20555043; \
		break; \
	case 1: \
		REGi(0) = 0 | (0 << 4) | (cpu->gen << 8); \
		REGi(3) = 0; \
		REGi(2) = 0x100; \
		REGi(1) = 0; \
		if (cpu->fpu) REGi(2) |= 1; \
		if (cpu->gen > 5) REGi(2) |= 0x8820; \
		if (cpu->gen > 5 && cpu->fpu) { \
			REGi(2) |= CPUID_SIMD_FEATURE; \
			REGi(1) |= CPUID_SIMD_FEATURE2; \
		} \
		break; \
	default: \
		REGi(0) = 0; \
		REGi(3) = 0; \
		REGi(2) = 0; \
		REGi(1) = 0; \
		break; \
	}

#include <hardware/timer.h>
#define RDTSC() \
	uint64_t tsc = time_us_64(); \
	REGi(0) = tsc; \
	REGi(2) = tsc >> 32;

#define Mq Ms
#define CMPXCH8B(addr) \
	OptAddr meml1, meml2; \
	TRY(translate32(cpu, &meml1, 3, curr_seg, addr)); \
	TRY(translate32(cpu, &meml2, 3, curr_seg, addr + 4)); \
	uword lo = load32(cpu, &meml1); \
	uword hi = load32(cpu, &meml2); \
	if (REGi(0) == lo && REGi(2) == hi) { \
		cpu->flags |= ZF; \
		store32(cpu, &meml1, REGi(3)); \
		store32(cpu, &meml2, REGi(1)); \
	} else { \
		cpu->flags &= ~ZF; \
		REGi(0) = lo; \
		REGi(2) = hi; \
	} \
	cpu->cc.mask &= ~ZF;

#define CMOVw(a, b, la, sa, lb, sb) \
	COND() \
	if (cond) sa(a, lb(b));

#define CMOVd(a, b, la, sa, lb, sb) \
	COND() \
	if (cond) sa(a, lb(b));

#define WRMSR() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	switch (REGi(1)) { \
	case 0x174: cpu->sysenter.cs = REGi(0); break; \
	case 0x176: cpu->sysenter.eip = REGi(0); break; \
	case 0x175: cpu->sysenter.esp = REGi(0); break; \
	default: cpu_debug(cpu); THROW(EX_GP, 0); \
	}

#define RDMSR() \
	if (cpu->cpl != 0) THROW(EX_GP, 0); \
	switch (REGi(1)) { \
	case 0x174: REGi(0) = cpu->sysenter.cs; REGi(2) = 0; break; \
	case 0x176: REGi(0) = cpu->sysenter.eip; REGi(2) = 0; break; \
	case 0x175: REGi(0) = cpu->sysenter.esp; REGi(2) = 0; break; \
	default: cpu_debug(cpu); THROW(EX_GP, 0); \
	}

static void __sysenter(CPUI386 *cpu, int pl, int cs)
{
	cpu->seg[SEG_CS].sel = (cs & 0xfffc) | pl;
	cpu->seg[SEG_CS].base = 0;
	cpu->seg[SEG_CS].limit = 0xffffffff;
	cpu->seg[SEG_CS].flags = SEG_D_BIT | 0x5b | (pl << 5);
	cpu->cpl = pl;
	cpu->code16 = false;
	cpu->sp_mask = 0xffffffff;
	cpu->seg[SEG_SS].sel = ((cs + 8) & 0xfffc) | pl;
	cpu->seg[SEG_SS].base = 0;
	cpu->seg[SEG_SS].limit = 0xffffffff;
	cpu->seg[SEG_SS].flags = SEG_B_BIT | 0x53 | (pl << 5);
}

#define SYSENTER() \
	if (!(cpu->cr0 & 1) || (cpu->sysenter.cs & ~0x3) == 0) THROW(EX_GP, 0); \
	cpu->flags &= ~(VM | IF); \
	__sysenter(cpu, 0, cpu->sysenter.cs); \
	REGi(4) = cpu->sysenter.esp; \
	cpu->next_ip = cpu->sysenter.eip; cpu->prefetch_base = (u32)-1;

#define SYSEXIT() \
	if (!(cpu->cr0 & 1) || (cpu->sysenter.cs & ~0x3) == 0 || cpu->cpl) THROW(EX_GP, 0); \
	__sysenter(cpu, 3, cpu->sysenter.cs + 16); \
	REGi(4) = REGi(1); \
	cpu->next_ip = REGi(2); cpu->prefetch_base = (u32)-1;

#if defined(I386_ENABLE_MMX) || defined(I386_ENABLE_SSE)
#define SIMD_i386_c
#include "simd.inc"
#undef SIMD_i386_c
#endif

static bool pmcall(CPUI386 *cpu, bool opsz16, uword addr, int sel, bool isjmp);
static bool IRAM_ATTR pmret(CPUI386 *cpu, bool opsz16, int off, bool isiret);

static bool verbose;

#define ARGCOUNT_IMPL(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, ...) _17
#define ARGCOUNT(...) ARGCOUNT_IMPL(~, ## __VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define PASTE0(a, b) a ## b
#define PASTE(a, b) PASTE0(a, b)
#define C_1(_1)      CX(_1)
#define C_2(_1, ...) CX(_1) C_1(__VA_ARGS__)
#define C_3(_1, ...) CX(_1) C_2(__VA_ARGS__)
#define C_4(_1, ...) CX(_1) C_3(__VA_ARGS__)
#define C_5(_1, ...) CX(_1) C_4(__VA_ARGS__)
#define C_6(_1, ...) CX(_1) C_5(__VA_ARGS__)
#define C_7(_1, ...) CX(_1) C_6(__VA_ARGS__)
#define C_8(_1, ...) CX(_1) C_7(__VA_ARGS__)
#define C_9(_1, ...) CX(_1) C_8(__VA_ARGS__)
#define C_10(_1, ...) CX(_1) C_9(__VA_ARGS__)
#define C_11(_1, ...) CX(_1) C_10(__VA_ARGS__)
#define C_12(_1, ...) CX(_1) C_11(__VA_ARGS__)
#define C_13(_1, ...) CX(_1) C_12(__VA_ARGS__)
#define C_14(_1, ...) CX(_1) C_13(__VA_ARGS__)
#define C_15(_1, ...) CX(_1) C_14(__VA_ARGS__)
#define C_16(_1, ...) CX(_1) C_15(__VA_ARGS__)
#define C(...) PASTE(C_, ARGCOUNT(__VA_ARGS__))(__VA_ARGS__)


#if BLOCK_JIT
/* -------------------------------------------------------------------------
 * FRANK_BLOCK_JIT_V1
 *
 * Small hot-block dynamic translator for RP2350.
 *
 * Cache footprint is intentionally tiny (~1.3 KB) so Z2 can keep FAST_FETCH.
 * The previous BB profile showed that a handful of guest blocks dominate, so
 * a large cache is not required for the first hardware test.
 *
 * Supported in v1:
 *   - NOP
 *   - MOV r16/r32, imm
 *   - MOV r16/r32, r16/r32 (ModR/M register form)
 *   - XCHG AX/EAX, r16/r32 and XCHG reg,reg
 *   - ADD/SUB/AND/OR/XOR/CMP/TEST register forms
 *   - ADD/SUB/AND/OR/XOR/CMP immediate register forms
 *   - INC/DEC register
 *   - short/near Jcc, short/near JMP
 *   - LOOP/LOOPE/LOOPNE/JCXZ/JECXZ
 *
 * Unsupported, memory, I/O, stack, far-control-flow, REP and flag-consuming
 * arithmetic fall back to the existing interpreter. No guest-visible state is
 * changed by the compiler itself.
 * ------------------------------------------------------------------------- */

/* FRANK_BLOCK_JIT_V2 */
#define BJ_CACHE_BITS  4
#define BJ_CACHE_SLOTS (1u << BJ_CACHE_BITS)
#define BJ_HOT_BITS    5
#define BJ_HOT_SLOTS   (1u << BJ_HOT_BITS)
#define BJ_HOT_THRESHOLD 3u
#define BJ_MAX_UOPS    8
#define BJ_TRACK_PAGES 2048u       /* 8 MiB / 4 KiB */
#define BJ_PAGE_BYTES  (BJ_TRACK_PAGES / 8u)

enum {
	BJ_NOP = 1,
	BJ_MOV_RI,
	BJ_MOV_RR,
	BJ_XCHG_RR,
	BJ_ADD_RR,
	BJ_SUB_RR,
	BJ_OR_RR,
	BJ_AND_RR,
	BJ_XOR_RR,
	BJ_CMP_RR,
	BJ_TEST_RR,
	BJ_ADD_RI,
	BJ_SUB_RI,
	BJ_OR_RI,
	BJ_AND_RI,
	BJ_XOR_RI,
	BJ_CMP_RI,
	BJ_INC_R,
	BJ_DEC_R,
	BJ_JCC,
	BJ_JMP,
	BJ_LOOP
};

typedef struct {
	u32 imm;
	u8 op;
	u8 dst;
	u8 src;
	u8 width;
	u8 len;
	u8 aux;
	u8 _pad0;
	u8 _pad1;
} bj_uop_t;

typedef struct {
	uword linear;
	uword cs_base;
	uword mmu_key;
	uword start_ip;
	u16 byte_len;
	u16 last_off;
	u16 phys_page;
	u8 count;
	u8 code16;
	u8 valid;
	u8 has_branch;
	bj_uop_t u[BJ_MAX_UOPS];
} bj_block_t;

static bj_block_t bj_cache[BJ_CACHE_SLOTS];
static u8 bj_page_bits[BJ_PAGE_BYTES];

typedef struct {
	uword key;
	u8 seen;
	u8 _pad[3];
} bj_hot_t;
static bj_hot_t bj_hot[BJ_HOT_SLOTS];

volatile u32 g_bjit_hits __attribute__((used));
volatile u32 g_bjit_misses __attribute__((used));
volatile u32 g_bjit_compiles __attribute__((used));
volatile u32 g_bjit_uops __attribute__((used));
volatile u32 g_bjit_invalidations __attribute__((used));
volatile u32 g_bjit_skips __attribute__((used));
volatile u32 g_bjit_hotwait __attribute__((used));
volatile u32 g_bjit_rejects __attribute__((used));

static inline __attribute__((always_inline))
unsigned bj_hash_block(uword linear)
{
	return (unsigned)((linear * 2654435761u) >> (32 - BJ_CACHE_BITS));
}

static inline __attribute__((always_inline))
unsigned bj_hash_hot(uword key)
{
	return (unsigned)((key * 2654435761u) >> (32 - BJ_HOT_BITS));
}

static inline __attribute__((always_inline))
uword bj_context_key(CPUI386 *cpu, uword linear)
{
	return linear ^ cpu->cr3 ^ (cpu->cr0 & 0x80000001u) ^ cpu->a20_mask ^
	       ((uword)cpu->code16 << 30);
}

static inline __attribute__((always_inline))
bool bj_hot_enough(CPUI386 *cpu, uword linear)
{
	uword key = bj_context_key(cpu, linear);
	bj_hot_t *h = &bj_hot[bj_hash_hot(key)];

	if (h->key != key) {
		h->key = key;
		h->seen = 1;
		return false;
	}
	if (h->seen == 0xff)
		return false;
	if (h->seen < BJ_HOT_THRESHOLD) {
		h->seen++;
		return false;
	}
	return true;
}

static inline __attribute__((always_inline))
void bj_hot_reject(CPUI386 *cpu, uword linear)
{
	uword key = bj_context_key(cpu, linear);
	bj_hot_t *h = &bj_hot[bj_hash_hot(key)];
	h->key = key;
	h->seen = 0xff;
	g_bjit_rejects++;
}

static inline bool bj_page_marked(unsigned page)
{
	return page < BJ_TRACK_PAGES &&
	       (bj_page_bits[page >> 3] & (1u << (page & 7)));
}

static inline void bj_page_mark(unsigned page)
{
	if (page < BJ_TRACK_PAGES)
		bj_page_bits[page >> 3] |= (u8)(1u << (page & 7));
}

static void bj_rebuild_pages(void)
{
	memset(bj_page_bits, 0, sizeof(bj_page_bits));
	for (unsigned i = 0; i < BJ_CACHE_SLOTS; ++i)
		if (bj_cache[i].valid)
			bj_page_mark(bj_cache[i].phys_page);
}

static void bj_flush(void)
{
	memset(bj_cache, 0, sizeof(bj_cache));
	memset(bj_page_bits, 0, sizeof(bj_page_bits));
	memset(bj_hot, 0, sizeof(bj_hot));
}

static void bj_invalidate_page(unsigned page)
{
	bool changed = false;
	for (unsigned i = 0; i < BJ_CACHE_SLOTS; ++i) {
		if (bj_cache[i].valid && bj_cache[i].phys_page == page) {
			bj_cache[i].valid = 0;
			changed = true;
		}
	}
	if (changed) {
		g_bjit_invalidations++;
		bj_rebuild_pages();
		memset(bj_hot, 0, sizeof(bj_hot));
	}
}

static inline void bj_note_write(CPUI386 *cpu, uword addr, unsigned len)
{
	(void)cpu;
	unsigned p0 = addr >> 12;
	unsigned p1 = (addr + len - 1u) >> 12;
	if (unlikely(bj_page_marked(p0)))
		bj_invalidate_page(p0);
	if (p1 != p0 && unlikely(bj_page_marked(p1)))
		bj_invalidate_page(p1);
}

static inline u32 bj_get_reg(CPUI386 *cpu, unsigned r, unsigned width)
{
	u32 v = REGi(r);
	return width == 2 ? (u16)v : v;
}

static inline void bj_set_reg(CPUI386 *cpu, unsigned r, unsigned width, u32 v)
{
	if (width == 2)
		REGi(r) = (REGi(r) & 0xffff0000u) | (u16)v;
	else
		REGi(r) = v;
}

static inline uword bj_sext(u32 v, unsigned width)
{
	return width == 2 ? (uword)(sword)(s16)(u16)v
	                  : (uword)(sword)(s32)v;
}

static inline void bj_cc_arith(CPUI386 *cpu, int op, unsigned width,
                               u32 a, u32 b, u32 result)
{
	cpu->cc.src1 = bj_sext(a, width);
	cpu->cc.src2 = bj_sext(b, width);
	cpu->cc.dst  = bj_sext(result, width);
	cpu->cc.op   = op;
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
}

static inline void bj_cc_logic(CPUI386 *cpu, int op, unsigned width, u32 result)
{
	cpu->cc.dst  = bj_sext(result, width);
	cpu->cc.op   = op;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
}

static inline bool bj_cond(CPUI386 *cpu, unsigned c)
{
	switch (c & 15u) {
	case 0x0: return get_OF(cpu);
	case 0x1: return !get_OF(cpu);
	case 0x2: return get_CF(cpu);
	case 0x3: return !get_CF(cpu);
	case 0x4: return get_ZF(cpu);
	case 0x5: return !get_ZF(cpu);
	case 0x6: return get_CF(cpu) || get_ZF(cpu);
	case 0x7: return !get_CF(cpu) && !get_ZF(cpu);
	case 0x8: return get_SF(cpu);
	case 0x9: return !get_SF(cpu);
	case 0xa: return get_PF(cpu);
	case 0xb: return !get_PF(cpu);
	case 0xc: return get_SF(cpu) != get_OF(cpu);
	case 0xd: return get_SF(cpu) == get_OF(cpu);
	case 0xe: return get_ZF(cpu) || (get_SF(cpu) != get_OF(cpu));
	default:  return !get_ZF(cpu) && (get_SF(cpu) == get_OF(cpu));
	}
}

static inline u16 bj_rd16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 bj_rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
	       ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int bj_code_window(CPUI386 *cpu, uword ip,
                          const u8 **out, unsigned *avail,
                          unsigned *phys_page)
{
	if (cpu->code16)
		ip &= 0xffffu;

	OptAddr res;
	if (!translate8r(cpu, &res, SEG_CS, ip))
		return 0;

	if (in_iomem(res.addr1) || res.addr1 >= (uword)cpu->phys_mem_size)
		return 0;

	uword linear = cpu->seg[SEG_CS].base + ip;
	unsigned n = 4096u - (unsigned)(linear & 4095u);
	unsigned pn = 4096u - (unsigned)(res.addr1 & 4095u);
	if (pn < n) n = pn;

	unsigned memn = (unsigned)((uword)cpu->phys_mem_size - res.addr1);
	if (memn < n) n = memn;

	if (cpu->code16) {
		unsigned wrapn = 0x10000u - (unsigned)(ip & 0xffffu);
		if (wrapn < n) n = wrapn;
	}

	*out = cpu->phys_mem + res.addr1;
	*avail = n;
	*phys_page = (unsigned)(res.addr1 >> 12);
	return n != 0;
}

static int bj_decode_one(CPUI386 *cpu, const u8 *p, unsigned max,
                         bj_uop_t *u, bool *terminator)
{
	if (!max) return 0;

	unsigned pos = 0;
	bool opsz16 = cpu->code16;
	bool adsz16 = cpu->code16;

	for (;;) {
		if (pos >= max || pos >= 8) return 0;
		u8 x = p[pos];
		switch (x) {
		case 0x26: case 0x2e: case 0x36: case 0x3e:
		case 0x64: case 0x65:
			pos++;
			continue;
		case 0x66:
			opsz16 = !cpu->code16;
			pos++;
			continue;
		case 0x67:
			adsz16 = !cpu->code16;
			pos++;
			continue;
		case 0xf0: case 0xf2: case 0xf3:
			return 0;
		default:
			break;
		}
		break;
	}

	if (pos >= max) return 0;
	u8 op = p[pos++];
	unsigned width = opsz16 ? 2u : 4u;

	memset(u, 0, sizeof(*u));
	u->width = (u8)width;
	*terminator = false;

#define BJ_NEED(n) do { if (pos + (unsigned)(n) > max) return 0; } while (0)
#define BJ_MODRM_REGS(m, regv, rmv) do { \
	if (((m) >> 6) != 3) return 0; \
	(regv) = ((m) >> 3) & 7; \
	(rmv) = (m) & 7; \
} while (0)

	if (op == 0x90) {
		u->op = BJ_NOP;
	} else if (op >= 0x91 && op <= 0x97) {
		u->op = BJ_XCHG_RR;
		u->dst = 0;
		u->src = op & 7;
	}
	else if (op >= 0xb8 && op <= 0xbf) {
		u->op = BJ_MOV_RI;
		u->dst = op & 7;
		BJ_NEED(width);
		u->imm = width == 2 ? bj_rd16(p + pos) : bj_rd32(p + pos);
		pos += width;
	}
	else if (op >= 0x40 && op <= 0x47) {
		u->op = BJ_INC_R;
		u->dst = op & 7;
	} else if (op >= 0x48 && op <= 0x4f) {
		u->op = BJ_DEC_R;
		u->dst = op & 7;
	}
	else if (op == 0x05 || op == 0x0d || op == 0x25 ||
	         op == 0x2d || op == 0x35 || op == 0x3d) {
		BJ_NEED(width);
		u->dst = 0;
		u->imm = width == 2 ? bj_rd16(p + pos) : bj_rd32(p + pos);
		pos += width;
		switch (op) {
		case 0x05: u->op = BJ_ADD_RI; break;
		case 0x0d: u->op = BJ_OR_RI;  break;
		case 0x25: u->op = BJ_AND_RI; break;
		case 0x2d: u->op = BJ_SUB_RI; break;
		case 0x35: u->op = BJ_XOR_RI; break;
		default:   u->op = BJ_CMP_RI; break;
		}
	}
	else if (op == 0x01 || op == 0x03 || op == 0x09 || op == 0x0b ||
	         op == 0x21 || op == 0x23 || op == 0x29 || op == 0x2b ||
	         op == 0x31 || op == 0x33 || op == 0x39 || op == 0x3b ||
	         op == 0x85 || op == 0x87 || op == 0x89 || op == 0x8b) {
		BJ_NEED(1);
		u8 m = p[pos++];
		unsigned reg, rm;
		BJ_MODRM_REGS(m, reg, rm);

		if (op == 0x89) {
			u->op = BJ_MOV_RR; u->dst = rm; u->src = reg;
		} else if (op == 0x8b) {
			u->op = BJ_MOV_RR; u->dst = reg; u->src = rm;
		} else if (op == 0x87) {
			u->op = BJ_XCHG_RR; u->dst = rm; u->src = reg;
		} else if (op == 0x85) {
			u->op = BJ_TEST_RR; u->dst = rm; u->src = reg;
		} else {
			bool dir = (op & 2) != 0;
			u->dst = dir ? reg : rm;
			u->src = dir ? rm : reg;
			switch (op & 0xf8) {
			case 0x00: u->op = BJ_ADD_RR; break;
			case 0x08: u->op = BJ_OR_RR;  break;
			case 0x20: u->op = BJ_AND_RR; break;
			case 0x28: u->op = BJ_SUB_RR; break;
			case 0x30: u->op = BJ_XOR_RR; break;
			case 0x38: u->op = BJ_CMP_RR; break;
			default: return 0;
			}
		}
	}
	else if (op == 0x81 || op == 0x83) {
		BJ_NEED(1);
		u8 m = p[pos++];
		if ((m >> 6) != 3) return 0;
		unsigned sub = (m >> 3) & 7;
		u->dst = m & 7;

		if (sub == 2 || sub == 3) return 0;

		if (op == 0x83) {
			BJ_NEED(1);
			s32 simm = (s8)p[pos++];
			u->imm = width == 2 ? (u16)simm : (u32)simm;
		} else {
			BJ_NEED(width);
			u->imm = width == 2 ? bj_rd16(p + pos) : bj_rd32(p + pos);
			pos += width;
		}

		switch (sub) {
		case 0: u->op = BJ_ADD_RI; break;
		case 1: u->op = BJ_OR_RI;  break;
		case 4: u->op = BJ_AND_RI; break;
		case 5: u->op = BJ_SUB_RI; break;
		case 6: u->op = BJ_XOR_RI; break;
		case 7: u->op = BJ_CMP_RI; break;
		default: return 0;
		}
	}
	else if (op == 0xc7) {
		BJ_NEED(1);
		u8 m = p[pos++];
		if (((m >> 6) != 3) || (((m >> 3) & 7) != 0)) return 0;
		u->op = BJ_MOV_RI;
		u->dst = m & 7;
		BJ_NEED(width);
		u->imm = width == 2 ? bj_rd16(p + pos) : bj_rd32(p + pos);
		pos += width;
	}
	else if (op >= 0x70 && op <= 0x7f) {
		BJ_NEED(1);
		u->op = BJ_JCC;
		u->aux = op & 15;
		u->imm = (u32)(s32)(s8)p[pos++];
		*terminator = true;
	}
	else if (op == 0xeb) {
		BJ_NEED(1);
		u->op = BJ_JMP;
		u->imm = (u32)(s32)(s8)p[pos++];
		*terminator = true;
	} else if (op == 0xe9) {
		BJ_NEED(width);
		u->op = BJ_JMP;
		u->imm = width == 2
		       ? (u32)(s32)(s16)bj_rd16(p + pos)
		       : (u32)(s32)bj_rd32(p + pos);
		pos += width;
		*terminator = true;
	}
	else if (op >= 0xe0 && op <= 0xe3) {
		BJ_NEED(1);
		u->op = BJ_LOOP;
		u->aux = op;
		u->width = adsz16 ? 2 : 4;
		u->imm = (u32)(s32)(s8)p[pos++];
		*terminator = true;
	}
	else if (op == 0x0f) {
		BJ_NEED(1);
		u8 op2 = p[pos++];
		if (op2 < 0x80 || op2 > 0x8f) return 0;
		BJ_NEED(width);
		u->op = BJ_JCC;
		u->aux = op2 & 15;
		u->imm = width == 2
		       ? (u32)(s32)(s16)bj_rd16(p + pos)
		       : (u32)(s32)bj_rd32(p + pos);
		pos += width;
		*terminator = true;
	}
	else {
		return 0;
	}

	if (pos > 15) return 0;
	u->len = (u8)pos;
	return (int)pos;

#undef BJ_MODRM_REGS
#undef BJ_NEED
}

static inline __attribute__((always_inline))
bj_block_t *bj_lookup(CPUI386 *cpu, uword ip)
{
	uword linear = cpu->seg[SEG_CS].base + ip;
	uword mmu_key = cpu->cr3 ^ (cpu->cr0 & 0x80000001u) ^ cpu->a20_mask;
	bj_block_t *b = &bj_cache[bj_hash_block(linear)];

	if (likely(b->valid &&
	           b->linear == linear &&
	           b->cs_base == cpu->seg[SEG_CS].base &&
	           b->mmu_key == mmu_key &&
	           b->code16 == (u8)cpu->code16))
		return b;

	return NULL;
}

static bj_block_t *bj_compile(CPUI386 *cpu, uword ip)
{
	const u8 *code;
	unsigned avail, phys_page;
	if (!bj_code_window(cpu, ip, &code, &avail, &phys_page))
		return NULL;

	bj_block_t tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.linear = cpu->seg[SEG_CS].base + ip;
	tmp.cs_base = cpu->seg[SEG_CS].base;
	tmp.mmu_key = cpu->cr3 ^ (cpu->cr0 & 0x80000001u) ^ cpu->a20_mask;
	tmp.start_ip = ip;
	tmp.phys_page = (u16)phys_page;
	tmp.code16 = (u8)cpu->code16;

	unsigned pos = 0;
	for (unsigned i = 0; i < BJ_MAX_UOPS && pos < avail; ++i) {
		bool term = false;
		int n = bj_decode_one(cpu, code + pos, avail - pos, &tmp.u[i], &term);
		if (n <= 0) break;
		tmp.last_off = (u16)pos;
		pos += (unsigned)n;
		tmp.count++;
		if (term) {
			tmp.has_branch = 1;
			break;
		}
	}

	if (tmp.count < 2)
		return NULL;

	tmp.byte_len = (u16)pos;
	tmp.valid = 1;

	/* Direct-mapped cache. */
	bj_block_t *dst = &bj_cache[bj_hash_block(tmp.linear)];
	*dst = tmp;
	bj_rebuild_pages();
	g_bjit_compiles++;
	return dst;
}

static int IRAM_ATTR bj_exec(CPUI386 *cpu, const bj_block_t *b, int max_steps)
{
	if (!b || b->count == 0 || b->count > max_steps)
		return 0;

	uword ip = cpu->next_ip;
	if (cpu->code16) ip &= 0xffffu;

	for (unsigned i = 0; i < b->count; ++i) {
		const bj_uop_t *u = &b->u[i];
		cpu->ip = ip;

		u32 a, v, r;

		switch (u->op) {
		case BJ_NOP:
			break;

		case BJ_MOV_RI:
			bj_set_reg(cpu, u->dst, u->width, u->imm);
			break;

		case BJ_MOV_RR:
			bj_set_reg(cpu, u->dst, u->width,
			           bj_get_reg(cpu, u->src, u->width));
			break;

		case BJ_XCHG_RR:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = bj_get_reg(cpu, u->src, u->width);
			bj_set_reg(cpu, u->dst, u->width, v);
			bj_set_reg(cpu, u->src, u->width, a);
			break;

		case BJ_ADD_RR:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = bj_get_reg(cpu, u->src, u->width);
			r = a + v;
			bj_cc_arith(cpu, CC_ADD, u->width, a, v, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_SUB_RR:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = bj_get_reg(cpu, u->src, u->width);
			r = a - v;
			bj_cc_arith(cpu, CC_SUB, u->width, a, v, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_OR_RR:
			r = bj_get_reg(cpu, u->dst, u->width) |
			    bj_get_reg(cpu, u->src, u->width);
			bj_cc_logic(cpu, CC_OR, u->width, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_AND_RR:
			r = bj_get_reg(cpu, u->dst, u->width) &
			    bj_get_reg(cpu, u->src, u->width);
			bj_cc_logic(cpu, CC_AND, u->width, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_XOR_RR:
			r = bj_get_reg(cpu, u->dst, u->width) ^
			    bj_get_reg(cpu, u->src, u->width);
			bj_cc_logic(cpu, CC_XOR, u->width, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_CMP_RR:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = bj_get_reg(cpu, u->src, u->width);
			r = a - v;
			bj_cc_arith(cpu, CC_SUB, u->width, a, v, r);
			break;

		case BJ_TEST_RR:
			r = bj_get_reg(cpu, u->dst, u->width) &
			    bj_get_reg(cpu, u->src, u->width);
			bj_cc_logic(cpu, CC_AND, u->width, r);
			break;

		case BJ_ADD_RI:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = u->width == 2 ? (u16)u->imm : u->imm;
			r = a + v;
			bj_cc_arith(cpu, CC_ADD, u->width, a, v, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_SUB_RI:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = u->width == 2 ? (u16)u->imm : u->imm;
			r = a - v;
			bj_cc_arith(cpu, CC_SUB, u->width, a, v, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_OR_RI:
			r = bj_get_reg(cpu, u->dst, u->width) | u->imm;
			bj_cc_logic(cpu, CC_OR, u->width, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_AND_RI:
			r = bj_get_reg(cpu, u->dst, u->width) & u->imm;
			bj_cc_logic(cpu, CC_AND, u->width, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_XOR_RI:
			r = bj_get_reg(cpu, u->dst, u->width) ^ u->imm;
			bj_cc_logic(cpu, CC_XOR, u->width, r);
			bj_set_reg(cpu, u->dst, u->width, r);
			break;

		case BJ_CMP_RI:
			a = bj_get_reg(cpu, u->dst, u->width);
			v = u->width == 2 ? (u16)u->imm : u->imm;
			r = a - v;
			bj_cc_arith(cpu, CC_SUB, u->width, a, v, r);
			break;

		case BJ_INC_R: {
			int cf = get_CF(cpu);
			a = bj_get_reg(cpu, u->dst, u->width);
			r = a + 1u;
			cpu->cc.dst = bj_sext(r, u->width);
			cpu->cc.op = u->width == 2 ? CC_INC16 : CC_INC32;
			SET_BIT(cpu->flags, cf, CF);
			cpu->cc.mask = PF | AF | ZF | SF | OF;
			bj_set_reg(cpu, u->dst, u->width, r);
			break;
		}

		case BJ_DEC_R: {
			int cf = get_CF(cpu);
			a = bj_get_reg(cpu, u->dst, u->width);
			r = a - 1u;
			cpu->cc.dst = bj_sext(r, u->width);
			cpu->cc.op = u->width == 2 ? CC_DEC16 : CC_DEC32;
			SET_BIT(cpu->flags, cf, CF);
			cpu->cc.mask = PF | AF | ZF | SF | OF;
			bj_set_reg(cpu, u->dst, u->width, r);
			break;
		}

		case BJ_JCC:
			ip += u->len;
			if (bj_cond(cpu, u->aux))
				ip += (s32)u->imm;
			if (cpu->code16) ip &= 0xffffu;
			goto block_done;

		case BJ_JMP:
			ip += u->len;
			ip += (s32)u->imm;
			if (cpu->code16) ip &= 0xffffu;
			goto block_done;

		case BJ_LOOP: {
			bool take;
			u32 count = bj_get_reg(cpu, 1, u->width);
			if (u->aux == 0xe3) {
				take = (count == 0);
			} else {
				count = u->width == 2 ? (u16)(count - 1u) : count - 1u;
				bj_set_reg(cpu, 1, u->width, count);
				if (u->aux == 0xe2) take = count != 0;
				else if (u->aux == 0xe1) take = count != 0 && get_ZF(cpu);
				else take = count != 0 && !get_ZF(cpu);
			}
			ip += u->len;
			if (take) ip += (s32)u->imm;
			if (cpu->code16) ip &= 0xffffu;
			goto block_done;
		}

		default:
			return 0;
		}

		ip += u->len;
		if (cpu->code16) ip &= 0xffffu;
	}

block_done:
	cpu->next_ip = ip;
	cpu->cycle += b->count;
	g_bjit_uops += b->count;
	return b->count;
}

static inline __attribute__((always_inline))
int bj_try_execute(CPUI386 *cpu, int max_steps)
{
	if (unlikely(cpu->flags & TF))
		return 0;

	uword ip = cpu->next_ip;
	if (cpu->code16) ip &= 0xffffu;

	/*
	 * Ordinary sequential flow advances by 1..15 bytes.
	 * Do no JIT lookup at all in that overwhelmingly common case.
	 */
	uword delta = ip - cpu->ip;
	if (likely(delta - 1u < 15u)) {
		g_bjit_skips++;
		return 0;
	}

	bj_block_t *b = bj_lookup(cpu, ip);
	if (likely(b != NULL)) {
		g_bjit_hits++;
		return bj_exec(cpu, b, max_steps);
	}

	g_bjit_misses++;

	/* Avoid compiling one-shot BIOS/DOS control-flow targets. */
	uword linear = cpu->seg[SEG_CS].base + ip;
	if (!bj_hot_enough(cpu, linear)) {
		g_bjit_hotwait++;
		return 0;
	}

	b = bj_compile(cpu, ip);
	if (!b) {
		bj_hot_reject(cpu, linear);
		return 0;
	}

	return bj_exec(cpu, b, max_steps);
}
#endif /* BLOCK_JIT */


#if NATIVE_JIT
/* -------------------------------------------------------------------------
 * FRANK_NATIVE_JIT_V4_LOOPCHAIN
 * FRANK_NATIVE_JIT_V41_WIDE66
 * FRANK_NATIVE_JIT_V42_ESABS16
 * FRANK_NATIVE_JIT_V43_FASTDISPATCH
 *
 * Native hot-loop recompiler for Cortex-M33.
 *
 * Unlike v3/v3.1, translated x86 registers live in r4-r11 for the entire
 * native loop.  A taken backward branch is compiled together with its body,
 * so a hot loop stays in generated Thumb code for many guest iterations and
 * returns to cpu_exec1 only when the loop exits or the current step budget is
 * exhausted.
 *
 * v4 deliberately accepts only loops whose control flow is proven simple:
 *   - exactly one backward branch, targeting the first x86 instruction;
 *   - no memory, I/O, stack, REP, CALL/RET, exceptions, or nested branches;
 *   - register/immediate 16- or 32-bit ALU/MOV body;
 *   - JZ/JNZ, JMP, LOOP, or JCXZ back edge.
 *
 * Unsupported loops stay entirely in the existing interpreter.
 * ------------------------------------------------------------------------- */

#include <stddef.h>

/* V8.4 keeps the same 32 compiled / 32 hot low-RAM metadata budget, but
 * arranges the compiled cache as 16 two-way sets. V8.3 was still direct-mapped:
 * two genuinely hot PCs hashing to the same slot repeatedly recompiled each
 * other even while the 16 KiB code arena had room. Two-way lookup removes that
 * conflict without adding another block or increasing the HDMI-sensitive SRAM
 * footprint. */
/*
 * Cache and hot-table capacity.
 *
 * Measured on this board with Tyrian 2000: the compiled-block cache held 32
 * blocks and the game's fast-path lookup hit 173 times a second against a
 * sampled miss rate that works out to some 306000 backedges a second - a hit
 * rate of 0.06%.  Every hot loop it compiled was evicted before it could be
 * used again, so the JIT executed 0.12% of the guest's instructions no matter
 * how much the emitter was taught to accept.
 *
 * Doubling both tables costs 3328 + 1664 bytes of SRAM, and that was tried on
 * hardware: it does not boot.  SRAM goes from 91.75% to 92.70%, pc_new()
 * still succeeds - g_diag_pc_new_failed stayed 0 - but pc_step() never runs
 * once and the board sits at a black screen.  So the ceiling is somewhere
 * between 91.75% and 92.70%, and it is not the emulator's own allocation
 * that hits it.  -DNJIT_BIG_CACHE=ON restores the doubled sizes for anyone
 * who frees the SRAM first; the default stays at what boots.
 */
/*
 * Per-entry diagnostics for the JIT tables.
 *
 * nj_hot_t carries a 32-byte copy of the guest code at every refusal and
 * nj_block_t two retirement counters; both exist only for
 * njit_diag_reject_snapshot() and njit_diag_block_snapshot(), the profiler
 * hotkeys.  They also make nj_hot_t 52 bytes instead of 20 and nj_block_t 104
 * instead of 96, which is 1024 + 256 bytes of SRAM at the current table sizes
 * and much more at any larger one.  Off by default; -DNJIT_DIAG=ON restores
 * the snapshots.
 */
#ifndef NJIT_DIAG
#define NJIT_DIAG 0
#endif

#ifndef NJ_CACHE_SET_BITS
#define NJ_CACHE_SET_BITS   4u
#endif
#define NJ_CACHE_SETS       (1u << NJ_CACHE_SET_BITS)
#define NJ_CACHE_WAYS       2u
#define NJ_CACHE_SLOTS      (NJ_CACHE_SETS * NJ_CACHE_WAYS)
#ifndef NJ_HOT_BITS
#define NJ_HOT_BITS         5u
#endif
#define NJ_HOT_SLOTS        (1u << NJ_HOT_BITS)
#define NJ_HOT_THRESHOLD    2u
#define NJ_DISCOVERY_MASK   255u /* sample one unsupported/uncompiled backedge in 256 */
/* V8.1 low-RAM hotfix: keep the executable cache at the v7 16 KiB size.
 * The Z2 HDMI path launches core 1 before guest/JIT execution, and the v8
 * 32 KiB code cache plus enlarged metadata consumed ~24 KiB more static SRAM.
 * Restoring the v7 footprint avoids stealing headroom from core1/HDMI stacks
 * while retaining the v8 supertrace/code-generation improvements. */
#ifndef NJ_CODE_BYTES
#define NJ_CODE_BYTES       16384u
#endif
#ifndef NJIT_SHARED_GUARD
#define NJIT_SHARED_GUARD 0
#endif

#define NJ_MAX_X86_INSNS    16u
#define NJ_MAX_ARM_BYTES    512u
/*
 * Guarded memory operands in a fully-native loop body.
 *
 * One costs roughly 250 bytes of Thumb - address arithmetic, the TLB walk,
 * the range/VGA/code-page guards and the shared fail epilogue - so the
 * register-only 512-byte reservation could not have held even two.  The cap
 * and the reservation are sized against each other, and both against the
 * code arena, which is only 8 KB on this board because SRAM for a larger one
 * does not exist: four memory operands plus the register work and the
 * epilogue land near 1200 bytes, leaving six such blocks resident at once.
 */
#define NJ_LOOP_MAX_MEM_INSNS 8u
#define NJ_LOOP_MAX_ARM_BYTES (NJIT_SHARED_GUARD ? 768u : 1280u)

/*
 * Measured on hardware 2026-09-04 and OFF as a result.
 *
 * Doom -timedemo demo3 went 7950 -> 9787 realtics (4.80 -> 3.90 fps) with this
 * on.  The generated code is not the problem: over ten seconds inside the
 * renderer NJCH_BRK_NOT_SINGLE did not advance at all, so not one loop block
 * executed there and the native work was still entirely v6 traces.  Doom's
 * inner loops are refused for reasons this change does not touch - 0F B6
 * (MOVZX from a byte in memory), the byte ALU forms 00/02/0a..., PUSH, and a
 * JGE back edge, none of which nj_decode_backedge()/nj_translate_body_one()
 * accept.
 *
 * What did change is the arena.  nj_compile_loop() asks nj_make_code_room()
 * for its reservation before every compile attempt, so raising it from 512 to
 * 1280 bytes evicted live trace blocks out of an 8 KB cache roughly ninety
 * times a second, and those traces were doing all the work.
 *
 * Turning this on again therefore needs the block cache to be able to hold
 * what it compiles.  SRAM for a bigger arena does not exist (91.75% used,
 * pc_new() has under 832 bytes of margin), so the way in is to stop inlining
 * ~250 bytes of guard per memory operand and call one shared out-of-line
 * guard routine instead.
 */
/*
 * Memory operands in fully-native loop bodies.
 *
 * This was measured at -23% on Doom when each one inlined ~250 bytes of guard
 * into an 8 KB arena.  With NJIT_SHARED_GUARD the guard is one call, so the
 * reason it lost no longer applies and it follows the guard automatically.
 */
#ifndef NJIT_LOOP_MEM
#define NJIT_LOOP_MEM NJIT_SHARED_GUARD
#endif
#define NJ_TRACK_PAGES      2048u
#define NJ_PAGE_BYTES       (NJ_TRACK_PAGES / 8u)

#define NJ_BRANCH_JZ        1u
#define NJ_BRANCH_JNZ       2u
#define NJ_BRANCH_JMP       3u
#define NJ_BRANCH_LOOP      4u
#define NJ_BRANCH_JCXZ      5u

#define NJ_FLAG_NONE        0u
#define NJ_FLAG_ARITH       1u
#define NJ_FLAG_LOGIC       2u
#define NJ_FLAG_INCDEC      3u

typedef struct {
    uword tag;
    uword mmu_key;
    uword cs_base;
    uword ds_base;
    uword es_base;
    uword ss_base;
    uword bp_value;
    uword sp_value;
    u16 ds_sel;
    u16 es_sel;
    u16 ss_sel;
    u16 _seg_pad;
    uword start_ip;
    uword branch_ip;
    uword fallthrough_ip;
    u16 byte_len;
    u16 phys_page;
    u16 phys_page2;        /* second physical code page when code_split != 0 */
    u16 code_split;        /* bytes on phys_page before crossing linear 4K boundary */
    u8 insns;              /* x86 instructions per loop iteration */
    u8 code16;
    u8 valid;
    u8 needs_refresh_cf;
    u8 needs_flags_in;
    u8 single_run;
    u8 uses_ds_static;
    u8 uses_es_static;
    u8 uses_ss_base;
    u8 uses_ss_static;
    u8 uses_df_static;
    u8 df_value;
    u8 static_write_count;
    uword static_write_phys[3];
    u16 *code;
    u16 arm_halfwords;       /* generated Thumb size; enables zero-SRAM compaction */
    u16 _arm_pad;
    /*
     * FRANK_WORKLOAD_PROFILE_V88: what this block actually retired.
     *
     * njit_diag_reject_snapshot() only ever shows blocks that were
     * REJECTED, so a hot loop that compiled into a short, unprofitable
     * trace is invisible in every capture in this sequence.  These two
     * fields make the compiled side visible too, which is what decides
     * whether the exact Symantec BP-stack block exists under EMM386 at
     * all or was displaced by a generic 5-instruction prefix trace.
     */
#if NJIT_DIAG
    u32 diag_entries;
    u32 diag_insns;
#endif
} nj_block_t;

/* FRANK_NJIT_HOT_TARGET_PROFILE */
/* FRANK_NJIT_HOT_TARGET_PROFILE_32B */
typedef struct {
    uword key;
    uword linear;
    uword start_ip;
    u32 rejected_hits;
    u8 seen;               /* 0xff = known unsupported until invalidation */
#if NJIT_DIAG
    u8 code_len;
    u8 code[32];
    u8 _pad[2];
#endif
} nj_hot_t;

static nj_block_t nj_cache[NJ_CACHE_SLOTS];
static nj_hot_t nj_hot[NJ_HOT_SLOTS];
static u8 nj_page_bits[NJ_PAGE_BYTES];
/* One pseudo-LRU/round-robin bit per two-way set; changed only on a true
 * conflict insertion, never on the hot execution path. */
static u16 nj_cache_replace_bits;

/* FRANK_NATIVE_JIT_V44_SAMPLED_DISCOVERY
 *
 * nj_compiled_bloom is only a positive hint. Bits are deliberately never
 * cleared on block invalidation; a stale bit merely causes an occasional full
 * cache lookup, never execution of stale code.
 *
 * Unknown/uncompiled backedges enter slow discovery only once every 64
 * occurrences. This removes the direct-mapped negative-cache lookup from the
 * normal interpreter path while still discovering genuinely hot loops.
 */
/* 128-bit positive bloom filter for compiled backedge heads.  V7's single
 * 32-bit word becomes almost saturated once a few dozen blocks are resident,
 * at which point unsupported interpreter backedges pay a pointless full cache
 * lookup.  Two independently mixed bits over 128 bits keep that false-positive
 * rate much lower while the hot path still needs only two word tests. */
#define NJ_BLOOM_BITS 128u
#define NJ_BLOOM_WORDS (NJ_BLOOM_BITS / 32u)
static u32 nj_compiled_bloom[NJ_BLOOM_WORDS];
static u32 nj_discovery_tick;

static inline __attribute__((always_inline))
void nj_bloom_pos(uword linear, unsigned *b0, unsigned *b1)
{
    uword h0 = linear * 0x9e3779b1u;
    uword h1 = (linear ^ (linear >> 11) ^ 0x85ebca6bu) * 0xc2b2ae35u;
    h0 ^= h0 >> 16;
    h1 ^= h1 >> 15;
    *b0 = (unsigned)(h0 & (NJ_BLOOM_BITS - 1u));
    *b1 = (unsigned)(h1 & (NJ_BLOOM_BITS - 1u));
}

static inline __attribute__((always_inline))
void nj_bloom_add(uword linear)
{
    unsigned b0, b1; nj_bloom_pos(linear, &b0, &b1);
    nj_compiled_bloom[b0 >> 5] |= 1u << (b0 & 31u);
    nj_compiled_bloom[b1 >> 5] |= 1u << (b1 & 31u);
}

static inline __attribute__((always_inline))
bool nj_bloom_maybe(uword linear)
{
    unsigned b0, b1; nj_bloom_pos(linear, &b0, &b1);
    return (nj_compiled_bloom[b0 >> 5] & (1u << (b0 & 31u))) &&
           (nj_compiled_bloom[b1 >> 5] & (1u << (b1 & 31u)));
}

/* Executable SRAM, same placement strategy as the abandoned upstream JIT. */
static u16 __attribute__((section(".data"), aligned(4)))
nj_code[NJ_CODE_BYTES / 2u];
static u16 *nj_code_ptr = nj_code;

static void nj_flush(void);
static void nj_rebuild_pages(void);

/*
 * V8.2 compacting code arena.
 *
 * Generated blocks are position-independent with respect to their own start:
 * all intra-block Thumb branches are relative and external helper targets are
 * materialised as absolute immediates. Therefore a whole block can be moved
 * with memmove() without re-patching it.
 *
 * V8.1 appended every recompilation forever and flushed the entire 16 KiB
 * arena when it reached the end. With longer supertraces Doom accumulated
 * hundreds of full-cache flushes even though the direct-mapped metadata held
 * only 16 live blocks. Compacting only the currently-live blocks recycles the
 * holes left by replacements/invalidations with no additional SRAM.
 */
static void nj_compact_code(void)
{
    unsigned order[NJ_CACHE_SLOTS];
    unsigned n = 0;

    for (unsigned i = 0; i < NJ_CACHE_SLOTS; ++i)
        if (nj_cache[i].valid && nj_cache[i].code && nj_cache[i].arm_halfwords)
            order[n++] = i;

    /* Sort by old source address so downward memmove cannot overwrite a block
     * that has not been copied yet. NJ_CACHE_SLOTS is tiny (16). */
    for (unsigned i = 1; i < n; ++i) {
        unsigned x = order[i];
        unsigned j = i;
        while (j && nj_cache[order[j - 1u]].code > nj_cache[x].code) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = x;
    }

    u16 *dst = nj_code;
    for (unsigned k = 0; k < n; ++k) {
        nj_block_t *b = &nj_cache[order[k]];
        unsigned hw = b->arm_halfwords;
        if (b->code != dst)
            memmove(dst, b->code, hw * sizeof(u16));
        b->code = dst;
        dst += hw;
    }
    nj_code_ptr = dst;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
}

static bool nj_make_code_room(unsigned bytes)
{
    unsigned need_hw = (bytes + 1u) / 2u;
    u16 *end = nj_code + (NJ_CODE_BYTES / 2u);

    if ((unsigned)(end - nj_code_ptr) >= need_hw)
        return true;

    nj_compact_code();
    if ((unsigned)(end - nj_code_ptr) >= need_hw)
        return true;

    /* V8.3: never throw away the whole native cache just because the live
     * working set currently fills the 16 KiB arena. Evict the largest live
     * block(s) one at a time, rebuild the cheap page/bloom summaries, and
     * compact again. This keeps the other hot blocks warm and avoids the
     * compile storm caused by a global flush. */
    while ((unsigned)(end - nj_code_ptr) < need_hw) {
        int victim = -1;
        unsigned largest = 0;
        for (unsigned i = 0; i < NJ_CACHE_SLOTS; ++i) {
            if (nj_cache[i].valid && nj_cache[i].arm_halfwords > largest) {
                largest = nj_cache[i].arm_halfwords;
                victim = (int)i;
            }
        }
        if (victim < 0)
            return false;
        nj_cache[victim].valid = 0;
        nj_rebuild_pages();
        nj_compact_code();
    }
    return true;
}

volatile u32 g_njit_hits __attribute__((used));
volatile u32 g_njit_misses __attribute__((used));
volatile u32 g_njit_compiles __attribute__((used));
volatile u32 g_njit_insns __attribute__((used));
volatile u32 g_njit_native_iters __attribute__((used));
volatile u32 g_njit_invalidations __attribute__((used));
volatile u32 g_njit_hotwait __attribute__((used));
volatile u32 g_njit_rejects __attribute__((used));
volatile u32 g_njit_flushes __attribute__((used));

/* FRANK_NJIT_REJECT_PROFILE
 * Why hot candidates fail to become native loops.
 * Updated only on compilation attempts, not on the guest instruction path.
 */
enum {
    NJR_CODE_WINDOW = 0,
    NJR_BODY_OPCODE,
    NJR_FLAG_SCRATCH,
    NJR_INCDEC_CF,
    NJR_NO_BRANCH,
    NJR_EMPTY_BODY,
    NJR_JCC_NO_FLAGS,
    NJR_PATCH,
    NJR_EMIT,
    NJR_COUNT
};
volatile u32 g_njit_rej_reason[NJR_COUNT] __attribute__((used));
volatile u32 g_njit_rej_last_ip __attribute__((used));
volatile u32 g_njit_rej_last_pos __attribute__((used));
volatile u32 g_njit_rej_last_opcode __attribute__((used));
volatile u32 g_njit_rej_last_body_insns __attribute__((used));

volatile u32 g_njit_rej_op[8] __attribute__((used));
volatile u32 g_njit_rej_op_count[8] __attribute__((used));

/*
 * Where the broad v6 trace stops.
 *
 * The loop compiler's reject histogram only says why *it* declined; the trace
 * compiler is the fallback that actually runs, and it ends the trace at the
 * first instruction it cannot emit.  With native coverage measured at 0.4% of
 * Doom's instruction stream, what limits the JIT is trace length, so the
 * useful histogram is the opcode each trace died on.
 */
/*
 * Kept in PSRAM, not SRAM.  A 1 KB array of counters in SRAM was enough to
 * make pc_new() fail its allocation and the board came up to a black screen -
 * the failure audiodiag.h warns about.  Guest physical 0xa8000 is inside the
 * VGA aperture, which the emulator redirects to gfx_buffer, so nothing else
 * is backed by PSRAM there; FrankDiag lives in the same hole at 0xa1000.
 *
 * Slots 0..255 are the opcode histogram, 256 is traces compiled and 257 is
 * traces that stopped early.  Zero it from the host before a measurement.
 */
#define NJ_V6_STOP ((volatile u32 *)(0x11000000u + 0x000a8000u))

/*
 * FRANK_NJIT_EXIT_RING - the last sixteen native block exits.
 *
 * A JIT that returns a wrong next_ip does not fail where the mistake is: the
 * interpreter fetches from the bad address and takes a bus fault somewhere
 * else entirely, which is all a boot loop tells you.  This ring lives in the
 * PSRAM diagnostic hole so it survives the watchdog reboot, and names the
 * block that produced the bad exit.
 *
 * Debug only - four PSRAM writes per block execution is not a shipping cost.
 */
/*
 * FRANK_NJIT_EXIT_CHECK
 *
 * Catch a block that leaves a next_ip the machine cannot fetch from, at the
 * moment it happens rather than wherever the interpreter later faults.
 *
 * The evidence for the shared-guard boot loop was a precise bus fault in the
 * interpreter's prefetch on guest physical 0xF010FF50 - i.e. some block had
 * already returned nonsense and the fault landed somewhere else entirely.
 * A block must leave next_ip inside the code segment; anything past the limit
 * would #GP on real hardware and is far more likely to be our bug.
 *
 * On a violation this records the offending block in the watchdog scratch
 * (which survives a reset) and switches the JIT off, so the board keeps
 * running on the interpreter and stays available to be read.
 */
#ifndef NJIT_EXIT_CHECK
#define NJIT_EXIT_CHECK 0
#endif
#if NJIT_EXIT_CHECK
#define NJ_BAD_MAGIC 0x4E4A4244u   /* "NJBD" */
static bool nj_disabled;
static void nj_note_bad_exit(CPUI386 *cpu, const nj_block_t *b, uword nip)
{
    /* RP2350 watchdog scratch[0..3]; the only store that survives a reset.
     * Addressed directly because i386.c does not include the pico headers. */
    volatile u32 *scratch = (volatile u32 *)0x400d800cu;
    scratch[0] = NJ_BAD_MAGIC;
    scratch[1] = (u32)b->start_ip;
    scratch[2] = (u32)nip;
    scratch[3] = (u32)b->byte_len |
                 ((u32)b->insns << 16) |
                 ((u32)b->single_run << 24) |
                 ((u32)cpu->code16 << 25);
    nj_flush();
    nj_disabled = true;
}
static inline bool nj_exit_sane(CPUI386 *cpu, const nj_block_t *b)
{
    uword nip = cpu->next_ip;
    if (cpu->code16) nip &= 0xffffu;
    if (unlikely(nip > cpu->seg[SEG_CS].limit)) {
        nj_note_bad_exit(cpu, b, nip);
        return false;
    }
    return true;
}
#endif

#ifndef NJIT_EXIT_RING
#define NJIT_EXIT_RING 0
#endif
#if NJIT_EXIT_RING
#define NJ_XR ((volatile u32 *)(0x11000000u + 0x000a9000u))
#define NJ_XR_SLOTS 16u
static unsigned nj_xr_idx;
static inline void nj_xr_note(const nj_block_t *b, uword nip, int done)
{
    if (unlikely(NJ_XR[NJ_XR_SLOTS * 4u + 1u] == 0x46524F5Au))
        return;                    /* frozen by a fault - keep the evidence */
    volatile u32 *r = NJ_XR + (nj_xr_idx & (NJ_XR_SLOTS - 1u)) * 4u;
    r[0] = (u32)b->start_ip;
    r[1] = (u32)nip;
    r[2] = (u32)done;
    r[3] = (u32)b->byte_len | ((u32)b->insns << 16) |
           ((u32)b->single_run << 24);
    NJ_XR[NJ_XR_SLOTS * 4u] = ++nj_xr_idx;
}
#else
#define nj_xr_note(b, nip, done) do { } while (0)
#endif
#define NJ_V6_STOP_TRACES  256
#define NJ_V6_STOP_STOPPED 257

/*
 * FRANK_NATIVE_JIT_V8_10_DIAG
 *
 * Two blind spots that eleven captures could not see through.
 *
 * 1. nj_compile_v45_bpstack() is the only compiler that ever produced a
 *    flags=6 block, and that one block is the whole difference between
 *    5.96 MIPS (jitstats001/002, no EMM386) and 1.90 MIPS (jitstats004-010,
 *    V86).  It has nine unconditional "return NULL" exits and not one of
 *    them touches a counter, so no capture can say whether the loop is
 *    never seen at a hot IP at all, or seen and refused by a guard.
 *    NJBP_MATCHED is the pivot: below it the address was simply wrong,
 *    above it the bytes were the exact Symantec loop and a specific guard
 *    turned it down.
 *
 * 2. nj_exec_chain() breaks for five different reasons and reports none.
 *    The 0x1075e3 block retires 2.25 of its 7 instructions across ~52k
 *    entries, which can only happen if the chain breaks on the partial
 *    side-exit test every single time.  NJCH_BRK_PARTIAL and
 *    NJCH_PARTIAL_LOST measure that directly.
 *
 * Diagnostics only: no guard, no emitted code and no dispatcher decision
 * changes in this build.  The counters live in .bss, are written only on
 * the compile path and once per chained block, and are reset by Win+F7
 * with every other window counter.
 */
enum {
    NJBP_ATTEMPTS = 0,     /* every nj_compile_v45_bpstack() call */
    NJBP_NOT_CODE16,
    NJBP_SP_MASK,
    NJBP_CODE_WINDOW,
    NJBP_PATTERN,          /* window read, bytes are not the exact loop */
    NJBP_MATCHED,          /* bytes ARE the exact loop: everything past
                              this point is a guard refusal, not a miss */
    NJBP_SPLIT_NONCONTIG,
    NJBP_STATIC_M2,
    NJBP_STATIC_M4,
    NJBP_STATIC_M6,
    NJBP_STATIC_STK,
    NJBP_M6_NONADJ,
    NJBP_CODE_OVERLAP,
    NJBP_NO_ROOM,
    NJBP_EMIT,
    NJBP_OK,
    NJBP_COUNT
};
volatile u32 g_njit_bp[NJBP_COUNT] __attribute__((used));

static inline nj_block_t *nj_bp_rej(unsigned why)
{
    g_njit_bp[why]++;
    return NULL;
}

enum {
    NJCH_CALLS = 0,        /* nj_exec_chain() calls */
    NJCH_BLOCKS,           /* blocks executed across all chains */
    NJCH_BRK_ZERO,         /* nj_exec_loop() retired nothing */
    NJCH_BRK_NOT_SINGLE,   /* loop block: chaining does not apply */
    NJCH_BRK_PARTIAL,      /* done != nominal: guarded side exit */
    NJCH_BRK_BUDGET,       /* max_steps consumed */
    NJCH_BRK_NEGCACHE,     /* continuation head known unsupported */
    NJCH_BRK_COMPILE,      /* continuation would not compile */
    NJCH_BRK_MAXBLOCKS,    /* hit NJ_V8_CHAIN_MAX_BLOCKS */
    NJCH_PARTIAL_LOST,     /* sum of (nominal - done) over partial exits */
    /*
     * FRANK_NATIVE_JIT_V8_10_1_DIAG.
     *
     * Captures 012 and 015 both put 94-96% of every chain break on the
     * partial side exit, but "partial" fuses two opposite events.
     *
     * A memory guard (nj_v8_finish_guard) reports the instruction as NOT
     * retired and leaves next_ip on that instruction, which is inside the
     * block's own bytes.  The interpreter must run it once; breaking is
     * correct.
     *
     * A compiled branch exit (nj_v8_exit_stub_imm) reports the branch as
     * retired and sets next_ip to its target, normally outside the block.
     * There breaking is pure waste: the chain discards a continuation for
     * no reason, and PARTIAL_READY counts the ones where a compiled block
     * for that target already exists.
     *
     * A backward branch into the same block lands INSIDE too, so INSIDE is
     * an upper bound on guard exits, not an exact count.  OUTSIDE is exact.
     */
    NJCH_PARTIAL_INSIDE,   /* next_ip within the block: guard, or back-branch */
    NJCH_PARTIAL_OUTSIDE,  /* next_ip beyond the block: ordinary control flow */
    NJCH_PARTIAL_READY,    /* ...and a compiled block for it probably exists */
    NJCH_COUNT
};
volatile u32 g_njit_ch[NJCH_COUNT] __attribute__((used));

/* main.c declares these with literal sizes so the record it prints can never
 * silently drift out of step with the enums above. */
_Static_assert(NJBP_COUNT == 16, "NJBP_SLOTS in main.c must match NJBP_COUNT");
_Static_assert(NJCH_COUNT == 13, "NJCH_SLOTS in main.c must match NJCH_COUNT");


/*
 * Full rejection histogram, one u16 per opcode.
 *
 * The 8-slot table below evicts by lowest count, which hides the shape of the
 * problem: implementing MOV r/m16,Sreg (0x8c) removed 1593 refusals and
 * immediately exposed 0x0f with 990, invisible until then because commoner
 * entries kept pushing it out. Deciding whether broad opcode coverage is
 * reachable at all needs the whole distribution, not its top eight.
 *
 * It lives in SCRATCH_X deliberately. Two earlier attempts put it in .bss and
 * both killed the board: the firmware panicked inside pc_new() and stopped at
 * DIAG_PRE_PC_NEW with no video, which looks exactly like dead hardware. The
 * second attempt did that with *more* free heap than the working build had
 * (47,248 vs 43,600 bytes), so plain heap exhaustion was not the mechanism and
 * this image is simply intolerant of changes to its main RAM layout.
 *
 * SCRATCH_X is a separate 4 KB region that the heap never sees, and it was 75%
 * used, so 512 bytes fit. Better still, overflowing it is a *link* error
 * rather than a board that boots into a panic - which is the whole point after
 * breaking the display twice.
 */
volatile u16 g_njit_rej_hist[256]
    __attribute__((section(".scratch_x.rejhist"), used));

/*
 * ModRM byte of refused instructions.
 *
 * This slot previously held the second byte of 0x0f opcodes, which did its job:
 * it showed that the 37.7% hiding behind the 0x0f escape was almost entirely
 * 0F B7, MOVZX r,r/m16. Implementing that for register sources then changed
 * nothing at all (1024 refusals -> 1022), which says the real operand is
 * memory, not a register.
 *
 * That is the question this histogram now answers, because it decides how much
 * work the remaining refusals actually are. The top two bits of ModRM give the
 * addressing form:
 *
 *   mod=11  register operand      - already cheap
 *   mod=00 rm=110  [disp16]       - absolute, offset known at compile time, so
 *                                   nj_v45_static_word() and the existing
 *                                   nj_block_matches() guard cover it
 *   anything else                 - address depends on runtime registers, and
 *                                   needs a TLB probe inside generated code
 *
 * Reusing the same 256 bytes keeps SCRATCH_X exactly where it is; .bss is not
 * an option on this image, which panics in pc_new() if it grows.
 */
volatile u8 g_njit_rej_modrm[256]
    __attribute__((section(".scratch_x.rejmodrm"), used));

/* Skip prefixes, skip the opcode (two bytes for 0x0f), record the ModRM. */
static void nj_rej_note_modrm(const u8 *p, unsigned max)
{
    unsigned i = 0;
    while (i < max && i < 8u) {
        u8 b = p[i];
        if (b == 0x26u || b == 0x2eu || b == 0x36u || b == 0x3eu ||
            b == 0x64u || b == 0x65u || b == 0x66u || b == 0x67u) {
            i++;
            continue;
        }
        break;
    }
    if (i >= max) return;
    i += (p[i] == 0x0fu) ? 2u : 1u;
    if (i >= max) return;
    if (g_njit_rej_modrm[p[i]] != 0xffu) g_njit_rej_modrm[p[i]]++;
}

static void nj_rej_note_opcode(u8 op)
{
    int empty = -1, min_i = 0;
    u32 min_n = 0xffffffffu;

    if (g_njit_rej_hist[op] != 0xffffu) g_njit_rej_hist[op]++;



    for (int i = 0; i < 8; ++i) {
        if (g_njit_rej_op_count[i] && (u8)g_njit_rej_op[i] == op) {
            g_njit_rej_op_count[i]++;
            return;
        }
        if (!g_njit_rej_op_count[i] && empty < 0) empty = i;
        if (g_njit_rej_op_count[i] < min_n) {
            min_n = g_njit_rej_op_count[i];
            min_i = i;
        }
    }

    int i = empty >= 0 ? empty : min_i;
    g_njit_rej_op[i] = op;
    g_njit_rej_op_count[i] = 1;
}

static void nj_rej_note(unsigned reason, uword ip, unsigned pos,
                        unsigned opcode, unsigned body_insns)
{
    if (reason < NJR_COUNT) g_njit_rej_reason[reason]++;
    g_njit_rej_last_ip = ip;
    g_njit_rej_last_pos = pos;
    g_njit_rej_last_opcode = opcode;
    g_njit_rej_last_body_insns = body_insns;
    if (reason == NJR_BODY_OPCODE)
        nj_rej_note_opcode((u8)opcode);
}

static inline __attribute__((always_inline))
unsigned nj_hash(uword v, unsigned bits)
{
    return (unsigned)((v * 2654435761u) >> (32u - bits));
}

static inline __attribute__((always_inline))
uword nj_mmu_key(CPUI386 *cpu)
{
    /*
     * V8 deliberately does NOT key generated code by CR3. Dynamic memory is
     * translated through the live guest TLB at run time and code fetch is
     * validated against the currently mapped physical code page(s) before
     * every native entry. A CR3 reload therefore does not make otherwise
     * identical ARM code stale. CPL remains part of the key because the
     * generated permission-row lookup is specialized for user/supervisor.
     */
    return (cpu->cr0 & 0x80000001u) ^ cpu->a20_mask ^
           ((uword)cpu->code16 << 30) ^ ((uword)(cpu->cpl & 3) << 27);
}

static inline __attribute__((always_inline))
uword nj_context_key(CPUI386 *cpu, uword linear)
{
    return linear ^ nj_mmu_key(cpu);
}

static inline __attribute__((always_inline))
uword nj_hot_context_key(CPUI386 *cpu, uword linear)
{
    /* Negative/discovery entries describe code bytes not generated ARM. Keep
     * CR3 here so an unsupported head in one address space cannot poison a
     * different mapping at the same linear address. Compiled blocks themselves
     * remain CR3-independent and are protected by physical mapping guards. */
    uword key=nj_context_key(cpu,linear);
    if(cpu->cr0 & CR0_PG) key ^= cpu->cr3;
    return key;
}

static inline __attribute__((always_inline))
unsigned nj_cache_set(CPUI386 *cpu, uword linear)
{
    return nj_hash(nj_context_key(cpu, linear), NJ_CACHE_SET_BITS);
}

/* Choose an insertion victim only after the new block has been emitted. An
 * invalid way is always preferred; otherwise alternate the two ways. The old
 * block's code becomes a harmless hole reclaimed by the existing compactor. */
static nj_block_t *nj_cache_insert_slot(CPUI386 *cpu, uword linear)
{
    unsigned set = nj_cache_set(cpu, linear);
    unsigned base = set * NJ_CACHE_WAYS;
    for (unsigned w = 0; w < NJ_CACHE_WAYS; ++w)
        if (!nj_cache[base + w].valid)
            return &nj_cache[base + w];

    unsigned way = (nj_cache_replace_bits >> set) & 1u;
    nj_cache_replace_bits ^= (u16)(1u << set);
    nj_cache[base + way].valid = 0;
    return &nj_cache[base + way];
}

static inline bool nj_page_marked(unsigned p)
{
    return p < NJ_TRACK_PAGES &&
           (nj_page_bits[p >> 3] & (1u << (p & 7)));
}

static inline void nj_page_mark(unsigned p)
{
    if (p < NJ_TRACK_PAGES)
        nj_page_bits[p >> 3] |= (u8)(1u << (p & 7));
}

static void nj_rebuild_pages(void)
{
    memset(nj_page_bits, 0, sizeof(nj_page_bits));
    memset(nj_compiled_bloom, 0, sizeof(nj_compiled_bloom));
    for (unsigned i = 0; i < NJ_CACHE_SLOTS; ++i) {
        if (nj_cache[i].valid) {
            nj_page_mark(nj_cache[i].phys_page);
            if (nj_cache[i].code_split) nj_page_mark(nj_cache[i].phys_page2);
            nj_bloom_add(nj_cache[i].tag);
        }
    }
}

static void nj_flush(void)
{
    memset(nj_cache, 0, sizeof(nj_cache));
    memset(nj_hot, 0, sizeof(nj_hot));
    memset(nj_page_bits, 0, sizeof(nj_page_bits));
    nj_cache_replace_bits = 0;
    memset(nj_compiled_bloom, 0, sizeof(nj_compiled_bloom));
    nj_code_ptr = nj_code;
    g_njit_flushes++;
}

static void nj_invalidate_page(unsigned page)
{
    bool changed = false;
    for (unsigned i = 0; i < NJ_CACHE_SLOTS; ++i) {
        if (nj_cache[i].valid &&
            (nj_cache[i].phys_page == page ||
             (nj_cache[i].code_split && nj_cache[i].phys_page2 == page))) {
            nj_cache[i].valid = 0;
            changed = true;
        }
    }
    if (changed) {
        g_njit_invalidations++;
        nj_rebuild_pages();
        memset(nj_hot, 0, sizeof(nj_hot));
    }
}

/*
 * Exact invalidation used by native blocks that write to statically-known
 * guest RAM.  The normal interpreter invalidates per 4 KiB page, which is
 * deliberately conservative.  A native stack loop may legitimately keep
 * data and code on the same guest page, so page-wide invalidation would throw
 * away the block on every entry.  Here we can use the exact write ranges.
 */
static void nj_invalidate_exact_range(uword addr, unsigned len,
                                      const nj_block_t *except)
{
    if (!len) return;
    uword end = addr + len;
    if (end < addr) end = (uword)-1;

    bool changed = false;
    for (unsigned i = 0; i < NJ_CACHE_SLOTS; ++i) {
        nj_block_t *b = &nj_cache[i];
        if (!b->valid || b == except) continue;

        uword bs = ((uword)b->phys_page << 12) | (b->tag & 0xfffu);
        unsigned first_len = b->code_split ? b->code_split : b->byte_len;
        uword be = bs + first_len;
        bool overlap = addr < be && bs < end;

        if (!overlap && b->code_split && b->byte_len > b->code_split) {
            uword bs2 = (uword)b->phys_page2 << 12;
            uword be2 = bs2 + (b->byte_len - b->code_split);
            overlap = addr < be2 && bs2 < end;
        }
        if (overlap) {
            b->valid = 0;
            changed = true;
        }
    }

    if (changed) {
        g_njit_invalidations++;
        nj_rebuild_pages();
        memset(nj_hot, 0, sizeof(nj_hot));
    }
}

/*
 * FRANK_NATIVE_JIT_V51_EXACT_WRITE_INVALIDATION
 *
 * Interpreter writes arrive here with a guest *physical* address.  The old
 * path invalidated every translated block on the whole 4 KiB page.  That is
 * correct but disastrous for DOS code where stack/data and code commonly
 * share a page: the productive Symantec block was compiled, executed once,
 * then discarded by an unrelated stack/data store on the same page.
 *
 * We already have an exact physical-range overlap checker for native static
 * stores. Reuse it here.  This keeps self-modifying-code correctness while
 * preserving a cached block when a write touches unrelated bytes on its page.
 */
static inline void nj_note_write(CPUI386 *cpu, uword addr, unsigned len)
{
    (void)cpu;
    if (unlikely(!len))
        return;

    unsigned p0 = addr >> 12;
    unsigned p1 = (addr + len - 1u) >> 12;

    /*
     * The page bitmap remains a very cheap rejection filter.  If neither page
     * contains translated code there cannot be an overlap.
     */
    if (likely(!nj_page_marked(p0) &&
               (p1 == p0 || !nj_page_marked(p1))))
        return;

    nj_invalidate_exact_range(addr, len, NULL);
}

static int nj_code_window(CPUI386 *cpu, uword ip,
                          const u8 **out, unsigned *avail,
                          unsigned *phys_page);

/*
 * FRANK_NJIT_REJECT_RETRY — let a blacklisted head back in occasionally.
 *
 * nj_reject() writes seen=0xff and nothing ever clears it except the slot
 * being evicted by a different key. That is correct for a *structural*
 * refusal: an opcode the emitter does not support will not become supported.
 * It is wrong for a *state-dependent* one, where the guest page simply was not
 * resident at the moment the guard looked, and would have been a millisecond
 * later.
 *
 * v8.10-diag measured what that costs on the Symantec Overall Performance
 * Index run: 138,806 of 144,469 nj_exec_chain() calls (96.1%) ended at the
 * negative cache, and chains averaged 1.00 blocks per call - the chaining
 * mechanism never extended past its first block at all. Only 7 of those
 * refusals came from a compile that actually failed in this window, so the
 * rest were decisions made long before and never revisited.
 *
 * Letting a head retry every NJ_REJECT_RETRY executions bounds the cost: a
 * structural refusal fails again and pays one compile attempt per that many
 * executions, while a state-dependent one finally gets the second look it
 * never had. Set NJ_REJECT_RETRY to 0 to restore the permanent blacklist.
 */
#ifndef NJ_REJECT_RETRY
#define NJ_REJECT_RETRY 256u
#endif

volatile u32 g_njit_retry_admitted __attribute__((used));

static inline bool nj_reject_retry_due(nj_hot_t *h)
{
    h->rejected_hits++;
#if NJ_REJECT_RETRY
    if (h->rejected_hits >= NJ_REJECT_RETRY) {
        h->rejected_hits = 0;
        h->seen = NJ_HOT_THRESHOLD;     /* eligible again on this pass */
        g_njit_retry_admitted++;
        return true;
    }
#endif
    return false;
}

static inline bool nj_hot_enough(CPUI386 *cpu, uword linear)
{
    uword key = nj_hot_context_key(cpu, linear);
    nj_hot_t *h = &nj_hot[nj_hash(key, NJ_HOT_BITS)];
    if (h->key != key) {
        memset(h, 0, sizeof(*h));
        h->key = key;
        h->linear = linear;
        h->seen = 1;
        return false;
    }
    if (h->seen == 0xff) {
        return nj_reject_retry_due(h);
    }
    if (h->seen < NJ_HOT_THRESHOLD) {
        h->seen++;
        return false;
    }
    return true;
}

static inline void nj_reject(CPUI386 *cpu, uword linear, uword start_ip)
{
    uword key = nj_hot_context_key(cpu, linear);
    nj_hot_t *h = &nj_hot[nj_hash(key, NJ_HOT_BITS)];

    h->key = key;
    h->linear = linear;
    h->start_ip = start_ip;
    h->seen = 0xff;
    h->rejected_hits = 1;
#if NJIT_DIAG
    h->code_len = 0;

    const u8 *code;
    unsigned avail, phys_page;
    if (nj_code_window(cpu, start_ip, &code, &avail, &phys_page)) {
        unsigned n = avail < sizeof(h->code) ? avail : sizeof(h->code);
        memcpy(h->code, code, n);
        h->code_len = (u8)n;
    }
#endif

    g_njit_rejects++;
}

void njit_diag_reset_hot(void)
{
    /*
     * F7 is a benchmark checkpoint, not just a counter reset. Start from a
     * genuinely cold native cache so boot-time polling loops cannot leak into
     * the measurement, and make discovery deterministic.
     */
    nj_flush();
    nj_discovery_tick = 0;
}

unsigned njit_diag_reject_snapshot(u32 *linear, u32 *ip, u32 *hits,
                                   u8 *bytes, u8 *lens, unsigned cap)
{
#if !NJIT_DIAG
    /* The per-entry copies these report are compiled out; see NJIT_DIAG. */
    return 0;
#else
    if (!linear || !ip || !hits || !bytes || !lens || !cap) return 0;

    for (unsigned i = 0; i < cap; ++i) {
        linear[i] = 0;
        ip[i] = 0;
        hits[i] = 0;
        lens[i] = 0;
        memset(bytes + i * 32u, 0, 32u);
    }

    unsigned used = 0;
    for (unsigned s = 0; s < NJ_HOT_SLOTS; ++s) {
        const nj_hot_t *h = &nj_hot[s];
        if (h->seen != 0xff || h->rejected_hits == 0) continue;

        unsigned pos = 0;
        while (pos < used && hits[pos] >= h->rejected_hits) pos++;
        if (pos >= cap) continue;

        unsigned lim = used < cap ? used : cap - 1u;
        if (used < cap) used++;

        for (unsigned j = lim; j > pos; --j) {
            linear[j] = linear[j - 1u];
            ip[j] = ip[j - 1u];
            hits[j] = hits[j - 1u];
            lens[j] = lens[j - 1u];
            memcpy(bytes + j * 32u, bytes + (j - 1u) * 32u, 32u);
        }

        linear[pos] = h->linear;
        ip[pos] = h->start_ip;
        hits[pos] = h->rejected_hits;
        lens[pos] = h->code_len;
        memcpy(bytes + pos * 32u, h->code, 32u);
    }

    return used;
#endif
}

/*
 * FRANK_WORKLOAD_PROFILE_V88: the compiled counterpart of
 * njit_diag_reject_snapshot().  Returns the live cache slots ordered by
 * the guest instructions they actually retired, which is the only ranking
 * that says where native execution time went.
 */
unsigned njit_diag_block_snapshot(u32 *linear, u32 *insns, u32 *entries,
                                  u8 *ninsns, u8 *flags, unsigned cap)
{
#if !NJIT_DIAG
    /* The per-block counters these report are compiled out; see NJIT_DIAG. */
    return 0;
#else
    if (!linear || !insns || !entries || !ninsns || !flags || !cap) return 0;

    for (unsigned i = 0; i < cap; ++i) {
        linear[i] = 0; insns[i] = 0; entries[i] = 0;
        ninsns[i] = 0; flags[i] = 0;
    }

    unsigned used = 0;
    for (unsigned s = 0; s < NJ_CACHE_SLOTS; ++s) {
        const nj_block_t *b = &nj_cache[s];
        if (!b->valid || b->diag_entries == 0) continue;

        unsigned pos = 0;
        while (pos < used && insns[pos] >= b->diag_insns) pos++;
        if (pos >= cap) continue;

        unsigned lim = used < cap ? used : cap - 1u;
        if (used < cap) used++;

        for (unsigned j = lim; j > pos; --j) {
            linear[j] = linear[j - 1u];
            insns[j] = insns[j - 1u];
            entries[j] = entries[j - 1u];
            ninsns[j] = ninsns[j - 1u];
            flags[j] = flags[j - 1u];
        }

        linear[pos] = (u32)b->tag;
        insns[pos] = b->diag_insns;
        entries[pos] = b->diag_entries;
        ninsns[pos] = b->insns;
        /* bit0 single_run, bit1 code16, bit2 exact BP-stack block */
        flags[pos] = (u8)((b->single_run ? 1u : 0u) |
                          (b->code16 ? 2u : 0u) |
                          (b->static_write_count == 3u ? 4u : 0u));
    }

    return used;
#endif
}

/* ----------------------------- Thumb emitter ----------------------------- */

typedef struct {
    u16 *p;
    u16 *start;
    u16 *limit;
    bool failed;
} nj_emit_t;

typedef struct {
    unsigned pos;
    bool op16;
    bool addr16;
    int seg;
} nj_v6_pfx_t;

typedef struct {
    int base;
    int index;
    unsigned scale;
    s32 disp;
    int seg;
    unsigned used;
    bool addr16;
} nj_v6_ea_t;

typedef struct {
    u16 *at[8];
    unsigned cond[8];
    unsigned n;
} nj_v6_guard_t;


static inline void nj_e16(nj_emit_t *e, u16 v)
{
    if (unlikely(e->p >= e->limit)) { e->failed = true; return; }
    *e->p++ = v;
}

static inline void nj_e32(nj_emit_t *e, u32 v)
{
    if (unlikely(e->p + 2 > e->limit)) { e->failed = true; return; }
    *e->p++ = (u16)(v >> 16);
    *e->p++ = (u16)v;
}

static inline void nj_mov_imm(nj_emit_t *e, unsigned rd, u32 imm)
{
    u16 lo = (u16)imm;
    u16 hi = (u16)(imm >> 16);
    u32 w = 0xF2400000u;
    w |= ((u32)(lo >> 12) & 0xfu) << 16;
    w |= ((u32)(lo >> 11) & 1u) << 26;
    w |= ((u32)(lo >> 8) & 7u) << 12;
    w |= (u32)(rd & 0xfu) << 8;
    w |= lo & 0xffu;
    nj_e32(e, w);
    if (hi) {
        u32 t = 0xF2C00000u;
        t |= ((u32)(hi >> 12) & 0xfu) << 16;
        t |= ((u32)(hi >> 11) & 1u) << 26;
        t |= ((u32)(hi >> 8) & 7u) << 12;
        t |= (u32)(rd & 0xfu) << 8;
        t |= hi & 0xffu;
        nj_e32(e, t);
    }
}

/* MOV (register), T1/T2 16-bit form; does not change APSR. */
static inline void nj_mov_reg(nj_emit_t *e, unsigned rd, unsigned rm)
{
    u16 v = 0x4600u;
    v |= (u16)(rd & 7u);
    v |= (u16)(((rd >> 3) & 1u) << 7);
    v |= (u16)((rm & 15u) << 3);
    nj_e16(e, v);
}

static inline void nj_ldr32(nj_emit_t *e, unsigned rt, unsigned rn, unsigned off)
{
    if (unlikely(off >= 4096u)) { e->failed = true; return; }
    nj_e32(e, 0xF8D00000u | ((u32)rn << 16) | ((u32)rt << 12) | off);
}

static inline void nj_str32(nj_emit_t *e, unsigned rt, unsigned rn, unsigned off)
{
    if (unlikely(off >= 4096u)) { e->failed = true; return; }
    nj_e32(e, 0xF8C00000u | ((u32)rn << 16) | ((u32)rt << 12) | off);
}

static inline void nj_ldrh(nj_emit_t *e, unsigned rt, unsigned rn, unsigned off)
{
    if (unlikely(off >= 4096u)) { e->failed = true; return; }
    nj_e32(e, 0xF8B00000u | ((u32)rn << 16) | ((u32)rt << 12) | off);
}

static inline void nj_strh(nj_emit_t *e, unsigned rt, unsigned rn, unsigned off)
{
    if (unlikely(off >= 4096u)) { e->failed = true; return; }
    nj_e32(e, 0xF8A00000u | ((u32)rn << 16) | ((u32)rt << 12) | off);
}

/* v6 byte and shift primitives used by the general trace fast path. */
static inline void nj_ldrb(nj_emit_t *e, unsigned rt, unsigned rn, unsigned off)
{
    if (unlikely(off >= 4096u)) { e->failed = true; return; }
    nj_e32(e, 0xF8900000u | ((u32)rn << 16) | ((u32)rt << 12) | off);
}

static inline void nj_strb(nj_emit_t *e, unsigned rt, unsigned rn, unsigned off)
{
    if (unlikely(off >= 4096u)) { e->failed = true; return; }
    nj_e32(e, 0xF8800000u | ((u32)rn << 16) | ((u32)rt << 12) | off);
}

static inline void nj_lsls_imm(nj_emit_t *e, unsigned rd, unsigned rm, unsigned sh)
{
    if (unlikely(rd > 7u || rm > 7u || sh > 31u)) { e->failed = true; return; }
    nj_e16(e, (u16)(((sh & 31u) << 6) | ((rm & 7u) << 3) | (rd & 7u)));
}

static inline void nj_lsrs_imm(nj_emit_t *e, unsigned rd, unsigned rm, unsigned sh)
{
    if (unlikely(rd > 7u || rm > 7u || sh == 0u || sh > 32u)) {
        e->failed = true; return;
    }
    unsigned enc = sh == 32u ? 0u : sh;
    nj_e16(e, (u16)(0x0800u | ((enc & 31u) << 6) |
                     ((rm & 7u) << 3) | (rd & 7u)));
}

static inline void nj_asrs_imm(nj_emit_t *e, unsigned rd, unsigned rm, unsigned sh)
{
    if (unlikely(rd > 7u || rm > 7u || sh == 0u || sh > 32u)) {
        e->failed = true; return;
    }
    unsigned enc = sh == 32u ? 0u : sh;
    nj_e16(e, (u16)(0x1000u | ((enc & 31u) << 6) |
                     ((rm & 7u) << 3) | (rd & 7u)));
}

/* LSL (register), 16-bit low-register form: Rdn <<= (Rm & 0xff). */
static inline void nj_lsl_reg(nj_emit_t *e, unsigned rdn, unsigned rm)
{
    if (unlikely(rdn > 7u || rm > 7u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x4080u | ((rm & 7u) << 3) | (rdn & 7u)));
}

static inline void nj_uxtb(nj_emit_t *e, unsigned rd, unsigned rm)
{
    if (unlikely(rd > 7u || rm > 7u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0xB2C0u | ((rm & 7u) << 3) | (rd & 7u)));
}

static inline void nj_tst_low(nj_emit_t *e, unsigned rn, unsigned rm)
{
    if (unlikely(rn > 7u || rm > 7u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x4200u | ((rm & 7u) << 3) | (rn & 7u)));
}

static inline void nj_and_low(nj_emit_t *e, unsigned rdn, unsigned rm)
{
    if (unlikely(rdn > 7u || rm > 7u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x4000u | ((rm & 7u) << 3) | (rdn & 7u)));
}

static inline void nj_orr_low(nj_emit_t *e, unsigned rdn, unsigned rm)
{
    if (unlikely(rdn > 7u || rm > 7u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x4300u | ((rm & 7u) << 3) | (rdn & 7u)));
}

static inline void nj_adds3(nj_emit_t *e) { nj_e16(e, 0x188bu); } /* r3=r1+r2 */
static inline void nj_subs3(nj_emit_t *e) { nj_e16(e, 0x1a8bu); } /* r3=r1-r2 */
static inline void nj_and1(nj_emit_t *e)  { nj_e16(e, 0x4011u); } /* r1 &= r2 */
static inline void nj_eor1(nj_emit_t *e)  { nj_e16(e, 0x4051u); } /* r1 ^= r2 */
static inline void nj_orr1(nj_emit_t *e)  { nj_e16(e, 0x4311u); } /* r1 |= r2 */

static inline void nj_uxth(nj_emit_t *e, unsigned rd, unsigned rm)
{
    nj_e16(e, (u16)(0xB280u | ((rm & 7u) << 3) | (rd & 7u)));
}

static inline void nj_sxth(nj_emit_t *e, unsigned rd, unsigned rm)
{
    nj_e16(e, (u16)(0xB200u | ((rm & 7u) << 3) | (rd & 7u)));
}

static inline void nj_sxtb(nj_emit_t *e, unsigned rd, unsigned rm)
{
    if (unlikely(rd > 7u || rm > 7u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0xB240u | ((rm & 7u) << 3) | (rd & 7u)));
}

/*
 * Flag-free bitfield moves, for the 8-bit operand forms below.
 *
 * Both are 32-bit Thumb-2 encodings that leave APSR alone, which is the whole
 * reason to use them here: the block compiler carries x86 flags lazily in the
 * ARM condition flags and in r1-r3, so anything that touches either forces a
 * refusal.  BFI and UBFX touch neither.
 *
 * UBFX<c> Rd, Rn, #lsb, #width   11110 0 111100 Rn : 0 imm3 Rd imm2 0 width-1
 * BFI<c>  Rd, Rn, #lsb, #width   11110 0 110110 Rn : 0 imm3 Rd imm2 0 msb
 * with lsb = imm3:imm2 and msb = lsb + width - 1.  BFI with Rn = PC is BFC,
 * so Rn 15 is refused rather than silently assembled into something else.
 */
static inline void nj_ubfx(nj_emit_t *e, unsigned rd, unsigned rn,
                           unsigned lsb, unsigned width)
{
    if (unlikely(rd > 15u || rn > 15u || width == 0u ||
                 lsb + width > 32u)) { e->failed = true; return; }
    u32 w = 0xF3C00000u | ((u32)(rn & 15u) << 16);
    w |= (u32)((lsb >> 2) & 7u) << 12;
    w |= (u32)(rd & 15u) << 8;
    w |= (u32)(lsb & 3u) << 6;
    w |= (u32)((width - 1u) & 31u);
    nj_e32(e, w);
}

static inline void nj_bfi(nj_emit_t *e, unsigned rd, unsigned rn,
                          unsigned lsb, unsigned width)
{
    if (unlikely(rd > 15u || rn > 14u || width == 0u ||
                 lsb + width > 32u)) { e->failed = true; return; }
    u32 w = 0xF3600000u | ((u32)(rn & 15u) << 16);
    w |= (u32)((lsb >> 2) & 7u) << 12;
    w |= (u32)(rd & 15u) << 8;
    w |= (u32)(lsb & 3u) << 6;
    w |= (u32)((lsb + width - 1u) & 31u);
    nj_e32(e, w);
}

static inline void nj_cmp_imm0(nj_emit_t *e, unsigned rn)
{
    nj_e16(e, (u16)(0x2800u | ((rn & 7u) << 8)));
}

/* High-register CMP encoding; e.g. CMP r0,lr == 0x4570. */
static inline void nj_cmp_reg(nj_emit_t *e, unsigned rn, unsigned rm)
{
    u16 v = 0x4500u;
    v |= (u16)(rn & 7u);
    v |= (u16)(((rn >> 3) & 1u) << 7);
    v |= (u16)((rm & 15u) << 3);
    nj_e16(e, v);
}

static inline void nj_adds_imm8(nj_emit_t *e, unsigned rd, unsigned imm)
{
    if (unlikely(rd > 7u || imm > 255u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x3000u | (rd << 8) | imm));
}

static inline void nj_subs_imm8(nj_emit_t *e, unsigned rd, unsigned imm)
{
    if (unlikely(rd > 7u || imm > 255u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x3800u | (rd << 8) | imm));
}

/* Thumb-2 ADD{S}/SUB{S}.W Rd,Rd,#imm8, including high registers.
 * The immediate is deliberately kept to imm8; byte-walk loops only need 1. */
static inline void nj_addsub_imm_w(nj_emit_t *e, unsigned rd, unsigned imm,
                                   bool sub, bool setflags)
{
    if (unlikely(rd > 15u || imm > 255u)) { e->failed = true; return; }
    u16 hi = (u16)((sub ? (setflags ? 0xF1B0u : 0xF1A0u)
                         : (setflags ? 0xF110u : 0xF100u)) | (rd & 15u));
    u16 lo = (u16)(((rd & 15u) << 8) | (imm & 0xffu));
    nj_e16(e, hi);
    nj_e16(e, lo);
}

static inline u16 *nj_bcond_placeholder(nj_emit_t *e)
{
    u16 *at = e->p;
    nj_e16(e, 0xD000u);
    return at;
}

/* PUSH {lr} / POP {lr}.  A BL overwrites lr, and lr is NJ_BUDGET_REG, so
 * every call site has to bracket the call with these.  The 16-bit POP cannot
 * name lr, hence the wide form. */
static inline void nj_push_lr(nj_emit_t *e) { nj_e16(e, 0xB500u); }
static inline void nj_pop_lr(nj_emit_t *e)  { nj_e32(e, 0xE8BD4000u); }

static inline void nj_bx_lr(nj_emit_t *e)   { nj_e16(e, 0x4770u); }

/*
 * Call an absolute Thumb address, through a register.
 *
 * NOT a plain BL.  BL is PC-relative, and nj_compact_code() moves finished
 * blocks around the arena - which is safe for the branches inside a block,
 * because source and target move together, but silently breaks a branch to a
 * fixed address outside it.  That is what boot-looped the first shared-guard
 * build: blocks ran correctly until the arena filled and compacted, then every
 * call into the trampolines landed at an offset that was no longer right, and
 * the guest jumped into the middle of the interpreter.
 *
 * MOVW/MOVT materialise the address itself, so the sequence means the same
 * thing wherever the block ends up.  Six bytes more than a BL, against ~200
 * saved by not inlining the guard at all.
 */
static inline void nj_blx_abs(nj_emit_t *e, const u16 *target, unsigned rn)
{
    nj_mov_imm(e, rn, (u32)(uintptr_t)target | 1u);   /* Thumb bit */
    nj_e16(e, (u16)(0x4780u | ((rn & 0xfu) << 3)));   /* BLX rn */
}

static inline u16 *nj_b_placeholder(nj_emit_t *e)
{
    u16 *at = e->p;
    nj_e16(e, 0xE000u);
    return at;
}

static bool nj_patch_bcond(u16 *at, u16 *target, unsigned cond)
{
    intptr_t off = (u8 *)target - ((u8 *)at + 4);
    if ((off & 1) || off < -256 || off > 254 || cond > 13u) return false;
    *at = (u16)(0xD000u | (cond << 8) | (((u32)(off >> 1)) & 0xffu));
    return true;
}

static bool nj_patch_b(u16 *at, u16 *target)
{
    intptr_t off = (u8 *)target - ((u8 *)at + 4);
    if ((off & 1) || off < -2048 || off > 2046) return false;
    *at = (u16)(0xE000u | (((u32)(off >> 1)) & 0x7ffu));
    return true;
}

static inline void nj_push_low(nj_emit_t *e, unsigned mask)
{
    if (unlikely(mask & ~0xffu)) { e->failed = true; return; }
    nj_e16(e, (u16)(0xB400u | mask));
}

static inline void nj_pop_low(nj_emit_t *e, unsigned mask)
{
    if (unlikely(mask & ~0xffu)) { e->failed = true; return; }
    nj_e16(e, (u16)(0xBC00u | mask));
}

static inline void nj_blx_reg(nj_emit_t *e, unsigned rm)
{
    if (unlikely(rm > 15u)) { e->failed = true; return; }
    nj_e16(e, (u16)(0x4780u | ((rm & 15u) << 3)));
}

static inline void nj_push_guest(nj_emit_t *e)
{
    nj_e32(e, 0xE92D4FF0u); /* push.w {r4-r11,lr} */
}

/*
 * PUSH {r0} / POP {r0}.  Neither touches APSR, so they can bracket a memory
 * sequence without disturbing lazily-held guest flags.  Every path that can
 * leave the block pops before nj_pop_guest(), so SP is always balanced.
 */
static inline void nj_push_r0(nj_emit_t *e) { nj_e16(e, 0xB401u); }
static inline void nj_pop_r0(nj_emit_t *e)  { nj_e16(e, 0xBC01u); }

static inline void nj_pop_guest(nj_emit_t *e)
{
    nj_e32(e, 0xE8BD8FF0u); /* pop.w {r4-r11,pc} */
}

/*
 * 8-bit operand forms in the native emitter.  Build with -DNJIT_OP8=OFF to
 * get exactly the previous behaviour back without touching this file.
 */
#ifndef NJIT_OP8
#define NJIT_OP8 1
#endif

/* -------------------------- Persistent register map ---------------------- */

#define NJ_CPU_REG       12u
#define NJ_BUDGET_REG    14u
#define NJ_ITER_REG      0u
#define NJ_GUEST_REG(r)  (4u + (unsigned)(r))
/*
 * x86 byte registers, 0..7, are AL CL DL BL AH CH DH BH - the low and the
 * high byte of EAX..EBX.  A byte operand therefore never names a guest
 * register above EBX, so it never leaves ARM r4..r7 and every byte form below
 * stays inside the persistent register map with no spill.
 */
#define NJ_B8_GUEST(b)   ((unsigned)(b) & 3u)
#define NJ_B8_SHIFT(b)   ((unsigned)(b) >= 4u ? 8u : 0u)

#define NJ_GPR_OFF(r)    ((unsigned)offsetof(CPUI386, gprx) + ((unsigned)(r) * 4u))
#define NJ_IP_OFF        ((unsigned)offsetof(CPUI386, ip))
#define NJ_NEXT_IP_OFF   ((unsigned)offsetof(CPUI386, next_ip))
#define NJ_PREFETCH_OFF  ((unsigned)offsetof(CPUI386, prefetch_base))
#define NJ_FLAGS_OFF     ((unsigned)offsetof(CPUI386, flags))
#define NJ_CC_OP         ((unsigned)offsetof(CPUI386, cc.op))
#define NJ_CC_DST        ((unsigned)offsetof(CPUI386, cc.dst))
#define NJ_CC_DST2       ((unsigned)offsetof(CPUI386, cc.dst2))
#define NJ_CC_SRC1       ((unsigned)offsetof(CPUI386, cc.src1))
#define NJ_CC_SRC2       ((unsigned)offsetof(CPUI386, cc.src2))
#define NJ_CC_MASK       ((unsigned)offsetof(CPUI386, cc.mask))

static inline void nj_emit_prologue(nj_emit_t *e, bool code16)
{
    nj_push_guest(e);
    nj_mov_reg(e, NJ_CPU_REG, 0);       /* r12 = CPUI386* */
    nj_mov_reg(e, NJ_BUDGET_REG, 1);    /* lr = max native iterations */
    nj_mov_imm(e, NJ_ITER_REG, 0);      /* r0 = iterations executed */
    for (unsigned r = 0; r < 8; ++r) {
        if (code16) nj_ldrh(e, NJ_GUEST_REG(r), NJ_CPU_REG, NJ_GPR_OFF(r));
        else        nj_ldr32(e, NJ_GUEST_REG(r), NJ_CPU_REG, NJ_GPR_OFF(r));
    }
}

static inline void nj_emit_store_guest(nj_emit_t *e, bool code16)
{
    for (unsigned r = 0; r < 8; ++r) {
        if (code16) nj_strh(e, NJ_GUEST_REG(r), NJ_CPU_REG, NJ_GPR_OFF(r));
        else        nj_str32(e, NJ_GUEST_REG(r), NJ_CPU_REG, NJ_GPR_OFF(r));
    }
}

static inline void nj_emit_cc_flush(nj_emit_t *e, bool code16,
                                    unsigned flag_kind, unsigned ccop)
{
    if (flag_kind == NJ_FLAG_NONE) return;

    if (flag_kind == NJ_FLAG_ARITH) {
        if (code16) {
            nj_sxth(e, 1, 1);
            nj_sxth(e, 2, 2);
            nj_sxth(e, 3, 3);
        }
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_SRC1);
        nj_str32(e, 2, NJ_CPU_REG, NJ_CC_SRC2);
        nj_str32(e, 3, NJ_CPU_REG, NJ_CC_DST);
        nj_mov_imm(e, 1, ccop);
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_OP);
        nj_mov_imm(e, 1, CF | PF | AF | ZF | SF | OF);
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_MASK);
    } else if (flag_kind == NJ_FLAG_LOGIC) {
        if (code16) nj_sxth(e, 3, 3);
        nj_str32(e, 3, NJ_CPU_REG, NJ_CC_DST);
        nj_mov_imm(e, 1, ccop);
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_OP);
        nj_mov_imm(e, 1, CF | PF | ZF | SF | OF);
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_MASK);
    } else { /* INC/DEC: CF remains materialised in cpu->flags. */
        if (code16) nj_sxth(e, 3, 3);
        nj_str32(e, 3, NJ_CPU_REG, NJ_CC_DST);
        nj_mov_imm(e, 1, ccop);
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_OP);
        nj_mov_imm(e, 1, PF | AF | ZF | SF | OF);
        nj_str32(e, 1, NJ_CPU_REG, NJ_CC_MASK);
    }
}

static inline void nj_v8_emit_cc_flush_width(nj_emit_t *e, unsigned size,
                                             unsigned flag_kind, unsigned ccop)
{
    if (flag_kind == NJ_FLAG_NONE) return;
    if (size != 1u && size != 2u && size != 4u) { e->failed = true; return; }

    if (flag_kind == NJ_FLAG_ARITH) {
        if (size == 1u) { nj_sxtb(e,1,1); nj_sxtb(e,2,2); nj_sxtb(e,3,3); }
        else if (size == 2u) { nj_sxth(e,1,1); nj_sxth(e,2,2); nj_sxth(e,3,3); }
        nj_str32(e,1,NJ_CPU_REG,NJ_CC_SRC1);
        nj_str32(e,2,NJ_CPU_REG,NJ_CC_SRC2);
        nj_str32(e,3,NJ_CPU_REG,NJ_CC_DST);
        nj_mov_imm(e,1,ccop); nj_str32(e,1,NJ_CPU_REG,NJ_CC_OP);
        nj_mov_imm(e,1,CF|PF|AF|ZF|SF|OF); nj_str32(e,1,NJ_CPU_REG,NJ_CC_MASK);
    } else if (flag_kind == NJ_FLAG_LOGIC) {
        if (size == 1u) nj_sxtb(e,3,3);
        else if (size == 2u) nj_sxth(e,3,3);
        nj_str32(e,3,NJ_CPU_REG,NJ_CC_DST);
        nj_mov_imm(e,1,ccop); nj_str32(e,1,NJ_CPU_REG,NJ_CC_OP);
        nj_mov_imm(e,1,CF|PF|ZF|SF|OF); nj_str32(e,1,NJ_CPU_REG,NJ_CC_MASK);
    } else { /* INC/DEC: preserve materialised CF, lazy-evaluate the rest. */
        if (size == 1u) nj_sxtb(e,3,3);
        else if (size == 2u) nj_sxth(e,3,3);
        nj_str32(e,3,NJ_CPU_REG,NJ_CC_DST);
        nj_mov_imm(e,1,ccop); nj_str32(e,1,NJ_CPU_REG,NJ_CC_OP);
        nj_mov_imm(e,1,PF|AF|ZF|SF|OF); nj_str32(e,1,NJ_CPU_REG,NJ_CC_MASK);
    }
}

static inline void nj_emit_common_exit(nj_emit_t *e, bool code16,
                                       unsigned flag_kind, unsigned ccop,
                                       uword branch_ip)
{
    /* LR already contains the desired guest next_ip on every exit path. */
    nj_emit_cc_flush(e, code16, flag_kind, ccop);
    nj_str32(e, NJ_BUDGET_REG, NJ_CPU_REG, NJ_NEXT_IP_OFF);

    nj_mov_imm(e, 1, branch_ip);
    nj_str32(e, 1, NJ_CPU_REG, NJ_IP_OFF);
    nj_mov_imm(e, 1, 0xffffffffu);
    nj_str32(e, 1, NJ_CPU_REG, NJ_PREFETCH_OFF);

    nj_emit_store_guest(e, code16);
    nj_pop_guest(e);
}

static inline u16 nj_rd16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 nj_rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Result of translating one non-control-flow body instruction. */
typedef struct {
    unsigned flag_kind;
    unsigned ccop;
    bool clobbers_flag_scratch;
    bool uses_es_static;
    /*
     * A memory operand embeds the segment base of its effective address as an
     * immediate, so the block is only valid while that base and selector are
     * unchanged.  These ride out to nj_block_matches() exactly like the trace
     * compiler's equivalents.
     */
    bool uses_ds_static;
    bool uses_ss_base;
    bool used_memory;
} nj_body_info_t;

/*
 * Memory operands inside a fully-native loop body.
 *
 * The loop compiler had no side-exit path at all: any instruction it could not
 * emit rejected the entire block.  nj_translate_body_one() was therefore
 * register-only, and Doom's renderer - which is nothing but memory traffic -
 * never produced a single native loop.  Every one of its inner loops fell
 * through to the v6 trace compiler and was cut into ~3-instruction
 * straight-line pieces.
 *
 * The address/guard sequence itself is the one the trace compiler already
 * uses.  Two things had to be built around it:
 *
 *  - r0 carries the loop's live iteration count, and both the paging
 *    translation and the store's code-page check use r0 as scratch.  It is
 *    saved on the host stack across the sequence and restored on the success
 *    path and on the guard-failure path alike, so the count the block returns
 *    stays exact.
 *  - a guard failure must leave a consistent guest.  It exits with next_ip at
 *    the faulting instruction having executed none of it, and returns the
 *    number of *complete* iterations.  The interpreter then performs that one
 *    access with the full page-fault and split-access semantics it already
 *    has, and the next native entry finds a warm TLB.
 *
 * mc == NULL means "register operands only", which is what the v6 trace
 * compiler passes: it has its own memory paths and its own exit-link
 * machinery, and must not get this one's inline epilogue.
 */
typedef struct {
    bool state16;      /* guest register width of the block's epilogue */
    uword instr_ip;    /* guest IP of the instruction being translated */
} nj_loop_mem_t;

static bool nj_v6_decode_ea(const u8 *p, unsigned max, u8 modrm,
                            bool addr16, int seg_override, nj_v6_ea_t *ea);
static void nj_v6_emit_ea(CPUI386 *cpu, nj_emit_t *e, const nj_v6_ea_t *ea,
                          bool add_seg);
static bool nj_v6_note_seg(CPUI386 *cpu, int seg,
                           bool *use_ds, bool *use_es, bool *use_ss);
static void nj_v6_read_r8(nj_emit_t *e, unsigned reg);
static void nj_v6_write_r8(nj_emit_t *e, unsigned reg);
static void nj_v6_write_r16(nj_emit_t *e, unsigned reg);
static int nj_body_mem_open(CPUI386 *cpu, nj_emit_t *e, nj_body_info_t *bi,
                            const nj_loop_mem_t *mc,
                            const u8 *p, unsigned max, u8 modrm,
                            bool addr16, int seg_override,
                            unsigned size, bool write,
                            nj_v6_guard_t *g, unsigned *used);
static bool nj_loop_mem_finish(nj_emit_t *e, nj_v6_guard_t *g,
                               const nj_loop_mem_t *mc);

/*
 * A WIDE66 candidate is a 16-bit code block whose first instruction selects
 * 32-bit operands. Segment prefixes and 0x67 may legally precede 0x66.
 */
static bool nj_wide66_candidate(CPUI386 *cpu, const u8 *p, unsigned max)
{
    if (!cpu->code16 || !max) return false;

    unsigned pos = 0;
    while (pos < max && pos < 8u) {
        switch (p[pos]) {
        case 0x26: case 0x2e: case 0x36: case 0x3e:
        case 0x64: case 0x65:
        case 0x67:
            pos++;
            continue;
        case 0x66:
            return true;
        default:
            return false;
        }
    }
    return false;
}


/* For diagnostics: report the opcode after ordinary prefixes, not the prefix
 * byte itself. */
static unsigned nj_effective_opcode(const u8 *p, unsigned max)
{
    unsigned pos = 0;
    while (pos < max && pos < 8u) {
        switch (p[pos]) {
        case 0x26: case 0x2e: case 0x36: case 0x3e:
        case 0x64: case 0x65:
        case 0x66: case 0x67:
            pos++;
            continue;
        default:
            return p[pos];
        }
    }
    return 0xffffffffu;
}

static int nj_translate_body_one(CPUI386 *cpu, nj_emit_t *e,
                                 const u8 *p, unsigned max,
                                 bool force32,
                                 nj_body_info_t *bi,
                                 const nj_loop_mem_t *mc)
{
    if (!max) return 0;
    unsigned pos = 0;
    memset(bi, 0, sizeof(*bi));

    bool have66 = false;
    bool have67 = false;
    int seg_override = -1;

    for (;;) {
        if (pos >= max || pos >= 8u) return 0;
        switch (p[pos]) {
        case 0x26: seg_override = SEG_ES; pos++; continue;
        case 0x2e: seg_override = SEG_CS; pos++; continue;
        case 0x36: seg_override = SEG_SS; pos++; continue;
        case 0x3e: seg_override = SEG_DS; pos++; continue;
        case 0x64: seg_override = SEG_FS; pos++; continue;
        case 0x65: seg_override = SEG_GS; pos++; continue;
        case 0x67:
            /* Irrelevant to a register-only form, but it selects the address
             * size of every memory operand below. */
            have67 = true;
            pos++;
            continue;
        case 0x66:
            if (!force32) return 0;
            have66 = true;
            pos++;
            continue;
        case 0xf0: case 0xf2: case 0xf3:
            return 0;
        default:
            break;
        }
        break;
    }

    if (pos >= max) return 0;
    u8 op = p[pos++];

    /*
     * In a WIDE66 block all width-sensitive body instructions must carry
     * 0x66.  NOP is width-neutral and may appear without it.
     */
    if (force32 && !have66 && op != 0x90)
        return 0;

    unsigned w = force32 ? 4u : (cpu->code16 ? 2u : 4u);
    bool addr16 = (bool)cpu->code16 != have67;

#define NJ_NEED(n) do { if (pos + (unsigned)(n) > max) return 0; } while (0)
#define NJ_FINISH_RESULT() do { \
    if (w == 2u) nj_uxth(e, 3, 3); \
    nj_cmp_imm0(e, 3); \
} while (0)

    if (op == 0x90) {
        /* NOP */
    }
    else if (op >= 0x91 && op <= 0x97) {
        unsigned r = op & 7u;
        nj_mov_reg(e, 1, NJ_GUEST_REG(0));
        nj_mov_reg(e, NJ_GUEST_REG(0), NJ_GUEST_REG(r));
        nj_mov_reg(e, NJ_GUEST_REG(r), 1);
        bi->clobbers_flag_scratch = true;
    }
    else if (op >= 0xb8 && op <= 0xbf) {
        unsigned r = op & 7u;
        NJ_NEED(w);
        u32 imm = w == 2u ? nj_rd16(p + pos) : nj_rd32(p + pos);
        pos += w;
        nj_mov_imm(e, NJ_GUEST_REG(r), imm);
    }
    else if (op >= 0x40 && op <= 0x4f) {
        unsigned r = op & 7u;
        bool dec = op >= 0x48;
        nj_mov_reg(e, 1, NJ_GUEST_REG(r));
        nj_mov_imm(e, 2, 1);
        if (dec) nj_subs3(e); else nj_adds3(e);
        if (w == 2u) nj_uxth(e, 3, 3);
        nj_mov_reg(e, NJ_GUEST_REG(r), 3);
        nj_cmp_imm0(e, 3);
        bi->flag_kind = NJ_FLAG_INCDEC;
        bi->ccop = dec ? (w == 2u ? CC_DEC16 : CC_DEC32)
                       : (w == 2u ? CC_INC16 : CC_INC32);
    }
    else if (op == 0x0f) {
        /*
         * Two-byte opcodes. 0x0f was the largest single entry in the v8.10-diag
         * reject histogram - 1024 of 2719 refusals, 37.7% - but 0x0f is an
         * escape, not an instruction, so that number said nothing on its own.
         * Splitting it by second byte showed it is almost entirely one
         * instruction: 0F B7 saturated the counter while everything else under
         * 0x0f was in single digits.
         *
         * MOVZX is the ideal thing to find there. It sets no flags, so it
         * cannot silently corrupt a later conditional the way an arithmetic
         * op would, and zero-extending 16 bits into 32 is exactly UXTH, which
         * the emitter already has.
         */
        NJ_NEED(1);
        u8 op2 = p[pos++];

        if (op2 == 0xb7u) {             /* MOVZX r, r/m16 */
            NJ_NEED(1);
            u8 m = p[pos++];
            unsigned reg = (m >> 3) & 7u;
            unsigned rm = m & 7u;

            /* Register source only. A memory source needs the load path and
             * its guards; it stays refused and stays visible in the counters. */
            if ((m & 0xc0u) != 0xc0u) return 0;

            nj_uxth(e, NJ_GUEST_REG(reg), NJ_GUEST_REG(rm));
        } else {
            return 0;
        }
    }
    else if (op == 0x8c) {
        /*
         * MOV r/m16, Sreg — store a segment selector.
         *
         * v8.10-diag's reject table made this the single most valuable opcode
         * to add: 1593 of 2868 recorded refusals (55.5%) were this one byte,
         * more than the next three put together. Every one of those refusals
         * blacklisted a chain head, and the chain counters showed the result -
         * 96% of nj_exec_chain() calls stopped at the negative cache and
         * chains averaged 1.00 blocks, i.e. never extended at all.
         *
         * The selector lives at a fixed address inside CPUI386, so it costs a
         * materialised pointer and one halfword load - no helper call, no
         * memory guard, nothing that can fault. `sel` is a uword but only the
         * low 16 bits are the selector, and LDRH on little-endian takes
         * exactly those.
         *
         * Register destinations only. A memory destination would need the
         * store path and its guards, which is a separate change; it falls
         * through to the existing refusal and stays visible in the counters.
         */
        NJ_NEED(1);
        u8 m = p[pos++];
        unsigned sreg = (m >> 3) & 7u;
        unsigned rm = m & 7u;

        if ((m & 0xc0u) != 0xc0u) return 0;     /* memory destination */
        if (sreg > SEG_GS) return 0;            /* ES,CS,SS,DS,FS,GS only */

        nj_mov_imm(e, 1, (u32)(uintptr_t)&cpu->seg[sreg].sel);
        nj_ldrh(e, NJ_GUEST_REG(rm), 1, 0);
        bi->clobbers_flag_scratch = true;
    }
    else if (op == 0x89 || op == 0x8b || op == 0x87) {
        NJ_NEED(1);
        u8 m = p[pos++];
        unsigned reg = (m >> 3) & 7u;
        unsigned rm = m & 7u;

        /*
         * v4.2 measured hot path: MOV r16, ES:[disp16].
         *
         * Keep this intentionally narrow.  In real mode with paging off,
         * translate() reduces to ES.base + disp16, and the benchmark target
         * is ordinary low RAM.  Embedding the host pointer avoids a C helper
         * call in every spin-loop iteration.
         */
        if (op == 0x8b && w == 2u && !force32 &&
            seg_override == SEG_ES && (m & 0xc7u) == 0x06u) {
            NJ_NEED(2);
            uword disp = nj_rd16(p + pos);
            pos += 2;

            /* Real mode only for this first memory fast-path. */
            if (cpu->cr0 & 1u) return 0;

            uword paddr = cpu->seg[SEG_ES].base + disp;
            if (paddr + 1u < paddr ||
                paddr + 1u >= (uword)cpu->phys_mem_size)
                return 0;
            if (in_iomem(paddr) || in_iomem(paddr + 1u))
                return 0;
#if REMOTE_MEM
            if (is_remote(paddr) || is_remote(paddr + 1u))
                return 0;
#endif

            u32 host = (u32)(uintptr_t)(cpu->phys_mem + paddr);
            nj_mov_imm(e, 1, host);
            nj_ldrh(e, NJ_GUEST_REG(reg), 1, 0);
            bi->clobbers_flag_scratch = true;
            bi->uses_es_static = true;
        }
        else if ((m >> 6) != 3) {
            /*
             * The word/dword load and store.  0x8b was the second largest
             * give-up in the renderer's stop histogram; with the guarded
             * sequence it is now the ordinary case rather than the reason a
             * whole loop is refused.  XCHG with memory is left out: it is
             * architecturally LOCKed and belongs on the interpreter path.
             */
            if (op == 0x87) return 0;

            nj_v6_guard_t g;
            unsigned used;
            bool write = (op == 0x89);
            if (!nj_body_mem_open(cpu, e, bi, mc, p + pos, max - pos, m,
                                  addr16, seg_override, w, write, &g, &used))
                return 0;

            if (op == 0x8b) {
                if (w == 2u) nj_ldrh(e, NJ_GUEST_REG(reg), 3, 0);
                else         nj_ldr32(e, NJ_GUEST_REG(reg), 3, 0);
            } else {
                if (w == 2u) nj_strh(e, NJ_GUEST_REG(reg), 3, 0);
                else         nj_str32(e, NJ_GUEST_REG(reg), 3, 0);
            }

            if (!nj_loop_mem_finish(e, &g, mc)) return 0;
            pos += used;
        }
        else {
            if (op == 0x87) {
                nj_mov_reg(e, 1, NJ_GUEST_REG(rm));
                nj_mov_reg(e, NJ_GUEST_REG(rm), NJ_GUEST_REG(reg));
                nj_mov_reg(e, NJ_GUEST_REG(reg), 1);
                bi->clobbers_flag_scratch = true;
            } else {
                unsigned sr = op == 0x89 ? reg : rm;
                unsigned dr = op == 0x89 ? rm : reg;
                nj_mov_reg(e, NJ_GUEST_REG(dr), NJ_GUEST_REG(sr));
            }
        }
    }
    else if (op == 0x8d) {
        /*
         * LEA.  The one memory-form instruction that performs no access at
         * all, so it needs neither a guard nor a side exit - just the address
         * arithmetic the guarded path already had to be able to emit.  It is
         * how the renderer computes every span pointer it then loads through.
         */
        NJ_NEED(1);
        u8 m = p[pos++];
        if ((m >> 6) == 3u) return 0;       /* LEA with a register source is #UD */
        if (!mc) return 0;

        nj_v6_ea_t ea;
        if (!nj_v6_decode_ea(p + pos, max - pos, m, addr16, seg_override, &ea))
            return 0;

        nj_v6_emit_ea(cpu, e, &ea, false);  /* offset only, no segment base */
        if (w == 2u) nj_uxth(e, 3, 3);
        nj_mov_reg(e, NJ_GUEST_REG((m >> 3) & 7u), 3);
        pos += ea.used;
        bi->clobbers_flag_scratch = true;
    }
    else if (NJIT_OP8 && (op == 0x88 || op == 0x8a)) {
        /*
         * MOV r/m8, r8 and MOV r8, r/m8 - register forms.
         *
         * Measured on this board while Tyrian 2000 was running, 0x8a was the
         * single largest entry in the reject histogram at 22.0% and 0x88
         * added 2.6%; with 0xc6 below them the byte moves were about a third
         * of every refusal, and the game was executing 0.1% of its
         * instructions natively as a result.
         *
         * They are admitted for the same reason MOVZX was: they set no flags,
         * so letting them into a block cannot silently corrupt a conditional
         * that comes later.  A low-byte source needs no extract at all, since
         * BFI takes its source from the bottom of Rn; only a high-byte source
         * (AH..BH) needs the scratch register, and that is declared so the
         * block compiler refuses it while lazy flags are still live.
         *
         * Memory forms stay refused - they need the load path and its guards,
         * and they stay visible in the counters.
         */
        NJ_NEED(1);
        u8 m = p[pos++];
        unsigned rb = (m >> 3) & 7u;
        unsigned mb = m & 7u;

        if ((m >> 6) != 3u) {
            /* The byte load and store the renderer's column loops are made
             * of - MOV AL,[ESI] / MOV [EDI],AL. */
            nj_v6_guard_t g;
            unsigned used;
            bool write = (op == 0x88);
            if (!nj_body_mem_open(cpu, e, bi, mc, p + pos, max - pos, m,
                                  addr16, seg_override, 1u, write, &g, &used))
                return 0;

            if (op == 0x8a) {
                nj_ldrb(e, 3, 3, 0);
                nj_v6_write_r8(e, rb);
            } else {
                /* r0 is already saved on the stack, so it is free to hold the
                 * host pointer while AH-style sources are extracted into r3. */
                nj_mov_reg(e, 0, 3);
                nj_v6_read_r8(e, rb);
                nj_strb(e, 3, 0, 0);
            }

            if (!nj_loop_mem_finish(e, &g, mc)) return 0;
            pos += used;
            goto body_done;
        }

        unsigned sb = (op == 0x88) ? rb : mb;
        unsigned db = (op == 0x88) ? mb : rb;
        unsigned sg = NJ_GUEST_REG(NJ_B8_GUEST(sb));
        unsigned dg = NJ_GUEST_REG(NJ_B8_GUEST(db));

        if (NJ_B8_SHIFT(sb) == 0u) {
            nj_bfi(e, dg, sg, NJ_B8_SHIFT(db), 8u);
        } else {
            nj_ubfx(e, 1, sg, 8u, 8u);
            nj_bfi(e, dg, 1, NJ_B8_SHIFT(db), 8u);
            bi->clobbers_flag_scratch = true;
        }
    }
    else if (NJIT_OP8 && op >= 0xb0 && op <= 0xb7) {
        /* MOV r8, imm8. No flags, but the immediate has to reach a register
         * before BFI can insert it, so this always costs the scratch. */
        NJ_NEED(1);
        u8 imm = p[pos++];
        unsigned db = op & 7u;
        nj_mov_imm(e, 1, imm);
        nj_bfi(e, NJ_GUEST_REG(NJ_B8_GUEST(db)), 1, NJ_B8_SHIFT(db), 8u);
        bi->clobbers_flag_scratch = true;
    }
    else if (NJIT_OP8 && op == 0xc6) {
        /* MOV r/m8, imm8, register form - 11.0% of the refusals measured. */
        NJ_NEED(1);
        u8 m = p[pos++];
        if ((m >> 6) != 3u) return 0;
        if (((m >> 3) & 7u) != 0u) return 0;   /* only /0 is MOV */
        NJ_NEED(1);
        u8 imm = p[pos++];
        unsigned db = m & 7u;
        nj_mov_imm(e, 1, imm);
        nj_bfi(e, NJ_GUEST_REG(NJ_B8_GUEST(db)), 1, NJ_B8_SHIFT(db), 8u);
        bi->clobbers_flag_scratch = true;
    }
    else if (op == 0x01 || op == 0x03 || op == 0x09 || op == 0x0b ||
             op == 0x21 || op == 0x23 || op == 0x29 || op == 0x2b ||
             op == 0x31 || op == 0x33 || op == 0x39 || op == 0x3b ||
             op == 0x85) {
        NJ_NEED(1);
        u8 m = p[pos++];
        unsigned reg = (m >> 3) & 7u;
        unsigned rm = m & 7u;
        bool dir = (op & 2u) != 0;
        bool mem = (m >> 6) != 3u;
        /* CMP and TEST compute flags and discard the result. */
        bool stores = (op != 0x39 && op != 0x3b && op != 0x85);
        /* Without the direction bit the r/m operand is the destination, so a
         * memory r/m makes this a read-modify-write. */
        bool mem_dst = mem && !dir;
        unsigned dst = dir ? reg : rm;
        unsigned sr = dir ? rm : reg;
        if (op == 0x85) { dst = rm; sr = reg; }

        nj_v6_guard_t g;
        unsigned used = 0;

        if (mem) {
            if (!nj_body_mem_open(cpu, e, bi, mc, p + pos, max - pos, m,
                                  addr16, seg_override, w,
                                  mem_dst && stores, &g, &used))
                return 0;
            /* Park the host pointer in the stacked r0 so the ALU keeps r1-r3
             * for its operands and its lazily-evaluated result. */
            nj_mov_reg(e, 0, 3);
            if (mem_dst) {
                if (w == 2u) nj_ldrh(e, 1, 0, 0); else nj_ldr32(e, 1, 0, 0);
                nj_mov_reg(e, 2, NJ_GUEST_REG(reg));
            } else {
                nj_mov_reg(e, 1, NJ_GUEST_REG(reg));
                if (w == 2u) nj_ldrh(e, 2, 0, 0); else nj_ldr32(e, 2, 0, 0);
            }
        } else {
            nj_mov_reg(e, 1, NJ_GUEST_REG(dst));
            nj_mov_reg(e, 2, NJ_GUEST_REG(sr));
        }

/* Write the result back to whichever operand the direction bit selected. */
#define NJ_ALU_STORE() do {     if (mem_dst) {         if (w == 2u) nj_strh(e, 3, 0, 0); else nj_str32(e, 3, 0, 0);     } else {         nj_mov_reg(e, NJ_GUEST_REG(dst), 3);     } } while (0)

        if (op == 0x01 || op == 0x03) {
            nj_adds3(e);
            if (w == 2u) nj_uxth(e, 3, 3);
            NJ_ALU_STORE();
            nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_ADD;
        } else if (op == 0x29 || op == 0x2b || op == 0x39 || op == 0x3b) {
            nj_subs3(e);
            if (w == 2u) nj_uxth(e, 3, 3);
            if (op == 0x29 || op == 0x2b) NJ_ALU_STORE();
            nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_SUB;
        } else if (op == 0x09 || op == 0x0b) {
            nj_orr1(e); nj_mov_reg(e, 3, 1);
            if (w == 2u) nj_uxth(e, 3, 3);
            NJ_ALU_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_OR;
        } else if (op == 0x21 || op == 0x23 || op == 0x85) {
            nj_and1(e); nj_mov_reg(e, 3, 1);
            if (w == 2u) nj_uxth(e, 3, 3);
            if (op != 0x85) NJ_ALU_STORE();
            nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_AND;
        } else {
            nj_eor1(e); nj_mov_reg(e, 3, 1);
            if (w == 2u) nj_uxth(e, 3, 3);
            NJ_ALU_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_XOR;
        }

#undef NJ_ALU_STORE

        if (mem) {
            if (!nj_loop_mem_finish(e, &g, mc)) return 0;
            pos += used;
        }
    }
    else if (op == 0x05 || op == 0x0d || op == 0x25 ||
             op == 0x2d || op == 0x35 || op == 0x3d) {
        NJ_NEED(w);
        u32 imm = w == 2u ? nj_rd16(p + pos) : nj_rd32(p + pos);
        pos += w;
        nj_mov_reg(e, 1, NJ_GUEST_REG(0));
        nj_mov_imm(e, 2, imm);

        if (op == 0x05) {
            nj_adds3(e); if (w == 2u) nj_uxth(e, 3, 3);
            nj_mov_reg(e, NJ_GUEST_REG(0), 3); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_ADD;
        } else if (op == 0x2d || op == 0x3d) {
            nj_subs3(e); if (w == 2u) nj_uxth(e, 3, 3);
            if (op == 0x2d) nj_mov_reg(e, NJ_GUEST_REG(0), 3);
            nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_SUB;
        } else if (op == 0x0d) {
            nj_orr1(e); nj_mov_reg(e, 3, 1); if (w == 2u) nj_uxth(e, 3, 3);
            nj_mov_reg(e, NJ_GUEST_REG(0), 3); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_OR;
        } else if (op == 0x25) {
            nj_and1(e); nj_mov_reg(e, 3, 1); if (w == 2u) nj_uxth(e, 3, 3);
            nj_mov_reg(e, NJ_GUEST_REG(0), 3); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_AND;
        } else {
            nj_eor1(e); nj_mov_reg(e, 3, 1); if (w == 2u) nj_uxth(e, 3, 3);
            nj_mov_reg(e, NJ_GUEST_REG(0), 3); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_XOR;
        }
    }
    else if (op == 0x81 || op == 0x83) {
        NJ_NEED(1);
        u8 m = p[pos++];
        unsigned sub = (m >> 3) & 7u;
        unsigned dst = m & 7u;
        bool mem = (m >> 6) != 3u;
        if (sub == 2 || sub == 3) return 0; /* ADC/SBB consume old CF */

        /*
         * The immediate follows the displacement, so the effective address has
         * to be sized before the immediate can be read.  nj_v6_decode_ea() is
         * a pure decode and emits nothing, which makes this peek free.
         */
        unsigned ea_used = 0;
        if (mem) {
            nj_v6_ea_t pea;
            if (!mc) return 0;
            if (!nj_v6_decode_ea(p + pos, max - pos, m, addr16,
                                 seg_override, &pea))
                return 0;
            ea_used = pea.used;
        }

        unsigned ipos = pos + ea_used;
        u32 imm;
        if (op == 0x83) {
            if (ipos + 1u > max) return 0;
            s32 si = (s8)p[ipos];
            imm = w == 2u ? (u16)si : (u32)si;
            ipos += 1u;
        } else {
            if (ipos + w > max) return 0;
            imm = w == 2u ? nj_rd16(p + ipos) : nj_rd32(p + ipos);
            ipos += w;
        }

        nj_v6_guard_t g;
        unsigned used = 0;
        if (mem) {
            /* /7 is CMP: it reads the destination and writes only flags. */
            if (!nj_body_mem_open(cpu, e, bi, mc, p + pos, max - pos, m,
                                  addr16, seg_override, w, sub != 7u,
                                  &g, &used))
                return 0;
            nj_mov_reg(e, 0, 3);
            if (w == 2u) nj_ldrh(e, 1, 0, 0); else nj_ldr32(e, 1, 0, 0);
        } else {
            nj_mov_reg(e, 1, NJ_GUEST_REG(dst));
        }
        nj_mov_imm(e, 2, imm);

#define NJ_G1_STORE() do {     if (mem) {         if (w == 2u) nj_strh(e, 3, 0, 0); else nj_str32(e, 3, 0, 0);     } else {         nj_mov_reg(e, NJ_GUEST_REG(dst), 3);     } } while (0)

        switch (sub) {
        case 0:
            nj_adds3(e); if (w == 2u) nj_uxth(e, 3, 3);
            NJ_G1_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_ADD; break;
        case 1:
            nj_orr1(e); nj_mov_reg(e, 3, 1); if (w == 2u) nj_uxth(e, 3, 3);
            NJ_G1_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_OR; break;
        case 4:
            nj_and1(e); nj_mov_reg(e, 3, 1); if (w == 2u) nj_uxth(e, 3, 3);
            NJ_G1_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_AND; break;
        case 5:
            nj_subs3(e); if (w == 2u) nj_uxth(e, 3, 3);
            NJ_G1_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_SUB; break;
        case 6:
            nj_eor1(e); nj_mov_reg(e, 3, 1); if (w == 2u) nj_uxth(e, 3, 3);
            NJ_G1_STORE(); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_LOGIC; bi->ccop = CC_XOR; break;
        case 7:
            nj_subs3(e); if (w == 2u) nj_uxth(e, 3, 3); nj_cmp_imm0(e, 3);
            bi->flag_kind = NJ_FLAG_ARITH; bi->ccop = CC_SUB; break;
        default:
            return 0;
        }

#undef NJ_G1_STORE

        if (mem) {
            if (!nj_loop_mem_finish(e, &g, mc)) return 0;
        }
        pos = ipos;
    }
    else if (op == 0xc7) {
        NJ_NEED(1);
        u8 m = p[pos++];
        if ((m >> 6) != 3 || ((m >> 3) & 7u) != 0) return 0;
        NJ_NEED(w);
        u32 imm = w == 2u ? nj_rd16(p + pos) : nj_rd32(p + pos);
        pos += w;
        nj_mov_imm(e, NJ_GUEST_REG(m & 7u), imm);
    }
    else {
        return 0;
    }

body_done:
    if (e->failed || pos > 15u) return 0;
    return (int)pos;

#undef NJ_FINISH_RESULT
#undef NJ_NEED
}

typedef struct {
    unsigned type;
    unsigned len;
    uword target;
    uword fallthrough;
} nj_branch_t;

static bool nj_decode_backedge(CPUI386 *cpu, const u8 *p, unsigned max,
                               uword ip, uword loop_start, nj_branch_t *br)
{
    if (!max) return false;
    memset(br, 0, sizeof(*br));

    /* Keep v4 branch semantics intentionally narrow and auditable. */
    u8 op = p[0];
    sword disp;
    unsigned len;

    if (op == 0x74 || op == 0x75) { /* JZ/JNZ rel8 */
        if (max < 2u) return false;
        disp = (s8)p[1]; len = 2u;
        br->type = op == 0x74 ? NJ_BRANCH_JZ : NJ_BRANCH_JNZ;
    } else if (op == 0x0f && max >= 2u && (p[1] == 0x84 || p[1] == 0x85)) {
        unsigned w = cpu->code16 ? 2u : 4u; /* near JZ/JNZ */
        if (max < 2u + w) return false;
        disp = w == 2u ? (s16)nj_rd16(p + 2) : (s32)nj_rd32(p + 2);
        len = 2u + w;
        br->type = p[1] == 0x84 ? NJ_BRANCH_JZ : NJ_BRANCH_JNZ;
    } else if (op == 0xeb) {       /* JMP rel8 */
        if (max < 2u) return false;
        disp = (s8)p[1]; len = 2u; br->type = NJ_BRANCH_JMP;
    } else if (op == 0xe9) {       /* JMP rel16/rel32 */
        unsigned w = cpu->code16 ? 2u : 4u;
        if (max < 1u + w) return false;
        disp = w == 2u ? (s16)nj_rd16(p + 1) : (s32)nj_rd32(p + 1);
        len = 1u + w; br->type = NJ_BRANCH_JMP;
    } else if (op == 0xe2) {       /* LOOP rel8 */
        if (max < 2u) return false;
        disp = (s8)p[1]; len = 2u; br->type = NJ_BRANCH_LOOP;
    } else if (op == 0xe3) {       /* JCXZ/JECXZ rel8 */
        if (max < 2u) return false;
        disp = (s8)p[1]; len = 2u; br->type = NJ_BRANCH_JCXZ;
    } else {
        return false;
    }

    uword fall = ip + len;
    uword target = fall + disp;
    if (cpu->code16) { fall &= 0xffffu; target &= 0xffffu; }
    if (target != loop_start) return false;

    br->len = len;
    br->target = target;
    br->fallthrough = fall;
    return true;
}

static int nj_code_window(CPUI386 *cpu, uword ip,
                          const u8 **out, unsigned *avail,
                          unsigned *phys_page)
{
    if (cpu->code16) ip &= 0xffffu;
    OptAddr res;
    if (!translate8r(cpu, &res, SEG_CS, ip)) return 0;
    if (in_iomem(res.addr1) || res.addr1 >= (uword)cpu->phys_mem_size) return 0;

    uword linear = cpu->seg[SEG_CS].base + ip;
    unsigned n = 4096u - (unsigned)(linear & 4095u);
    unsigned pn = 4096u - (unsigned)(res.addr1 & 4095u);
    if (pn < n) n = pn;
    unsigned memn = (unsigned)((uword)cpu->phys_mem_size - res.addr1);
    if (memn < n) n = memn;
    if (cpu->code16) {
        unsigned wrapn = 0x10000u - (unsigned)(ip & 0xffffu);
        if (wrapn < n) n = wrapn;
    }
    *out = cpu->phys_mem + res.addr1;
    *avail = n;
    *phys_page = (unsigned)(res.addr1 >> 12);
    return n != 0;
}

/* V8 compiler staging buffer. A <=192-byte trace can span at most two 4K pages. */
#define NJ_V8_DECODE_MAX 192u
static u8 nj_v8_decode_buf[NJ_V8_DECODE_MAX];

static int nj_v8_code_window(CPUI386 *cpu, uword ip,
                             const u8 **out, unsigned *avail,
                             unsigned *phys_page, unsigned *phys_page2,
                             unsigned *code_split)
{
    const u8 *p1;
    unsigned n1, pg1;
    if (!nj_code_window(cpu, ip, &p1, &n1, &pg1)) return 0;

    *phys_page = pg1;
    *phys_page2 = 0;
    *code_split = 0;
    if (n1 >= NJ_V8_DECODE_MAX) {
        *out = p1;
        *avail = n1;
        return n1 != 0;
    }

    /* A 16-bit code stream may cross an ordinary 4K page boundary, but do not
     * speculate across the 64K IP wrap.  The latter needs explicit segmented
     * wrap semantics and is not relevant to normal DOS hot loops. */
    if (cpu->code16 && (unsigned)(ip & 0xffffu) + n1 >= 0x10000u) {
        *out = p1;
        *avail = n1;
        return n1 != 0;
    }

    /* The first window ended at a linear/physical 4K boundary. Try the next
     * guest code page; if it is absent/MMIO, the first-page prefix is still
     * perfectly usable. */
    uword ip2 = ip + n1;
    uword linear2 = cpu->seg[SEG_CS].base + ip2;
    uword paddr2;

    /* Do not call translate8r() speculatively for the second page. A refill
     * here could set PTE accessed state or even leave a guest #PF pending for
     * code that the current execution path never reaches. Cross-page trace
     * growth is therefore opportunistic: use a live TLB hit when paging is on,
     * otherwise stop cleanly at the first page. */
    if (cpu->cr0 & CR0_PG) {
        uword lpgno = linear2 >> 12;
        struct tlb_entry *ent = &cpu->tlb.tab[lpgno % tlb_size];
        if (ent->lpgno != lpgno || ent->pte_lookup[cpu->cpl > 0][0]) {
            *out = p1; *avail = n1; return 1;
        }
        paddr2 = ent->xaddr ^ linear2;
    } else {
        /* Match normal non-paged translation, including the A20 gate. */
        paddr2 = linear2 & cpu->a20_mask;
    }

    if (in_iomem(paddr2) || paddr2 >= (uword)cpu->phys_mem_size) {
        *out = p1; *avail = n1; return 1;
    }

    unsigned n2 = 4096u - (unsigned)(paddr2 & 4095u);
    unsigned memn = (unsigned)((uword)cpu->phys_mem_size - paddr2);
    if (memn < n2) n2 = memn;
    if (n2 > NJ_V8_DECODE_MAX - n1) n2 = NJ_V8_DECODE_MAX - n1;
    if (!n2) { *out = p1; *avail = n1; return 1; }

    memcpy(nj_v8_decode_buf, p1, n1);
    memcpy(nj_v8_decode_buf + n1, cpu->phys_mem + paddr2, n2);
    *out = nj_v8_decode_buf;
    *avail = n1 + n2;
    *phys_page2 = (unsigned)(paddr2 >> 12);
    *code_split = n1;
    return 1;
}

/* A cached paged block survives TLB clears, but it may execute only after
 * the current TLB confirms that its linear code page still maps to the same
 * physical page from which the x86 bytes were compiled.  On a TLB miss we
 * simply return to the interpreter; normal instruction fetch refills the TLB
 * and the next hot backedge can reuse the block without recompiling it. */
static inline bool nj_code_page_mapping_valid(CPUI386 *cpu, uword linear,
                                              unsigned phys_page)
{
    uword lpgno = linear >> 12;
    struct tlb_entry *ent = &cpu->tlb.tab[lpgno % tlb_size];
    if (unlikely(ent->lpgno != lpgno)) return false;
    if (unlikely(ent->pte_lookup[cpu->cpl > 0][0])) return false;
    uword paddr = ent->xaddr ^ linear;
    return (unsigned)(paddr >> 12) == phys_page;
}

static inline bool nj_code_mapping_valid(CPUI386 *cpu,
                                         const nj_block_t *b, uword linear)
{
    if (!(cpu->cr0 & CR0_PG)) return true;
    /* v8.6 exact BP-stack blocks validate code+data together from current
     * page tables in nj_exec_loop(), so they must not require code TLB
     * residency here. */
    if (b->static_write_count == 3u) return true;
    if (!nj_code_page_mapping_valid(cpu, linear, b->phys_page)) return false;
    if (b->code_split) {
        uword linear2 = (linear & ~(uword)0xfffu) + 0x1000u;
        if (!nj_code_page_mapping_valid(cpu, linear2, b->phys_page2)) return false;
    }
    return true;
}

/*
 * v8.6 paged BP-stack page-table guard.
 *
 * v8.5 used the live direct-mapped guest TLB for the exact Symantec loop's
 * code/data pages. Under EMM386 code and stack pages can evict each other from
 * the same TLB slot, so a perfectly valid loop can become impossible to enter.
 *
 * The exact BP-stack block therefore walks the current 386 page tables directly
 * once per native entry.  No guest TLB entry is populated or displaced.  This
 * is cheap compared with the hundreds of eight-instruction iterations normally
 * retired by one exact-loop entry.  Accessed/Dirty bits are changed only after
 * every mapping has validated and native execution is actually about to begin.
 */
static __attribute__((noinline)) bool
nj_v45_paged_walk_addr(CPUI386 *cpu, uword linear, bool write,
                       uword *out_phys, u8 **out_pde, u8 **out_pte)
{
    if (!(cpu->cr0 & CR0_PG))
        return false;

    uword pd_base = cpu->cr3 & ~(uword)0xfffu;
    uword pde_addr = pd_base + ((linear >> 22) & 0x3ffu) * 4u;
    if (pde_addr + 3u < pde_addr ||
        pde_addr + 3u >= (uword)cpu->phys_mem_size ||
        in_iomem(pde_addr) || in_iomem(pde_addr + 3u))
        return false;
#if REMOTE_MEM
    if (is_remote(pde_addr) || is_remote(pde_addr + 3u))
        return false;
#endif

    uword pde = pload32(cpu, pde_addr);
    if (!(pde & 1u))
        return false;

    uword pt_base = pde & ~(uword)0xfffu;
    uword pte_addr = pt_base + ((linear >> 12) & 0x3ffu) * 4u;
    if (pte_addr + 3u < pte_addr ||
        pte_addr + 3u >= (uword)cpu->phys_mem_size ||
        in_iomem(pte_addr) || in_iomem(pte_addr + 3u))
        return false;
#if REMOTE_MEM
    if (is_remote(pte_addr) || is_remote(pte_addr + 3u))
        return false;
#endif

    uword pte_raw = pload32(cpu, pte_addr);
    if (!(pte_raw & 1u))
        return false;

    /* Mirror tlb_refill()/translate_lpgno(): PDE R/W and U/S may only remove
     * rights granted by the PTE, then use the exact same permission table. */
    uword eff = pte_raw & ((pde & 7u) | 0xfffffff8u);
    if (pte_lookup[!!(cpu->cr0 & CR0_WP)][(eff >> 1) & 3u]
                  [cpu->cpl > 0][write ? 1 : 0])
        return false;

    uword phys = (pte_raw & ~(uword)0xfffu) | (linear & 0xfffu);
    if (phys >= (uword)cpu->phys_mem_size || in_iomem(phys))
        return false;
#if REMOTE_MEM
    if (is_remote(phys))
        return false;
#endif

    if (out_phys) *out_phys = phys;
    if (out_pde) *out_pde = cpu->phys_mem + pde_addr;
    if (out_pte) *out_pte = cpu->phys_mem + pte_addr;
    return true;
}

static __attribute__((noinline)) bool
nj_v45_paged_walk_word(CPUI386 *cpu, uword linear, bool write,
                       uword *out_phys, u8 **out_pde, u8 **out_pte)
{
    /* The fast path issues one native halfword access; preserve the emulator's
     * two-page semantics by rejecting a guest word split across 4K. */
    if ((linear & 0xfffu) == 0xfffu)
        return false;
    uword phys;
    if (!nj_v45_paged_walk_addr(cpu, linear, write,
                                &phys, out_pde, out_pte))
        return false;
    if (phys + 1u < phys || phys + 1u >= (uword)cpu->phys_mem_size ||
        in_iomem(phys + 1u))
        return false;
#if REMOTE_MEM
    if (is_remote(phys + 1u))
        return false;
#endif
    if (out_phys) *out_phys = phys;
    return true;
}

/* Exact-loop code mapping guard. Generic blocks keep the cheaper live-TLB
 * guard; only this block pays page-table walks. */
static __attribute__((noinline)) bool
nj_v45_paged_code_valid(CPUI386 *cpu, const nj_block_t *b)
{
    uword p1;
    if (!nj_v45_paged_walk_addr(cpu, b->tag, false, &p1, NULL, NULL) ||
        (unsigned)(p1 >> 12) != b->phys_page)
        return false;
    if (b->code_split) {
        uword linear2 = (b->tag & ~(uword)0xfffu) + 0x1000u;
        uword p2;
        if (!nj_v45_paged_walk_addr(cpu, linear2, false, &p2, NULL, NULL) ||
            (unsigned)(p2 >> 12) != b->phys_page2)
            return false;
    }
    return true;
}

static __attribute__((noinline)) bool
nj_v45_paged_static_valid(CPUI386 *cpu, const nj_block_t *b, bool mark_dirty)
{
    if (!(cpu->cr0 & CR0_PG) || b->static_write_count != 3u ||
        !nj_v45_paged_code_valid(cpu, b))
        return false;

    u16 bp = (u16)b->bp_value;
    u16 sp = (u16)b->sp_value;
    uword ss = b->ss_base;
    uword l_m2  = ss + (uword)(u16)(bp - 2u);
    uword l_m4  = ss + (uword)(u16)(bp - 4u);
    uword l_m6  = ss + (uword)(u16)(bp - 6u);
    uword l_stk = ss + (uword)(u16)(sp - 2u);

    if (b->static_write_phys[1] < 2u)
        return false;
    uword expected_m6 = b->static_write_phys[1] - 2u;

    uword p_m2, p_m4, p_m6, p_stk;
    u8 *pde_m2 = NULL, *pte_m2 = NULL;
    u8 *pde_m4 = NULL, *pte_m4 = NULL;
    u8 *pde_m6 = NULL, *pte_m6 = NULL;
    u8 *pde_stk = NULL, *pte_stk = NULL;

    if (!nj_v45_paged_walk_word(cpu, l_m2, true, &p_m2, &pde_m2, &pte_m2) ||
        !nj_v45_paged_walk_word(cpu, l_m4, true, &p_m4, &pde_m4, &pte_m4) ||
        !nj_v45_paged_walk_word(cpu, l_m6, false, &p_m6, &pde_m6, &pte_m6) ||
        !nj_v45_paged_walk_word(cpu, l_stk, true, &p_stk, &pde_stk, &pte_stk) ||
        p_m2 != b->static_write_phys[0] ||
        p_m4 != b->static_write_phys[1] ||
        p_m6 != expected_m6 ||
        p_stk != b->static_write_phys[2])
        return false;

    if (mark_dirty) {
        *pde_m2  |= 1u << 5;  *pte_m2  |= (1u << 5) | (1u << 6);
        *pde_m4  |= 1u << 5;  *pte_m4  |= (1u << 5) | (1u << 6);
        *pde_m6  |= 1u << 5;  *pte_m6  |= 1u << 5;
        *pde_stk |= 1u << 5;  *pte_stk |= (1u << 5) | (1u << 6);
    }
    return true;
}

static inline __attribute__((always_inline))
bool nj_block_matches(CPUI386 *cpu, const nj_block_t *b, uword linear, uword key)
{
    return b->valid && b->tag == linear && b->mmu_key == key &&
           b->cs_base == cpu->seg[SEG_CS].base &&
           (!b->uses_ds_static ||
            (b->ds_base == cpu->seg[SEG_DS].base &&
             b->ds_sel == cpu->seg[SEG_DS].sel)) &&
           (!b->uses_es_static ||
            (b->es_base == cpu->seg[SEG_ES].base &&
             b->es_sel == cpu->seg[SEG_ES].sel)) &&
           (!b->uses_ss_base ||
            (b->ss_base == cpu->seg[SEG_SS].base &&
             b->ss_sel == cpu->seg[SEG_SS].sel)) &&
           (!b->uses_ss_static ||
            (b->ss_base == cpu->seg[SEG_SS].base &&
             b->ss_sel == cpu->seg[SEG_SS].sel &&
             b->bp_value == (uword)cpu->gprx[5].r16 &&
             b->sp_value == (uword)cpu->gprx[4].r16)) &&
           (!b->uses_df_static || b->df_value == (u8)!!(cpu->flags & DF)) &&
           b->code16 == (u8)cpu->code16 &&
           nj_code_mapping_valid(cpu, b, linear);
}

static inline __attribute__((always_inline))
nj_block_t *nj_lookup_linear(CPUI386 *cpu, uword linear)
{
    uword key = nj_mmu_key(cpu);
    unsigned base = nj_hash(linear ^ key, NJ_CACHE_SET_BITS) * NJ_CACHE_WAYS;
    nj_block_t *b0 = &nj_cache[base];
    if (likely(nj_block_matches(cpu, b0, linear, key))) return b0;
    nj_block_t *b1 = &nj_cache[base + 1u];
    if (likely(nj_block_matches(cpu, b1, linear, key))) return b1;
    return NULL;
}

static inline __attribute__((always_inline))
nj_block_t *nj_lookup(CPUI386 *cpu, uword ip)
{
    return nj_lookup_linear(cpu, cpu->seg[SEG_CS].base + ip);
}

static nj_block_t *nj_compile_loop(CPUI386 *cpu, uword start_ip)
{
    const u8 *code;
    unsigned avail, phys_page, phys_page2, code_split;
    if (!nj_v8_code_window(cpu, start_ip, &code, &avail,
                           &phys_page, &phys_page2, &code_split)) {
        nj_rej_note(NJR_CODE_WINDOW, start_ip, 0, 0xffffffffu, 0);
        return NULL;
    }

    /*
     * BIOS tick polling loop seen by Symantec:
     *   MOV DX,ES:[046Ch] / CMP AX,DX / JZ back
     *
     * Native execution made this loop report enormous MIPS but cannot shorten
     * the wall-clock wait for the BIOS timer.  Compiling it therefore only
     * adds dispatch/cache pressure and depresses the actual CPU benchmark.
     */
    static const u8 timer_poll[] = {
        0x26,0x8b,0x16,0x6c,0x04,0x3b,0xc2,0x74,0xf7
    };
    if (avail >= sizeof(timer_poll) &&
        memcmp(code, timer_poll, sizeof(timer_poll)) == 0)
        return NULL;

    /*
     * See NJ_LOOP_MAX_ARM_BYTES for why the reservation grew and what bounds
     * it.
     */
    unsigned room = NJIT_LOOP_MEM ? NJ_LOOP_MAX_ARM_BYTES : NJ_MAX_ARM_BYTES;
    if (!nj_make_code_room(room))
        return NULL;

    nj_emit_t e = {
        .p = nj_code_ptr,
        .start = nj_code_ptr,
        .limit = nj_code_ptr + (room / 2u),
        .failed = false
    };

    bool code16 = cpu->code16;
    bool wide66 = nj_wide66_candidate(cpu, code, avail);
    bool state16 = code16 && !wide66;

    nj_emit_prologue(&e, state16);
    u16 *body_start = e.p;

    unsigned pos = 0;
    unsigned body_insns = 0;
    unsigned last_flag_kind = NJ_FLAG_NONE;
    unsigned last_ccop = 0;
    bool flag_scratch_live = false;
    bool cf_dirty = false;
    bool needs_refresh_cf = false;
    bool uses_es_static = false;
    bool uses_ds_static = false;
    bool uses_ss_base = false;
    bool have_flag_op = false;
    unsigned mem_insns = 0;
    nj_branch_t br;
    bool have_branch = false;
    uword branch_ip = 0;

    while (body_insns < NJ_MAX_X86_INSNS && pos < avail) {
        uword gip = start_ip + pos;
        if (code16) gip &= 0xffffu;

        if (nj_decode_backedge(cpu, code + pos, avail - pos, gip, start_ip, &br)) {
            branch_ip = gip;
            have_branch = true;
            break;
        }

        nj_loop_mem_t mc = { .state16 = state16, .instr_ip = gip };
        const nj_loop_mem_t *mcp = NJIT_LOOP_MEM ? &mc : NULL;

        u16 *before = e.p;
        nj_body_info_t bi;
        int n = nj_translate_body_one(cpu, &e, code + pos, avail - pos,
                                      wide66, &bi, mcp);

        if (n > 0 && !e.failed &&
            flag_scratch_live && bi.clobbers_flag_scratch) {
            /*
             * This instruction wants r1-r3, which still hold the operands of
             * the last flag-producing instruction for the lazy flush at the
             * exit.  Rejecting the whole loop here is what kept every memory
             * operand - and every byte move, MOVZX and XCHG - out of a native
             * body.  Rewind instead, materialise the pending CC into the
             * guest, and translate this instruction again onto that state.
             *
             * The translator is a pure function of (cpu, code) apart from the
             * emit buffer, so re-running it after the rewind is exact.
             */
            e.p = before;
            nj_emit_cc_flush(&e, state16, last_flag_kind, last_ccop);
            last_flag_kind = NJ_FLAG_NONE;
            last_ccop = 0;
            flag_scratch_live = false;
            before = e.p;
            n = nj_translate_body_one(cpu, &e, code + pos, avail - pos,
                                      wide66, &bi, mcp);
        }

        if (n <= 0 || e.failed) {
            unsigned op = nj_effective_opcode(code + pos, avail - pos);
            nj_rej_note_modrm(code + pos, avail - pos);
            nj_rej_note(e.failed ? NJR_EMIT : NJR_BODY_OPCODE,
                        start_ip, pos, op, body_insns);
            e.p = before;
            e.failed = false;
            return NULL;
        }

        if (bi.used_memory && ++mem_insns > NJ_LOOP_MAX_MEM_INSNS) {
            nj_rej_note(NJR_EMIT, start_ip, pos, code[pos], body_insns);
            return NULL;
        }

        if (bi.uses_es_static)
            uses_es_static = true;
        if (bi.uses_ds_static)
            uses_ds_static = true;
        if (bi.uses_ss_base)
            uses_ss_base = true;

        if (bi.flag_kind != NJ_FLAG_NONE) {
            if (bi.flag_kind == NJ_FLAG_INCDEC) {
                if (cf_dirty) {
                    nj_rej_note(NJR_INCDEC_CF, start_ip, pos, code[pos], body_insns);
                    return NULL; /* DEC/INC must preserve prior CF. */
                }
                needs_refresh_cf = true;
            } else {
                cf_dirty = true;
                needs_refresh_cf = false;
            }
            last_flag_kind = bi.flag_kind;
            last_ccop = bi.ccop;
            flag_scratch_live = true;
            have_flag_op = true;
        }

        pos += (unsigned)n;
        body_insns++;
    }

    if (!have_branch) {
        nj_rej_note(NJR_NO_BRANCH, start_ip, pos,
                    pos < avail ? code[pos] : 0xffffffffu, body_insns);
        return NULL;
    }

    if (wide66 &&
        (br.type == NJ_BRANCH_LOOP || br.type == NJ_BRANCH_JCXZ)) {
        /* These use CX in 16-bit address mode; preserve-high-half support is
         * deliberately deferred until we know the benchmark needs it. */
        nj_rej_note(NJR_BODY_OPCODE, start_ip, pos,
                    pos < avail ? code[pos] : 0xffffffffu, body_insns);
        return NULL;
    }
    if (body_insns == 0u) {
        nj_rej_note(NJR_EMPTY_BODY, start_ip, pos,
                    pos < avail ? code[pos] : 0xffffffffu, body_insns);
        return NULL;
    }

    /* JZ/JNZ needs Z from a translated flag-producing instruction. */
    if ((br.type == NJ_BRANCH_JZ || br.type == NJ_BRANCH_JNZ) &&
        !have_flag_op) {
        nj_rej_note(NJR_JCC_NO_FLAGS, start_ip, pos,
                    pos < avail ? code[pos] : 0xffffffffu, body_insns);
        return NULL;
    }

    unsigned iter_insns = body_insns + 1u; /* include the x86 back-edge */

    /*
     * Branch-tail layout:
     *   condition false -> normal_exit
     *   condition true  -> ++iters, budget?, back to body
     *   normal_exit     -> ++iters, LR=fallthrough, common_exit
     *   budget_exit     -> LR=start, common_exit
     */
    u16 *to_normal = NULL;

    if ((br.type == NJ_BRANCH_JZ || br.type == NJ_BRANCH_JNZ) &&
        last_flag_kind == NJ_FLAG_NONE) {
        /*
         * The pending CC was materialised mid-body because a later
         * instruction needed r1-r3, so the host condition flags no longer
         * describe the last x86 flag-producing instruction.  Every flag form
         * this translator emits takes ZF from "result == 0", and the flush
         * stored exactly that result in cc.dst - sign-extended for 16-bit
         * operands, which preserves zero-ness.
         *
         * While a CC is still pending the host flags are guaranteed to be
         * live: every instruction that could disturb APSR sets
         * clobbers_flag_scratch and therefore forces the flush above first.
         */
        nj_ldr32(&e, 1, NJ_CPU_REG, NJ_CC_DST);
        nj_cmp_imm0(&e, 1);
    }

    if (br.type == NJ_BRANCH_JZ) {
        to_normal = nj_bcond_placeholder(&e); /* patched as BNE */
    } else if (br.type == NJ_BRANCH_JNZ) {
        to_normal = nj_bcond_placeholder(&e); /* patched as BEQ */
    } else if (br.type == NJ_BRANCH_LOOP) {
        /* Guest ECX is mapped to r5. LOOP itself does not modify x86 flags. */
        nj_subs_imm8(&e, NJ_GUEST_REG(1), 1);
        if (code16) nj_uxth(&e, NJ_GUEST_REG(1), NJ_GUEST_REG(1));
        nj_cmp_imm0(&e, NJ_GUEST_REG(1));
        to_normal = nj_bcond_placeholder(&e); /* BEQ */
    } else if (br.type == NJ_BRANCH_JCXZ) {
        nj_cmp_imm0(&e, NJ_GUEST_REG(1));
        to_normal = nj_bcond_placeholder(&e); /* BNE */
    }

    /* Taken path. */
    nj_adds_imm8(&e, NJ_ITER_REG, 1);
    nj_cmp_reg(&e, NJ_ITER_REG, NJ_BUDGET_REG);
    u16 *to_budget = nj_bcond_placeholder(&e); /* BHS */
    u16 *back = nj_b_placeholder(&e);

    u16 *normal_exit = e.p;
    if (br.type != NJ_BRANCH_JMP) {
        nj_adds_imm8(&e, NJ_ITER_REG, 1);
        nj_mov_imm(&e, NJ_BUDGET_REG, br.fallthrough);
        u16 *normal_to_common = nj_b_placeholder(&e);

        u16 *budget_exit = e.p;
        nj_mov_imm(&e, NJ_BUDGET_REG, start_ip);
        u16 *common_exit = e.p;

        nj_emit_common_exit(&e, state16, last_flag_kind, last_ccop, branch_ip);

        if (!nj_patch_b(back, body_start) ||
            !nj_patch_b(normal_to_common, common_exit)) {
            nj_rej_note(NJR_PATCH, start_ip, pos, code[pos], body_insns);
            return NULL;
        }

        unsigned invcond;
        if (br.type == NJ_BRANCH_JZ) invcond = 1u;       /* NE */
        else if (br.type == NJ_BRANCH_JNZ) invcond = 0u; /* EQ */
        else if (br.type == NJ_BRANCH_LOOP) invcond = 0u;/* EQ */
        else invcond = 1u;                               /* JCXZ false = NE */
        if (!nj_patch_bcond(to_normal, normal_exit, invcond)) {
            nj_rej_note(NJR_PATCH, start_ip, pos, code[pos], body_insns);
            return NULL;
        }
        if (!nj_patch_bcond(to_budget, budget_exit, 2u)) {
            nj_rej_note(NJR_PATCH, start_ip, pos, code[pos], body_insns);
            return NULL; /* HS */
        }
    } else {
        /* Unconditional self-loop: only the budget path can leave native code. */
        u16 *budget_exit = e.p;
        nj_mov_imm(&e, NJ_BUDGET_REG, start_ip);
        nj_emit_common_exit(&e, state16, last_flag_kind, last_ccop, branch_ip);
        if (!nj_patch_b(back, body_start)) {
            nj_rej_note(NJR_PATCH, start_ip, pos, code[pos], body_insns);
            return NULL;
        }
        if (!nj_patch_bcond(to_budget, budget_exit, 2u)) {
            nj_rej_note(NJR_PATCH, start_ip, pos, code[pos], body_insns);
            return NULL; /* HS */
        }
    }

    if (e.failed) {
        nj_rej_note(NJR_EMIT, start_ip, pos,
                    pos < avail ? code[pos] : 0xffffffffu, body_insns);
        return NULL;
    }

    uword linear = cpu->seg[SEG_CS].base + start_ip;
    nj_block_t *b = nj_cache_insert_slot(cpu, linear);
    memset(b, 0, sizeof(*b));
    b->tag = linear;
    b->mmu_key = nj_mmu_key(cpu);
    b->cs_base = cpu->seg[SEG_CS].base;
    b->ds_base = cpu->seg[SEG_DS].base;
    b->es_base = cpu->seg[SEG_ES].base;
    b->ss_base = cpu->seg[SEG_SS].base;
    b->ds_sel = cpu->seg[SEG_DS].sel;
    b->es_sel = cpu->seg[SEG_ES].sel;
    b->ss_sel = cpu->seg[SEG_SS].sel;
    b->start_ip = start_ip;
    b->branch_ip = branch_ip;
    b->fallthrough_ip = br.fallthrough;
    b->byte_len = (u16)(pos + br.len);
    b->phys_page = (u16)phys_page;
    b->code_split = (u16)((code_split && (pos + br.len) > code_split) ? code_split : 0u);
    b->phys_page2 = b->code_split ? (u16)phys_page2 : 0u;
    b->insns = (u8)iter_insns;
    b->code16 = (u8)code16;
    b->needs_refresh_cf = (u8)needs_refresh_cf;
    b->needs_flags_in = 0;
    b->single_run = 0;
    b->uses_ds_static = (u8)uses_ds_static;
    b->uses_es_static = (u8)uses_es_static;
    b->uses_ss_base = (u8)uses_ss_base;
    b->uses_ss_static = 0;
    b->static_write_count = 0;
    b->code = e.start;
    b->arm_halfwords = (u16)(e.p - e.start);
    b->valid = 1;

    nj_code_ptr = e.p;
    nj_page_mark(phys_page);
    if (b->code_split) nj_page_mark(b->phys_page2);
    nj_bloom_add(linear);
    g_njit_compiles++;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
    return b;
}


/* -------------------------------------------------------------------------
 * FRANK_NATIVE_JIT_V45_BPSTACK_LOOP
 *
 * Exact native compiler for the measured Symantec productive hot loop.
 *
 * Safety envelope:
 *   - 16-bit code and 16-bit stack (real mode, protected mode or VM86);
 *   - exact 25-byte instruction sequence above;
 *   - all four touched words must be ordinary local RAM;
 *   - generated host pointers are guarded by SS.base, BP and SP in the cache;
 *   - with paging active, current page-table mappings/permissions are guarded
 *     at every native entry, independent of direct-mapped TLB collisions, and
 *     guest PDE/PTE Accessed/Dirty bits are preserved.
 *
 * PUSH/POP is folded with respect to SP (net zero), but its RAM side effect is
 * preserved: the pushed word is still written to SS:[SP-2], and the popped
 * value is written back to SS:[BP-2].
 * ------------------------------------------------------------------------- */

static bool nj_v45_local_word(CPUI386 *cpu, uword base, u16 off,
                              u32 *host, uword *phys)
{
    uword p = base + (uword)off;
    if (p + 1u < p || p + 1u >= (uword)cpu->phys_mem_size)
        return false;
    if (in_iomem(p) || in_iomem(p + 1u))
        return false;
#if REMOTE_MEM
    if (is_remote(p) || is_remote(p + 1u))
        return false;
#endif
    if (host) *host = (u32)(uintptr_t)(cpu->phys_mem + p);
    if (phys) *phys = p;
    return true;
}

/* Compile-time paged counterpart.  Resolve from the current page tables rather
 * than from direct-mapped TLB residency.  This is a non-faulting observation:
 * no TLB entry is populated and no Accessed/Dirty bit is changed here. */
static bool nj_v45_paged_word(CPUI386 *cpu, uword linear, bool write,
                              u32 *host, uword *phys)
{
    uword p;
    if (!nj_v45_paged_walk_word(cpu, linear, write, &p, NULL, NULL))
        return false;
    if (host) *host = (u32)(uintptr_t)(cpu->phys_mem + p);
    if (phys) *phys = p;
    return true;
}

static bool nj_v45_static_word(CPUI386 *cpu, uword base, u16 off, bool write,
                               u32 *host, uword *phys)
{
    if (cpu->cr0 & CR0_PG)
        return nj_v45_paged_word(cpu, base + (uword)off, write, host, phys);
    return nj_v45_local_word(cpu, base, off, host, phys);
}

/* Read exactly the 25-byte exact-loop pattern without requiring a second code
 * page to be resident in the direct-mapped TLB. */
static bool nj_v45_bpstack_code_window(CPUI386 *cpu, uword start_ip,
                                       const u8 **out, unsigned *avail,
                                       unsigned *phys_page,
                                       unsigned *phys_page2,
                                       unsigned *code_split)
{
    if (!(cpu->cr0 & CR0_PG))
        return nj_v8_code_window(cpu, start_ip, out, avail,
                                 phys_page, phys_page2, code_split);

    enum { NEED = 25u };
    if (cpu->code16 && (unsigned)(start_ip & 0xffffu) + NEED > 0x10000u)
        return false;

    uword linear = cpu->seg[SEG_CS].base + start_ip;
    uword p1;
    if (!nj_v45_paged_walk_addr(cpu, linear, false, &p1, NULL, NULL))
        return false;

    unsigned n1 = 0x1000u - (unsigned)(linear & 0xfffu);
    if (n1 > NEED) n1 = NEED;
    if (p1 + n1 < p1 || p1 + n1 > (uword)cpu->phys_mem_size)
        return false;

    *phys_page = (unsigned)(p1 >> 12);
    *phys_page2 = 0u;
    *code_split = 0u;
    if (n1 == NEED) {
        *out = cpu->phys_mem + p1;
        *avail = NEED;
        return true;
    }

    uword linear2 = linear + n1;
    uword p2;
    if (!nj_v45_paged_walk_addr(cpu, linear2, false, &p2, NULL, NULL))
        return false;
    unsigned n2 = NEED - n1;
    if (p2 + n2 < p2 || p2 + n2 > (uword)cpu->phys_mem_size)
        return false;

    memcpy(nj_v8_decode_buf, cpu->phys_mem + p1, n1);
    memcpy(nj_v8_decode_buf + n1, cpu->phys_mem + p2, n2);
    *out = nj_v8_decode_buf;
    *avail = NEED;
    *phys_page2 = (unsigned)(p2 >> 12);
    *code_split = n1;
    return true;
}

static nj_block_t *nj_compile_v45_bpstack(CPUI386 *cpu, uword start_ip)
{
    static const u8 pat[] = {
        0xff,0x76,0xfe,
        0x8b,0x46,0xfc,
        0x03,0x46,0xfa,
        0x89,0x46,0xfc,
        0x8f,0x46,0xfe,
        0xff,0x46,0xfe,
        0x81,0x7e,0xfe,0xd0,0x07,
        0x7c,0xe7
    };

    g_njit_bp[NJBP_ATTEMPTS]++;

    if (!cpu->code16)
        return nj_bp_rej(NJBP_NOT_CODE16);

    /*
     * Direct host pointers are valid in every non-paged mode.  PE itself is
     * not a reason to reject: VM86 and 16-bit protected-mode code still use
     * the already-resolved SS.base exactly as translate() does.  Keep a
     * 16-bit stack because this exact loop folds PUSH+POP with net-zero SP.
     */
    if (cpu->sp_mask != 0xffffu)
        return nj_bp_rej(NJBP_SP_MASK);

    const u8 *code;
    unsigned avail, phys_page, phys_page2, code_split;
    if (!nj_v45_bpstack_code_window(cpu, start_ip, &code, &avail,
                                  &phys_page, &phys_page2, &code_split))
        return nj_bp_rej(NJBP_CODE_WINDOW);
    if (avail < sizeof(pat) || memcmp(code, pat, sizeof(pat)) != 0)
        return nj_bp_rej(NJBP_PATTERN);

    /* The exact 25 bytes are here.  Every refusal below is a guard saying
     * "this loop, but not in this machine state" - which is the only thing
     * that can explain a V86 capture with no flags=6 block in it. */
    g_njit_bp[NJBP_MATCHED]++;
    /* This exact fast path embeds direct RAM pointers.  In non-paged mode a
     * normal 4K crossing is physically contiguous; reject only exotic A20
     * wrap/non-contiguous cases rather than weakening its alias checks. */
    if (!(cpu->cr0 & CR0_PG) && code_split && phys_page2 != phys_page + 1u)
        return nj_bp_rej(NJBP_SPLIT_NONCONTIG);

    u16 bp = cpu->gprx[5].r16;
    u16 sp = cpu->gprx[4].r16;
    uword ss = cpu->seg[SEG_SS].base;

    u32 ptr_m2, ptr_m4, ptr_m6, ptr_stk;
    uword pa_m2, pa_m4, pa_m6, pa_stk;

    /* Kept as four separate tests purely so the counter says which word
     * failed: BP-2/BP-4/BP-6 are the callee frame, SP-2 is the live stack
     * top, and under EMM386 those can sit in differently mapped pages. */
    if (!nj_v45_static_word(cpu, ss, (u16)(bp - 2u), true, &ptr_m2, &pa_m2))
        return nj_bp_rej(NJBP_STATIC_M2);
    if (!nj_v45_static_word(cpu, ss, (u16)(bp - 4u), true, &ptr_m4, &pa_m4))
        return nj_bp_rej(NJBP_STATIC_M4);
    if (!nj_v45_static_word(cpu, ss, (u16)(bp - 6u), false, &ptr_m6, &pa_m6))
        return nj_bp_rej(NJBP_STATIC_M6);
    if (!nj_v45_static_word(cpu, ss, (u16)(sp - 2u), true, &ptr_stk, &pa_stk))
        return nj_bp_rej(NJBP_STATIC_STK);

    /* Avoid another metadata word for BP-6. The three BP locals are adjacent
     * in the x86 stack frame; if paging maps the page boundary non-contiguously
     * just reject this rare layout and use the general JIT/interpreter. */
    if ((cpu->cr0 & CR0_PG) && pa_m6 + 2u != pa_m4)
        return nj_bp_rej(NJBP_M6_NONADJ);

    /*
     * Stack and code may share a guest page (common in small DOS programs).
     * That is safe as long as the exact words written by this native loop do
     * not overlap its own x86 bytes.  Other cached blocks are invalidated by
     * exact range in nj_exec_loop() before native execution.
     */
    uword code_phys = ((uword)phys_page << 12) |
                      ((cpu->seg[SEG_CS].base + start_ip) & 0xfffu);
    unsigned code_len1 = (code_split && sizeof(pat) > code_split) ? code_split : (unsigned)sizeof(pat);
    uword code_end1 = code_phys + code_len1;
    uword code_phys2 = (uword)phys_page2 << 12;
    unsigned code_len2 = (code_split && sizeof(pat) > code_split)
                       ? (unsigned)sizeof(pat) - code_split : 0u;
    const uword writes[] = { pa_m2, pa_m4, pa_stk };
    for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        uword wend = writes[i] + 2u;
        if ((writes[i] < code_end1 && code_phys < wend) ||
            (code_len2 && writes[i] < code_phys2 + code_len2 &&
             code_phys2 < wend))
            return nj_bp_rej(NJBP_CODE_OVERLAP);
    }

    if (!nj_make_code_room(NJ_MAX_ARM_BYTES))
        return nj_bp_rej(NJBP_NO_ROOM);

    nj_emit_t e = {
        .p = nj_code_ptr,
        .start = nj_code_ptr,
        .limit = nj_code_ptr + (NJ_MAX_ARM_BYTES / 2u),
        .failed = false
    };

    nj_emit_prologue(&e, true);
    u16 *body_start = e.p;

    /*
     * PUSH word [BP-2]
     *   r3 = value, preserve architectural stack RAM write.
     * POP word [BP-2] has no net SP change and reads exactly that pushed word.
     * The intervening instructions do not use SP, so folding SP itself is safe.
     */
    nj_mov_imm(&e, 1, ptr_m2);
    nj_ldrh(&e, 3, 1, 0);
    nj_mov_imm(&e, 1, ptr_stk);
    nj_strh(&e, 3, 1, 0);

    /* MOV AX,[BP-4] */
    nj_mov_imm(&e, 1, ptr_m4);
    nj_ldrh(&e, NJ_GUEST_REG(0), 1, 0);

    /* ADD AX,[BP-6] */
    nj_mov_imm(&e, 2, ptr_m6);
    nj_ldrh(&e, 2, 2, 0);
    nj_mov_reg(&e, 1, NJ_GUEST_REG(0));
    nj_adds3(&e);                 /* r3 = AX + mem */
    nj_uxth(&e, 3, 3);
    nj_mov_reg(&e, NJ_GUEST_REG(0), 3);

    /* MOV [BP-4],AX */
    nj_mov_imm(&e, 1, ptr_m4);
    nj_strh(&e, NJ_GUEST_REG(0), 1, 0);

    /* POP word [BP-2] -- value is still the one pushed at loop start. */
    nj_mov_imm(&e, 1, ptr_stk);
    nj_ldrh(&e, 3, 1, 0);
    nj_mov_imm(&e, 1, ptr_m2);
    nj_strh(&e, 3, 1, 0);

    /* INC word [BP-2] */
    nj_adds_imm8(&e, 3, 1);
    nj_uxth(&e, 3, 3);
    nj_mov_imm(&e, 1, ptr_m2);
    nj_strh(&e, 3, 1, 0);

    /*
     * CMP word [BP-2],07D0h.
     *
     * r1/r2/r3 are deliberately left as src1/src2/dst for the existing lazy
     * CC flush on either native exit.  Signed JL is evaluated from a separate
     * ARM CMP of sign-extended src1 against +2000, which exactly matches x86
     * 16-bit signed comparison.
     */
    nj_mov_reg(&e, 1, 3);
    nj_mov_imm(&e, 2, 0x07d0u);
    nj_subs3(&e);
    nj_uxth(&e, 3, 3);
    nj_sxth(&e, 1, 1);
    nj_cmp_reg(&e, 1, 2);

    /* JL taken => loop. BGE is the normal-exit inverse. */
    u16 *to_normal = nj_bcond_placeholder(&e);

    /* Taken path: one complete 8-instruction x86 iteration retired. */
    nj_adds_imm8(&e, NJ_ITER_REG, 1);
    nj_cmp_reg(&e, NJ_ITER_REG, NJ_BUDGET_REG);
    u16 *to_budget = nj_bcond_placeholder(&e); /* BHS */
    u16 *back = nj_b_placeholder(&e);

    /* JL false: branch itself retired, continue after the 25-byte loop. */
    u16 *normal_exit = e.p;
    nj_adds_imm8(&e, NJ_ITER_REG, 1);
    nj_mov_imm(&e, NJ_BUDGET_REG, (start_ip + 25u) & 0xffffu);
    u16 *normal_to_common = nj_b_placeholder(&e);

    /* Budget exhausted after a taken JL: resume at loop start next call. */
    u16 *budget_exit = e.p;
    nj_mov_imm(&e, NJ_BUDGET_REG, start_ip & 0xffffu);

    u16 *common_exit = e.p;
    nj_emit_common_exit(&e, true, NJ_FLAG_ARITH, CC_SUB,
                        (start_ip + 23u) & 0xffffu);

    if (!nj_patch_bcond(to_normal, normal_exit, 10u) ||  /* BGE */
        !nj_patch_bcond(to_budget, budget_exit, 2u) ||    /* BHS */
        !nj_patch_b(back, body_start) ||
        !nj_patch_b(normal_to_common, common_exit) ||
        e.failed)
        return nj_bp_rej(NJBP_EMIT);

    uword linear = cpu->seg[SEG_CS].base + (start_ip & 0xffffu);
    nj_block_t *b = nj_cache_insert_slot(cpu, linear);
    memset(b, 0, sizeof(*b));

    b->tag = linear;
    b->mmu_key = nj_mmu_key(cpu);
    b->cs_base = cpu->seg[SEG_CS].base;
    b->ds_base = cpu->seg[SEG_DS].base;
    b->es_base = cpu->seg[SEG_ES].base;
    b->ss_base = ss;
    b->ds_sel = cpu->seg[SEG_DS].sel;
    b->es_sel = cpu->seg[SEG_ES].sel;
    b->ss_sel = cpu->seg[SEG_SS].sel;
    b->bp_value = bp;
    b->sp_value = sp;
    b->start_ip = start_ip & 0xffffu;
    b->branch_ip = (start_ip + 23u) & 0xffffu;
    b->fallthrough_ip = (start_ip + 25u) & 0xffffu;
    b->byte_len = (u16)sizeof(pat);
    b->phys_page = (u16)phys_page;
    b->code_split = (u16)((code_split && sizeof(pat) > code_split) ? code_split : 0u);
    b->phys_page2 = b->code_split ? (u16)phys_page2 : 0u;
    b->insns = 8u;
    b->code16 = 1u;
    b->valid = 1u;
    b->needs_refresh_cf = 0u;
    b->needs_flags_in = 0u;
    b->single_run = 0u;
    b->uses_ds_static = 0u;
    b->uses_es_static = 0u;
    b->uses_ss_base = 0u;
    b->uses_ss_static = 1u;
    b->static_write_count = 3u;
    b->static_write_phys[0] = pa_m2;
    b->static_write_phys[1] = pa_m4;
    b->static_write_phys[2] = pa_stk;
    b->code = e.start;
    b->arm_halfwords = (u16)(e.p - e.start);

    nj_code_ptr = e.p;
    nj_page_mark(phys_page);
    if (b->code_split) nj_page_mark(b->phys_page2);
    nj_bloom_add(linear);
    g_njit_compiles++;
    g_njit_bp[NJBP_OK]++;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
    return b;
}


/* -------------------------------------------------------------------------
 * FRANK_NATIVE_JIT_V6_HYBRID_TRACE
 *
 * General 32-bit native-prefix trace compiler.
 *
 * This is deliberately a HYBRID JIT rather than another all-or-nothing loop
 * recogniser. A hot backward target is decoded sequentially. Common integer,
 * ModR/M/SIB memory and shift-double instructions become Thumb-2; when the
 * trace reaches an instruction that is not yet worth lowering natively, the
 * generated block exits *before* that instruction and the existing interpreter
 * resumes with architecturally correct state.
 *
 * That gives the whole x86 core a JIT-aware execution path without requiring
 * every rare/system/FPU opcode to be duplicated in the native backend on day
 * one. The old v4/v5 loop JIT remains first choice for the small loops it can
 * chain completely; v6 is the broad fallback for workloads such as Doom.
 *
 * v8 dynamic memory uses the live guest TLB when paging is active and direct
 * linear addressing when it is off. Segment bases and the current paged code
 * mapping are guarded at dispatch. Dynamic stores still test the translated
 * code-page bitmap; writes to any page containing JIT code side-exit to the
 * interpreter, which performs the exact SMC invalidation from v5.1.
 * ------------------------------------------------------------------------- */

#define NJ_V6_MAX_INSNS  24u

#define NJ_V6_MAX_BYTES  NJ_V8_DECODE_MAX
#define NJ_V6_MAX_ARM_BYTES 2048u

static bool nj_v6_prefix(CPUI386 *cpu, const u8 *p, unsigned max,
                         nj_v6_pfx_t *x)
{
    memset(x, 0, sizeof(*x));
    x->op16 = cpu->code16;
    x->addr16 = cpu->code16;
    x->seg = -1;

    while (x->pos < max && x->pos < 8u) {
        switch (p[x->pos]) {
        case 0x26: x->seg = SEG_ES; x->pos++; continue;
        case 0x2e: x->seg = SEG_CS; x->pos++; continue;
        case 0x36: x->seg = SEG_SS; x->pos++; continue;
        case 0x3e: x->seg = SEG_DS; x->pos++; continue;
        case 0x64: x->seg = SEG_FS; x->pos++; continue;
        case 0x65: x->seg = SEG_GS; x->pos++; continue;
        case 0x66: x->op16 = !x->op16; x->pos++; continue;
        case 0x67: x->addr16 = !x->addr16; x->pos++; continue;
        case 0xf0: case 0xf2: case 0xf3:
            return false; /* interpreter fallback keeps LOCK/REP semantics */
        default:
            return true;
        }
    }
    return false;
}

static bool nj_v6_note_seg(CPUI386 *cpu, int seg,
                           bool *use_ds, bool *use_es, bool *use_ss)
{
    /*
     * Match the interpreter's currently-enabled protected-mode segcheck:
     * null data selectors fault before any memory access.
     */
    if ((cpu->cr0 & 1u) && !(cpu->flags & VM) &&
        cpu->seg[seg].limit == 0 &&
        (cpu->seg[seg].sel & ~3u) == 0)
        return false;

    switch (seg) {
    case SEG_DS: *use_ds = true; return true;
    case SEG_ES: *use_es = true; return true;
    case SEG_SS: *use_ss = true; return true;
    case SEG_CS: return true;  /* cs_base is already part of every cache guard */
    default:
        return false;          /* FS/GS remain interpreter fallback for now */
    }
}

static bool nj_v6_decode_ea(const u8 *p, unsigned max, u8 modrm,
                            bool addr16, int seg_override, nj_v6_ea_t *ea)
{
    memset(ea, 0, sizeof(*ea));
    ea->base = -1;
    ea->index = -1;
    ea->scale = 0;
    ea->disp = 0;
    ea->seg = seg_override;
    ea->addr16 = addr16;

    unsigned mod = modrm >> 6;
    unsigned rm = modrm & 7u;
    unsigned pos = 0;
    if (mod == 3u) return false;

    if (addr16) {
        if (mod == 0u && rm == 6u) {
            if (max < 2u) return false;
            ea->disp = (s32)(u16)nj_rd16(p);
            pos += 2u;
            if (ea->seg < 0) ea->seg = SEG_DS;
        } else {
            switch (rm) {
            case 0: ea->base = 3; ea->index = 6; break; /* BX+SI */
            case 1: ea->base = 3; ea->index = 7; break; /* BX+DI */
            case 2: ea->base = 5; ea->index = 6; break; /* BP+SI */
            case 3: ea->base = 5; ea->index = 7; break; /* BP+DI */
            case 4: ea->base = 6; break;                /* SI */
            case 5: ea->base = 7; break;                /* DI */
            case 6: ea->base = 5; break;                /* BP */
            case 7: ea->base = 3; break;                /* BX */
            }
            if (mod == 1u) {
                if (max < pos + 1u) return false;
                ea->disp = (s8)p[pos++];
            } else if (mod == 2u) {
                if (max < pos + 2u) return false;
                ea->disp = (s32)(u16)nj_rd16(p + pos);
                pos += 2u;
            }
            if (ea->seg < 0)
                ea->seg = (rm == 2u || rm == 3u ||
                           (rm == 6u && mod != 0u)) ? SEG_SS : SEG_DS;
        }
    } else {
        if (rm == 4u) {
            if (max < 1u) return false;
            u8 sib = p[pos++];
            unsigned b = sib & 7u;
            unsigned i = (sib >> 3) & 7u;
            ea->scale = sib >> 6;
            if (i != 4u) ea->index = (int)i;

            if (b == 5u && mod == 0u) {
                if (max < pos + 4u) return false;
                ea->disp = (s32)nj_rd32(p + pos);
                pos += 4u;
                if (ea->seg < 0) ea->seg = SEG_DS;
            } else {
                ea->base = (int)b;
                if (ea->seg < 0)
                    ea->seg = (b == 4u || b == 5u) ? SEG_SS : SEG_DS;
            }
        } else if (rm == 5u && mod == 0u) {
            if (max < 4u) return false;
            ea->disp = (s32)nj_rd32(p);
            pos += 4u;
            if (ea->seg < 0) ea->seg = SEG_DS;
        } else {
            ea->base = (int)rm;
            if (ea->seg < 0) ea->seg = rm == 5u ? SEG_SS : SEG_DS;
        }

        if (mod == 1u) {
            if (max < pos + 1u) return false;
            ea->disp += (s8)p[pos++];
        } else if (mod == 2u) {
            if (max < pos + 4u) return false;
            ea->disp += (s32)nj_rd32(p + pos);
            pos += 4u;
        }
    }

    ea->used = pos;
    return ea->seg >= 0;
}

/* Emit guest effective/linear address into r3. v7 translates it through the live TLB below. */
static void nj_v6_emit_ea(CPUI386 *cpu, nj_emit_t *e, const nj_v6_ea_t *ea, bool add_seg)
{
    if (ea->base >= 0) {
        nj_mov_reg(e, 1, NJ_GUEST_REG((unsigned)ea->base));
        if (ea->addr16) nj_uxth(e, 1, 1);
    } else {
        nj_mov_imm(e, 1, 0);
    }

    if (ea->index >= 0) {
        nj_mov_reg(e, 2, NJ_GUEST_REG((unsigned)ea->index));
        if (ea->addr16) nj_uxth(e, 2, 2);
        if (ea->scale) nj_lsls_imm(e, 2, 2, ea->scale);
        nj_adds3(e);
        nj_mov_reg(e, 1, 3);
    }

    if (ea->disp) {
        nj_mov_imm(e, 2, (u32)ea->disp);
        nj_adds3(e);
        nj_mov_reg(e, 1, 3);
    }

    if (ea->addr16) nj_uxth(e, 1, 1);

    if (add_seg) {
        uword sb = cpu->seg[ea->seg].base;
        if (sb) {
            nj_mov_imm(e, 2, sb);
            nj_adds3(e);
            nj_mov_reg(e, 1, 3);
        }
    }

    nj_mov_reg(e, 3, 1);
}

static void nj_v6_guard_add(nj_emit_t *e, nj_v6_guard_t *g, unsigned cond)
{
    if (g->n >= sizeof(g->at) / sizeof(g->at[0])) {
        e->failed = true;
        return;
    }
    g->at[g->n] = nj_bcond_placeholder(e);
    g->cond[g->n] = cond;
    g->n++;
}

/* RP2350-side offsets used by the generated paged-memory fast path. */
#define NJ_TLB_TAB_OFF        ((unsigned)offsetof(CPUI386, tlb.tab))
#define NJ_TLB_LPGNO_OFF      ((unsigned)offsetof(struct tlb_entry, lpgno))
#define NJ_TLB_XADDR_OFF      ((unsigned)offsetof(struct tlb_entry, xaddr))
#define NJ_TLB_PTELOOKUP_OFF  ((unsigned)offsetof(struct tlb_entry, pte_lookup))
#define NJ_TLB_PPTE_OFF       ((unsigned)offsetof(struct tlb_entry, ppte))

/*
 * v7 runtime paging translation.
 *
 * Input:  r3 = guest linear address
 * Output: r3 = guest physical address on the success path
 *
 * We deliberately side-exit instead of calling tlb_refill() from generated
 * Thumb code.  A miss, protection failure, or cross-page access is executed
 * once by the interpreter, which already has the exact page-fault and split
 * access semantics.  Hot TLB hits stay completely native.
 *
 * tlb_refill() sets the accessed bit.  For native writes we additionally set
 * the PTE dirty bit exactly like translate_lpgno().
 */
static bool nj_v7_emit_linear_to_phys_inline(CPUI386 *cpu, nj_emit_t *e,
                                             unsigned size, bool write,
                                             nj_v6_guard_t *g)
{
    if (!(cpu->cr0 & CR0_PG))
        return true;

    if (!size || size > 4u || !cpu->tlb.tab)
        return false;

    unsigned ent_size = (unsigned)sizeof(struct tlb_entry);
    unsigned ent_shift;
    if (ent_size == 16u) ent_shift = 4u;
    else if (ent_size == 32u) ent_shift = 5u; /* host-side structural tests */
    else return false;

    nj_mov_reg(e, 0, 3);                         /* r0 = linear */

    /* Split-page reads/writes retain the interpreter's ADDR_OK2 path. */
    if (size > 1u) {
        nj_mov_reg(e, 1, 0);
        nj_lsls_imm(e, 1, 1, 20);               /* page offset via <<20>>20 */
        nj_lsrs_imm(e, 1, 1, 20);
        nj_mov_imm(e, 2, 0x1000u - size);
        nj_cmp_reg(e, 1, 2);
        nj_v6_guard_add(e, g, 8u);               /* HI => crosses page */
    }

    /* r3 = &cpu->tlb.tab[(linear >> 12) & (tlb_size - 1)] */
    nj_mov_reg(e, 1, 0);
    nj_lsrs_imm(e, 1, 1, 12);                   /* lpgno */
    nj_mov_imm(e, 2, (u32)(tlb_size - 1u));
    nj_and_low(e, 1, 2);                        /* direct-mapped index */
    nj_lsls_imm(e, 1, 1, ent_shift);            /* byte offset */
    nj_ldr32(e, 2, NJ_CPU_REG, NJ_TLB_TAB_OFF);
    nj_adds3(e);                                /* r3 = entry */

    /* Tag mismatch => interpreter refills the TLB. */
    nj_ldr32(e, 2, 3, NJ_TLB_LPGNO_OFF);
    nj_mov_reg(e, 1, 0);
    nj_lsrs_imm(e, 1, 1, 12);
    nj_cmp_reg(e, 2, 1);
    nj_v6_guard_add(e, g, 1u);                  /* NE */

    /* pte_lookup[cpl > 0][write] is already precomputed by tlb_refill(). */
    nj_ldr32(e, 2, 3, NJ_TLB_PTELOOKUP_OFF);
    unsigned perm_off = (cpu->cpl > 0 ? 2u * (unsigned)sizeof(int) : 0u) +
                        (write ? (unsigned)sizeof(int) : 0u);
    nj_ldr32(e, 2, 2, perm_off);
    nj_cmp_imm0(e, 2);
    nj_v6_guard_add(e, g, 1u);                  /* NE => protection fault */

    /* Compute physical address while r3 still points at the TLB entry. */
    nj_ldr32(e, 2, 3, NJ_TLB_XADDR_OFF);
    nj_mov_reg(e, 1, 0);
    nj_eor1(e);                                 /* r1 = linear ^ xaddr */

    if (write) {
        nj_mov_reg(e, 0, 1);                    /* save physical address */
        nj_ldr32(e, 2, 3, NJ_TLB_PPTE_OFF);
        nj_ldrb(e, 1, 2, 0);
        nj_mov_imm(e, 3, 1u << 6);
        nj_orr_low(e, 1, 3);
        nj_strb(e, 1, 2, 0);                    /* PTE dirty */
        nj_mov_reg(e, 3, 0);
    } else {
        nj_mov_reg(e, 3, 1);
    }

    return !e->failed;
}

/*
 * r3 = guest physical address. Leave r3 unchanged on the success path.
 * ARM condition codes: HI=8, CC/LO=3, NE=1.
 */
static bool nj_v6_emit_mem_guard_inline(CPUI386 *cpu, nj_emit_t *e,
                                        unsigned size, bool write,
                                        nj_v6_guard_t *g)
{
    if (!size || size > 4u ||
        (uword)cpu->phys_mem_size < size)
        return false;

    uword lim = (uword)cpu->phys_mem_size - size;
    nj_mov_imm(e, 2, lim);
    nj_cmp_reg(e, 3, 2);
    nj_v6_guard_add(e, g, 8u); /* HI: paddr > last valid start */

    /* Side-exit on VGA/MMIO physical aperture 0xA0000..0xBFFFF. */
    uword vlo = 0xA0000u - (size - 1u);
    nj_mov_imm(e, 2, vlo);
    nj_cmp_reg(e, 3, 2);
    u16 *below_vga = nj_bcond_placeholder(e); /* LO => definitely safe */
    nj_mov_imm(e, 2, 0xC0000u);
    nj_cmp_reg(e, 3, 2);
    nj_v6_guard_add(e, g, 3u); /* LO => overlaps VGA */
    u16 *after_vga = e->p;
    if (!nj_patch_bcond(below_vga, after_vga, 3u)) {
        e->failed = true;
        return false;
    }

#if REMOTE_MEM
    /*
     * C2 REMOTE_MEM needs the remote accessor and therefore stays on the
     * interpreter path for v6 dynamic memory accesses.
     */
    return false;
#else
    if (write) {
        /*
         * A direct native write is safe only when its physical page contains
         * no translated x86 code.  The existing compact page bitmap makes
         * this check cheap and has no false negatives.
         *
         * The v6 direct-store path is enabled only while guest RAM fits in
         * the bitmap's tracked 8 MiB range.
         */
        if ((uword)cpu->phys_mem_size > ((uword)NJ_TRACK_PAGES << 12))
            return false;

        nj_mov_reg(e, 0, 3);                        /* save paddr */

        nj_mov_reg(e, 1, 3);
        nj_lsrs_imm(e, 1, 1, 15);                  /* byte index = page >> 3 */
        nj_mov_imm(e, 2, (u32)(uintptr_t)nj_page_bits);
        nj_adds3(e);                                /* r3 = &bits[index] */
        nj_ldrb(e, 2, 3, 0);                       /* r2 = byte bits */

        nj_mov_reg(e, 3, 0);
        nj_lsrs_imm(e, 3, 3, 12);                  /* page */
        nj_mov_imm(e, 1, 7u);
        nj_and_low(e, 3, 1);                       /* r3 = bit index */
        nj_mov_imm(e, 1, 1u);
        nj_lsl_reg(e, 1, 3);                       /* r1 = 1 << bit */
        nj_tst_low(e, 2, 1);
        nj_v6_guard_add(e, g, 1u);                  /* NE => code page */

        nj_mov_reg(e, 3, 0);                       /* restore paddr */
    }
    return !e->failed;
#endif
}

/*
 * Complete one guarded memory instruction.
 * Success skips the inline fail epilogue. Guard failure returns with
 * next_ip at this instruction and r0 = number of already-completed guest ops.
 */
static bool nj_v6_finish_guard(nj_emit_t *e, nj_v6_guard_t *g,
                               bool state16, uword instr_ip,
                               unsigned completed)
{
    u16 *skip_fail = nj_b_placeholder(e);
    u16 *fail = e->p;

    nj_mov_imm(e, NJ_ITER_REG, completed);
    nj_mov_imm(e, NJ_BUDGET_REG, instr_ip);
    nj_emit_common_exit(e, state16, NJ_FLAG_NONE, 0, instr_ip);

    u16 *cont = e->p;
    if (!nj_patch_b(skip_fail, cont)) return false;
    for (unsigned i = 0; i < g->n; ++i)
        if (!nj_patch_bcond(g->at[i], fail, g->cond[i]))
            return false;
    return !e->failed;
}

#define NJ_V8_MAX_EXIT_LINKS 64u
typedef struct {
    u16 *to_common[NJ_V8_MAX_EXIT_LINKS];
    unsigned n;
} nj_v8_exit_links_t;

static bool nj_v8_record_common_branch(nj_emit_t *e, nj_v8_exit_links_t *x)
{
    if (x->n >= NJ_V8_MAX_EXIT_LINKS) { e->failed=true; return false; }
    x->to_common[x->n++]=nj_b_placeholder(e);
    return !e->failed;
}

/* Tiny per-exit stub. r0=completed, lr=next_ip, r2=architectural IP;
 * all paths converge on one full guest-state epilogue appended to the block. */
static bool nj_v8_exit_stub_imm(nj_emit_t *e, nj_v8_exit_links_t *x,
                                unsigned completed, uword next_ip,
                                uword instr_ip)
{
    nj_mov_imm(e,NJ_ITER_REG,completed);
    nj_mov_imm(e,NJ_BUDGET_REG,next_ip);
    nj_mov_imm(e,2,instr_ip);
    return nj_v8_record_common_branch(e,x);
}

/* Same, but LR already contains a dynamic next_ip (RET). */
static bool nj_v8_exit_stub_lr(nj_emit_t *e, nj_v8_exit_links_t *x,
                               unsigned completed, uword instr_ip)
{
    nj_mov_imm(e,NJ_ITER_REG,completed);
    nj_mov_imm(e,2,instr_ip);
    return nj_v8_record_common_branch(e,x);
}

static void nj_v8_emit_shared_exit(nj_emit_t *e, bool code16)
{
    nj_str32(e,NJ_BUDGET_REG,NJ_CPU_REG,NJ_NEXT_IP_OFF);
    nj_str32(e,2,NJ_CPU_REG,NJ_IP_OFF);
    nj_mov_imm(e,1,0xffffffffu);
    nj_str32(e,1,NJ_CPU_REG,NJ_PREFETCH_OFF);
    nj_emit_store_guest(e,code16);
    nj_pop_guest(e);
}

static bool nj_v8_patch_exit_links(nj_v8_exit_links_t *x, u16 *common)
{
    for(unsigned i=0;i<x->n;++i)
        if(!nj_patch_b(x->to_common[i],common)) return false;
    return true;
}

/* Guard failure executes zero bytes of the current x86 instruction. The
 * success path skips a tiny fail stub instead of carrying a full epilogue for
 * every memory operation. */
static bool nj_v8_finish_guard(nj_emit_t *e, nj_v6_guard_t *g,
                               nj_v8_exit_links_t *x, uword instr_ip,
                               unsigned completed)
{
    u16 *skip_fail=nj_b_placeholder(e);
    u16 *fail=e->p;
    if(!nj_v8_exit_stub_imm(e,x,completed,instr_ip,instr_ip)) return false;
    u16 *cont=e->p;
    if(!nj_patch_b(skip_fail,cont)) return false;
    for(unsigned i=0;i<g->n;++i)
        if(!nj_patch_bcond(g->at[i],fail,g->cond[i])) return false;
    return !e->failed;
}

static void nj_v6_host_ptr_inline(CPUI386 *cpu, nj_emit_t *e)
{
    nj_mov_reg(e, 1, 3);
    nj_mov_imm(e, 2, (u32)(uintptr_t)cpu->phys_mem);
    nj_adds3(e); /* r3 = host pointer */
}

/* -------------------------------------------------------------------------
 * FRANK_NJIT_SHARED_GUARD
 *
 * One out-of-line copy of the memory guard, called instead of inlined.
 *
 * Every memory operand used to carry its own copy of the whole sequence -
 * the TLB walk, the physical range test, the VGA aperture test and the
 * code-page bitmap test - which is about 250 bytes of Thumb.  In an 8 KB
 * arena that is unaffordable: measured on this board, letting the loop
 * compiler emit them cost 23% on Doom purely through eviction, and the trace
 * compiler's blocks stayed so fat that DRACIHIS ran at 6.3% native coverage
 * with 3.5-instruction blocks.
 *
 * Out of line, a memory operand costs a push/bl/pop, a compare and a branch -
 * about 50 bytes including the address arithmetic and the access itself.
 *
 * The trampolines bake in phys_mem, phys_mem_size, the TLB table pointer, the
 * paging mode and the CPL, exactly as the inlined copies did.  The first three
 * are fixed after pc_new(); the last two are part of nj_mmu_key(), and a block
 * only ever executes while its own mmu_key matches the CPU's - so rebuilding
 * the trampolines whenever the key changes keeps every runnable block paired
 * with a trampoline built for its own key.
 *
 * They live in their own buffer rather than in the arena so that
 * nj_compact_code() cannot move them out from under a BL that is already
 * encoded, and nj_flush() cannot reclaim them.
 * ------------------------------------------------------------------------- */

/*
 * Sized from what the six trampolines actually emit, not an estimate: the
 * first guess of 768 halfwords overflowed, and because the patching below ran
 * before the overflow was checked, nj_patch_bcond() wrote one halfword past
 * the end - straight into nj_guard_entry, which follows it in .bss.  A
 * corrupted entry pointer made every block BL into nothing, so no block ever
 * returned and the board boot-looped with an empty exit ring.
 *
 * nj_guard_used records the real figure so it can be read back over SWD.
 */
/*
 * Several sets of trampolines, one per MMU key, each at its own address.
 *
 * Rebuilding a single set on every key change meant flushing the whole block
 * cache each time, because a block carries the absolute address of the set it
 * was compiled against.  Under EMM386 the guest crosses between real mode and
 * V86 constantly - 642 flushes in one DRACIHIS run - so the JIT never
 * accumulated anything.
 *
 * One set per key at a stable address keeps blocks valid across a mode switch
 * and flushes nothing.  A set is about 800 bytes, so four fit the buffer a
 * single oversized set used to have.  Only running out of slots costs a flush.
 */
/* A set measured ~950 bytes with paging on; 400 halfwords was not enough and
 * silently refused every build.  Three sets cover real mode, V86 and
 * protected mode, which is what a DOS guest actually cycles through. */
#define NJ_GUARD_SETS 3u
#define NJ_GUARD_SET_HW 560u
static u16 nj_guard_code[NJ_GUARD_SETS * NJ_GUARD_SET_HW];
static u16 *nj_guard_entry[NJ_GUARD_SETS][6];
static uword nj_guard_key[NJ_GUARD_SETS];
static bool nj_guard_set_live[NJ_GUARD_SETS];
static unsigned nj_guard_cur;
static unsigned nj_guard_next_victim;

/* read/write x size 1/2/4 */
static inline unsigned nj_guard_index(unsigned size, bool write)
{
    unsigned sz = size == 1u ? 0u : size == 2u ? 1u : 2u;
    return (write ? 3u : 0u) + sz;
}

static bool nj_guards_build(CPUI386 *cpu, unsigned slot)
{
    u16 *base = nj_guard_code + slot * NJ_GUARD_SET_HW;
    nj_emit_t e = {
        .p = base,
        .start = base,
        .limit = base + NJ_GUARD_SET_HW,
        .failed = false
    };

    nj_guard_set_live[slot] = false;
    for (unsigned i = 0; i < 6u; ++i) nj_guard_entry[slot][i] = NULL;

    static const unsigned sizes[3] = { 1u, 2u, 4u };
    for (unsigned w = 0; w < 2u; ++w) {
        for (unsigned k = 0; k < 3u; ++k) {
            unsigned size = sizes[k];
            bool write = (w != 0);
            u16 *entry = e.p;
            nj_v6_guard_t g;
            memset(&g, 0, sizeof(g));

            /*
             * r3 = guest linear address on entry, host pointer on return, or
             * zero when the interpreter has to do this one.  r0 carries the
             * loop compiler's live iteration count, so it is saved here rather
             * than at every call site.
             */
            nj_push_r0(&e);
            if (!nj_v7_emit_linear_to_phys_inline(cpu, &e, size, write, &g) ||
                !nj_v6_emit_mem_guard_inline(cpu, &e, size, write, &g))
                return false;
            nj_v6_host_ptr_inline(cpu, &e);
            nj_pop_r0(&e);
            nj_bx_lr(&e);

            u16 *fail = e.p;
            nj_pop_r0(&e);
            nj_mov_imm(&e, 3, 0);
            nj_bx_lr(&e);

            /* Order matters: a failed emit leaves g.at[] pointing at the
             * limit, so patching first would write out of bounds. */
            if (e.failed)
                return false;
            for (unsigned i = 0; i < g.n; ++i)
                if (!nj_patch_bcond(g.at[i], fail, g.cond[i]))
                    return false;

            nj_guard_entry[slot][nj_guard_index(size, write)] = entry;
        }
    }

    nj_guard_key[slot] = nj_mmu_key(cpu);
    nj_guard_set_live[slot] = true;
    nj_guard_cur = slot;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
    return true;
}

/*
 * Are the trampolines the ones this mode needs?
 *
 * This is a pure test.  Building them is only ever done from
 * nj_guards_refresh() at a safe point, because a rebuild has to flush the code
 * arena - the six are laid out end to end and their sizes depend on the mode,
 * so a rebuild moves them and every BL already encoded into a block becomes
 * wrong.  Doing that from inside nj_v6_emit_guard_call(), i.e. in the middle
 * of emitting a block, reset nj_code_ptr underneath the emitter and left the
 * half-built block registered in free space for the next compile to overwrite.
 */
static inline bool nj_guards_ok(CPUI386 *cpu)
{
    return nj_guard_set_live[nj_guard_cur] &&
           nj_guard_key[nj_guard_cur] == nj_mmu_key(cpu);
}

/* Safe point only: no block is being emitted and the arena may be flushed. */
/*
 * Returns void: a set that cannot be built is a reason to refuse memory
 * operands, not a reason to switch the JIT off.  Reporting failure up to
 * nj_try_execute() did exactly that and cost all of the JIT's coverage.
 */
static void nj_guards_refresh(CPUI386 *cpu)
{
    if (!NJIT_SHARED_GUARD)
        return;
    if (likely(nj_guards_ok(cpu)))
        return;

    uword key = nj_mmu_key(cpu);
    for (unsigned i = 0; i < NJ_GUARD_SETS; ++i) {
        if (nj_guard_set_live[i] && nj_guard_key[i] == key) {
            nj_guard_cur = i;      /* built earlier for this mode - no flush */
            return;
        }
    }
    for (unsigned i = 0; i < NJ_GUARD_SETS; ++i) {
        if (!nj_guard_set_live[i]) {
            nj_guards_build(cpu, i);
            return;
        }
    }
    /* Out of slots: reusing one changes what its address means, so the blocks
     * compiled against it have to go. */
    unsigned victim = nj_guard_next_victim++ % NJ_GUARD_SETS;
    if (nj_code_ptr != nj_code)
        nj_flush();
    nj_guards_build(cpu, victim);
}

/*
 * Drop-in replacement for the inlined
 *   nj_v7_emit_linear_to_phys() + nj_v6_emit_mem_guard() + nj_v6_host_ptr()
 * sequence.  r3 holds the linear address going in and the host pointer coming
 * out; a refusal is reported through the caller's existing guard list, so the
 * fail path and nj_v8_finish_guard() plumbing are unchanged.
 */
/*
 * OFF by default.  The out-of-line guard is the right shape - it takes a
 * memory operand from ~250 bytes of Thumb to ~50 - but the build that enables
 * it boot-loops, and bisection against an otherwise identical image pins the
 * fault on this code rather than on anything else in the tree.  It is kept
 * so the work is not lost, not because it is finished.
 */
static bool nj_v6_emit_guard_call(CPUI386 *cpu, nj_emit_t *e,
                                  unsigned size, bool write,
                                  nj_v6_guard_t *g)
{
    if (!NJIT_SHARED_GUARD) return false;
    if (size != 1u && size != 2u && size != 4u) return false;
    if (!nj_guards_ok(cpu)) return false;
    u16 *target = nj_guard_entry[nj_guard_cur][nj_guard_index(size, write)];
    if (!target) return false;

    nj_push_lr(e);
    nj_blx_abs(e, target, 2u);   /* r2 is scratch; the trampoline clobbers it */
    nj_pop_lr(e);
    nj_cmp_imm0(e, 3);
    nj_v6_guard_add(e, g, 0u);      /* EQ: the trampoline refused */
    return !e->failed;
}

/*
 * The seventeen call sites are unchanged: they still ask for a linear-to-
 * physical translation, then a guard, then a host pointer.  What they get now
 * is one call that does all three.  Keeping the names means the rewind and
 * nj_v8_finish_guard() plumbing at every site stays exactly as audited.
 */
static bool nj_v7_emit_linear_to_phys(CPUI386 *cpu, nj_emit_t *e,
                                      unsigned size, bool write,
                                      nj_v6_guard_t *g)
{
    return nj_v6_emit_guard_call(cpu, e, size, write, g);
}

static inline bool nj_v6_emit_mem_guard(CPUI386 *cpu, nj_emit_t *e,
                                        unsigned size, bool write,
                                        nj_v6_guard_t *g)
{
    (void)cpu; (void)e; (void)size; (void)write; (void)g;
    return true;   /* folded into the call above */
}

static inline void nj_v6_host_ptr(CPUI386 *cpu, nj_emit_t *e)
{
    (void)cpu; (void)e;   /* the trampoline returns a host pointer already */
}

static void nj_v6_read_r8(nj_emit_t *e, unsigned reg)
{
    unsigned g = reg & 3u;
    nj_mov_reg(e, 3, NJ_GUEST_REG(g));
    if (reg & 4u) nj_lsrs_imm(e, 3, 3, 8u);
    nj_uxtb(e, 3, 3);
}

static void nj_v6_write_r8(nj_emit_t *e, unsigned reg)
{
    unsigned g = reg & 3u;
    nj_uxtb(e, 3, 3);

    if (!(reg & 4u)) {
        nj_mov_reg(e, 1, NJ_GUEST_REG(g));
        nj_lsrs_imm(e, 1, 1, 8u);
        nj_lsls_imm(e, 1, 1, 8u);                  /* clear low byte */
        nj_orr_low(e, 1, 3);
        nj_mov_reg(e, NJ_GUEST_REG(g), 1);
    } else {
        nj_lsls_imm(e, 3, 3, 8u);                  /* new high byte */
        nj_mov_reg(e, 1, NJ_GUEST_REG(g));
        nj_uxtb(e, 2, 1);                          /* old low byte */
        nj_orr_low(e, 2, 3);
        nj_lsrs_imm(e, 1, 1, 16u);
        nj_lsls_imm(e, 1, 1, 16u);                 /* old upper 16 */
        nj_orr_low(e, 1, 2);
        nj_mov_reg(e, NJ_GUEST_REG(g), 1);
    }
}

static void nj_v6_write_r16(nj_emit_t *e, unsigned reg)
{
    nj_uxth(e, 3, 3);
    nj_mov_reg(e, 1, NJ_GUEST_REG(reg));
    nj_lsrs_imm(e, 1, 1, 16u);
    nj_lsls_imm(e, 1, 1, 16u);
    nj_orr_low(e, 1, 3);
    nj_mov_reg(e, NJ_GUEST_REG(reg), 1);
}

/* -------------------------------------------------------------------------
 * Guarded memory operands for the fully-native loop compiler.
 *
 * See the nj_loop_mem_t comment for why this exists.  The three pieces are
 * kept apart so the caller can put the actual load or store between them and
 * still get one shared guard-failure epilogue.
 * ------------------------------------------------------------------------- */

/*
 * Emit: save the iteration count, compute the effective address, translate it
 * through the live TLB, guard it, and leave a host pointer in r3.
 *
 * On a compile-time refusal the caller rewinds e->p, so the PUSH emitted here
 * is discarded with everything else.
 */
static bool nj_loop_mem_begin(CPUI386 *cpu, nj_emit_t *e,
                              const nj_v6_ea_t *ea, unsigned size, bool write,
                              nj_v6_guard_t *g)
{
    memset(g, 0, sizeof(*g));
    nj_push_r0(e);
    nj_v6_emit_ea(cpu, e, ea, true);
    if (!nj_v7_emit_linear_to_phys(cpu, e, size, write, g) ||
        !nj_v6_emit_mem_guard(cpu, e, size, write, g))
        return false;
    nj_v6_host_ptr(cpu, e);
    return !e->failed;
}

/*
 * Close the sequence.  The success path skips a small inline epilogue rather
 * than every memory instruction carrying a full one:
 *
 *      <access>
 *      b     cont
 *  fail:                     <- every guard branches here
 *      pop   {r0}            ; complete iterations, unchanged by the guards
 *      mov   lr, #instr_ip   ; next_ip: re-execute this instruction
 *      <common exit>
 *  cont:
 *      pop   {r0}
 *
 * Flags are passed as NJ_FLAG_NONE because the loop compiler materialises any
 * pending lazy CC before it lets a memory instruction clobber r1-r3.
 */
static bool nj_loop_mem_finish(nj_emit_t *e, nj_v6_guard_t *g,
                               const nj_loop_mem_t *mc)
{
    u16 *skip_fail = nj_b_placeholder(e);
    u16 *fail = e->p;

    nj_pop_r0(e);
    nj_mov_imm(e, NJ_BUDGET_REG, mc->instr_ip);
    nj_emit_common_exit(e, mc->state16, NJ_FLAG_NONE, 0, mc->instr_ip);

    u16 *cont = e->p;
    if (!nj_patch_b(skip_fail, cont)) return false;
    for (unsigned i = 0; i < g->n; ++i)
        if (!nj_patch_bcond(g->at[i], fail, g->cond[i]))
            return false;

    nj_pop_r0(e);
    return !e->failed;
}

/*
 * Decode a memory ModR/M, take the segment dependency, and open the guarded
 * sequence.  Returns 0 (caller refuses the instruction) when the operand is
 * not one this path can serve - an FS/GS override, a null data selector, or a
 * configuration in which nj_v6_emit_mem_guard() cannot prove a direct access
 * is safe.
 */
static int nj_body_mem_open(CPUI386 *cpu, nj_emit_t *e, nj_body_info_t *bi,
                            const nj_loop_mem_t *mc,
                            const u8 *p, unsigned max, u8 modrm,
                            bool addr16, int seg_override,
                            unsigned size, bool write,
                            nj_v6_guard_t *g, unsigned *used)
{
    nj_v6_ea_t ea;
    bool use_ds = false, use_es = false, use_ss = false;

    if (!mc) return 0;
    if (!nj_v6_decode_ea(p, max, modrm, addr16, seg_override, &ea)) return 0;
    if (!nj_v6_note_seg(cpu, ea.seg, &use_ds, &use_es, &use_ss)) return 0;
    if (!nj_loop_mem_begin(cpu, e, &ea, size, write, g)) return 0;

    if (use_ds) bi->uses_ds_static = true;
    if (use_es) bi->uses_es_static = true;
    if (use_ss) bi->uses_ss_base = true;
    bi->clobbers_flag_scratch = true;
    bi->used_memory = true;
    *used = ea.used;
    return 1;
}

/* Immediate SHLD/SHRD, register destination, 32-bit operand size. */
static bool nj_v6_emit_shx32(nj_emit_t *e, unsigned dst, unsigned src,
                             unsigned count, bool right)
{
    count &= 31u;
    if (!count) return true;

    /* Preserve both original operands; src may alias dst. */
    nj_mov_reg(e, 1, NJ_GUEST_REG(dst));
    nj_str32(e, 1, NJ_CPU_REG, NJ_CC_SRC1);
    nj_mov_reg(e, 2, NJ_GUEST_REG(src));
    nj_str32(e, 2, NJ_CPU_REG, NJ_CC_SRC2);

    if (!right) {
        nj_mov_reg(e, 1, NJ_GUEST_REG(dst));
        nj_lsls_imm(e, 1, 1, count);
        nj_mov_reg(e, 2, NJ_GUEST_REG(src));
        nj_lsrs_imm(e, 2, 2, 32u - count);
        nj_orr_low(e, 1, 2);
    } else {
        nj_mov_reg(e, 1, NJ_GUEST_REG(dst));
        nj_lsrs_imm(e, 1, 1, count);
        nj_mov_reg(e, 2, NJ_GUEST_REG(src));
        nj_lsls_imm(e, 2, 2, 32u - count);
        nj_orr_low(e, 1, 2);
    }

    nj_mov_reg(e, 3, 1);
    nj_mov_reg(e, NJ_GUEST_REG(dst), 3);
    nj_str32(e, 3, NJ_CPU_REG, NJ_CC_DST);

    /* Compute dst2 exactly like the interpreter's SHLD/SHRD helper. */
    nj_ldr32(e, 1, NJ_CPU_REG, NJ_CC_SRC1);
    if (count == 1u) {
        nj_mov_reg(e, 3, 1);
    } else if (!right) {
        nj_lsls_imm(e, 1, 1, count - 1u);
        nj_ldr32(e, 2, NJ_CPU_REG, NJ_CC_SRC2);
        nj_lsrs_imm(e, 2, 2, 32u - (count - 1u));
        nj_orr_low(e, 1, 2);
        nj_mov_reg(e, 3, 1);
    } else {
        nj_lsrs_imm(e, 1, 1, count - 1u);
        nj_ldr32(e, 2, NJ_CPU_REG, NJ_CC_SRC2);
        nj_lsls_imm(e, 2, 2, 32u - (count - 1u));
        nj_orr_low(e, 1, 2);
        nj_mov_reg(e, 3, 1);
    }
    nj_str32(e, 3, NJ_CPU_REG, NJ_CC_DST2);

    nj_mov_imm(e, 1, right ? CC_SHRD : CC_SHLD);
    nj_str32(e, 1, NJ_CPU_REG, NJ_CC_OP);
    nj_mov_imm(e, 1, CF | PF | ZF | SF | OF);
    nj_str32(e, 1, NJ_CPU_REG, NJ_CC_MASK);
    return !e->failed;
}

/* Full x86 condition evaluator for the less common Jcc conditions.
 * Z/NZ stay inline below; other conditions use this small helper rather than
 * terminating the supertrace. Guest GPRs live in callee-saved r4-r11, and the
 * generated call preserves r12 (CPU pointer) explicitly. */
static int IRAM_ATTR nj_v8_eval_jcc(CPUI386 *cpu, unsigned cc)
{
    switch (cc & 15u) {
    case 0x0: return get_OF(cpu);
    case 0x1: return !get_OF(cpu);
    case 0x2: return get_CF(cpu);
    case 0x3: return !get_CF(cpu);
    case 0x4: return get_ZF(cpu);
    case 0x5: return !get_ZF(cpu);
    case 0x6: return get_CF(cpu) || get_ZF(cpu);
    case 0x7: return !get_CF(cpu) && !get_ZF(cpu);
    case 0x8: return get_SF(cpu);
    case 0x9: return !get_SF(cpu);
    case 0xa: return get_PF(cpu);
    case 0xb: return !get_PF(cpu);
    case 0xc: return get_SF(cpu) != get_OF(cpu);
    case 0xd: return get_SF(cpu) == get_OF(cpu);
    case 0xe: return get_ZF(cpu) || (get_SF(cpu) != get_OF(cpu));
    default:  return !get_ZF(cpu) && (get_SF(cpu) == get_OF(cpu));
    }
}

static bool nj_v8_emit_eval_jcc(nj_emit_t *e, unsigned cc)
{
    /* Entry SP is 8-byte aligned; prologue saved 9 regs (36 bytes), so one
     * low-register push both saves r12 and restores ABI alignment for BLX. */
    nj_mov_reg(e,0,NJ_CPU_REG);
    nj_push_low(e,1u << 0);
    nj_mov_imm(e,1,cc & 15u);
    nj_mov_imm(e,3,(u32)(uintptr_t)&nj_v8_eval_jcc);
    nj_blx_reg(e,3);
    nj_pop_low(e,1u << 3);
    nj_mov_reg(e,NJ_CPU_REG,3);
    return !e->failed;
}

static bool nj_v8_emit_generic_jcc_side_exit(nj_emit_t *e, nj_v8_exit_links_t *x,
                                              unsigned cc, uword target,
                                              uword instr_ip, unsigned completed)
{
    if (!nj_v8_emit_eval_jcc(e,cc)) return false;
    nj_cmp_imm0(e,0);
    u16 *taken_b=nj_bcond_placeholder(e); /* NE => condition true */
    u16 *cont_b=nj_b_placeholder(e);
    u16 *taken=e->p;
    if(!nj_v8_exit_stub_imm(e,x,completed,target,instr_ip)) return false;
    u16 *cont=e->p;
    return nj_patch_bcond(taken_b,taken,1u) && nj_patch_b(cont_b,cont) && !e->failed;
}

/* V8 JZ/JNZ side exit. The fall-through path stays in the same generated
 * trace. Taken control flow returns architecturally correct state to the C
 * chain dispatcher, which can immediately enter/compile the target block. */
static bool nj_v8_emit_jz_side_exit(nj_emit_t *e, nj_v8_exit_links_t *x, bool is_jz,
                                    uword target, uword instr_ip,
                                    unsigned completed)
{
    nj_ldr32(e, 1, NJ_CPU_REG, NJ_CC_MASK);
    nj_mov_imm(e, 2, ZF);
    nj_tst_low(e, 1, 2);
    u16 *to_flags = nj_bcond_placeholder(e);      /* EQ: ZF is materialized */

    nj_ldr32(e, 1, NJ_CPU_REG, NJ_CC_DST);
    nj_cmp_imm0(e, 1);
    u16 *lazy_taken = nj_bcond_placeholder(e);   /* JZ EQ / JNZ NE */
    u16 *lazy_cont = nj_b_placeholder(e);

    u16 *flags_path = e->p;
    nj_ldr32(e, 1, NJ_CPU_REG, NJ_FLAGS_OFF);
    nj_tst_low(e, 1, 2);
    u16 *flags_cont = nj_bcond_placeholder(e);    /* JZ: EQ cont; JNZ: NE cont */

    u16 *taken = e->p;
    if(!nj_v8_exit_stub_imm(e,x,completed,target,instr_ip)) return false;
    u16 *cont = e->p;

    if (!nj_patch_bcond(to_flags, flags_path, 0u) ||
        !nj_patch_bcond(lazy_taken, taken, is_jz ? 0u : 1u) ||
        !nj_patch_b(lazy_cont, cont) ||
        !nj_patch_bcond(flags_cont, cont, is_jz ? 0u : 1u))
        return false;
    return !e->failed;
}

/* LOOP rel8 side exit. ECX is already decremented in the generated code.
 * Taken branch exits to the chain dispatcher; fall-through remains in trace. */
static bool nj_v8_emit_loop_side_exit(nj_emit_t *e, nj_v8_exit_links_t *x,
                                      uword target, uword instr_ip, unsigned completed)
{
    nj_cmp_imm0(e, NJ_GUEST_REG(1));
    u16 *taken_b = nj_bcond_placeholder(e);       /* NE */
    u16 *cont_b = nj_b_placeholder(e);
    u16 *taken = e->p;
    if(!nj_v8_exit_stub_imm(e,x,completed,target,instr_ip)) return false;
    u16 *cont = e->p;
    if (!nj_patch_bcond(taken_b, taken, 1u) || !nj_patch_b(cont_b, cont))
        return false;
    return !e->failed;
}

/* Register C1/D1 SHL/SHR/SAR. Mirrors the interpreter lazy-flag formulas. */
static bool nj_v8_emit_shift_imm32(nj_emit_t *e, unsigned dst,
                                   unsigned subop, unsigned count)
{
    count &= 31u;
    if (!count) return true;
    if (subop != 4u && subop != 5u && subop != 7u) return false;

    nj_mov_reg(e, 1, NJ_GUEST_REG(dst));          /* original x */
    if (subop == 5u) nj_str32(e, 1, NJ_CPU_REG, NJ_CC_SRC1);

    if (subop == 4u) nj_lsls_imm(e, 3, 1, count);
    else if (subop == 5u) nj_lsrs_imm(e, 3, 1, count);
    else nj_asrs_imm(e, 3, 1, count);
    nj_mov_reg(e, NJ_GUEST_REG(dst), 3);
    nj_str32(e, 3, NJ_CPU_REG, NJ_CC_DST);

    /* Carry is the last bit shifted out. */
    nj_mov_reg(e, 2, 1);
    if (subop == 4u) {
        nj_lsrs_imm(e, 2, 2, 32u - count);
    } else if (count > 1u) {
        nj_lsrs_imm(e, 2, 2, count - 1u);
    } /* count==1: carry is original bit0, so no shift */
    nj_mov_imm(e, 3, 1u);
    nj_and_low(e, 2, 3);
    nj_str32(e, 2, NJ_CPU_REG, NJ_CC_DST2);

    nj_mov_imm(e, 1, subop == 4u ? CC_SHL : (subop == 5u ? CC_SHR : CC_SAR));
    nj_str32(e, 1, NJ_CPU_REG, NJ_CC_OP);
    nj_mov_imm(e, 1, CF | PF | ZF | SF | OF);
    nj_str32(e, 1, NJ_CPU_REG, NJ_CC_MASK);
    return !e->failed;
}

/* Initial JZ/JNZ block retained for the old path; V8 supertraces no longer
 * select it because they can carry JZ/JNZ inline. */
static nj_block_t *nj_v6_compile_jz_entry(CPUI386 *cpu, uword start_ip,
                                          const u8 *code, unsigned avail,
                                          unsigned phys_page)
{
    nj_v6_pfx_t px;
    if (!nj_v6_prefix(cpu, code, avail, &px)) return NULL;
    if (px.pos >= avail) return NULL;
    u8 op = code[px.pos];
    bool is_jz;
    unsigned jlen;
    sword d;

    if (op == 0x74 || op == 0x75) {
        if (avail < px.pos + 2u) return NULL;
        is_jz = op == 0x74;
        jlen = 2u;
        d = (s8)code[px.pos + 1u];
    } else if (op == 0x0f && avail >= px.pos + 2u &&
               (code[px.pos + 1u] == 0x84 || code[px.pos + 1u] == 0x85)) {
        is_jz = code[px.pos + 1u] == 0x84;
        unsigned dw = px.op16 ? 2u : 4u;
        if (avail < px.pos + 2u + dw) return NULL;
        jlen = 2u + dw;
        d = dw == 2u ? (s16)nj_rd16(code + px.pos + 2u)
                     : (s32)nj_rd32(code + px.pos + 2u);
    } else {
        return NULL;
    }

    uword fall = start_ip + px.pos + jlen;
    uword target = fall + d;

    if (!nj_make_code_room(NJ_V6_MAX_ARM_BYTES))
        return NULL;

    nj_emit_t e = {
        .p = nj_code_ptr, .start = nj_code_ptr,
        .limit = nj_code_ptr + (NJ_V6_MAX_ARM_BYTES / 2u), .failed = false
    };
    nj_emit_prologue(&e, false);

    nj_ldr32(&e, 1, NJ_CPU_REG, NJ_FLAGS_OFF);
    nj_mov_imm(&e, 2, ZF);
    nj_tst_low(&e, 1, 2);
    u16 *to_taken = nj_bcond_placeholder(&e);

    nj_mov_imm(&e, NJ_ITER_REG, 1u);
    nj_mov_imm(&e, NJ_BUDGET_REG, fall);
    u16 *fall_to_exit = nj_b_placeholder(&e);

    u16 *taken = e.p;
    nj_mov_imm(&e, NJ_ITER_REG, 1u);
    nj_mov_imm(&e, NJ_BUDGET_REG, target);

    u16 *exit = e.p;
    nj_emit_common_exit(&e, false, NJ_FLAG_NONE, 0, start_ip);

    unsigned cond = is_jz ? 1u : 0u; /* JZ: bit set => TST NE */
    if (!nj_patch_bcond(to_taken, taken, cond) ||
        !nj_patch_b(fall_to_exit, exit) || e.failed)
        return NULL;

    uword linear = cpu->seg[SEG_CS].base + start_ip;
    nj_block_t *b = nj_cache_insert_slot(cpu, linear);
    memset(b, 0, sizeof(*b));
    b->tag = linear;
    b->mmu_key = nj_mmu_key(cpu);
    b->cs_base = cpu->seg[SEG_CS].base;
    b->ds_base = cpu->seg[SEG_DS].base;
    b->es_base = cpu->seg[SEG_ES].base;
    b->ss_base = cpu->seg[SEG_SS].base;
    b->ds_sel = cpu->seg[SEG_DS].sel;
    b->es_sel = cpu->seg[SEG_ES].sel;
    b->ss_sel = cpu->seg[SEG_SS].sel;
    b->start_ip = start_ip;
    b->branch_ip = start_ip;
    b->fallthrough_ip = fall;
    b->byte_len = (u16)(px.pos + jlen);
    b->phys_page = (u16)phys_page;
    b->insns = 1u;
    b->code16 = 0u;
    b->valid = 1u;
    b->needs_flags_in = 1u;
    b->single_run = 1u;
    b->code = e.start;
    b->arm_halfwords = (u16)(e.p - e.start);

    nj_code_ptr = e.p;
    nj_page_mark(phys_page);
    nj_bloom_add(linear);
    g_njit_compiles++;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
    return b;
}

/* -------------------------------------------------------------------------
 * FRANK_NATIVE_JIT_V87_BYTEWALK_LOOP
 *
 * Generic page-bounded native compiler for a common DOS/V86 micro-loop shape:
 *
 *   INC/DEC reg ; byte memory op ; JZ/JNZ back to the INC/DEC
 *
 * Two byte-memory operations are currently admitted:
 *   C6 /0  MOV r/m8,imm8   (branch observes INC/DEC flags)
 *   80 /7  CMP r/m8,imm8   (branch observes CMP flags)
 *
 * The effective address must change by exactly one byte per iteration because
 * the modified register occurs exactly once in the EA at scale 1.  Entry code
 * executes the register update only in the local ARM register file, validates
 * the first linear page/TLB mapping and caps the native iteration budget at the
 * 16-bit-address and 4-KiB page boundary.  A failed entry guard returns r0=0
 * without storing the locally modified guest register, so the interpreter sees
 * the exact pre-instruction architectural state.
 *
 * After a successful entry, all iterations remain inside the validated page;
 * there is no TLB lookup or C side exit in the native loop body.
 * ------------------------------------------------------------------------- */

static bool nj_bw_cap_budget(nj_emit_t *e, unsigned safe_reg)
{
    nj_cmp_reg(e, NJ_BUDGET_REG, safe_reg);
    u16 *keep = nj_bcond_placeholder(e); /* LS: existing budget <= safe */
    nj_mov_reg(e, NJ_BUDGET_REG, safe_reg);
    return nj_patch_bcond(keep, e->p, 9u) && !e->failed;
}

static bool nj_bw_finish_entry_guard(nj_emit_t *e, nj_v6_guard_t *g)
{
    u16 *skip_fail = nj_b_placeholder(e);
    u16 *fail = e->p;
    /* Paging/physical guards use r0 as scratch. The loop ABI requires a
     * guard failure to report exactly zero completed iterations. */
    nj_mov_imm(e, NJ_ITER_REG, 0);
    nj_pop_guest(e);
    u16 *cont = e->p;
    if (!nj_patch_b(skip_fail, cont)) return false;
    for (unsigned i = 0; i < g->n; ++i)
        if (!nj_patch_bcond(g->at[i], fail, g->cond[i]))
            return false;
    return !e->failed;
}

static bool nj_bw_prefix1_ok(const u8 *p, const nj_v6_pfx_t *px)
{
    /* For the register INC/DEC instruction only operand-size override is
     * useful here.  Keeping this strict avoids treating ignored prefixes as
     * part of a new optimization class before they are measured. */
    if (px->pos > 1u) return false;
    return px->pos == 0u || p[0] == 0x66u;
}

static bool nj_bw_ea_unit_delta(const nj_v6_ea_t *ea, unsigned reg)
{
    unsigned n = 0;
    if (ea->base == (int)reg) n++;
    if (ea->index == (int)reg) {
        if (ea->scale != 0u) return false;
        n++;
    }
    return n == 1u;
}

static void nj_bw_modify_reg(nj_emit_t *e, unsigned reg, bool dec,
                             unsigned width)
{
    unsigned ar = NJ_GUEST_REG(reg);
    nj_addsub_imm_w(e, ar, 1u, dec, true);
    if (width == 2u) {
        /* UXTH's compact encoding addresses only r0-r7. Guest DI/BP/SI/SP
         * live in r11/r9/r10/r8, so wrap through a low scratch register. */
        nj_mov_reg(e, 3, ar);
        nj_uxth(e, 3, 3);
        nj_mov_reg(e, ar, 3);
    }
}

/* r3 contains an effective offset before segment-base addition.  Cap LR so
 * repeated +/-1 accesses cannot cross the 16-bit effective-address wrap. */
static bool nj_bw_cap_addr16(nj_emit_t *e, int dir)
{
    nj_mov_reg(e, NJ_ITER_REG, 3); /* temporary save; r0 is architecturally 0 */
    if (dir > 0) {
        nj_mov_imm(e, 1, 0x10000u);
        nj_mov_reg(e, 2, 3);
        nj_subs3(e);               /* r3 = 0x10000 - first_offset */
        if (!nj_bw_cap_budget(e, 3)) return false;
    } else {
        nj_mov_reg(e, 1, 3);
        nj_adds_imm8(e, 1, 1u);    /* first_offset + 1 */
        if (!nj_bw_cap_budget(e, 1)) return false;
    }
    nj_mov_reg(e, 3, NJ_ITER_REG);
    nj_mov_imm(e, NJ_ITER_REG, 0);
    return !e->failed;
}

/* r3 contains the first guest linear byte address.  Cap LR to this 4-KiB
 * linear page so one TLB validation covers every native iteration. */
static bool nj_bw_cap_page(nj_emit_t *e, int dir)
{
    nj_mov_reg(e, NJ_ITER_REG, 3); /* save linear */
    if (dir > 0) {
        nj_mov_reg(e, 2, 3);
        nj_lsls_imm(e, 2, 2, 20u);
        nj_lsrs_imm(e, 2, 2, 20u); /* r2 = page offset */
        nj_mov_imm(e, 1, 0x1000u);
        nj_subs3(e);               /* r3 = 0x1000 - offset */
        if (!nj_bw_cap_budget(e, 3)) return false;
    } else {
        nj_mov_reg(e, 1, 3);
        nj_lsls_imm(e, 1, 1, 20u);
        nj_lsrs_imm(e, 1, 1, 20u);
        nj_adds_imm8(e, 1, 1u);    /* offset + 1 */
        if (!nj_bw_cap_budget(e, 1)) return false;
    }
    nj_mov_reg(e, 3, NJ_ITER_REG);
    nj_mov_imm(e, NJ_ITER_REG, 0);
    return !e->failed;
}

static nj_block_t *nj_compile_bytewalk_loop(CPUI386 *cpu, uword start_ip)
{
    /* v8.7.1: this optimization exists solely for measured EMM386 VM86
     * hotspots. Never let it change BIOS/DOS real-mode startup behavior. */
    if (!cpu->code16 || !(cpu->flags & VM) || !(cpu->cr0 & CR0_PG))
        return NULL;
    /* The broad dynamic-memory helper leaves non-paged addresses linear.
     * Decline the rare A20-masked case rather than silently bypass the gate. */
    if (!(cpu->cr0 & CR0_PG) && cpu->a20_mask != 0xffffffffu) return NULL;

    const u8 *code;
    unsigned avail, phys_page, phys_page2, code_split;
    if (!nj_v8_code_window(cpu, start_ip, &code, &avail,
                           &phys_page, &phys_page2, &code_split) || !avail)
        return NULL;

    nj_v6_pfx_t p1;
    if (!nj_v6_prefix(cpu, code, avail, &p1) || !nj_bw_prefix1_ok(code, &p1) ||
        p1.pos >= avail)
        return NULL;

    u8 op1 = code[p1.pos];
    if (op1 < 0x40u || op1 > 0x4fu) return NULL;
    bool dec = op1 >= 0x48u;
    unsigned modreg = op1 & 7u;
    unsigned mod_width = p1.op16 ? 2u : 4u;
    unsigned pos = p1.pos + 1u;

    if (pos >= avail) return NULL;
    nj_v6_pfx_t p2;
    if (!nj_v6_prefix(cpu, code + pos, avail - pos, &p2) ||
        pos + p2.pos >= avail)
        return NULL;

    u8 op2 = code[pos + p2.pos];
    bool mem_store = op2 == 0xc6u;
    bool mem_cmp = op2 == 0x80u;
    if (!mem_store && !mem_cmp) return NULL;

    unsigned mpos = pos + p2.pos + 1u;
    if (mpos >= avail) return NULL;
    u8 m = code[mpos++];
    unsigned subop = (m >> 3) & 7u;
    if ((mem_store && subop != 0u) || (mem_cmp && subop != 7u) ||
        (m >> 6) == 3u)
        return NULL;

    nj_v6_ea_t ea;
    if (!nj_v6_decode_ea(code + mpos, avail - mpos, m,
                         p2.addr16, p2.seg, &ea))
        return NULL;
    mpos += ea.used;
    if (mpos >= avail) return NULL;
    u8 imm = code[mpos++];

    /* Keep the persistent register representation simple and exact: a 16-bit
     * modifier is paired with 16-bit addressing; a 32-bit modifier with
     * 32-bit addressing.  Mixed combinations remain with the broad JIT. */
    if ((mod_width == 2u) != ea.addr16) return NULL;
    if (!nj_bw_ea_unit_delta(&ea, modreg)) return NULL;

    uword branch_ip = start_ip + mpos;
    branch_ip &= 0xffffu;
    nj_branch_t br;
    if (!nj_decode_backedge(cpu, code + mpos, avail - mpos,
                            branch_ip, start_ip, &br) ||
        (br.type != NJ_BRANCH_JZ && br.type != NJ_BRANCH_JNZ))
        return NULL;

    bool use_ds = false, use_es = false, use_ss = false;
    if (!nj_v6_note_seg(cpu, ea.seg, &use_ds, &use_es, &use_ss))
        return NULL;

    if (!nj_make_code_room(NJ_V6_MAX_ARM_BYTES)) return NULL;
    nj_emit_t e = {
        .p = nj_code_ptr,
        .start = nj_code_ptr,
        .limit = nj_code_ptr + (NJ_V6_MAX_ARM_BYTES / 2u),
        .failed = false
    };

    bool state16 = mod_width == 2u;
    int dir = dec ? -1 : 1;
    nj_emit_prologue(&e, state16);

    /* Execute the first INC/DEC speculatively in local ARM registers.  If an
     * entry memory guard fails, the return-zero stub below never stores it. */
    nj_bw_modify_reg(&e, modreg, dec, mod_width);

    /* First effective offset after the architectural INC/DEC. */
    nj_v6_emit_ea(cpu, &e, &ea, false);
    if (ea.addr16 && !nj_bw_cap_addr16(&e, dir)) return NULL;

    /* Add the statically guarded segment base. */
    uword sb = cpu->seg[ea.seg].base;
    if (sb) {
        nj_mov_reg(&e, 1, 3);
        nj_mov_imm(&e, 2, sb);
        nj_adds3(&e);
        /* nj_adds3 already leaves the linear result in r3. */
    }
    if (!nj_bw_cap_page(&e, dir)) return NULL;

    nj_v6_guard_t g = {0};
    if (!nj_v7_emit_linear_to_phys(cpu, &e, 1u, mem_store, &g) ||
        !nj_v6_emit_mem_guard(cpu, &e, 1u, mem_store, &g))
        return NULL;
    nj_v6_host_ptr(cpu, &e);       /* r3 = host pointer to first byte */
    if (!nj_bw_finish_entry_guard(&e, &g)) return NULL;
    /* Translation/guard helpers deliberately borrow r0; start the native
     * loop's completed-iteration counter from its ABI-defined zero. */
    nj_mov_imm(&e, NJ_ITER_REG, 0);

    /* Persistent loop temporaries after all entry guards:
     *   r1 = immediate byte, r2 = current host byte pointer. */
    nj_mov_reg(&e, 2, 3);
    nj_mov_imm(&e, 1, imm);

    u16 *body = e.p;
    if (mem_store) {
        /* Guards and the 16-bit wrap helper may clobber APSR. JZ/JNZ only
         * needs INC/DEC's Z here, so derive it from the committed result on
         * every iteration immediately before the flag-neutral store. */
        nj_cmp_imm0(&e, NJ_GUEST_REG(modreg));
        nj_strb(&e, 1, 2, 0);
    } else {
        nj_ldrb(&e, 3, 2, 0);
        nj_cmp_reg(&e, 3, 1);       /* CMP byte,imm8 */
    }

    /* Condition false leaves the native micro-loop. */
    u16 *to_normal = nj_bcond_placeholder(&e);

    /* Taken backedge: retire one complete three-instruction iteration. */
    nj_adds_imm8(&e, NJ_ITER_REG, 1u);
    nj_cmp_reg(&e, NJ_ITER_REG, NJ_BUDGET_REG);
    u16 *to_budget = nj_bcond_placeholder(&e); /* HS */

    /* The entry budget guarantees this pointer/register step remains in the
     * already validated 4-KiB page and (for addr16) before effective wrap. */
    if (dir > 0) nj_adds_imm8(&e, 2, 1u);
    else         nj_subs_imm8(&e, 2, 1u);
    nj_bw_modify_reg(&e, modreg, dec, mod_width); /* also restores INC/DEC Z */
    u16 *back = nj_b_placeholder(&e);

    u16 *normal_exit = e.p;
    nj_adds_imm8(&e, NJ_ITER_REG, 1u);
    nj_mov_imm(&e, NJ_BUDGET_REG, br.fallthrough);
    u16 *normal_to_common = nj_b_placeholder(&e);

    u16 *budget_exit = e.p;
    nj_mov_imm(&e, NJ_BUDGET_REG, start_ip);
    u16 *common_exit = e.p;

    if (mem_store) {
        nj_mov_reg(&e, 3, NJ_GUEST_REG(modreg));
        nj_v8_emit_cc_flush_width(&e, mod_width, NJ_FLAG_INCDEC,
                                  dec ? (mod_width == 2u ? CC_DEC16 : CC_DEC32)
                                      : (mod_width == 2u ? CC_INC16 : CC_INC32));
    } else {
        /* Reconstruct exact byte-CMP lazy operands from the final tested byte. */
        nj_ldrb(&e, 3, 2, 0);
        nj_mov_reg(&e, 1, 3);
        nj_mov_imm(&e, 2, imm);
        nj_subs3(&e);                /* r3 = src1 - src2 */
        nj_v8_emit_cc_flush_width(&e, 1u, NJ_FLAG_ARITH, CC_SUB);
    }
    nj_str32(&e, NJ_BUDGET_REG, NJ_CPU_REG, NJ_NEXT_IP_OFF);
    nj_mov_imm(&e, 1, branch_ip);
    nj_str32(&e, 1, NJ_CPU_REG, NJ_IP_OFF);
    nj_mov_imm(&e, 1, 0xffffffffu);
    nj_str32(&e, 1, NJ_CPU_REG, NJ_PREFETCH_OFF);
    nj_emit_store_guest(&e, state16);
    nj_pop_guest(&e);

    unsigned invcond = br.type == NJ_BRANCH_JZ ? 1u : 0u; /* NE / EQ */
    if (!nj_patch_bcond(to_normal, normal_exit, invcond) ||
        !nj_patch_bcond(to_budget, budget_exit, 2u) ||     /* HS */
        !nj_patch_b(back, body) ||
        !nj_patch_b(normal_to_common, common_exit) || e.failed)
        return NULL;

    unsigned byte_len = mpos + br.len;
    uword linear = cpu->seg[SEG_CS].base + start_ip;
    nj_block_t *b = nj_cache_insert_slot(cpu, linear);
    memset(b, 0, sizeof(*b));
    b->tag = linear;
    b->mmu_key = nj_mmu_key(cpu);
    b->cs_base = cpu->seg[SEG_CS].base;
    b->ds_base = cpu->seg[SEG_DS].base;
    b->es_base = cpu->seg[SEG_ES].base;
    b->ss_base = cpu->seg[SEG_SS].base;
    b->ds_sel = cpu->seg[SEG_DS].sel;
    b->es_sel = cpu->seg[SEG_ES].sel;
    b->ss_sel = cpu->seg[SEG_SS].sel;
    b->start_ip = start_ip;
    b->branch_ip = branch_ip;
    b->fallthrough_ip = br.fallthrough;
    b->byte_len = (u16)byte_len;
    b->phys_page = (u16)phys_page;
    b->code_split = (u16)((code_split && byte_len > code_split) ? code_split : 0u);
    b->phys_page2 = b->code_split ? (u16)phys_page2 : 0u;
    b->insns = 3u;
    b->code16 = 1u;
    b->needs_refresh_cf = mem_store ? 1u : 0u;
    b->needs_flags_in = 0u;
    b->single_run = 0u;
    b->uses_ds_static = (u8)use_ds;
    b->uses_es_static = (u8)use_es;
    b->uses_ss_base = (u8)use_ss;
    b->uses_ss_static = 0u;
    b->static_write_count = 0u;
    b->code = e.start;
    b->arm_halfwords = (u16)(e.p - e.start);
    b->valid = 1u;

    nj_code_ptr = e.p;
    nj_page_mark(phys_page);
    if (b->code_split) nj_page_mark(b->phys_page2);
    nj_bloom_add(linear);
    g_njit_compiles++;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
    return b;
}


static nj_block_t *nj_compile_v6_trace(CPUI386 *cpu, uword start_ip)
{
    /*
     * V8.2 extends the broad prefix/supertrace fallback to ordinary 16-bit
     * real-mode/VM86 code.  The old v4/v5 full-loop compiler remains first
     * choice, but M602's Symantec launch path contains nested Jcc/string and
     * memory operations that cannot satisfy the old all-or-nothing loop shape.
     *
     * Keep 16-bit protected-mode descriptors with arbitrary limits in the
     * interpreter for now; real mode and VM86 are the measured target.
     */
    bool trace16 = cpu->code16;
    bool mixed_v86 = trace16 && (cpu->flags & VM) && (cpu->cr0 & CR0_PG);
    if (trace16 && (cpu->cr0 & 1u) && !(cpu->flags & VM))
        return NULL;

    const u8 *code;
    unsigned avail, phys_page, phys_page2, code_split;
    if (!nj_v8_code_window(cpu, start_ip, &code, &avail,
                           &phys_page, &phys_page2, &code_split))
        return NULL;
    if (!avail) return NULL;
    if (avail > NJ_V6_MAX_BYTES) avail = NJ_V6_MAX_BYTES;

    static const u8 timer_poll[] = {
        0x26,0x8b,0x16,0x6c,0x04,0x3b,0xc2,0x74,0xf7
    };
    if (avail >= sizeof(timer_poll) &&
        memcmp(code, timer_poll, sizeof(timer_poll)) == 0)
        return NULL;

    /* V8 keeps JZ/JNZ inside the same supertrace instead of emitting the
     * old one-instruction branch-entry block. */

    if (!nj_make_code_room(NJ_V6_MAX_ARM_BYTES))
        return NULL;

    u16 *hard_limit=nj_code_ptr + (NJ_V6_MAX_ARM_BYTES / 2u);
    nj_emit_t e = {
        .p = nj_code_ptr, .start = nj_code_ptr,
        .limit = hard_limit - (96u / 2u), .failed = false
    };
    nj_v8_exit_links_t exits; memset(&exits,0,sizeof(exits));
    nj_emit_prologue(&e, mixed_v86 ? false : trace16);

    unsigned pos = 0;
    unsigned insns = 0;
    uword last_ip = start_ip;
    bool needs_refresh_cf = false;
    bool use_ds = false, use_es = false, use_ss = false;
    bool use_df = false;
    u8 df_value = 0;
    bool cf_dirty = false; /* lazy CF exists in cpu->cc, not cpu->flags */
    bool terminal_exit = false;
    uword continuation_ip = 0;

    u8 nj_stop_op = 0;
    while (insns < NJ_V6_MAX_INSNS && pos < avail) {
        unsigned ipos = pos;
        u16 *emit_before = e.p;
        unsigned exits_before = exits.n;
        uword gip = start_ip + pos;
        if (trace16) gip &= 0xffffu;
        nj_v6_pfx_t px;
        if (!nj_v6_prefix(cpu, code + pos, avail - pos, &px))
            break;
        if (px.pos >= avail - pos) break;

        /* Startup-safe split: outside EMM386 VM86+paging use the exact v8.6
         * 16-bit broad-JIT envelope.  66/67 remain interpreter-owned there. */
        if (trace16 && !mixed_v86 && (!px.op16 || !px.addr16))
            break;

        unsigned op_pos = pos + px.pos;
        nj_stop_op = code[op_pos];
        u8 op = code[op_pos];

        /* ---- All Jcc stay inside the supertrace. Z/NZ use a tiny inline
         * lazy-Z evaluator; the other 14 conditions call the exact existing
         * flag helpers and side-exit only when the branch is taken. ---- */
        unsigned jcc_cc = 0xffffffffu;
        unsigned jcc_len = 0;
        sword jcc_disp = 0;
        if (op >= 0x70 && op <= 0x7f) {
            if (op_pos + 2u > avail) break;
            jcc_cc = op & 15u; jcc_len = 2u;
            jcc_disp = (s8)code[op_pos + 1u];
        } else if (op == 0x0f && op_pos + 4u <= avail &&
                   code[op_pos + 1u] >= 0x80 && code[op_pos + 1u] <= 0x8f) {
            unsigned dw = px.op16 ? 2u : 4u;
            if (op_pos + 2u + dw > avail) break;
            jcc_cc = code[op_pos + 1u] & 15u; jcc_len = 2u + dw;
            jcc_disp = dw == 2u ? (s16)nj_rd16(code + op_pos + 2u)
                                : (s32)nj_rd32(code + op_pos + 2u);
        }
        if (jcc_cc != 0xffffffffu) {
            uword fall = start_ip + op_pos + jcc_len;
            uword target = fall + jcc_disp;
            /* Short Jcc and native 16-bit near Jcc update IP modulo 16 bits.
             * A 66h near Jcc in a 16-bit code segment has a 32-bit target;
             * do not silently wrap it.  V86/16-bit CS still has a 64K limit,
             * so let the interpreter raise the architectural fault if the
             * computed EIP is outside the segment. */
            bool near32 = (op == 0x0f && !px.op16);
            if (trace16 && !near32) { fall &= 0xffffu; target &= 0xffffu; }
            else if (trace16 && target > cpu->seg[SEG_CS].limit) {
                e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break;
            }
            bool ok;
            if (jcc_cc == 4u || jcc_cc == 5u)
                ok = nj_v8_emit_jz_side_exit(&e, &exits, jcc_cc == 4u, target, gip, insns + 1u);
            else
                ok = nj_v8_emit_generic_jcc_side_exit(&e, &exits, jcc_cc, target, gip, insns + 1u);
            if (!ok) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
            pos = op_pos + jcc_len; last_ip=gip; insns++;
            /* V8.4: a backward conditional branch ends the current native
             * block. Both outcomes have now completed exactly b->insns, so
             * nj_exec_chain can immediately enter the taken target (usually
             * the loop head) or the fall-through without bouncing through the
             * interpreter. V8.3 kept decoding fall-through instructions, which
             * made a taken backedge a partial side-exit and killed chaining. */
            if (jcc_disp < 0) {
                /* V8.4.1 correctness fix: stop decoding here, but this is NOT
                 * an unconditional terminal exit. The not-taken/fall-through
                 * path reaches the normal block epilogue and must initialize
                 * r0=completed, LR=fall-through next_ip and r2=architectural
                 * branch IP before the shared exit. V8.4 set terminal_exit,
                 * skipped those writes and returned stale control state. */
                continuation_ip = fall;
                break;
            }
            continue;
        }

        /* LOOP rel8 (32-bit address size). Decrement ECX natively and keep
         * fall-through in-trace; taken iterations chain back to the target. */
        if (px.pos == 0u && op == 0xe2 && op_pos + 2u <= avail) {
            sword d = (s8)code[op_pos + 1u];
            uword target = start_ip + op_pos + 2u + d;
            if (trace16) target &= 0xffffu;
            nj_mov_reg(&e, 1, NJ_GUEST_REG(1));
            if (px.addr16) nj_uxth(&e,1,1);
            nj_mov_imm(&e, 2, 1u); nj_subs3(&e);
            if (px.addr16) {
                nj_uxth(&e,3,3);
                if (trace16 && !mixed_v86) nj_mov_reg(&e,NJ_GUEST_REG(1),3);
                else nj_v6_write_r16(&e,1);
            } else {
                nj_mov_reg(&e,NJ_GUEST_REG(1),3);
            }
            if (!nj_v8_emit_loop_side_exit(&e, &exits, target, gip, insns + 1u)) {
                e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break;
            }
            pos = op_pos + 2u; last_ip=gip; insns++;
            if (d < 0) {
                /* V8.4.1: like backward Jcc, the taken path already has its
                 * side-exit stub, while fall-through must use the normal exit
                 * state setup below. Stop trace growth without marking the
                 * block as an unconditional terminal. */
                continuation_ip = start_ip + op_pos + 2u;
                if (trace16) continuation_ip &= 0xffffu;
                break;
            }
            continue;
        }

        /* Unconditional relative branches are cheap terminal native blocks;
         * the C chain dispatcher immediately continues at their target. */
        if (px.pos == 0u && (op == 0xe9 || op == 0xeb)) {
            unsigned dw = px.op16 ? 2u : 4u;
            unsigned blen = op == 0xeb ? 2u : 1u + dw;
            if (op_pos + blen > avail) break;
            sword d = op == 0xeb ? (s8)code[op_pos + 1u]
                                 : (dw == 2u ? (s16)nj_rd16(code + op_pos + 1u)
                                             : (s32)nj_rd32(code + op_pos + 1u));
            uword target = start_ip + op_pos + blen + d;
            if (trace16 && (op == 0xeb || px.op16)) target &= 0xffffu;
            else if (trace16 && target > cpu->seg[SEG_CS].limit) {
                e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break;
            }
            if(!nj_v8_exit_stub_imm(&e,&exits,insns+1u,target,gip)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
            pos = op_pos + blen; last_ip = gip; insns++;
            terminal_exit = true; continuation_ip = target;
            break;
        }

        /* Near CALL rel16/rel32. Mixed-width prototype also supports
         * a 16-bit stack address size, preserving upper ESP. */
        if (op == 0xe8 &&
            (cpu->sp_mask == 0xffffffffu || (mixed_v86 && cpu->sp_mask == 0xffffu)) &&
            ((cpu->sp_mask == 0xffffffffu && cpu->seg[SEG_SS].limit == 0xffffffffu) ||
             (mixed_v86 && cpu->sp_mask == 0xffffu && cpu->seg[SEG_SS].limit >= 0xffffu)) &&
            nj_v6_note_seg(cpu,SEG_SS,&use_ds,&use_es,&use_ss)) {
            bool stack16=mixed_v86 && cpu->sp_mask==0xffffu;
            unsigned sw=px.op16?2u:4u;
            unsigned ilen=1u+sw;
            if(op_pos+ilen>avail) break;
            sword d=sw==2u?(s16)nj_rd16(code+op_pos+1u):(s32)nj_rd32(code+op_pos+1u);
            uword fall=start_ip+op_pos+ilen;
            uword target=fall+d;
            if(px.op16) target&=0xffffu;
            else if(trace16 && target>cpu->seg[SEG_CS].limit){
                e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;
            }
            nj_v6_guard_t g;memset(&g,0,sizeof(g));
            if(stack16){
                nj_mov_reg(&e,1,NJ_GUEST_REG(4));nj_uxth(&e,1,1);
                nj_mov_imm(&e,2,sw);nj_cmp_reg(&e,1,2);
                nj_v6_guard_add(&e,&g,3u); /* LO => wrapped push */
            }
            nj_v6_ea_t sea={.base=4,.index=-1,.scale=0,.disp=-(s32)sw,
                              .seg=SEG_SS,.used=0,.addr16=stack16};
            nj_v6_emit_ea(cpu,&e,&sea,true);
            if(!nj_v7_emit_linear_to_phys(cpu,&e,sw,true,&g)||
               !nj_v6_emit_mem_guard(cpu,&e,sw,true,&g)){
                e.p=emit_before;e.failed=false;pos=ipos;break;
            }
            nj_v6_host_ptr(cpu,&e);nj_mov_imm(&e,1,fall);
            if(sw==2u)nj_strh(&e,1,3,0);else nj_str32(&e,1,3,0);
            if(!nj_v8_finish_guard(&e,&g,&exits,gip,insns)){e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;}
            nj_mov_reg(&e,1,NJ_GUEST_REG(4));if(stack16)nj_uxth(&e,1,1);
            nj_mov_imm(&e,2,sw);nj_subs3(&e);
            if(stack16){nj_uxth(&e,3,3);nj_v6_write_r16(&e,4);}else nj_mov_reg(&e,NJ_GUEST_REG(4),3);
            if(!nj_v8_exit_stub_imm(&e,&exits,insns+1u,target,gip)){e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;}
            pos=op_pos+ilen;last_ip=gip;insns++;terminal_exit=true;continuation_ip=target;
            break;
        }

        /* RET/RET imm16 with 16/32-bit operand size, flat 32-bit stack. */
        if ((op==0xc3 || op==0xc2) && cpu->sp_mask==0xffffffffu &&
            cpu->seg[SEG_SS].limit==0xffffffffu &&
            (op!=0xc2 || op_pos+3u<=avail) &&
            nj_v6_note_seg(cpu,SEG_SS,&use_ds,&use_es,&use_ss)) {
            unsigned sw=px.op16?2u:4u;
            unsigned extra=op==0xc2?nj_rd16(code+op_pos+1u):0u;
            nj_v6_ea_t sea={.base=4,.index=-1,.scale=0,.disp=0,.seg=SEG_SS,.used=0,.addr16=false};
            nj_v6_emit_ea(cpu,&e,&sea,true);
            nj_v6_guard_t g;memset(&g,0,sizeof(g));
            if(!nj_v7_emit_linear_to_phys(cpu,&e,sw,false,&g)||
               !nj_v6_emit_mem_guard(cpu,&e,sw,false,&g)){
                e.p=emit_before;e.failed=false;pos=ipos;break;
            }
            nj_v6_host_ptr(cpu,&e);
            if(sw==2u)nj_ldrh(&e,2,3,0);else nj_ldr32(&e,2,3,0);
            if(!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
            nj_mov_reg(&e,NJ_BUDGET_REG,2);
            nj_mov_reg(&e,1,NJ_GUEST_REG(4));nj_mov_imm(&e,2,sw+extra);nj_adds3(&e);
            nj_mov_reg(&e,NJ_GUEST_REG(4),3);
            if(!nj_v8_exit_stub_lr(&e,&exits,insns+1u,gip)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
            pos=op_pos+(op==0xc2?3u:1u);last_ip=gip;insns++;terminal_exit=true;continuation_ip=0;
            break;
        }

        /* Register PUSH/POP and immediate PUSH. Support 16- or 32-bit
         * stack address size; for stack16 preserve upper ESP and side-exit
         * instead of attempting wrapped multi-byte accesses. */
        if (((op>=0x50 && op<=0x5f)||op==0x68||op==0x6a) &&
            (cpu->sp_mask==0xffffffffu || (mixed_v86 && cpu->sp_mask==0xffffu)) &&
            ((cpu->sp_mask==0xffffffffu && cpu->seg[SEG_SS].limit==0xffffffffu) ||
             (mixed_v86 && cpu->sp_mask==0xffffu && cpu->seg[SEG_SS].limit>=0xffffu)) &&
            nj_v6_note_seg(cpu,SEG_SS,&use_ds,&use_es,&use_ss)) {
            bool stack16=mixed_v86 && cpu->sp_mask==0xffffu;
            bool stack_done=false;bool pop=op>=0x58&&op<=0x5f;unsigned reg=op&7u;
            unsigned sw=px.op16?2u:4u;
            if(pop && reg==4u){
                /* POP ESP/SP ordering stays interpreter-owned. */
            }else if(pop){
                nj_v6_guard_t g;memset(&g,0,sizeof(g));
                if(stack16 && sw>1u){
                    nj_mov_reg(&e,1,NJ_GUEST_REG(4));nj_uxth(&e,1,1);
                    nj_mov_imm(&e,2,0xffffu-(sw-1u));nj_cmp_reg(&e,1,2);
                    nj_v6_guard_add(&e,&g,8u); /* HI => crosses SS limit */
                }
                nj_v6_ea_t sea={.base=4,.index=-1,.scale=0,.disp=0,.seg=SEG_SS,.used=0,.addr16=stack16};
                nj_v6_emit_ea(cpu,&e,&sea,true);
                if(!nj_v7_emit_linear_to_phys(cpu,&e,sw,false,&g)||!nj_v6_emit_mem_guard(cpu,&e,sw,false,&g)){e.p=emit_before;e.failed=false;pos=ipos;break;}
                nj_v6_host_ptr(cpu,&e);if(sw==2u)nj_ldrh(&e,0,3,0);else nj_ldr32(&e,0,3,0);
                if(!nj_v8_finish_guard(&e,&g,&exits,gip,insns)){e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;}
                nj_mov_reg(&e,1,NJ_GUEST_REG(4));if(stack16)nj_uxth(&e,1,1);nj_mov_imm(&e,2,sw);nj_adds3(&e);
                if(stack16){nj_uxth(&e,3,3);nj_v6_write_r16(&e,4);}else nj_mov_reg(&e,NJ_GUEST_REG(4),3);
                if(sw==2u){nj_mov_reg(&e,3,0);nj_v6_write_r16(&e,reg);}else nj_mov_reg(&e,NJ_GUEST_REG(reg),0);
                pos=op_pos+1u;stack_done=true;
            }else{
                unsigned ilen=1u;u32 imm=0;bool imm_push=false;
                if(op==0x68){if(op_pos+1u+sw>avail)break;imm=sw==2u?nj_rd16(code+op_pos+1u):nj_rd32(code+op_pos+1u);ilen=1u+sw;imm_push=true;}
                else if(op==0x6a){if(op_pos+2u>avail)break;s32 si=(s8)code[op_pos+1u];imm=sw==2u?(u16)si:(u32)si;ilen=2u;imm_push=true;}
                nj_v6_guard_t g;memset(&g,0,sizeof(g));
                if(stack16){nj_mov_reg(&e,1,NJ_GUEST_REG(4));nj_uxth(&e,1,1);nj_mov_imm(&e,2,sw);nj_cmp_reg(&e,1,2);nj_v6_guard_add(&e,&g,3u);}
                nj_v6_ea_t sea={.base=4,.index=-1,.scale=0,.disp=-(s32)sw,.seg=SEG_SS,.used=0,.addr16=stack16};
                nj_v6_emit_ea(cpu,&e,&sea,true);
                if(!nj_v7_emit_linear_to_phys(cpu,&e,sw,true,&g)||!nj_v6_emit_mem_guard(cpu,&e,sw,true,&g)){e.p=emit_before;e.failed=false;pos=ipos;break;}
                nj_v6_host_ptr(cpu,&e);
                if(imm_push){nj_mov_imm(&e,1,imm);if(sw==2u)nj_strh(&e,1,3,0);else nj_str32(&e,1,3,0);}
                else{if(sw==2u)nj_strh(&e,NJ_GUEST_REG(reg),3,0);else nj_str32(&e,NJ_GUEST_REG(reg),3,0);}
                if(!nj_v8_finish_guard(&e,&g,&exits,gip,insns)){e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;}
                nj_mov_reg(&e,1,NJ_GUEST_REG(4));if(stack16)nj_uxth(&e,1,1);nj_mov_imm(&e,2,sw);nj_subs3(&e);
                if(stack16){nj_uxth(&e,3,3);nj_v6_write_r16(&e,4);}else nj_mov_reg(&e,NJ_GUEST_REG(4),3);
                pos=op_pos+ilen;stack_done=true;
            }
            if(stack_done){last_ip=gip;insns++;continue;}
        }

        /* LEAVE = ESP<-EBP; POP EBP. */
        if(px.pos==0u && op==0xc9 && cpu->sp_mask==0xffffffffu &&
           cpu->seg[SEG_SS].limit==0xffffffffu &&
           nj_v6_note_seg(cpu,SEG_SS,&use_ds,&use_es,&use_ss)) {
            nj_v6_ea_t sea={.base=5,.index=-1,.scale=0,.disp=0,.seg=SEG_SS,.used=0,.addr16=false};
            nj_v6_emit_ea(cpu,&e,&sea,true);nj_v6_guard_t g;memset(&g,0,sizeof(g));
            if(!nj_v7_emit_linear_to_phys(cpu,&e,4u,false,&g)||
               !nj_v6_emit_mem_guard(cpu,&e,4u,false,&g)){
                e.p=emit_before;e.failed=false;pos=ipos;break;
            }
            nj_v6_host_ptr(cpu,&e);nj_ldr32(&e,0,3,0);
            if(!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
            nj_mov_reg(&e,1,NJ_GUEST_REG(5));nj_mov_imm(&e,2,4u);nj_adds3(&e);
            nj_mov_reg(&e,NJ_GUEST_REG(4),3);nj_mov_reg(&e,NJ_GUEST_REG(5),0);
            pos=op_pos+1u;last_ip=gip;insns++;continue;
        }

        /* Other control flow remains interpreter fallback. */
        if ((op >= 0x70 && op <= 0x7f) || op == 0xe8 || op == 0xe9 ||
            op == 0xeb || op == 0xc2 || op == 0xc3 || op == 0xca ||
            op == 0xcb || op == 0xcf)
            break;

        bool done = false;

        /* ---- LODSB/LODSW/LODSD: hot scan/checksum loops ------------- */
        if (!done && (op == 0xac || op == 0xad)) {
            unsigned size = op == 0xac ? 1u : (px.op16 ? 2u : 4u);
            int seg = px.seg >= 0 ? px.seg : SEG_DS;
            if ((cpu->cr0 & 1u) && !(cpu->flags & VM) &&
                cpu->seg[seg].limit != 0xffffffffu) break;
            if (!nj_v6_note_seg(cpu,seg,&use_ds,&use_es,&use_ss)) break;
            nj_v6_ea_t ea={.base=6,.index=-1,.scale=0,.disp=0,
                            .seg=seg,.used=0,.addr16=px.addr16};
            nj_v6_emit_ea(cpu,&e,&ea,true);
            nj_v6_guard_t g; memset(&g,0,sizeof(g));
            if (!nj_v7_emit_linear_to_phys(cpu,&e,size,false,&g) ||
                !nj_v6_emit_mem_guard(cpu,&e,size,false,&g)) {
                e.p=emit_before;e.failed=false;pos=ipos;break;
            }
            nj_v6_host_ptr(cpu,&e);
            if (size == 1u) {
                nj_ldrb(&e,3,3,0);
                nj_v6_write_r8(&e,0);
            } else if (size == 2u) {
                nj_ldrh(&e,3,3,0);
                nj_v6_write_r16(&e,0);
            } else {
                nj_ldr32(&e,3,3,0);
                nj_mov_reg(&e,NJ_GUEST_REG(0),3);
            }
            if (!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) {
                e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;
            }
            use_df=true; df_value=(u8)!!(cpu->flags & DF);
            nj_mov_reg(&e,1,NJ_GUEST_REG(6));
            if (px.addr16) nj_uxth(&e,1,1);
            nj_mov_imm(&e,2,size);
            if (df_value) nj_subs3(&e); else nj_adds3(&e);
            if (px.addr16) {
                nj_uxth(&e,3,3);
                if (trace16 && !mixed_v86) nj_mov_reg(&e,NJ_GUEST_REG(6),3);
                else nj_v6_write_r16(&e,6);
            } else {
                nj_mov_reg(&e,NJ_GUEST_REG(6),3);
            }
            pos=op_pos+1u; done=true;
        }

        /* ---- 0F A4/AC: immediate SHLD/SHRD r32,r32,imm8 ------------ */
        if (op == 0x0f && !px.op16 && op_pos + 3u < avail) {
            u8 op2 = code[op_pos + 1u];
            if (op2 == 0xa4 || op2 == 0xac) {
                u8 m = code[op_pos + 2u];
                if ((m >> 6) == 3u) {
                    unsigned dst = m & 7u;
                    unsigned src = (m >> 3) & 7u;
                    unsigned cnt = code[op_pos + 3u] & 31u;
                    if (nj_v6_emit_shx32(&e, dst, src, cnt, op2 == 0xac)) {
                        pos = op_pos + 4u;
                        if (cnt) cf_dirty = true;
                        done = true;
                    }
                }
            }
        }

        /* ---- MOV/LEA with memory ModR/M ----------------------------- */
        if (!done && (op == 0x88 || op == 0x8a || op == 0x89 ||
                      op == 0x8b || op == 0x8d)) {
            if (op_pos + 1u >= avail) break;
            u8 m = code[op_pos + 1u];
            unsigned mod = m >> 6;
            unsigned reg = (m >> 3) & 7u;

            if (mod == 3u && op != 0x8d) {
                unsigned rm = m & 7u;
                if (op == 0x8a) {
                    nj_v6_read_r8(&e, rm);
                    nj_v6_write_r8(&e, reg);
                } else if (op == 0x88) {
                    nj_v6_read_r8(&e, reg);
                    nj_v6_write_r8(&e, rm);
                } else if (op == 0x8b) {
                    if (px.op16) { nj_mov_reg(&e, 3, NJ_GUEST_REG(rm)); nj_v6_write_r16(&e, reg); }
                    else nj_mov_reg(&e, NJ_GUEST_REG(reg), NJ_GUEST_REG(rm));
                } else if (op == 0x89) {
                    if (px.op16) { nj_mov_reg(&e, 3, NJ_GUEST_REG(reg)); nj_v6_write_r16(&e, rm); }
                    else nj_mov_reg(&e, NJ_GUEST_REG(rm), NJ_GUEST_REG(reg));
                }
                pos = op_pos + 2u;
                done = true;
            } else if (mod != 3u) {
                nj_v6_ea_t ea;
                if (!nj_v6_decode_ea(code + op_pos + 2u,
                                     avail - (op_pos + 2u), m,
                                     px.addr16, px.seg, &ea))
                    break;
                if (!nj_v6_note_seg(cpu, ea.seg, &use_ds, &use_es, &use_ss))
                    break;

                unsigned ilen = px.pos + 2u + ea.used;

                if (op == 0x8d) {
                    /* LEA uses offset only, not the segment base. */
                    nj_v6_emit_ea(cpu, &e, &ea, false);

                    if (px.op16) nj_v6_write_r16(&e, reg);
                    else nj_mov_reg(&e, NJ_GUEST_REG(reg), 3);
                    pos += ilen;
                    done = true;
                } else {
                    unsigned size = (op == 0x88 || op == 0x8a) ? 1u :
                                    (px.op16 ? 2u : 4u);
                    bool write = (op == 0x88 || op == 0x89);

                    nj_v6_emit_ea(cpu, &e, &ea, true);
                    nj_v6_guard_t g;
                    memset(&g, 0, sizeof(g));
                    if (!nj_v7_emit_linear_to_phys(cpu, &e, size, write, &g) ||
                        !nj_v6_emit_mem_guard(cpu, &e, size, write, &g)) {
                        e.p = emit_before; e.failed = false; pos = ipos;
                        break;
                    }
                    nj_v6_host_ptr(cpu, &e);

                    if (op == 0x8a) {
                        nj_ldrb(&e, 3, 3, 0);
                        nj_v6_write_r8(&e, reg);
                    } else if (op == 0x88) {
                        /* Preserve host pointer while extracting AL/AH-style source. */
                        nj_mov_reg(&e, 0, 3);
                        nj_v6_read_r8(&e, reg);
                        nj_strb(&e, 3, 0, 0);
                    } else if (op == 0x8b) {
                        if (size == 2u) nj_ldrh(&e, 3, 3, 0);
                        else nj_ldr32(&e, 3, 3, 0);
                        if (size == 2u) nj_v6_write_r16(&e, reg);
                        else nj_mov_reg(&e, NJ_GUEST_REG(reg), 3);
                    } else { /* 89 */
                        if (size == 2u)
                            nj_strh(&e, NJ_GUEST_REG(reg), 3, 0);
                        else
                            nj_str32(&e, NJ_GUEST_REG(reg), 3, 0);
                    }

                    if (!nj_v8_finish_guard(&e, &g, &exits, gip, insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
                    pos += ilen;
                    done = true;
                }
            }
        }

        /* ---- byte accumulator immediate ALU (04/0C/24/2C/34/3C) ---- */
        if (!done && (op==0x04 || op==0x0c || op==0x24 ||
                      op==0x2c || op==0x34 || op==0x3c)) {
            if (op_pos + 2u > avail) break;
            u8 imm=code[op_pos+1u];
            nj_v6_read_r8(&e,0); nj_mov_reg(&e,1,3);
            nj_mov_imm(&e,2,imm);
            bool write_result = op != 0x3c;
            unsigned fk,ccop;
            if(op==0x04){nj_adds3(&e);fk=NJ_FLAG_ARITH;ccop=CC_ADD;}
            else if(op==0x2c || op==0x3c){nj_subs3(&e);fk=NJ_FLAG_ARITH;ccop=CC_SUB;}
            else if(op==0x0c){nj_orr1(&e);nj_mov_reg(&e,3,1);fk=NJ_FLAG_LOGIC;ccop=CC_OR;}
            else if(op==0x24){nj_and1(&e);nj_mov_reg(&e,3,1);fk=NJ_FLAG_LOGIC;ccop=CC_AND;}
            else {nj_eor1(&e);nj_mov_reg(&e,3,1);fk=NJ_FLAG_LOGIC;ccop=CC_XOR;}
            nj_uxtb(&e,3,3);
            if(write_result) nj_v6_write_r8(&e,0);
            nj_v8_emit_cc_flush_width(&e,1u,fk,ccop);
            cf_dirty=true;
            pos=op_pos+2u;done=true;
        }

        /* ---- byte CMP/TEST (38/3A/84), including memory ------------ */
        if (!done && (op == 0x38 || op == 0x3a || op == 0x84)) {
            if (op_pos + 1u >= avail) break;
            u8 m = code[op_pos + 1u];
            unsigned mod = m >> 6;
            unsigned reg = (m >> 3) & 7u;
            unsigned rm = m & 7u;
            bool testop = op == 0x84;

            if (mod == 3u) {
                unsigned dst = op == 0x3a ? reg : rm;
                unsigned src = op == 0x3a ? rm : reg;
                nj_v6_read_r8(&e,dst); nj_mov_reg(&e,1,3);
                nj_v6_read_r8(&e,src); nj_mov_reg(&e,2,3);
                if (testop) { nj_and1(&e); nj_mov_reg(&e,3,1); }
                else nj_subs3(&e);
                nj_uxtb(&e,3,3);
                nj_v8_emit_cc_flush_width(&e,1u,
                    testop ? NJ_FLAG_LOGIC : NJ_FLAG_ARITH,
                    testop ? CC_AND : CC_SUB);
                cf_dirty=true;
                pos=op_pos+2u; done=true;
            } else {
                nj_v6_ea_t ea;
                if (!nj_v6_decode_ea(code+op_pos+2u,avail-(op_pos+2u),m,
                                     px.addr16,px.seg,&ea)) break;
                if (!nj_v6_note_seg(cpu,ea.seg,&use_ds,&use_es,&use_ss)) break;
                unsigned ilen=px.pos+2u+ea.used;
                nj_v6_emit_ea(cpu,&e,&ea,true);
                nj_v6_guard_t g; memset(&g,0,sizeof(g));
                if (!nj_v7_emit_linear_to_phys(cpu,&e,1u,false,&g) ||
                    !nj_v6_emit_mem_guard(cpu,&e,1u,false,&g)) {
                    e.p=emit_before;e.failed=false;pos=ipos;break;
                }
                nj_v6_host_ptr(cpu,&e);
                nj_ldrb(&e,0,3,0);
                if (!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) {
                    e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;
                }
                if (op == 0x3a) {
                    nj_v6_read_r8(&e,reg); nj_mov_reg(&e,1,3);
                    nj_mov_reg(&e,2,0);
                } else {
                    nj_mov_reg(&e,1,0);
                    nj_v6_read_r8(&e,reg); nj_mov_reg(&e,2,3);
                }
                if (testop) { nj_and1(&e); nj_mov_reg(&e,3,1); }
                else nj_subs3(&e);
                nj_uxtb(&e,3,3);
                nj_v8_emit_cc_flush_width(&e,1u,
                    testop ? NJ_FLAG_LOGIC : NJ_FLAG_ARITH,
                    testop ? CC_AND : CC_SUB);
                cf_dirty=true;
                pos += ilen; done=true;
            }
        }

        /* ---- FE/FF memory INC/DEC ----------------------------------- */
        if (!done && (op == 0xfe || op == 0xff) && op_pos + 1u < avail) {
            u8 m=code[op_pos+1u];
            unsigned sub=(m>>3)&7u;
            if ((m>>6)!=3u && (sub==0u || sub==1u)) {
                if (cf_dirty) break; /* INC/DEC must preserve the old CF. */
                unsigned size=op==0xfe?1u:(px.op16?2u:4u);
                nj_v6_ea_t ea;
                if (!nj_v6_decode_ea(code+op_pos+2u,avail-(op_pos+2u),m,
                                     px.addr16,px.seg,&ea)) break;
                if (!nj_v6_note_seg(cpu,ea.seg,&use_ds,&use_es,&use_ss)) break;
                unsigned ilen=px.pos+2u+ea.used;
                nj_v6_emit_ea(cpu,&e,&ea,true);
                nj_v6_guard_t g; memset(&g,0,sizeof(g));
                if (!nj_v7_emit_linear_to_phys(cpu,&e,size,true,&g) ||
                    !nj_v6_emit_mem_guard(cpu,&e,size,true,&g)) {
                    e.p=emit_before;e.failed=false;pos=ipos;break;
                }
                nj_v6_host_ptr(cpu,&e);
                nj_mov_reg(&e,0,3); /* host pointer */
                if(size==1u) nj_ldrb(&e,1,0,0);
                else if(size==2u) nj_ldrh(&e,1,0,0);
                else nj_ldr32(&e,1,0,0);
                nj_mov_imm(&e,2,1u);
                if(sub==0u) nj_adds3(&e); else nj_subs3(&e);
                if(size==1u) { nj_uxtb(&e,3,3); nj_strb(&e,3,0,0); }
                else if(size==2u) { nj_uxth(&e,3,3); nj_strh(&e,3,0,0); }
                else nj_str32(&e,3,0,0);
                if (!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) {
                    e.p=emit_before;e.failed=false;exits.n=exits_before;pos=ipos;break;
                }
                unsigned ccop = sub==0u
                    ? (size==1u?CC_INC8:(size==2u?CC_INC16:CC_INC32))
                    : (size==1u?CC_DEC8:(size==2u?CC_DEC16:CC_DEC32));
                nj_v8_emit_cc_flush_width(&e,size,NJ_FLAG_INCDEC,ccop);
                needs_refresh_cf=true;
                pos += ilen; done=true;
            }
        }

        /* ---- common word/dword ALU with one memory operand ---------- */
        if (!done && (op == 0x01 || op == 0x03 || op == 0x09 || op == 0x0b ||
                      op == 0x21 || op == 0x23 || op == 0x29 || op == 0x2b ||
                      op == 0x31 || op == 0x33 || op == 0x39 || op == 0x3b ||
                      op == 0x85)) {
            if (op_pos + 1u >= avail) break;
            u8 m = code[op_pos + 1u];
            unsigned mod = m >> 6;
            unsigned reg = (m >> 3) & 7u;

            if (mod == 3u) {
                unsigned rm = m & 7u;
                bool reg_dst = (op & 2u) != 0u;
                bool cmpop = op == 0x39 || op == 0x3b;
                bool testop = op == 0x85;
                unsigned dst = reg_dst ? reg : rm;
                unsigned src = reg_dst ? rm : reg;
                if (testop) { dst = rm; src = reg; }
                nj_mov_reg(&e, 1, NJ_GUEST_REG(dst));
                nj_mov_reg(&e, 2, NJ_GUEST_REG(src));
                if (px.op16) { nj_uxth(&e, 1, 1); nj_uxth(&e, 2, 2); }
                unsigned fk = NJ_FLAG_NONE, ccop = 0;
                bool write_result = !cmpop && !testop;
                if (op == 0x01 || op == 0x03) { nj_adds3(&e); fk=NJ_FLAG_ARITH; ccop=CC_ADD; }
                else if (op == 0x29 || op == 0x2b || cmpop) { nj_subs3(&e); fk=NJ_FLAG_ARITH; ccop=CC_SUB; }
                else if (op == 0x09 || op == 0x0b) { nj_orr1(&e); nj_mov_reg(&e,3,1); fk=NJ_FLAG_LOGIC; ccop=CC_OR; }
                else if (op == 0x21 || op == 0x23 || testop) { nj_and1(&e); nj_mov_reg(&e,3,1); fk=NJ_FLAG_LOGIC; ccop=CC_AND; }
                else { nj_eor1(&e); nj_mov_reg(&e,3,1); fk=NJ_FLAG_LOGIC; ccop=CC_XOR; }
                if (px.op16) nj_uxth(&e, 3, 3);
                if (write_result) {
                    if (px.op16) nj_v6_write_r16(&e, dst);
                    else nj_mov_reg(&e, NJ_GUEST_REG(dst), 3);
                }
                nj_emit_cc_flush(&e, px.op16, fk, ccop);
                cf_dirty = true;
                pos = op_pos + 2u; done = true;
            } else {
                nj_v6_ea_t ea;
                if (!nj_v6_decode_ea(code + op_pos + 2u,
                                     avail - (op_pos + 2u), m,
                                     px.addr16, px.seg, &ea))
                    break;
                if (!nj_v6_note_seg(cpu, ea.seg, &use_ds, &use_es, &use_ss))
                    break;

                bool reg_dst = (op & 2u) != 0u;
                bool cmpop = op == 0x39 || op == 0x3b;
                bool testop = op == 0x85;
                bool mem_write = !reg_dst && !cmpop && !testop;
                unsigned size = px.op16 ? 2u : 4u;
                unsigned ilen = px.pos + 2u + ea.used;

                nj_v6_emit_ea(cpu, &e, &ea, true);
                nj_v6_guard_t g;
                memset(&g, 0, sizeof(g));
                if (!nj_v7_emit_linear_to_phys(cpu, &e, size, mem_write, &g) ||
                    !nj_v6_emit_mem_guard(cpu, &e, size, mem_write, &g)) {
                    e.p = emit_before; e.failed = false; pos = ipos;
                    break;
                }
                nj_v6_host_ptr(cpu, &e);

                if (reg_dst) {
                    if (size == 2u) nj_ldrh(&e, 2, 3, 0);
                    else nj_ldr32(&e, 2, 3, 0);
                    if (!nj_v8_finish_guard(&e, &g, &exits, gip, insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }

                    nj_mov_reg(&e, 1, NJ_GUEST_REG(reg));
                    if (size == 2u) nj_uxth(&e, 1, 1);
                } else {
                    /* Preserve host pointer for an optional RMW store. */
                    nj_mov_reg(&e, 0, 3);
                    if (size == 2u) nj_ldrh(&e, 1, 3, 0);
                    else nj_ldr32(&e, 1, 3, 0);
                    nj_mov_reg(&e, 2, NJ_GUEST_REG(reg));
                    if (size == 2u) nj_uxth(&e, 2, 2);
                    if (!nj_v8_finish_guard(&e, &g, &exits, gip, insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
                }

                unsigned fk = NJ_FLAG_NONE, ccop = 0;
                bool write_result = !cmpop && !testop;

                if (op == 0x01 || op == 0x03) {
                    nj_adds3(&e); fk = NJ_FLAG_ARITH; ccop = CC_ADD;
                } else if (op == 0x29 || op == 0x2b ||
                           op == 0x39 || op == 0x3b) {
                    nj_subs3(&e); fk = NJ_FLAG_ARITH; ccop = CC_SUB;
                } else if (op == 0x09 || op == 0x0b) {
                    nj_orr1(&e); nj_mov_reg(&e, 3, 1);
                    fk = NJ_FLAG_LOGIC; ccop = CC_OR;
                } else if (op == 0x21 || op == 0x23 || op == 0x85) {
                    nj_and1(&e); nj_mov_reg(&e, 3, 1);
                    fk = NJ_FLAG_LOGIC; ccop = CC_AND;
                } else {
                    nj_eor1(&e); nj_mov_reg(&e, 3, 1);
                    fk = NJ_FLAG_LOGIC; ccop = CC_XOR;
                }

                if (size == 2u) nj_uxth(&e, 3, 3);

                if (write_result) {
                    if (reg_dst) {
                        if (size == 2u) nj_v6_write_r16(&e, reg);
                        else nj_mov_reg(&e, NJ_GUEST_REG(reg), 3);
                    } else {
                        if (size == 2u) nj_strh(&e, 3, 0, 0);
                        else nj_str32(&e, 3, 0, 0);
                    }
                }

                nj_emit_cc_flush(&e, size == 2u, fk, ccop);
                pos += ilen;
                done = true;
            }
        }

        /* ---- 80/81/83 immediate ALU, register or memory ----------- */
        if (!done && (op == 0x80 || op == 0x81 || op == 0x83) &&
            op_pos + 2u <= avail) {
            u8 m=code[op_pos+1u];
            unsigned mod=m>>6, sub=(m>>3)&7u, rm=m&7u;
            if (sub==0u || sub==1u || sub==4u || sub==5u || sub==6u || sub==7u) {
                unsigned size = op==0x80 ? 1u : (px.op16 ? 2u : 4u);
                unsigned immbytes = op==0x81 ? size : 1u;
                unsigned immpos = op_pos + 2u;
                nj_v6_ea_t ea;
                if (mod != 3u) {
                    if (!nj_v6_decode_ea(code+op_pos+2u,avail-(op_pos+2u),m,
                                         px.addr16,px.seg,&ea)) break;
                    if (!nj_v6_note_seg(cpu,ea.seg,&use_ds,&use_es,&use_ss)) break;
                    immpos += ea.used;
                }
                if (immpos + immbytes > avail) break;
                u32 imm;
                if (op==0x83) {
                    s32 si=(s8)code[immpos];
                    imm = size==2u ? (u16)si : (u32)si;
                } else if (size==1u) imm=code[immpos];
                else if (size==2u) imm=nj_rd16(code+immpos);
                else imm=nj_rd32(code+immpos);

                bool write_result=sub!=7u;
                nj_v6_guard_t g; memset(&g,0,sizeof(g));
                if (mod==3u) {
                    if (size==1u) { nj_v6_read_r8(&e,rm); nj_mov_reg(&e,1,3); }
                    else { nj_mov_reg(&e,1,NJ_GUEST_REG(rm)); if(size==2u) nj_uxth(&e,1,1); }
                } else {
                    nj_v6_emit_ea(cpu,&e,&ea,true);
                    if (!nj_v7_emit_linear_to_phys(cpu,&e,size,write_result,&g) ||
                        !nj_v6_emit_mem_guard(cpu,&e,size,write_result,&g)) {
                        e.p=emit_before;e.failed=false;pos=ipos;break;
                    }
                    nj_v6_host_ptr(cpu,&e);
                    if (write_result) nj_mov_reg(&e,0,3); /* host pointer */
                    if(size==1u) nj_ldrb(&e,1,3,0);
                    else if(size==2u) nj_ldrh(&e,1,3,0);
                    else nj_ldr32(&e,1,3,0);
                }
                nj_mov_imm(&e,2,imm);
                unsigned fk,ccop;
                if(sub==0u){nj_adds3(&e);fk=NJ_FLAG_ARITH;ccop=CC_ADD;}
                else if(sub==1u){nj_orr1(&e);nj_mov_reg(&e,3,1);fk=NJ_FLAG_LOGIC;ccop=CC_OR;}
                else if(sub==4u){nj_and1(&e);nj_mov_reg(&e,3,1);fk=NJ_FLAG_LOGIC;ccop=CC_AND;}
                else if(sub==5u || sub==7u){nj_subs3(&e);fk=NJ_FLAG_ARITH;ccop=CC_SUB;}
                else {nj_eor1(&e);nj_mov_reg(&e,3,1);fk=NJ_FLAG_LOGIC;ccop=CC_XOR;}
                if(size==1u) nj_uxtb(&e,3,3); else if(size==2u) nj_uxth(&e,3,3);
                nj_v8_emit_cc_flush_width(&e,size,fk,ccop);
                cf_dirty=true;
                if(write_result) {
                    if(mod==3u) {
                        if(size==1u) nj_v6_write_r8(&e,rm);
                        else if(size==2u) nj_v6_write_r16(&e,rm);
                        else nj_mov_reg(&e,NJ_GUEST_REG(rm),3);
                    } else {
                        if(size==1u) nj_strb(&e,3,0,0);
                        else if(size==2u) nj_strh(&e,3,0,0);
                        else nj_str32(&e,3,0,0);
                    }
                }
                if(mod!=3u && !nj_v8_finish_guard(&e,&g,&exits,gip,insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
                pos=immpos+immbytes;done=true;
            }
        }

        /* ---- F6/F7 /0 TEST immediate, register or memory ------------- */
        if (!done && (op==0xf6 || op==0xf7) && op_pos+2u<=avail) {
            u8 m=code[op_pos+1u]; unsigned mod=m>>6,sub=(m>>3)&7u,rm=m&7u;
            if(sub==0u) {
                unsigned size=op==0xf6?1u:(px.op16?2u:4u);
                unsigned immpos=op_pos+2u; nj_v6_ea_t ea;
                if(mod!=3u) {
                    if(!nj_v6_decode_ea(code+op_pos+2u,avail-(op_pos+2u),m,
                                        px.addr16,px.seg,&ea)) break;
                    if(!nj_v6_note_seg(cpu,ea.seg,&use_ds,&use_es,&use_ss)) break;
                    immpos+=ea.used;
                }
                if(immpos+size>avail) break;
                u32 imm=size==1u?code[immpos]:(size==2u?nj_rd16(code+immpos):nj_rd32(code+immpos));
                nj_v6_guard_t g;memset(&g,0,sizeof(g));
                if(mod==3u) {
                    if(size==1u){nj_v6_read_r8(&e,rm);nj_mov_reg(&e,1,3);}
                    else{nj_mov_reg(&e,1,NJ_GUEST_REG(rm));if(size==2u)nj_uxth(&e,1,1);}
                } else {
                    nj_v6_emit_ea(cpu,&e,&ea,true);
                    if(!nj_v7_emit_linear_to_phys(cpu,&e,size,false,&g)||
                       !nj_v6_emit_mem_guard(cpu,&e,size,false,&g)){
                        e.p=emit_before;e.failed=false;pos=ipos;break;
                    }
                    nj_v6_host_ptr(cpu,&e);
                    if(size==1u)nj_ldrb(&e,1,3,0);else if(size==2u)nj_ldrh(&e,1,3,0);else nj_ldr32(&e,1,3,0);
                }
                nj_mov_imm(&e,2,imm);nj_and1(&e);nj_mov_reg(&e,3,1);
                if(size==1u)nj_uxtb(&e,3,3);else if(size==2u)nj_uxth(&e,3,3);
                nj_v8_emit_cc_flush_width(&e,size,NJ_FLAG_LOGIC,CC_AND);cf_dirty=true;
                if(mod!=3u&&!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
                pos=immpos+size;done=true;
            }
        }

        /* ---- word/dword MOV immediate B8-BF ------------------------ */
        if(!done && mixed_v86 && op>=0xb8 && op<=0xbf){
            unsigned sz=px.op16?2u:4u;
            if(op_pos+1u+sz>avail) break;
            u32 imm=sz==2u?nj_rd16(code+op_pos+1u):nj_rd32(code+op_pos+1u);
            nj_mov_imm(&e,3,imm);
            if(sz==2u)nj_v6_write_r16(&e,op&7u);else nj_mov_reg(&e,NJ_GUEST_REG(op&7u),3);
            pos=op_pos+1u+sz;done=true;
        }

        /* ---- byte MOV immediate B0-B7 ------------------------------- */
        if(!done && op>=0xb0 && op<=0xb7 && op_pos+2u<=avail){
            nj_mov_imm(&e,3,code[op_pos+1u]);nj_v6_write_r8(&e,op&7u);
            pos=op_pos+2u;done=true;
        }

        /* ---- prefixed/unprefixed INC/DEC register ------------------- */
        if(!done && op>=0x40 && op<=0x4f){
            if(cf_dirty) break;
            unsigned r=op&7u;bool dec=op>=0x48;
            nj_mov_reg(&e,1,NJ_GUEST_REG(r));if(px.op16)nj_uxth(&e,1,1);
            nj_mov_imm(&e,2,1u);if(dec)nj_subs3(&e);else nj_adds3(&e);
            if(px.op16)nj_uxth(&e,3,3);
            if(px.op16)nj_v6_write_r16(&e,r);else nj_mov_reg(&e,NJ_GUEST_REG(r),3);
            nj_emit_cc_flush(&e,px.op16,NJ_FLAG_INCDEC,
                             dec?(px.op16?CC_DEC16:CC_DEC32):(px.op16?CC_INC16:CC_INC32));
            needs_refresh_cf=true;pos=op_pos+1u;done=true;
        }

        /* ---- MOVZX/MOVSX 0F B6/B7/BE/BF ------------------------- */
        if (!done && op == 0x0f && op_pos + 2u < avail) {
            u8 op2 = code[op_pos + 1u];
            if (op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) {
                u8 m = code[op_pos + 2u];
                unsigned mod = m >> 6, reg = (m >> 3) & 7u, rm = m & 7u;
                unsigned ssize = (op2 == 0xb6 || op2 == 0xbe) ? 1u : 2u;
                bool sign = op2 == 0xbe || op2 == 0xbf;
                unsigned ilen = px.pos + 3u;

                if (mod == 3u) {
                    if (ssize == 1u) nj_v6_read_r8(&e, rm);
                    else { nj_mov_reg(&e, 3, NJ_GUEST_REG(rm)); nj_uxth(&e,3,3); }
                    if (sign) { if (ssize == 1u) nj_sxtb(&e,3,3); else nj_sxth(&e,3,3); }
                    if (px.op16) nj_v6_write_r16(&e, reg);
                    else nj_mov_reg(&e, NJ_GUEST_REG(reg), 3);
                    pos = op_pos + 3u; done = true;
                } else {
                    nj_v6_ea_t ea;
                    if (!nj_v6_decode_ea(code + op_pos + 3u, avail - (op_pos + 3u),
                                         m, px.addr16, px.seg, &ea)) break;
                    if (!nj_v6_note_seg(cpu, ea.seg, &use_ds, &use_es, &use_ss)) break;
                    ilen += ea.used;
                    nj_v6_emit_ea(cpu, &e, &ea, true);
                    nj_v6_guard_t g; memset(&g,0,sizeof(g));
                    if (!nj_v7_emit_linear_to_phys(cpu,&e,ssize,false,&g) ||
                        !nj_v6_emit_mem_guard(cpu,&e,ssize,false,&g)) {
                        e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break;
                    }
                    nj_v6_host_ptr(cpu,&e);
                    if (ssize == 1u) nj_ldrb(&e,3,3,0); else nj_ldrh(&e,3,3,0);
                    if (sign) { if (ssize == 1u) nj_sxtb(&e,3,3); else nj_sxth(&e,3,3); }
                    if (px.op16) nj_v6_write_r16(&e,reg);
                    else nj_mov_reg(&e,NJ_GUEST_REG(reg),3);
                    if (!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
                    pos = op_pos + 3u + ea.used; done=true;
                }
            }
        }

        /* ---- C1/D1 register shifts: SHL/SHR/SAR --------------------- */
        if (!done && (op == 0xc1 || op == 0xd1) && !px.op16 && op_pos + 2u <= avail) {
            u8 m = code[op_pos + 1u];
            if ((m >> 6) == 3u) {
                unsigned sub = (m >> 3) & 7u;
                unsigned count = op == 0xd1 ? 1u :
                                 (op_pos + 3u <= avail ? code[op_pos + 2u] : 0u);
                if ((op == 0xd1 || op_pos + 3u <= avail) &&
                    nj_v8_emit_shift_imm32(&e, m & 7u, sub, count)) {
                    if (count & 31u) cf_dirty = true;
                    pos = op_pos + (op == 0xd1 ? 2u : 3u); done=true;
                }
            }
        }

        /* ---- C6/C7 MOV immediate to register/memory ------------------ */
        if (!done && (op == 0xc6 || op == 0xc7) && op_pos + 2u <= avail) {
            u8 m = code[op_pos + 1u];
            unsigned mod=m>>6, sub=(m>>3)&7u, rm=m&7u;
            unsigned size = op == 0xc6 ? 1u : (px.op16 ? 2u : 4u);
            if (sub == 0u) {
                if (mod == 3u) {
                    if (op_pos + 2u + size > avail) break;
                    u32 imm = size==1u ? code[op_pos+2u] :
                              (size==2u ? nj_rd16(code+op_pos+2u) : nj_rd32(code+op_pos+2u));
                    if (size==1u) { nj_mov_imm(&e,3,imm); nj_v6_write_r8(&e,rm); }
                    else if (size==2u) { nj_mov_imm(&e,3,imm); nj_v6_write_r16(&e,rm); }
                    else nj_mov_imm(&e,NJ_GUEST_REG(rm),imm);
                    pos=op_pos+2u+size; done=true;
                } else {
                    nj_v6_ea_t ea;
                    if (!nj_v6_decode_ea(code+op_pos+2u,avail-(op_pos+2u),m,
                                         px.addr16,px.seg,&ea)) break;
                    if (!nj_v6_note_seg(cpu,ea.seg,&use_ds,&use_es,&use_ss)) break;
                    unsigned immpos=op_pos+2u+ea.used;
                    if (immpos+size>avail) break;
                    u32 imm=size==1u?code[immpos]:(size==2u?nj_rd16(code+immpos):nj_rd32(code+immpos));
                    nj_v6_emit_ea(cpu,&e,&ea,true);
                    nj_v6_guard_t g; memset(&g,0,sizeof(g));
                    if (!nj_v7_emit_linear_to_phys(cpu,&e,size,true,&g) ||
                        !nj_v6_emit_mem_guard(cpu,&e,size,true,&g)) {
                        e.p=emit_before;e.failed=false;pos=ipos;break;
                    }
                    nj_v6_host_ptr(cpu,&e); nj_mov_imm(&e,1,imm);
                    if (size==1u) nj_strb(&e,1,3,0); else if(size==2u) nj_strh(&e,1,3,0); else nj_str32(&e,1,3,0);
                    if (!nj_v8_finish_guard(&e,&g,&exits,gip,insns)) { e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break; }
                    pos=immpos+size;done=true;
                }
            }
        }

        /* ---- moffs A0-A3 -------------------------------------------- */
        if (!done && op >= 0xa0 && op <= 0xa3) {
            unsigned asz = px.addr16 ? 2u : 4u;
            if (op_pos + 1u + asz > avail) break;

            uword off = asz == 2u ? (uword)nj_rd16(code + op_pos + 1u)
                                  : (uword)nj_rd32(code + op_pos + 1u);
            int seg = px.seg >= 0 ? px.seg : SEG_DS;
            if (!nj_v6_note_seg(cpu, seg, &use_ds, &use_es, &use_ss))
                break;

            nj_v6_ea_t ea = {
                .base = -1, .index = -1, .scale = 0, .disp = (s32)off,
                .seg = seg, .used = asz, .addr16 = px.addr16
            };
            unsigned size = (op == 0xa0 || op == 0xa2) ? 1u :
                            (px.op16 ? 2u : 4u);
            bool write = op == 0xa2 || op == 0xa3;

            nj_v6_emit_ea(cpu, &e, &ea, true);
            nj_v6_guard_t g;
            memset(&g, 0, sizeof(g));
            if (!nj_v7_emit_linear_to_phys(cpu, &e, size, write, &g) ||
                !nj_v6_emit_mem_guard(cpu, &e, size, write, &g)) {
                e.p = emit_before; e.failed = false; pos = ipos;
                break;
            }
            nj_v6_host_ptr(cpu, &e);

            if (op == 0xa0) {
                nj_ldrb(&e, 3, 3, 0);
                nj_v6_write_r8(&e, 0);
            } else if (op == 0xa2) {
                nj_mov_reg(&e, 0, 3);
                nj_v6_read_r8(&e, 0);
                nj_strb(&e, 3, 0, 0);
            } else if (op == 0xa1) {
                if (size == 2u) nj_ldrh(&e, 3, 3, 0);
                else nj_ldr32(&e, 3, 3, 0);
                if (size == 2u) nj_v6_write_r16(&e, 0);
                else nj_mov_reg(&e, NJ_GUEST_REG(0), 3);
            } else {
                if (size == 2u) nj_strh(&e, NJ_GUEST_REG(0), 3, 0);
                else nj_str32(&e, NJ_GUEST_REG(0), 3, 0);
            }

            if (!nj_v8_finish_guard(&e, &g, &exits, gip, insns)) {
                e.p=emit_before; e.failed=false; exits.n=exits_before; pos=ipos; break;
            }
            pos = op_pos + 1u + asz;
            done = true;
        }

        /*
         * Common register/immediate integer instructions use the existing
         * audited v4 emitter, but v6 flushes lazy CC immediately. That frees
         * r1-r3 for the next memory guard and lets a trace span arbitrary MOVs.
         */
        if (!done && px.pos == 0u && (!trace16 || !mixed_v86)) {
            nj_body_info_t bi;
            u16 *before = e.p;
            int n = nj_translate_body_one(cpu, &e, code + pos, avail - pos,
                                          false, &bi, NULL);
            if (n > 0 && !e.failed) {
                if (bi.uses_es_static) use_es = true;
                if (bi.flag_kind == NJ_FLAG_INCDEC) {
                    /* INC/DEC preserves CF. If an earlier native instruction
                     * left CF lazy, stop here so the next chained block can
                     * materialize it before executing INC/DEC. */
                    if (cf_dirty) {
                        e.p = before; e.failed = false;
                        n = 0;
                    } else {
                        needs_refresh_cf = true;
                    }
                } else if (bi.flag_kind != NJ_FLAG_NONE) {
                    cf_dirty = true;
                }
                if (n > 0) {
                    if (bi.flag_kind != NJ_FLAG_NONE)
                        nj_emit_cc_flush(&e, trace16, bi.flag_kind, bi.ccop);
                    pos += (unsigned)n;
                    done = true;
                }
            } else {
                e.p = before;
                e.failed = false;
            }
        }

        if (!done || e.failed) {
            e.p = emit_before;
            e.failed = false;
            exits.n = exits_before;
            pos = ipos;
            break;
        }

        last_ip = gip;
        insns++;
    }

    NJ_V6_STOP[NJ_V6_STOP_TRACES]++;
    if (insns < NJ_V6_MAX_INSNS && pos < avail) {
        NJ_V6_STOP[NJ_V6_STOP_STOPPED]++;
        NJ_V6_STOP[nj_stop_op]++;
    }

    if (!insns || pos == 0u || e.failed)
        return NULL;

    /* V8.2 proved that the new real-mode fallback works, but the M602 run
     * averaged only ~2.4 guest instructions per native entry and was slower
     * than the interpreter. A single-run 16-bit prefix must therefore retire
     * at least six instructions to amortize the C<->Thumb dispatcher and
     * full guest-state prologue/epilogue. V8.3 hardware still averaged only
     * 2.83 retired guest instructions per broad 16-bit entry at a four-insn
     * floor. The measured M602 candidates we actually want are 6/7/7-insn
     * traces, so six rejects the marginal fragments while retaining them.
     * Fully-native v4/v5 loops are not affected by this filter. */
    if (trace16 && insns < 6u)
        return NULL;

    /* Reserve was only for the one shared epilogue; allow it to consume the
     * remainder of the per-block hard limit now that translation is done. */
    e.limit=hard_limit;
    if (!terminal_exit) {
        /* Normal prefix exit: chain resumes at the first untranslated op. */
        continuation_ip = start_ip + pos;
        if (trace16) continuation_ip &= 0xffffu;
        nj_mov_imm(&e,NJ_ITER_REG,insns);
        nj_mov_imm(&e,NJ_BUDGET_REG,continuation_ip);
        nj_mov_imm(&e,2,last_ip);
    }
    u16 *common_exit=e.p;
    nj_v8_emit_shared_exit(&e, mixed_v86 ? false : trace16);
    if(e.failed || !nj_v8_patch_exit_links(&exits,common_exit)) return NULL;

    uword linear = cpu->seg[SEG_CS].base + start_ip;
    nj_block_t *b = nj_cache_insert_slot(cpu, linear);
    memset(b, 0, sizeof(*b));
    b->tag = linear;
    b->mmu_key = nj_mmu_key(cpu);
    b->cs_base = cpu->seg[SEG_CS].base;
    b->ds_base = cpu->seg[SEG_DS].base;
    b->es_base = cpu->seg[SEG_ES].base;
    b->ss_base = cpu->seg[SEG_SS].base;
    b->ds_sel = cpu->seg[SEG_DS].sel;
    b->es_sel = cpu->seg[SEG_ES].sel;
    b->ss_sel = cpu->seg[SEG_SS].sel;
    b->start_ip = start_ip;
    b->branch_ip = last_ip;
    b->fallthrough_ip = continuation_ip;
    b->byte_len = (u16)pos;
    b->phys_page = (u16)phys_page;
    b->code_split = (u16)((code_split && pos > code_split) ? code_split : 0u);
    b->phys_page2 = b->code_split ? (u16)phys_page2 : 0u;
    b->insns = (u8)insns;
    b->code16 = (u8)trace16;
    b->valid = 1u;
    b->needs_refresh_cf = (u8)needs_refresh_cf;
    b->needs_flags_in = 0u;
    b->single_run = 1u;
    b->uses_ds_static = use_ds;
    b->uses_es_static = use_es;
    b->uses_ss_base = use_ss;
    b->uses_ss_static = 0u;
    b->uses_df_static = (u8)use_df;
    b->df_value = df_value;
    b->static_write_count = 0u;
    b->code = e.start;
    b->arm_halfwords = (u16)(e.p - e.start);

    nj_code_ptr = e.p;
    nj_page_mark(phys_page);
    if (b->code_split) nj_page_mark(b->phys_page2);
    nj_bloom_add(linear);
    g_njit_compiles++;
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
    return b;
}

static nj_block_t *nj_compile_loop_v45(CPUI386 *cpu, uword start_ip)
{
    nj_block_t *b = nj_compile_v45_bpstack(cpu, start_ip);
    if (b) return b;

    /* v8.7.3: page-bounded native micro-loops are restricted to VM86+paging
     * inside nj_compile_bytewalk_loop(), so real-mode startup remains exactly
     * on the proven v8.7.2/v8.6 path. */
    b = nj_compile_bytewalk_loop(cpu, start_ip);
    if (b) return b;

    /* Keep fully chained simple loops as the fastest path. */
    b = nj_compile_loop(cpu, start_ip);
    if (b) return b;

    /*
     * Broad v6 fallback: compile as much native prefix as is profitable and
     * side-exit to the interpreter at the first unsupported instruction.
     */
    return nj_compile_v6_trace(cpu, start_ip);
}

/*
 * One add pair per native block entry.  A native entry already costs a
 * cache lookup, a mapping guard and a call into generated code, so this is
 * noise against it: Doom retires ~3.6 M entries per capture, i.e. well
 * under 0.05% of that run.
 */
static inline __attribute__((always_inline))
void nj_diag_note(const nj_block_t *b, u32 done)
{
    nj_block_t *nb = (nj_block_t *)b;
#if NJIT_DIAG
    nb->diag_entries++;
    nb->diag_insns += done;
#endif
}

static int IRAM_ATTR nj_exec_loop(CPUI386 *cpu, const nj_block_t *b, int max_steps)
{
    if (unlikely(!b || !b->valid || b->insns == 0 || max_steps < b->insns))
        return 0;

    /*
     * INC/DEC needs the old CF materialised; v6 branch-entry blocks need all
     * lazy flags materialised before reading cpu->flags directly.
     */
    if (b->needs_refresh_cf || b->needs_flags_in)
        refresh_flags(cpu);

    /* v8.5.1: the exact BP-stack block is the only current block with three
     * static halfword writes.  Under paging, validate its embedded physical
     * pointers here (not in generic lookup) and set PTE dirty bits once before
     * native stores.  A TLB miss/remap simply falls back to the interpreter. */
    if (unlikely((cpu->cr0 & CR0_PG) && b->static_write_count == 3u) &&
        unlikely(!nj_v45_paged_static_valid(cpu, b, true)))
        return 0;

    /*
     * Native static-RAM stores bypass pstore*(), so invalidate only guest code
     * that actually overlaps those words.  This runs once per native entry,
     * not once per loop iteration.
     */
    for (unsigned i = 0; i < b->static_write_count; ++i)
        nj_invalidate_exact_range(b->static_write_phys[i], 2u, b);

    typedef u32 (*nj_func_t)(CPUI386 *, u32);
    nj_func_t fn = (nj_func_t)((uintptr_t)b->code | 1u);

    if (b->single_run) {
        /*
         * v6 prefix traces return the exact number of guest instructions that
         * completed. A memory guard may side-exit before the nominal end.
         */
        u32 done = fn(cpu, 1u);
        nj_xr_note(b, cpu->next_ip, (int)done);
#if NJIT_EXIT_CHECK
        if (unlikely(!nj_exit_sane(cpu, b))) return 0;
#endif
        if (unlikely(done == 0 || done > (u32)b->insns))
            return 0;
        cpu->cycle += done;
        g_njit_native_iters++;
        g_njit_insns += done;
        nj_diag_note(b, done);
        return (int)done;
    }

    unsigned max_iters = (unsigned)max_steps / b->insns;
    if (!max_iters) return 0;

    u32 iters = fn(cpu, max_iters);
    nj_xr_note(b, cpu->next_ip, (int)iters);
#if NJIT_EXIT_CHECK
    if (unlikely(!nj_exit_sane(cpu, b))) return 0;
#endif
    if (unlikely(iters == 0 || iters > max_iters))
        return 0;

    u32 done = iters * (u32)b->insns;
    cpu->cycle += done;
    g_njit_native_iters += iters;
    g_njit_insns += done;
    nj_diag_note(b, done);
    return (int)done;
}

#define NJ_V8_CHAIN_MAX_BLOCKS 32u

/*
 * Continue directly from one single-run v6/v8 prefix into the next cached (or
 * on-demand compiled) native block.  nj_try_execute() is still entered only
 * from a genuinely hot backward branch, so sequential interpreter code keeps
 * the zero-dispatch-tax property of v4.4.
 *
 * Chained blocks intentionally do not increment g_njit_hits: that counter
 * remains "hot-backedge JIT entries", making native_guest_insns / hits a useful
 * measure of how much guest work one dispatcher entry now accomplishes.
 */
static int IRAM_ATTR nj_exec_chain(CPUI386 *cpu, const nj_block_t *first,
                                   int max_steps)
{
    const nj_block_t *b = first;
    int total = 0;
    unsigned n = 0;

    g_njit_ch[NJCH_CALLS]++;

    for (; n < NJ_V8_CHAIN_MAX_BLOCKS && b; ++n) {
        int left = max_steps - total;
        if (left <= 0) { g_njit_ch[NJCH_BRK_BUDGET]++; break; }

        bool may_continue = b->single_run != 0;
        unsigned nominal = b->insns;
        int done = nj_exec_loop(cpu, b, left);
        if (done <= 0) { g_njit_ch[NJCH_BRK_ZERO]++; break; }
        total += done;
        g_njit_ch[NJCH_BLOCKS]++;

        /* A guarded memory side-exit can complete only a prefix of b.  Let
         * the interpreter execute the faulting/missing-TLB instruction once
         * rather than immediately chaining back into another guard.
         *
         * V8_10_DIAG: the three conditions are separated only so the counters
         * can tell them apart.  The break decision is bit-for-bit the same. */
        if (!may_continue) { g_njit_ch[NJCH_BRK_NOT_SINGLE]++; break; }
        if ((unsigned)done != nominal) {
            g_njit_ch[NJCH_BRK_PARTIAL]++;
            g_njit_ch[NJCH_PARTIAL_LOST] += nominal - (unsigned)done;
            {
                uword nip = cpu->next_ip;
                if (cpu->code16) nip &= 0xffffu;
                if (nip >= b->start_ip &&
                    nip < (uword)b->start_ip + b->byte_len) {
                    g_njit_ch[NJCH_PARTIAL_INSIDE]++;
                } else {
                    g_njit_ch[NJCH_PARTIAL_OUTSIDE]++;
                    /* Bloom, not nj_lookup: two bit tests with no side
                     * effects.  A full lookup here would roughly double the
                     * dispatcher's lookup traffic and perturb the very MIPS
                     * figure this build exists to measure.  False positives
                     * make READY a slight over-estimate. */
                    if (nj_bloom_maybe(cpu->seg[SEG_CS].base + nip))
                        g_njit_ch[NJCH_PARTIAL_READY]++;
                }
            }
            break;
        }
        if (total >= max_steps) { g_njit_ch[NJCH_BRK_BUDGET]++; break; }
        if (NJ_SINGLE_STEP_IMPLEMENTED && (cpu->flags & TF)) break;

        uword ip = cpu->next_ip;
        if (cpu->code16) ip &= 0xffffu;
        uword linear = cpu->seg[SEG_CS].base + ip;

        b = nj_lookup(cpu, ip);
        if (b) continue;

        /* This continuation is downstream of a proven-hot backedge, so it is
         * profitable to compile immediately.  Remember unsupported heads in
         * the existing negative hot cache to avoid retrying every iteration. */
        uword key = nj_hot_context_key(cpu, linear);
        nj_hot_t *h = &nj_hot[nj_hash(key, NJ_HOT_BITS)];
        if (h->key == key && h->seen == 0xff &&
            !nj_reject_retry_due(h)) {
            g_njit_ch[NJCH_BRK_NEGCACHE]++;
            b = NULL;
            break;
        }

        b = nj_compile_loop_v45(cpu, ip);
        if (!b) {
            g_njit_ch[NJCH_BRK_COMPILE]++;
            nj_reject(cpu, linear, ip);
            break;
        }
    }

    if (n >= NJ_V8_CHAIN_MAX_BLOCKS) g_njit_ch[NJCH_BRK_MAXBLOCKS]++;

    return total;
}

/*
 * FRANK_NATIVE_JIT_V44_SAMPLED_DISCOVERY
 *
 * v4.3 still paid a direct-mapped negative-cache hash/lookup on every taken
 * backward branch. v4.4 keeps compiled targets fast, but samples discovery for
 * everything else.
 */
/*
 * Deliberately NOT IRAM_ATTR: sampled discovery is reached on one backedge
 * in NJ_DISCOVERY_MASK+1, so executing it from XIP-cached flash costs
 * almost nothing, and the ~620 bytes it returns to .data are what keep
 * nj_try_execute() inlined without crossing the 4 KB boundary described
 * above.
 */
static int nj_try_execute_slow(CPUI386 *cpu, int max_steps,
                               uword ip, uword linear)
{
    g_njit_misses++;

    if (!nj_hot_enough(cpu, linear)) {
        g_njit_hotwait++;
        return 0;
    }

    nj_block_t *b = nj_compile_loop_v45(cpu, ip);
    if (!b) {
        nj_reject(cpu, linear, ip);
        return 0;
    }
    return nj_exec_chain(cpu, b, max_steps);
}

/*
 * always_inline, restored.
 *
 * v8.9.2 made this a single out-of-line RAM function to claw back .data,
 * and it cost real throughput: the deterministic Symantec real-mode
 * control went 345 -> 322 points and its reported CPU clock 59 -> 49 MHz.
 * A call and return on a taken backedge is only ~0.2% of the window, so
 * that is not the cost - what is, is that cpu_exec1() must now spill its
 * dispatch-loop state around 13 call sites in its hottest loop.
 *
 * The .data budget is paid for in nj_try_execute_slow() instead, which is
 * reached on one backedge in 256 and is therefore the right place to give
 * up RAM residency.  The note below is kept because the cliff it
 * describes is still there.
 *
 * Previously:
 *
 * cpu_exec1() is __not_in_flash(), so all 13 NJ_HOT_BACKEDGE expansions of
 * this function were sitting in .data, and .data ended 8 bytes below the
 * 4 KB boundary .bss is aligned to.  That made any growth here cost 4096
 * bytes of link padding out of the malloc heap, which is what made v8.9
 * panic with "Out of memory" in pc_new() and never bring HDMI up.
 *
 * One RAM-resident copy instead of thirteen removes that cliff entirely.
 * The cost is a call and return on a taken backward branch only, against
 * roughly 265 host cycles per guest instruction on this board - far below
 * the noise floor of the measurements this is used for.
 */
static inline __attribute__((always_inline))
int nj_try_execute(CPUI386 *cpu, int max_steps)
{
    /* See FRANK_TF_V88.  An inert TF used to disable the JIT entirely
     * under EMM386; mode_flags in the stats dump still shows whether the
     * guest is leaving TF set. */
#if NJ_SINGLE_STEP_IMPLEMENTED
    if (unlikely(cpu->flags & TF)) return 0;
#endif

    /*
     * Keep the shared guard trampolines paired with the mode they were built
     * for, before any compiled block can call them.
     *
     * They bake in the paging mode and the CPL, which is safe per block
     * because nj_block_matches() compares mmu_key - but the trampolines are
     * global.  With EMM386 the guest crosses between real mode and V86
     * constantly, and an old block becoming valid again would otherwise call
     * trampolines built for the other mode: a TLB walk over a real-mode
     * address, a wild pointer, and a guest that triple-faults into a boot
     * loop.  That is exactly what happened the first time this was flashed.
     *
     * Refreshing here is enough: a block cannot change the mode itself (no
     * far jump or MOV CR0 is ever compiled), so the key cannot move underneath
     * a block that has already started.
     */
#if NJIT_EXIT_CHECK
    if (unlikely(nj_disabled)) return 0;
#endif
    nj_guards_refresh(cpu);

    uword ip = cpu->next_ip;
    if (cpu->code16) ip &= 0xffffu;

    const uword linear = cpu->seg[SEG_CS].base + ip;

    /*
     * Full cache lookup only when both 128-bit bloom probes match. This is a cheap
     * positive filter; stale bits are harmless because the cache entry is
     * still fully validated below.
     */
    if (unlikely(nj_bloom_maybe(linear))) {
        nj_block_t *b = nj_lookup_linear(cpu, linear);
        if (likely(b)) {
            g_njit_hits++;
            return nj_exec_chain(cpu, b, max_steps);
        }
    }

    /*
     * Discovery is deliberately sparse. A truly hot loop still reaches the
     * two-step threshold after roughly 768 executions, while unsupported
     * backedges pay almost no JIT tax.
     */
    if (likely(((++nj_discovery_tick) & NJ_DISCOVERY_MASK) != 0u))
        return 0;

    return nj_try_execute_slow(cpu, max_steps, ip, linear);
}

#endif /* NATIVE_JIT */

static bool IRAM_ATTR_CPU_EXEC1 cpu_exec1(CPUI386 *cpu, int stepcount)
{
#ifndef I386_OPT2
#define eswitch(b) switch(b)
#define ecase(a)   case a
#define ebreak     break
#define edefault   default
#define default_ud cpu_debug(cpu); THROW0(EX_UD)
#undef CX
#define CX(_1) case _1:
#else
#define eswitch(b)
#define ecase(a)   f ## a
#define ebreak     continue
#define edefault   f0xf1
#define default_ud THROW0(EX_UD)
#undef CX
#define CX(_1) f ## _1:
#endif

	u8 b1;
	u8 modrm;
	OptAddr meml;
	uword addr;
	for (; stepcount > 0; ) {
#if BLOCK_JIT
	int bj_done = bj_try_execute(cpu, stepcount);
	if (likely(bj_done > 0)) {
		stepcount -= bj_done;
		continue;
	}
#endif
	stepcount--;
	bool code16 = cpu->code16;
	uword sp_mask = cpu->sp_mask;

	if (code16) cpu->next_ip &= 0xffff;
	cpu->ip = cpu->next_ip;
	frank_diag_ip(cpu->ip, cpu->seg[SEG_CS].base, REGi(4) & (uint32_t)cpu->sp_mask);
	frank_diag_shadow(cpu->phys_mem);
#if BB_PROFILE
	{ static uint32_t bb_prev; bb_note(cpu->ip, bb_prev); bb_prev = cpu->ip; }
#endif
	TRY(fetch8(cpu, &b1));
#if DEBUG_CPU
	opcode = b1;
#endif
	cpu->cycle++;

#ifndef I386_OPT1
	if (verbose) {
		cpu_debug(cpu);
	}
#endif
	// prefix
	bool opsz16 = code16;
	bool adsz16 = code16;
	int rep = 0;
	/*bool lock = false;*/
	int curr_seg = -1;
#ifndef I386_OPT2
	for (;;) {
#define HANDLE_PREFIX(C, STMT) \
		if (b1 == C) { \
			STMT; \
			TRY(fetch8(cpu, &b1)); \
			continue; \
		}
		HANDLE_PREFIX(0x26, curr_seg = SEG_ES)
		HANDLE_PREFIX(0x2e, curr_seg = SEG_CS)
		HANDLE_PREFIX(0x36, curr_seg = SEG_SS)
		HANDLE_PREFIX(0x3e, curr_seg = SEG_DS)
		HANDLE_PREFIX(0x64, curr_seg = SEG_FS)
		HANDLE_PREFIX(0x65, curr_seg = SEG_GS)
		HANDLE_PREFIX(0x66, opsz16 = !code16)
		HANDLE_PREFIX(0x67, adsz16 = !code16)
		HANDLE_PREFIX(0xf3, rep = 1) // REP
		HANDLE_PREFIX(0xf2, rep = 2) // REPNE
		HANDLE_PREFIX(0xf0, /*lock = true*/)
#undef HANDLE_PREFIX
		break;
	}
#else
	static const void *pfxlabel[] __not_in_flash("pfxlabel") = {
/* 0x00 */	&&f0x00, &&f0x01, &&f0x02, &&f0x03, &&f0x04, &&f0x05, &&f0x06, &&f0x07,
/* 0x08 */	&&f0x08, &&f0x09, &&f0x0a, &&f0x0b, &&f0x0c, &&f0x0d, &&f0x0e, &&f0x0f,
/* 0x10 */	&&f0x10, &&f0x11, &&f0x12, &&f0x13, &&f0x14, &&f0x15, &&f0x16, &&f0x17,
/* 0x18 */	&&f0x18, &&f0x19, &&f0x1a, &&f0x1b, &&f0x1c, &&f0x1d, &&f0x1e, &&f0x1f,
/* 0x20 */	&&f0x20, &&f0x21, &&f0x22, &&f0x23, &&f0x24, &&f0x25, &&pfx26, &&f0x27,
/* 0x28 */	&&f0x28, &&f0x29, &&f0x2a, &&f0x2b, &&f0x2c, &&f0x2d, &&pfx2e, &&f0x2f,
/* 0x30 */	&&f0x30, &&f0x31, &&f0x32, &&f0x33, &&f0x34, &&f0x35, &&pfx36, &&f0x37,
/* 0x38 */	&&f0x38, &&f0x39, &&f0x3a, &&f0x3b, &&f0x3c, &&f0x3d, &&pfx3e, &&f0x3f,
/* 0x40 */	&&f0x40, &&f0x41, &&f0x42, &&f0x43, &&f0x44, &&f0x45, &&f0x46, &&f0x47,
/* 0x48 */	&&f0x48, &&f0x49, &&f0x4a, &&f0x4b, &&f0x4c, &&f0x4d, &&f0x4e, &&f0x4f,
/* 0x50 */	&&f0x50, &&f0x51, &&f0x52, &&f0x53, &&f0x54, &&f0x55, &&f0x56, &&f0x57,
/* 0x58 */	&&f0x58, &&f0x59, &&f0x5a, &&f0x5b, &&f0x5c, &&f0x5d, &&f0x5e, &&f0x5f,
/* 0x60 */	&&f0x60, &&f0x61, &&f0x62, &&f0x63, &&pfx64, &&pfx65, &&pfx66, &&pfx67,
/* 0x68 */	&&f0x68, &&f0x69, &&f0x6a, &&f0x6b, &&f0x6c, &&f0x6d, &&f0x6e, &&f0x6f,
/* 0x70 */	&&f0x70, &&f0x71, &&f0x72, &&f0x73, &&f0x74, &&f0x75, &&f0x76, &&f0x77,
/* 0x78 */	&&f0x78, &&f0x79, &&f0x7a, &&f0x7b, &&f0x7c, &&f0x7d, &&f0x7e, &&f0x7f,
/* 0x80 */	&&f0x80, &&f0x81, &&f0x82, &&f0x83, &&f0x84, &&f0x85, &&f0x86, &&f0x87,
/* 0x88 */	&&f0x88, &&f0x89, &&f0x8a, &&f0x8b, &&f0x8c, &&f0x8d, &&f0x8e, &&f0x8f,
/* 0x90 */	&&f0x90, &&f0x91, &&f0x92, &&f0x93, &&f0x94, &&f0x95, &&f0x96, &&f0x97,
/* 0x98 */	&&f0x98, &&f0x99, &&f0x9a, &&f0x9b, &&f0x9c, &&f0x9d, &&f0x9e, &&f0x9f,
/* 0xa0 */	&&f0xa0, &&f0xa1, &&f0xa2, &&f0xa3, &&f0xa4, &&f0xa5, &&f0xa6, &&f0xa7,
/* 0xa8 */	&&f0xa8, &&f0xa9, &&f0xaa, &&f0xab, &&f0xac, &&f0xad, &&f0xae, &&f0xaf,
/* 0xb0 */	&&f0xb0, &&f0xb1, &&f0xb2, &&f0xb3, &&f0xb4, &&f0xb5, &&f0xb6, &&f0xb7,
/* 0xb8 */	&&f0xb8, &&f0xb9, &&f0xba, &&f0xbb, &&f0xbc, &&f0xbd, &&f0xbe, &&f0xbf,
/* 0xc0 */	&&f0xc0, &&f0xc1, &&f0xc2, &&f0xc3, &&f0xc4, &&f0xc5, &&f0xc6, &&f0xc7,
/* 0xc8 */	&&f0xc8, &&f0xc9, &&f0xca, &&f0xcb, &&f0xcc, &&f0xcd, &&f0xce, &&f0xcf,
/* 0xd0 */	&&f0xd0, &&f0xd1, &&f0xd2, &&f0xd3, &&f0xd4, &&f0xd5, &&f0xd6, &&f0xd7,
/* 0xd8 */	&&f0xd8, &&f0xd9, &&f0xda, &&f0xdb, &&f0xdc, &&f0xdd, &&f0xde, &&f0xdf,
/* 0xe0 */	&&f0xe0, &&f0xe1, &&f0xe2, &&f0xe3, &&f0xe4, &&f0xe5, &&f0xe6, &&f0xe7,
/* 0xe8 */	&&f0xe8, &&f0xe9, &&f0xea, &&f0xeb, &&f0xec, &&f0xed, &&f0xee, &&f0xef,
/* 0xf0 */	&&pfxf0, &&f0xf1, &&pfxf2, &&pfxf3, &&f0xf4, &&f0xf5, &&f0xf6, &&f0xf7,
/* 0xf8 */	&&f0xf8, &&f0xf9, &&f0xfa, &&f0xfb, &&f0xfc, &&f0xfd, &&f0xfe, &&f0xff,
	};
	goto *pfxlabel[b1];
#define HANDLE_PREFIX(C, STMT) \
		pfx ## C: { \
			STMT; \
			TRY(fetch8(cpu, &b1)); \
			goto *pfxlabel[b1]; \
		}
		HANDLE_PREFIX(26, curr_seg = SEG_ES)
		HANDLE_PREFIX(2e, curr_seg = SEG_CS)
		HANDLE_PREFIX(36, curr_seg = SEG_SS)
		HANDLE_PREFIX(3e, curr_seg = SEG_DS)
		HANDLE_PREFIX(64, curr_seg = SEG_FS)
		HANDLE_PREFIX(65, curr_seg = SEG_GS)
		HANDLE_PREFIX(66, opsz16 = !code16)
		HANDLE_PREFIX(67, adsz16 = !code16)
		HANDLE_PREFIX(f3, rep = 1) // REP
		HANDLE_PREFIX(f2, rep = 2) // REPNE
		HANDLE_PREFIX(f0, /*lock = true*/)
#undef HANDLE_PREFIX
#endif
	eswitch(b1) {
#define I(_case, _rm, _rwm, _op) _case { _rm(_rwm, _op); ebreak; }
#include "i386ins.def"
#undef I

#undef CX
#define CX(_1) case _1:
#define GRPBEG TRY(peek8(cpu, &modrm)); switch((modrm >> 3) & 7) {
#define GRPCASE(_case, _rm, _rwm, _op) _case { _rm(_rwm, _op); ebreak; }
#define GRPEND default: default_ud; } ebreak;

	ecase(0x80): ecase(0x82): { // G1b
GRPBEG
#define IG1b GRPCASE
#include "i386ins.def"
#undef IG1b
GRPEND
	}

	ecase(0x81): { // G1v
GRPBEG
#define IG1v GRPCASE
#include "i386ins.def"
#undef IG1v
GRPEND
	}

	ecase(0x83): { // G1vIb
GRPBEG
#define IG1vIb GRPCASE
#include "i386ins.def"
#undef IG1vIb
GRPEND
	}

	ecase(0xc0): { // G2b
GRPBEG
#define IG2b GRPCASE
#include "i386ins.def"
#undef IG2b
GRPEND
	}

	ecase(0xc1): { // G2v
GRPBEG
#define IG2v GRPCASE
#include "i386ins.def"
#undef IG2v
GRPEND
	}

	ecase(0xd0): { // G2b1
GRPBEG
#define IG2b1 GRPCASE
#include "i386ins.def"
#undef IG2b1
GRPEND
	}

	ecase(0xd1): { // G2v1
GRPBEG
#define IG2v1 GRPCASE
#include "i386ins.def"
#undef IG2v1
GRPEND
	}

	ecase(0xd2): { // G2bC
GRPBEG
#define IG2bC GRPCASE
#include "i386ins.def"
#undef IG2bC
GRPEND
	}

	ecase(0xd3): { // G2v1
GRPBEG
#define IG2vC GRPCASE
#include "i386ins.def"
#undef IG2vC
GRPEND
	}

	ecase(0xf6): { // G3b
GRPBEG
#define IG3b GRPCASE
#include "i386ins.def"
#undef IG3b
GRPEND
	}

	ecase(0xf7): { // G3v
GRPBEG
#define IG3v GRPCASE
#include "i386ins.def"
#undef IG3v
GRPEND
	}

	ecase(0xfe): { // G4
GRPBEG
#define IG4 GRPCASE
#include "i386ins.def"
#undef IG4
GRPEND
	}

	ecase(0xff): { // G5
GRPBEG
#define IG5 GRPCASE
#include "i386ins.def"
#undef IG5
GRPEND
	}

	ecase(0x0f): { // two byte
		TRY(fetch8(cpu, &b1));
		switch(b1) {
#define I2(_case, _rm, _rwm, _op) _case { _rm(_rwm, _op); ebreak; }
#include "i386ins.def"
#undef I2

		case 0x00: { // G6
GRPBEG
#define IG6 GRPCASE
#include "i386ins.def"
#undef IG6
GRPEND
		}

		case 0x01: { // G7
GRPBEG
#define IG7 GRPCASE
#include "i386ins.def"
#undef IG7
GRPEND
		}

		case 0xba: { // G8
GRPBEG
#define IG8 GRPCASE
#include "i386ins.def"
#undef IG8
GRPEND
		}

		case 0xc7: { // G9
GRPBEG
#define IG9 GRPCASE
#include "i386ins.def"
#undef IG9
GRPEND
		}
		default: default_ud;
		}
		ebreak;
	}

	edefault: default_ud;
	}
	}
	return true;
}

// XXX: incomplete
enum { TS_JMP, TS_CALL, TS_IRET };
static bool task_switch(CPUI386 *cpu, int tss, int sw_type)
{
	OptAddr meml;
	int oldtss = cpu->seg[SEG_TR].sel;
	int tr_type = cpu->seg[SEG_TR].flags & 0xf;
	assert (tr_type == 9 || tr_type == 11);

	TRY1(translate(cpu, &meml, 2, SEG_TR, 0x20, 4, 0));
	store32(cpu, &meml, cpu->next_ip);

	refresh_flags(cpu);
	TRY1(translate(cpu, &meml, 2, SEG_TR, 0x24, 4, 0));
	if (sw_type == TS_IRET)
		store32(cpu, &meml, cpu->flags & ~NT);
	else
		store32(cpu, &meml, cpu->flags);

	for (int i = 0; i < 8; i++) {
		TRY1(translate(cpu, &meml, 2, SEG_TR, 0x28 + 4 * i, 4, 0));
		store32(cpu, &meml, REGi(i));
	}

	for (int i = 0; i < 6; i++) {
		TRY1(translate(cpu, &meml, 2, SEG_TR, 0x48 + 4 * i, 4, 0));
		store32(cpu, &meml, cpu->seg[i].sel);
	}

	// clear busy bit
	if (sw_type == TS_JMP || sw_type == TS_IRET) {
		uword addr = cpu->gdt.base + (cpu->seg[SEG_TR].sel & ~0x7);
		TRY1(translate_laddr(cpu, &meml, 3, addr + 4, 4, 0));
		store32(cpu, &meml, load32(cpu, &meml) & ~(1 << 9));
	}

	TRY1(set_seg(cpu, SEG_TR, tss));
	int new_tr_type = cpu->seg[SEG_TR].flags & 0xf;
	assert(new_tr_type == 9 || new_tr_type == 11);

	// set busy bit
	if (sw_type == TS_JMP || sw_type == TS_CALL) {
		uword addr = cpu->gdt.base + (tss & ~0x7);
		TRY1(translate_laddr(cpu, &meml, 3, addr + 4, 4, 0));
		store32(cpu, &meml, load32(cpu, &meml) | (1 << 9));
		cpu->seg[SEG_TR].flags |= 2;
	}

	cpu->cr0 |= 1 << 3; // set TS bit

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x60, 4, 0));
	TRY1(set_seg(cpu, SEG_LDT, load32(cpu, &meml)));

	for (int i = 0; i < 8; i++) {
		TRY1(translate(cpu, &meml, 1, SEG_TR, 0x28 + 4 * i, 4, 0));
		REGi(i) = load32(cpu, &meml);
	}

	for (int i = 0; i < 6; i++) {
		TRY1(translate(cpu, &meml, 1, SEG_TR, 0x48 + 4 * i, 4, 0));
		TRY1(set_seg(cpu, i, load32(cpu, &meml)));
	}

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x20, 4, 0));
	cpu->next_ip = load32(cpu, &meml); cpu->prefetch_base = (u32)-1;

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x24, 4, 0));
	cpu->flags = load32(cpu, &meml);
	cpu->flags &= EFLAGS_MASK;
	cpu->flags |= 0x2;
	if (sw_type == TS_CALL) {
		TRY1(translate(cpu, &meml, 2, SEG_TR, 0, 4, 0));
		store32(cpu, &meml, oldtss);
		cpu->flags |= NT;
	}

	TRY1(translate(cpu, &meml, 1, SEG_TR, 0x1c, 4, 0));
	cpu->cr3 = load32(cpu, &meml);
	tlb_clear(cpu);

	return true;
}

static bool pmcall(CPUI386 *cpu, bool opsz16, uword addr, int sel, bool isjmp)
{
	sel = sel & 0xffff;
	uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;

	if ((sel & ~0x3) == 0) THROW(EX_GP, 0);

	uword w1, w2;
	TRY(read_desc(cpu, sel, &w1, &w2));

	int s = (w2 >> 12) & 1;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;
	if (!p) {
//		dolog("pmcall: seg not present %04x\n", sel);
		THROW(EX_NP, sel & ~0x3);
	}

	if (s) {
		bool code = (w2 >> 8) & 0x8;
		bool conforming = (w2 >> 8) & 0x4;
		if (!code) THROW(EX_GP, sel & ~0x3);
		if (conforming) {
			// call conforming code segment
			if (dpl > cpu->cpl) THROW(EX_GP, sel & ~0x3);
			sel = (sel & 0xfffc) | cpu->cpl;
		} else {
			// call nonconforming code segment
			if ((sel & 0x3) > cpu->cpl || dpl != cpu->cpl)
				THROW(EX_GP, sel & ~0x3);
			sel = (sel & 0xfffc) | cpu->cpl;
		}

		if (!isjmp) {
			OptAddr meml1, meml2;
			uword sp = lreg32(4);
			if (opsz16) {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2) & sp_mask, 2, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4) & sp_mask, 2, 0));
				set_sp(sp - 4, sp_mask);
				saddr16(&meml1, cpu->seg[SEG_CS].sel);
				saddr16(&meml2, cpu->next_ip);
			} else {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4) & sp_mask, 4, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 8) & sp_mask, 4, 0));
				set_sp(sp - 8, sp_mask);
				saddr32(&meml1, cpu->seg[SEG_CS].sel);
				saddr32(&meml2, cpu->next_ip);
			}
		}
//		if ((sel & 3) != cpu->cpl)
//			dolog("pmcall PVL %d => %d\n", cpu->cpl, sel & 3);
		TRY1(set_seg(cpu, SEG_CS, sel));
		cpu->next_ip = addr; cpu->prefetch_base = (u32)-1;
	} else {
		int newcs = w1 >> 16;
		uword newip = (w1 & 0xffff) | (w2 & 0xffff0000);
		int gt = (w2 >> 8) & 0xf;
		int wc = w2 & 31;

		if (dpl < cpu->cpl || dpl < (sel & 3))
			THROW(EX_GP, sel & ~0x3);

		// only 32bit TSS is supported now
		int tr_type = cpu->seg[SEG_TR].flags & 0xf;
		if (tr_type == 9 || tr_type == 11) {
			if (gt == 9) {
				// 32 bit TSS avail segs
				return task_switch(cpu, sel,
						   isjmp ? TS_JMP : TS_CALL);
			}

			if (gt == 5) {
				// task gates
				return task_switch(cpu, newcs,
						   isjmp ? TS_JMP : TS_CALL);
			}
		}

		if (gt != 4 && gt != 12) {
			fprintf(stderr, "gate type = %d\n", gt);
			cpu_abort(cpu, -203);
		}

		// call gates
		// examine code segment selector in call gate descriptor
		if ((newcs & ~0x3) == 0) THROW(EX_GP, 0);
		uword neww2;
		TRY(read_desc(cpu, newcs, NULL, &neww2));

		// if not code segment
		if (((neww2 >> 11) & 0x3) != 0x3)
			THROW(EX_GP, newcs & ~0x3);

		int newdpl = (neww2 >> 13) & 0x3;
		int newp = (neww2 >> 15) & 1;
		if (!newp) THROW(EX_NP, newcs & ~0x3);
		if (newdpl > cpu->cpl) THROW(EX_GP, newcs & ~0x3);

		bool conforming = (neww2 >> 8) & 0x4;
		bool gate16 = (gt == 4);
		if (!conforming && newdpl < cpu->cpl) {
			// more privilege
			OptAddr msp0, mss0;
			uword oldss = cpu->seg[SEG_SS].sel;
			uword oldsp = REGi(4);
			uword params[31];
			uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;

			if (!gate16) {
				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY(translate(cpu, &meml, 1, SEG_SS, (oldsp + 4 * i) & sp_mask, 4, 0));
					params[i] = laddr32(&meml);
				}
			} else {
				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY(translate(cpu, &meml, 1, SEG_SS, (oldsp + 2 * i) & sp_mask, 2, 0));
					params[i] = laddr16(&meml);
				}
			}

			if (!(cpu->seg[SEG_TR].flags & 0x8)) {
				TRY(translate(cpu, &msp0, 1, SEG_TR, 2 + 4 * newdpl, 2, 0));
				TRY(translate(cpu, &mss0, 1, SEG_TR, 4 + 4 * newdpl, 2, 0));
				// TODO: Check SS...
				REGi(4) = load16(cpu, &msp0);
				TRY(set_seg(cpu, SEG_SS, load16(cpu, &mss0)));
			} else {
				TRY(translate(cpu, &msp0, 1, SEG_TR, 4 + 8 * newdpl, 4, 0));
				TRY(translate(cpu, &mss0, 1, SEG_TR, 8 + 8 * newdpl, 4, 0));
				// TODO: Check SS...
				REGi(4) = load32(cpu, &msp0);
				TRY(set_seg(cpu, SEG_SS, load32(cpu, &mss0)));
			}
			sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;

			if (!isjmp) {
			if (!gate16) {
				OptAddr meml1, meml2, meml3, meml4;
				uword sp = lreg32(4);
				TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
				TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
				TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * (3 + wc)) & sp_mask, 4, 0));
				TRY1(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * (4 + wc)) & sp_mask, 4, 0));

				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY1(translate(cpu, &meml, 2, SEG_SS, (sp - 4 * (2 + wc - i)) & sp_mask, 4, 0));
					saddr32(&meml, params[i]);
				}

				saddr32(&meml1, oldss);
				saddr32(&meml2, oldsp);
				saddr32(&meml3, cpu->seg[SEG_CS].sel);
				saddr32(&meml4, cpu->next_ip);
				set_sp(sp - 4 * (4 + wc), sp_mask);
			} else {
				OptAddr meml1, meml2, meml3, meml4;
				uword sp = lreg32(4);
				TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
				TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
				TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * (3 + wc)) & sp_mask, 2, 0));
				TRY1(translate(cpu, &meml4, 2, SEG_SS, (sp - 2 * (4 + wc)) & sp_mask, 2, 0));

				for (int i = 0; i < wc; i++) {
					OptAddr meml;
					TRY1(translate(cpu, &meml, 2, SEG_SS, (sp - 2 * (2 + wc - i)) & sp_mask, 2, 0));
					saddr16(&meml, params[i]);
				}

				saddr16(&meml1, oldss);
				saddr16(&meml2, oldsp);
				saddr16(&meml3, cpu->seg[SEG_CS].sel);
				saddr16(&meml4, cpu->next_ip);
				set_sp(sp - 2 * (4 + wc), sp_mask);
			}
			}
			newcs = (newcs & 0xfffc) | newdpl;
		} else {
			// same privilege
			if (!isjmp) {
			OptAddr meml1, meml2;
			uword sp = lreg32(4);
			uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
			if (gate16) {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
				saddr16(&meml1, cpu->seg[SEG_CS].sel);
				saddr16(&meml2, cpu->next_ip);
				set_sp(sp - 2 * 2, sp_mask);
			} else {
				TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
				TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
				saddr32(&meml1, cpu->seg[SEG_CS].sel);
				saddr32(&meml2, cpu->next_ip);
				set_sp(sp - 4 * 2, sp_mask);
			}
			}
			newcs = (newcs & 0xfffc) | cpu->cpl;
		}

		TRY1(set_seg(cpu, SEG_CS, newcs));

		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
	}
	return true;
}

// 0: exception
// 1: intra PVL
// 2: inter PVL
// 3: from v8086
static int __not_in_flash_func(__call_isr_check_cs)(CPUI386 *cpu, int sel, int ext, int *csdpl)
{
	sel = sel & 0xffff;
	OptAddr meml;
	uword off = sel & ~0x7;
	uword base;
	uword limit;
	if (sel & 0x4) {
		base = cpu->seg[SEG_LDT].base;
		limit = cpu->seg[SEG_LDT].limit;
	} else {
		base = cpu->gdt.base;
		limit = cpu->gdt.limit;
	}
	if ((sel & ~0x3) == 0 || off + 7 > limit) {
		dolog("__call_isr_check_cs null/limit: sel=%04x off=%x limit=%x\n", sel, off, limit);
    	THROW(EX_GP, ext);
	}

	TRY1(translate_laddr(cpu, &meml, 1, base + off + 4, 4, 0));
	uword w2 = load32(cpu, &meml);
	int s = (w2 >> 12) & 1;
	bool code = (w2 >> 8) & 0x8;
	bool conforming = (w2 >> 8) & 0x4;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;
	*csdpl = dpl;
	if (!s || !code || dpl > cpu->cpl) {
	    dolog("__call_isr_check_cs: sel=%04x s=%d code=%d dpl=%d cpl=%d ext=%d w2=%08x\n",
    	      sel, s, code, dpl, cpu->cpl, ext, w2);
		THROW(EX_GP, (sel & ~0x3) | ext);
	}

	if (!p) THROW(EX_NP, sel & ~0x3);

	if (!conforming && dpl < cpu->cpl) {
		if (!(cpu->flags & VM)) {
			return 2;
		} else {
			if (dpl != 0) {
				dolog("__call_isr_check_cs fail1: %d %d %d\n", conforming, dpl, cpu->cpl);
				THROW(EX_GP, (sel & ~0x3) | ext);
			} else {
				return 3;
			}
		}
	} else {
		if (cpu->flags & VM) {
			THROW(EX_GP, (sel & ~0x3) | ext);
		} else {
			if (conforming || dpl == cpu->cpl) {
				return 1;
			} else {
				dolog("__call_isr_check_cs fail2: %d %d %d\n", conforming, dpl, cpu->cpl);
				THROW(EX_GP, (sel & ~0x3) | ext);
			}
		}
	}
	__builtin_unreachable();
}

static bool IRAM_ATTR call_isr(CPUI386 *cpu, int no, bool pusherr, int ext)
{
	/* INT 2Fh network-attached-drive handler hook - intercept in V86 and real mode */
	if (no == 0x2F && cpu->int2f_handler && (!(cpu->cr0 & 1) || (cpu->flags & VM))) {
		if (cpu->int2f_handler(cpu, cpu->int2f_opaque)) return true; /* handled */
	}
	#if DEBUG_CPU
	if (cpu->flags & VM && no >= 0x20) {
		dolog("V86 INT %02xh\n", no);
	}
	#endif
	if (!(cpu->cr0 & 1)) {
		/* REAL-ADDRESS-MODE */
		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		OptAddr meml;
		uword base = cpu->idt.base;
		int off = no * 4;
		TRY1(translate_laddr(cpu, &meml, 1, base + off, 4, 0));
		uword w1 = load32(cpu, &meml);
		int newcs = w1 >> 16;
		uword newip = w1 & 0xffff;

		OptAddr meml1, meml2, meml3;
		uword sp = lreg32(4);
		TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
		TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
		TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * 3) & sp_mask, 2, 0));
		refresh_flags(cpu);
		cpu->cc.mask = 0;
		saddr16(&meml1, cpu->flags);
		saddr16(&meml2, cpu->seg[SEG_CS].sel);
		saddr16(&meml3, cpu->ip);
		sreg32(4, (sp - 2 * 3) & sp_mask);

		TRY1(set_seg(cpu, SEG_CS, newcs));
		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		cpu->ip = newip;
		cpu->flags &= ~(IF|TF);
		return true;
	}

	/* PROTECTED-MODE */
	OptAddr meml;
	uword base = cpu->idt.base;
	int off = no << 3;
	if (off + 7 > cpu->idt.limit) {
		dolog("call_isr error0 %d %d\n", off, cpu->idt.limit);
		THROW(EX_GP, off | 2 | ext);
	}

	TRY1(translate_laddr(cpu, &meml, 1, base + off, 4, 0));
	uword w1 = load32(cpu, &meml);
	TRY1(translate_laddr(cpu, &meml, 1, base + off + 4, 4, 0));
	uword w2 = load32(cpu, &meml);

	int gt = (w2 >> 8) & 0xf;
	if (gt != 6 && gt != 7 && gt != 0xe && gt != 0xf && gt != 5) {
//		dolog("call_isr error1 gt=%d\n", gt);
		THROW(EX_GP, off | 2 | ext);
	}

	int dpl = (w2 >> 13) & 0x3;
	if (!ext && dpl < cpu->cpl) THROW(EX_GP, off | 2);

	int p = (w2 >> 15) & 1;
	if (!p) {
		dolog("call_isr error3\n");
		THROW(EX_NP, off | 2 | ext);
	}

	/* task gate */
	if (gt == 5)
		return task_switch(cpu, w1 >> 16, TS_CALL);

	/* TRAP-OR-INTERRUPT-GATE */
	int newcs = w1 >> 16;
	uword newip = (w1 & 0xffff) | (w2 & 0xffff0000);
	bool gate16 = gt == 6 || gt == 7;

	int csdpl;
	switch(__call_isr_check_cs(cpu, newcs, ext, &csdpl)) {
	case 0: {
		return false;
	}
	case 1: /* intra PVL */ {
		OptAddr meml1, meml2, meml3, meml4;
		uword sp = lreg32(4);
		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		if (gate16) {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * 3) & sp_mask, 2, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 2 * 4) & sp_mask, 2, 0));
			}

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr16(&meml1, cpu->flags);

			saddr16(&meml2, cpu->seg[SEG_CS].sel);
			saddr16(&meml3, cpu->ip);
			if (pusherr) {
				saddr16(&meml4, cpu->excerr);
				dolog("EX intra PVL G16 INT %02xh %04x:%04x err=%04x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				set_sp(sp - 2 * 4, sp_mask);
			} else {
				set_sp(sp - 2 * 3, sp_mask);
			}
		} else {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * 3) & sp_mask, 4, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * 4) & sp_mask, 4, 0));
			}

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr32(&meml1, cpu->flags);

			saddr32(&meml2, cpu->seg[SEG_CS].sel);
			saddr32(&meml3, cpu->ip);
			if (pusherr) {
				saddr32(&meml4, cpu->excerr);
				dolog("EX intra PVL INT %02xh %04x:%08x err=%08x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				set_sp(sp - 4 * 4, sp_mask);
			} else {
				set_sp(sp - 4 * 3, sp_mask);
			}
		}
		newcs = (newcs & (~3)) | cpu->cpl;
		break;
	}
	case 2: /* inter PVL */ {
//		dolog("call_isr %d %x PVL %d => %d\n", no, no, cpu->cpl, csdpl);
		OptAddr msp0, mss0;
		int newpl = csdpl;
		uword oldss = cpu->seg[SEG_SS].sel;
		uword oldsp = REGi(4);
		uword newss, newsp;
		if (cpu->seg[SEG_TR].flags & 0x8) {
			TRY(translate(cpu, &msp0, 1, SEG_TR, 4 + 8 * newpl, 4, 0));
			TRY(translate(cpu, &mss0, 1, SEG_TR, 8 + 8 * newpl, 4, 0));
			newsp = load32(cpu, &msp0);
			newss = load32(cpu, &mss0) & 0xffff;
		} else {
			TRY(translate(cpu, &msp0, 1, SEG_TR, 2 + 4 * newpl, 2, 0));
			TRY(translate(cpu, &mss0, 1, SEG_TR, 4 + 4 * newpl, 2, 0));
			newsp = load16(cpu, &msp0);
			newss = load16(cpu, &mss0);
		}

		REGi(4) = newsp;
		TRY(set_seg(cpu, SEG_SS, newss));
		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		OptAddr meml1, meml2, meml3, meml4, meml5, meml6;
		uword sp = lreg32(4);
		if (gate16) {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 2 * 1) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 2 * 2) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 2 * 3) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 2 * 4) & sp_mask, 2, 0));
			TRY(translate(cpu, &meml5, 2, SEG_SS, (sp - 2 * 5) & sp_mask, 2, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml6, 2, SEG_SS, (sp - 2 * 6) & sp_mask, 2, 0));
			}
			saddr16(&meml1, oldss);
			saddr16(&meml2, oldsp);

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr16(&meml3, cpu->flags);

			saddr16(&meml4, cpu->seg[SEG_CS].sel);
			saddr16(&meml5, cpu->ip);
			if (pusherr) {
				saddr16(&meml6, cpu->excerr);
				dolog("EX inter PVL G16 INT %02xh %04x:%04x err=%04x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				set_sp(sp - 2 * 6, sp_mask);
			} else {
				set_sp(sp - 2 * 5, sp_mask);
			}
		} else {
			TRY(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * 3) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * 4) & sp_mask, 4, 0));
			TRY(translate(cpu, &meml5, 2, SEG_SS, (sp - 4 * 5) & sp_mask, 4, 0));
			if (pusherr) {
				TRY(translate(cpu, &meml6, 2, SEG_SS, (sp - 4 * 6) & sp_mask, 4, 0));
			}
			saddr32(&meml1, oldss);
			saddr32(&meml2, oldsp);

			refresh_flags(cpu);
			cpu->cc.mask = 0;
			saddr32(&meml3, cpu->flags);

			saddr32(&meml4, cpu->seg[SEG_CS].sel);
			saddr32(&meml5, cpu->ip);
			if (pusherr) {
				saddr32(&meml6, cpu->excerr);
				dolog("EX inter PVL INT %02xh %04x:%08x err=%08x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
				sreg32(4, sp - 4 * 6);
			} else {
				sreg32(4, sp - 4 * 5);
			}
		}
		newcs = (newcs & (~3)) | newpl;
		break;
	}
	case 3: /* from v8086 */ {
//		dolog("int from v8086\n");
		if (csdpl != 0) cpu_abort(cpu, -205);
		if (gate16) cpu_abort(cpu, -206);
//		dolog("call_isr %d %x PVL %d => 0\n", no, no, cpu->cpl, csdpl);
		OptAddr msp0, mss0;
		int newpl = 0;
		uword oldss = cpu->seg[SEG_SS].sel;
		uword oldsp = REGi(4);
		uword newss, newsp;
		if (!(cpu->seg[SEG_TR].flags & 0x8)) cpu_abort(cpu, -207);
		TRY(translate(cpu, &msp0, 1, SEG_TR, 4 + 8 * newpl, 4, 0));
		TRY(translate(cpu, &mss0, 1, SEG_TR, 8 + 8 * newpl, 4, 0));
		newsp = load32(cpu, &msp0);
		newss = load32(cpu, &mss0) & 0xffff;
		uword oldflags = cpu->flags;
		cpu->flags &= ~VM;
		REGi(4) = newsp;
		if (!set_seg(cpu, SEG_SS, newss)) {
			cpu->flags = oldflags;
			REGi(4) = oldsp;
			return false;
		}

		uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
		OptAddr memlg, memlf, memld, memle;
		OptAddr meml1, meml2, meml3, meml4, meml5, meml6;
		uword sp = lreg32(4);
		TRY1(translate(cpu, &memlg, 2, SEG_SS, (sp - 4 * 1) & sp_mask, 4, 0));
		TRY1(translate(cpu, &memlf, 2, SEG_SS, (sp - 4 * 2) & sp_mask, 4, 0));
		TRY1(translate(cpu, &memld, 2, SEG_SS, (sp - 4 * 3) & sp_mask, 4, 0));
		TRY1(translate(cpu, &memle, 2, SEG_SS, (sp - 4 * 4) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml1, 2, SEG_SS, (sp - 4 * 5) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml2, 2, SEG_SS, (sp - 4 * 6) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml3, 2, SEG_SS, (sp - 4 * 7) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml4, 2, SEG_SS, (sp - 4 * 8) & sp_mask, 4, 0));
		TRY1(translate(cpu, &meml5, 2, SEG_SS, (sp - 4 * 9) & sp_mask, 4, 0));
		if (pusherr) {
			TRY(translate(cpu, &meml6, 2, SEG_SS, (sp - 4 * 10) & sp_mask, 4, 0));
		}
		saddr32(&memlg, cpu->seg[SEG_GS].sel);
		saddr32(&memlf, cpu->seg[SEG_FS].sel);
		saddr32(&memld, cpu->seg[SEG_DS].sel);
		saddr32(&memle, cpu->seg[SEG_ES].sel);
		saddr32(&meml1, oldss);
		saddr32(&meml2, oldsp);

		refresh_flags(cpu);
		cpu->cc.mask = 0;
		saddr32(&meml3, cpu->flags | VM);

		saddr32(&meml4, cpu->seg[SEG_CS].sel);
		saddr32(&meml5, cpu->ip);
		if (pusherr) {
			saddr32(&meml6, cpu->excerr);
			dolog("EX v8086 INT %02xh %04x:%08x err=%08x\n", no, cpu->seg[SEG_CS].sel, cpu->ip, (unsigned)cpu->excerr);
			set_sp(sp - 4 * 10, sp_mask);
		} else {
			set_sp(sp - 4 * 9, sp_mask);
		}

		newcs = (newcs & (~3)) | newpl;
		TRY1(set_seg(cpu, SEG_DS, 0));
		TRY1(set_seg(cpu, SEG_ES, 0));
		TRY1(set_seg(cpu, SEG_FS, 0));
		TRY1(set_seg(cpu, SEG_GS, 0));
		cpu->flags &= ~(TF | RF | NT);
		TRY1(set_seg(cpu, SEG_CS, newcs));
		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		cpu->ip = newip;
		if (gt == 0x6 || gt == 0xe)
			cpu->flags &= ~IF;
		return true;
	}
	default: assert(false);
	}
	TRY1(set_seg(cpu, SEG_CS, newcs));
	cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
	cpu->ip = newip;
	cpu->flags &= ~(TF | RF | NT);
	if (gt == 0x6 || gt == 0xe)
		cpu->flags &= ~IF;
	return true;
}

static bool __pmiret_check_cs_same(CPUI386 *cpu, int sel)
{
	sel = sel & 0xffff;
	if ((sel & ~0x3) == 0) {
		dolog("__pmiret_check_cs_same: sel %04x\n", sel);
		THROW(EX_GP, sel & ~0x3);
	}
	uword w2;
	TRY(read_desc(cpu, sel, NULL, &w2));

	int s = (w2 >> 12) & 1;
	bool code = (w2 >> 8) & 0x8;
	bool conforming = (w2 >> 8) & 0x4;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;

	if (!s || !code) THROW(EX_GP, sel & ~0x3);

	if (!conforming) {
		if (dpl != cpu->cpl) THROW(EX_GP, sel & ~0x3);
	} else {
		if (dpl > cpu->cpl) THROW(EX_GP, sel & ~0x3);
	}

	if (!p) {
//		dolog("__pmiret_check_cs_same: seg not present %04x\n", sel);
		THROW(EX_NP, sel & ~0x3);
	}
	return true;
}

static bool __not_in_flash_func(__pmiret_check_cs_outer)(CPUI386 *cpu, int sel)
{
	sel = sel & 0xffff;
	if ((sel & ~0x3) == 0) {
		dolog("__pmiret_check_cs_outer: sel %04x\n", sel);
		THROW(EX_GP, sel & ~0x3);
	}
	uword w2;
	TRY(read_desc(cpu, sel, NULL, &w2));

	int s = (w2 >> 12) & 1;
	bool code = (w2 >> 8) & 0x8;
	bool conforming = (w2 >> 8) & 0x4;
	int dpl = (w2 >> 13) & 0x3;
	int p = (w2 >> 15) & 1;
	int rpl = sel & 3;
	
	if (!s || !code) THROW(EX_GP, sel & ~0x3);

	if (!conforming) {
		if (dpl != rpl) THROW(EX_GP, sel & ~0x3);
	} else {
    	// conforming: DPL must be <= RPL (Intel SDM Vol.2, IRET, outer privilege)
    	if (dpl > rpl) {
			dolog("__pmiret_check_cs_outer: DPL (%04x) must be <= RPL (%04x) %04x\n", dpl, rpl, sel);
			THROW(EX_GP, sel & ~0x3);
		}
	}

	if (!p) {
		dolog("__pmiret_check_cs_outer: seg not present %04x\n", sel);
		THROW(EX_NP, sel & ~0x3);
	}
	return true;
}

static bool pmret(CPUI386 *cpu, bool opsz16, int off, bool isiret)
{
	if (isiret) {
		if ((cpu->flags & VM)) THROW(EX_GP, 0);
		if ((cpu->flags & NT)) {
			OptAddr meml;
			TRY(translate(cpu, &meml, 1, SEG_TR, 0, 2, 0));
			int tssback = laddr16(&meml);
			dolog("IRET NT: tss curr: %04x back: %04x\n",
			      cpu->seg[SEG_TR].sel, tssback);
			// win2000 needs it...
			if (tssback == 0) THROW(EX_TS, 0);
			return task_switch(cpu, tssback, TS_IRET);
		}
		if (opsz16)
			off += 2;
		else
			off += 4;
	}

	OptAddr meml1, meml2, meml3, meml4, meml5;
	uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
	uword sp = lreg32(4);
	uword oldflags = cpu->flags;
	uword newip;
	int newcs;
	uword newflags = 0; // make the compiler happy
	if (opsz16) {
		/* ip */ TRY(translate(cpu, &meml1, 1, SEG_SS, sp & sp_mask, 2, 0));
		/* cs */ TRY(translate(cpu, &meml2, 1, SEG_SS, (sp + 2) & sp_mask, 2, 0));
		if (isiret) {
			/* flags */ TRY(translate(cpu, &meml3, 1, SEG_SS, (sp + 4) & sp_mask, 2, 0));
			newflags = (oldflags & 0xffff0000) | laddr16(&meml3);
		}
		newip = laddr16(&meml1);
		newcs = laddr16(&meml2);
	} else {
		/* ip */ TRY(translate(cpu, &meml1, 1, SEG_SS, sp & sp_mask, 4, 0));
		/* cs */ TRY(translate(cpu, &meml2, 1, SEG_SS, (sp + 4) & sp_mask, 4, 0));
		if (isiret) {
			/* flags */ TRY(translate(cpu, &meml3, 1, SEG_SS, (sp + 8) & sp_mask, 4, 0));
			newflags = laddr32(&meml3);
		}
		newip = laddr32(&meml1);
		newcs = laddr32(&meml2);
	}

	if (isiret) {
		uword mask = 0;
		if (cpu->cpl > 0) mask |= IOPL;
		if (get_IOPL(cpu) < cpu->cpl) mask |= IF;
		newflags = (oldflags & mask) | (newflags & ~mask);
		newflags &= EFLAGS_MASK;
		newflags |= 0x2;
	}

	if (isiret && (newflags & VM)) {
		if (cpu->cpl != 0) cpu_abort(cpu, -208);
		// return to v8086
//		dolog("pmiret PVL %d => %d (vm) %04x:%08x\n", cpu->cpl, 3, newcs, newip);
		OptAddr meml_vmes, meml_vmds, meml_vmfs, meml_vmgs;
		if (opsz16) cpu_abort(cpu, -209);
		TRY(translate(cpu, &meml4, 1, SEG_SS, (sp + 12) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml5, 1, SEG_SS, (sp + 16) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmes, 1, SEG_SS, (sp + 20) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmds, 1, SEG_SS, (sp + 24) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmfs, 1, SEG_SS, (sp + 28) & sp_mask, 4, 0));
		TRY(translate(cpu, &meml_vmgs, 1, SEG_SS, (sp + 32) & sp_mask, 4, 0));
		dolog("IRET->V86 raw: eip=%08x cs=%08x fl=%08x esp=%08x ss=%08x, sp=%08x eip=%08x cs=%04x fl=%08x v86esp=%08x v86ss=%04x\n",
		      laddr32(&meml1), laddr32(&meml2), laddr32(&meml3),
		      laddr32(&meml4), laddr32(&meml5),
			  sp, newip, newcs, newflags,
              laddr32(&meml4), laddr32(&meml5));
		cpu->flags = newflags;
		TRY1(set_seg(cpu, SEG_CS, newcs));
		cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		TRY1(set_seg(cpu, SEG_SS, laddr32(&meml5)));
		TRY1(set_seg(cpu, SEG_ES, laddr32(&meml_vmes)));
		TRY1(set_seg(cpu, SEG_DS, laddr32(&meml_vmds)));
		TRY1(set_seg(cpu, SEG_FS, laddr32(&meml_vmfs)));
		TRY(set_seg(cpu, SEG_GS, laddr32(&meml_vmgs)));
		set_sp(laddr32(&meml4), 0xffffffff);
	} else {
		int rpl = newcs & 3;
		if (rpl < cpu->cpl) THROW(EX_GP, newcs & ~0x3);
		if (rpl == cpu->cpl) {
			// return to same level
			TRY(__pmiret_check_cs_same(cpu, newcs));
//			dolog("pmiret PVL %d => %d %04x:%08x\n", cpu->cpl, newcs & 3, newcs, newip);
			if (isiret)
				cpu->flags = newflags;
			TRY1(set_seg(cpu, SEG_CS, newcs));

			if (opsz16) {
				set_sp(sp + 4 + off, sp_mask);
			} else {
				set_sp(sp + 8 + off, sp_mask);
			}
			cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
		} else {
			// return to outer level
			TRY(__pmiret_check_cs_outer(cpu, newcs));
			uword newsp;
			uword newss;
//			dolog("pmiret PVL %d => %d %04x:%08x\n", cpu->cpl, newcs & 3, newcs, newip);
			if (opsz16) {
				/* sp */ TRY(translate(cpu, &meml4, 1, SEG_SS, (sp + 4 + off) & sp_mask, 2, 0));
				/* ss */ TRY(translate(cpu, &meml5, 1, SEG_SS, (sp + 6 + off) & sp_mask, 2, 0));
				newsp = laddr16(&meml4);
				newss = laddr16(&meml5);
			} else {
				/* sp */ TRY(translate(cpu, &meml4, 1, SEG_SS, (sp + 8 + off) & sp_mask, 4, 0));
				/* ss */ TRY(translate(cpu, &meml5, 1, SEG_SS, (sp + 12 + off) & sp_mask, 4, 0));
				newsp = laddr32(&meml4);
				newss = laddr32(&meml5);
			}

			if (isiret)
				cpu->flags = newflags;
			TRY1(set_seg(cpu, SEG_CS, newcs));
			TRY1(set_seg(cpu, SEG_SS, newss));
			uword newsp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
			set_sp(newsp, newsp_mask);
			cpu->next_ip = newip; cpu->prefetch_base = (u32)-1;
			clear_segs(cpu);
		}
	}
	if (isiret)
		cpu->cc.mask = 0;
	return true;
}

void cpui386_step(CPUI386 *cpu, int stepcount)
{
	frank_diag_trace(cpu->seg[SEG_CS].base, cpu->ip);
	if ((cpu->flags & IF) && cpu->intr) {
		cpu->intr = false;
		cpu->halt = false;
		int no = cpu->cb.pic_read_irq(cpu->cb.pic);
		g_wl_hw_irq++;
		cpu->ip = cpu->next_ip;
		if (!call_isr(cpu, no, false, 1)) {
			if (!call_isr(cpu, EX_DF, true, 1)) {
				cpui386_reset(cpu);
				return;
			}
		}
	}

	if (cpu->halt) {
		usleep(1);
		return;
	}

	if (!cpu_exec1(cpu, stepcount)) {
		bool pusherr = false;
		switch (cpu->excno) {
		case EX_PF: g_wl_exc_pf++; break;
		case EX_GP: g_wl_exc_gp++; break;
		default:    g_wl_exc_other++; break;
		}
		switch (cpu->excno) {
		case EX_DF: case EX_TS: case EX_NP: case EX_SS: case EX_GP:
		case EX_PF:
			pusherr = true;
		}
		cpu->next_ip = cpu->ip;

		if (cpu->excno == EX_DF) {
			if (!call_isr(cpu, EX_DF, true, 1)) {
				cpui386_reset(cpu);
				return;
			}
		} else if (!call_isr(cpu, cpu->excno, pusherr, 1)) {
			if (!call_isr(cpu, EX_DF, true, 1)) {
				cpui386_reset(cpu);
				return;
			}
		}
	}
}

void cpu_setax(CPUI386 *cpu, u16 ax)
{
	sreg16(0, ax);
}

u16 cpu_getax(CPUI386 *cpu)
{
	return lreg16(0);
}

void cpu_setexc(CPUI386 *cpu, int excno, uword excerr)
{
	frank_diag_exc(cpu->seg[SEG_CS].base, cpu->ip, (uint32_t)excno,
	               (uint32_t)excerr, (uint32_t)cpu->flags);
	cpu->excno = excno;
	cpu->excerr = excerr;
}

void cpu_setflags(CPUI386 *cpu, uword set_mask, uword clear_mask)
{
	if (cpu->cc.mask & (set_mask | clear_mask)) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	cpu->flags |= set_mask;
	cpu->flags &= ~clear_mask;
	cpu->flags &= EFLAGS_MASK;
}

uword cpu_getflags(CPUI386 *cpu)
{
	if (cpu->cc.mask) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	return cpu->flags;
}

void cpui386_reset(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++) {
		REGi(i) = 0;
	}
	cpu->flags = 0x2;
	cpu->cpl = 0;
	cpu->code16 = true;
	cpu->sp_mask = 0xffff;
	cpu->halt = false;

	for (int i = 0; i < 8; i++) {
		cpu->seg[i].sel = 0;
		cpu->seg[i].base = 0;
		cpu->seg[i].limit = 0;
		cpu->seg[i].flags = 0;
	}
	cpu->seg[2].flags = (1 << 22);
	cpu->seg[1].flags = (1 << 22);

	cpu->ip = 0xfff0;
	cpu->next_ip = cpu->ip; cpu->prefetch_base = (u32)-1;
	cpu->seg[SEG_CS].sel = 0xf000;
	cpu->seg[SEG_CS].base = 0xf0000;

	cpu->idt.base = 0;
	cpu->idt.limit = 0x3ff;
	cpu->gdt.base = 0;
	cpu->gdt.limit = 0;

	cpu->cr0 = cpu->fpu ? 0x10 : 0;
	cpu->cr2 = 0;
	cpu->cr3 = 0;
	for (int i = 0; i < 8; i++)
		cpu->dr[i] = 0;

	cpu->cc.mask = 0;
	tlb_clear(cpu);

	cpu->sysenter.cs = 0;
	cpu->sysenter.eip = 0;
	cpu->sysenter.esp = 0;
}

void cpui386_reset_pm(CPUI386 *cpu, uint32_t start_addr)
{
	cpui386_reset(cpu);
	cpu->cr0 = 1;
	cpu->seg[SEG_CS].sel = 0x8;
	cpu->seg[SEG_CS].base = 0;
	cpu->seg[SEG_CS].limit = 0xffffffff;
	cpu->seg[SEG_CS].flags = SEG_D_BIT;
	cpu->next_ip = start_addr;
	cpu->cpl = 0;
	cpu->code16 = false;
	cpu->sp_mask = 0xffffffff;
	cpu->seg[SEG_SS].sel = 0x10;
	cpu->seg[SEG_SS].base = 0;
	cpu->seg[SEG_SS].limit = 0xffffffff;
	cpu->seg[SEG_SS].flags = SEG_B_BIT;

	cpu->seg[SEG_DS] = cpu->seg[SEG_SS];
	cpu->seg[SEG_ES] = cpu->seg[SEG_SS];
}

void IRAM_ATTR cpui386_raise_irq(CPUI386 *cpu)
{
	cpu->intr = true;
}

void cpui386_set_gpr(CPUI386 *cpu, int i, u32 val)
{
	sreg32(i, val);
}

long IRAM_ATTR cpui386_get_cycle(CPUI386 *cpu)
{
	return cpu->cycle;
}

/*
 * FRANK_WORKLOAD_PROFILE_V88: mode snapshot taken when the stats file is
 * written.  Together with the backedge split it distinguishes "the guest was
 * not in VM86+paging during the window" from "it was, and the JIT still saw
 * nothing worth compiling" - two situations that every previous capture in
 * this sequence left indistinguishable.
 */
void cpui386_diag_mode(CPUI386 *cpu, u32 out[5])
{
	out[0] = (u32)cpu->cr0;
	out[1] = (u32)cpu->flags;
	out[2] = (u32)cpu->cr3;
	out[3] = (u32)cpu->cpl;
	out[4] = (u32)cpu->code16;
}

CPUI386 *cpui386_new(int gen, char *phys_mem, long phys_mem_size, CPU_CB **cb)
{
	CPUI386 *cpu = malloc(sizeof(CPUI386));
	switch (gen) {
	case 3: cpu->flags_mask = EFLAGS_MASK_386; break;
	case 4: cpu->flags_mask = EFLAGS_MASK_486; break;
	case 5: case 6: cpu->flags_mask = EFLAGS_MASK_586; break;
	default: assert(false);
	}
	cpu->gen = gen;

	cpu->tlb.size = tlb_size;
#ifdef BUILD_ESP32
	{
		extern void *pcmalloc(long size);
		size_t tlb_bytes = sizeof(struct tlb_entry) * tlb_size;
		cpu->tlb.tab = malloc(tlb_bytes);
		if (!cpu->tlb.tab)
			cpu->tlb.tab = pcmalloc(tlb_bytes);
	}
#else
	cpu->tlb.tab = malloc(sizeof(struct tlb_entry) * tlb_size);
#endif

	cpu->phys_mem = (u8 *) phys_mem;
	cpu->phys_mem_size = phys_mem_size;

	cpu->cycle = 0;

	cpu->intr = false;

	cpu->fpu = NULL;

	cpui386_reset(cpu);

	memset(&(cpu->cb), 0, sizeof(CPU_CB));
	if (cb)
		*cb = &(cpu->cb);
	return cpu;
}

void cpui386_enable_fpu(CPUI386 *cpu)
{
	if (!cpu->fpu)
		cpu->fpu = fpu_new();
}

void cpui386_delete(CPUI386 *cpu)
{
	if (cpu->fpu)
		fpu_delete(cpu->fpu);
	free(cpu);
}

#if !defined(_WIN32) && !defined(__wasm__)
void cpui386_set_verbose() // for debugging
{
	verbose = true;
	freopen("/tmp/xlog", "w", stderr);
	setlinebuf(stderr);
}
#endif

static void cpu_debug(CPUI386 *cpu)
{
	static int nest;
	if (nest >= 1)
		return;
	nest++;
	bool code32 = cpu->seg[SEG_CS].flags & SEG_D_BIT;
	bool stack32 = cpu->seg[SEG_SS].flags & SEG_B_BIT;

	dolog("IP %08x|AX %08x|CX %08x|DX %08x|BX %08x|SP %08x|BP %08x|SI %08x|DI %08x|FL %08x|CS %04x|DS %04x|SS %04x|ES %04x|FS %04x|GS %04x|CR0 %08x|CR2 %08x|CR3 %08x|CPL %d|IOPL %d|CSBASE %08x/%08x|DSBASE %08x/%08x|SSBASE %08x/%08x|ESBASE %08x/%08x|GSBASE %08x/%08x %c%c\n",
		cpu->ip, REGi(0), REGi(1), REGi(2), REGi(3),
		REGi(4), REGi(5), REGi(6), REGi(7),
		cpu->flags, SEGi(SEG_CS), SEGi(SEG_DS), SEGi(SEG_SS),
		SEGi(SEG_ES), SEGi(SEG_FS), SEGi(SEG_GS),
		cpu->cr0, cpu->cr2, cpu->cr3, cpu->cpl, get_IOPL(cpu),
		cpu->seg[SEG_CS].base, cpu->seg[SEG_CS].limit,
		cpu->seg[SEG_DS].base, cpu->seg[SEG_DS].limit,
		cpu->seg[SEG_SS].base, cpu->seg[SEG_SS].limit,
		cpu->seg[SEG_ES].base, cpu->seg[SEG_ES].limit,
		cpu->seg[SEG_GS].base, cpu->seg[SEG_GS].limit,
		code32 ? 'D' : ' ', stack32 ? 'B' : ' ');
	uword cr2, excno, excerr;
	cr2 = cpu->cr2;
	excno = cpu->excno;
	excerr = cpu->excerr;
	dolog("code: ");
	for (int i = 0; i < 32; i++) {
		OptAddr res;
		if(translate8(cpu, &res, 1, SEG_CS, cpu->ip + i))
			dolog(" %02x", load8(cpu, &res));
		else
			dolog(" ??");
	}
	dolog("\n");
	dolog("stack: ");
	uword sp_mask = cpu->seg[SEG_SS].flags & SEG_B_BIT ? 0xffffffff : 0xffff;
	for (int i = 0; i < 32; i++) {
		OptAddr res;
		if(translate8(cpu, &res, 1, SEG_SS, (REGi(4) + i) & sp_mask))
			dolog(" %02x", load8(cpu, &res));
		else
			dolog(" ??");
	}
	dolog("\n");
	dolog("stkf : ");
	for (int i = 0; i < 32; i++) {
		OptAddr res;
		if(translate8(cpu, &res, 1, SEG_SS, (REGi(5) + i) & sp_mask))
			dolog(" %02x", load8(cpu, &res));
		else
			dolog(" ??");
	}
	dolog("\n");

	cpu->cr2 = cr2;
	cpu->excno = excno;
	cpu->excerr = excerr;
	nest--;
}

void cpu_set_a20(CPUI386 *cpu, int enabled)
{
	cpu->a20_mask = enabled ? 0xFFFFFFFFu : 0xFFEFFFFFu;
}

int cpu_get_a20(CPUI386 *cpu)
{
	return cpu->a20_mask >> 20 & 1;
}

void cpui386_get_state(CPUI386 *cpu, uint32_t *cs, uint32_t *ip, int *halt)
{
	*cs = cpu->seg[1].sel;  // SEG_CS = 1
	*ip = cpu->ip;
	*halt = cpu->halt ? 1 : 0;
}

u8 *cpu_get_phys_mem(CPUI386 *cpu) { return cpu->phys_mem; }


// Register accessors for disk/BIOS emulation
u8 cpu_get_al(CPUI386 *cpu) { return lreg8(0); }
u8 cpu_get_ah(CPUI386 *cpu) { return lreg8(4); }
u8 cpu_get_bl(CPUI386 *cpu) { return lreg8(3); }
u8 cpu_get_bh(CPUI386 *cpu) { return lreg8(7); }
u8 cpu_get_cl(CPUI386 *cpu) { return lreg8(1); }
u8 cpu_get_ch(CPUI386 *cpu) { return lreg8(5); }
u8 cpu_get_dl(CPUI386 *cpu) { return lreg8(2); }
u8 cpu_get_dh(CPUI386 *cpu) { return lreg8(6); }

void cpu_set_al(CPUI386 *cpu, u8 val) { sreg8(0, val); }
void cpu_set_ah(CPUI386 *cpu, u8 val) { sreg8(4, val); }
void cpu_set_bl(CPUI386 *cpu, u8 val) { sreg8(3, val); }
void cpu_set_bh(CPUI386 *cpu, u8 val) { sreg8(7, val); }
void cpu_set_cl(CPUI386 *cpu, u8 val) { sreg8(1, val); }
void cpu_set_ch(CPUI386 *cpu, u8 val) { sreg8(5, val); }
void cpu_set_dl(CPUI386 *cpu, u8 val) { sreg8(2, val); }
void cpu_set_dh(CPUI386 *cpu, u8 val) { sreg8(6, val); }

u16 cpu_get_bx(CPUI386 *cpu) { return lreg16(3); }
u16 cpu_get_cx(CPUI386 *cpu) { return lreg16(1); }
u16 cpu_get_dx(CPUI386 *cpu) { return lreg16(2); }
u16 cpu_get_es(CPUI386 *cpu) { return cpu->seg[SEG_ES].sel; }

void cpu_set_bx(CPUI386 *cpu, u16 val) { sreg16(3, val); }
void cpu_set_cx(CPUI386 *cpu, u16 val) { sreg16(1, val); }
void cpu_set_dx(CPUI386 *cpu, u16 val) { sreg16(2, val); }
u16 cpu_get_bp(CPUI386 *cpu) { return lreg16(5); }
u16 cpu_get_si(CPUI386 *cpu) { return lreg16(6); }
u16 cpu_get_di(CPUI386 *cpu) { return lreg16(7); }
void cpu_set_si(CPUI386 *cpu, u16 val) { sreg16(6, val); }
void cpu_set_di(CPUI386 *cpu, u16 val) { sreg16(7, val); }
u16 cpu_get_ds(CPUI386 *cpu) { return cpu->seg[SEG_DS].sel; }
u16 cpu_get_ss(CPUI386 *cpu) { return cpu->seg[SEG_SS].sel; }

void cpu_set_cf(CPUI386 *cpu, int val)
{
	if (cpu->cc.mask & CF) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	if (val)
		cpu->flags |= CF;
	else
		cpu->flags &= ~CF;
}

int cpu_get_cf(CPUI386 *cpu)
{
	if (cpu->cc.mask & CF) {
		refresh_flags(cpu);
		cpu->cc.mask = 0;
	}
	return (cpu->flags & CF) ? 1 : 0;
}

void cpu_set_int2f_handler(CPUI386 *cpu, int2f_handler_t handler, void *opaque)
{
	cpu->int2f_handler = handler;
	cpu->int2f_opaque  = opaque;
}
