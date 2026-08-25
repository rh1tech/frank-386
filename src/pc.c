#include "pc.h"
#include "mem.h"
#include "ide.h"
#include "dss.h"
#include "misc.h"
#include "profile_subsys.h"
#include "codeprofile.h"
#include "gameport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <hardware/watchdog.h>
#include "bios/bios.h"
#include "bulk_bounce.h"
#include "psram_init.h"
#include "config_save.h"

extern bool SELECT_VGA;

/* Общий bounce-буфер нативных bulk-обменов FatFs <-> гость.
   Условия, при которых его допустимо разделять, - в bulk_bounce.h. */
uint8_t guest_bulk_buf[GUEST_BULK_BUF_SIZE];

#include "mpu401.c.inl"
void netredirect_init(CPU *cpu, int enable);

unsigned long phys_mem_size = 8l << 20;
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
uint8_t* __scratch_y("guest_ram_base") guest_ram_base = (uint8_t *)PSRAM_BASE_ADDR;
uint8_t ram_pages[RAM_PAGES_SIZE]
    __attribute__((section(".bss.gfx_buffer.ram_pages"), aligned(4)));
#endif
void* g_pc;

#define cpu_raise_irq cpu_raise_irq

/* ---- Emulink FDD: simple virtual floppy on ports 0xF1F0/0xF1F4 ----------
 * Protocol (matches tiny386 / this BIOS):
 *   OUT 0xF1F0, cmd    – set command (resets argi to 0)
 *   OUT 0xF1F4, arg    – push argument (up to 4); executes after 3rd arg
 *   IN  0xF1F0         – read 32-bit status / result
 *   REP INSB  0xF1F4   – bulk read data (cmd 0x101)
 *   REP OUTSB 0xF1F4   – bulk write data (cmd 0x102)
 *
 * Commands:
 *   0x000 – identify: status = 0xAA55FF00
 *   0x100 – probe drives: status bit6=drv0 present, bit2=drv1 present
 *   0x101 – read sectors:  args[0]=drive, args[1]=CHS(c<<16|h<<8|s), args[2]=count
 *   0x102 – write sectors: same args, then REP OUTSB
 * -------------------------------------------------------------------------*/

static void emulink_exec(PC *pc)
{
	switch (pc->emulink.cmd) {
	case 0x000:
		pc->emulink.status = 0xaa55ff00;
		pc->emulink.cmd    = -1;
		break;
	case 0x100:
		pc->emulink.status = fdds_types(); 
		pc->emulink.cmd = -1;
		break;
	case 0x101: /* read */
	case 0x102: /* write */
		if (pc->emulink.argi == 3) {
			uint8_t  drv  = (uint8_t)pc->emulink.args[0];
			uint32_t chs  = pc->emulink.args[1];
			uint32_t cnt  = pc->emulink.args[2];
			if (drv >= 2 || !fdd_is_inserted(drv)) {
				pc->emulink.status = 0x80; /* error */
				pc->emulink.cmd    = -1;
				break;
			}
			int c = (int)(chs >> 16);
			int h = (int)((chs >> 8) & 0xff);
			int s = (int)(chs & 0xff);
			uint16_t heads = fdd_get_heads(drv);
			uint16_t sects = fdd_get_sects(drv);
			if (heads == 0 || sects == 0) {
				pc->emulink.status = 0x80;
				pc->emulink.cmd    = -1;
				break;
			}
			uint32_t lba = (uint32_t)(c * heads + h) * sects + (uint32_t)(s - 1);
			FIL *fil = fdd_get_file(drv);
			FRESULT fr = f_lseek(fil, lba * 512u);
			if (fr != FR_OK) {
				pc->emulink.status = 0x80;
				pc->emulink.cmd    = -1;
			} else {
				pc->emulink.status    = 0;
				pc->emulink.dataleft  = (int)(cnt * 512u);
			}
		}
		break;
	default:
		break;
	}
}

static uint32_t emulink_read32(PC *pc)
{
	return pc->emulink.status;
}

static void emulink_cmd_write(PC *pc, uint32_t val)
{
	pc->emulink.cmd  = (int)val;
	pc->emulink.argi = 0;
	emulink_exec(pc);
}

static void emulink_arg_write(PC *pc, uint32_t val)
{
	if (pc->emulink.argi < 4)
		pc->emulink.args[pc->emulink.argi++] = val;
	emulink_exec(pc);
}

/* bulk read: called from pc_io_read_string for port 0xF1F4 */
static int emulink_data_read(PC *pc, uint32_t addr, int size, int count)
{
	if (pc->emulink.cmd == 0x101 && pc->emulink.argi == 3) {
		uint8_t drv = (uint8_t)pc->emulink.args[0];
		if (!fdd_is_inserted(drv)) goto err;
		int len = size * count;
		if (len > pc->emulink.dataleft) goto err;
		FIL *fil = fdd_get_file(drv);
        UINT br = 0;
        uint8_t *buf = guest_bulk_buf;   /* общий bounce, не на стеке */
        for (int i = 0; i < len; i += 512) {
            UINT l = len - i;
            if (l > 512) l = 512;
            FRESULT fr = f_read(fil, buf, l, &br);
			if (fr != FR_OK || (int)br != l) goto err;
            for (int j = 0; j < l; ++j) {
                pstore8(addr + i + j, buf[j]);
            }
        }
		pc->emulink.dataleft -= len;
		if (pc->emulink.dataleft == 0) {
			pc->emulink.cmd    = -1;
			pc->emulink.status = 0;
		}
		return count;
	}
err:
	pc->emulink.cmd    = -1;
	pc->emulink.status = 0x80;
	return count;
}

/* bulk write: called from pc_io_write_string for port 0xF1F4 */
static int emulink_data_write(PC *pc, uint32_t addr, int size, int count)
{
	if (pc->emulink.cmd == 0x102 && pc->emulink.argi == 3) {
		uint8_t drv = (uint8_t)pc->emulink.args[0];
		if (!fdd_is_inserted(drv)) goto err;
		int len = size * count;
		if (len > pc->emulink.dataleft) goto err;
		FIL *fil = fdd_get_file(drv);
		UINT bw = 0;
        uint8_t *buf = guest_bulk_buf;   /* общий bounce, не на стеке */
        for (int i = 0; i < len; i += 512) {
            UINT l = len - i;
            if (l > 512) l = 512;
            for (int j = 0; j < l; ++j) {
                buf[j] = pload8(addr + i + j);
            }
            FRESULT fr = f_write(fil, buf, l, &bw);
			if (fr != FR_OK || (int)bw != l) goto err;
        }
		pc->emulink.dataleft -= len;
		if (pc->emulink.dataleft == 0) {
			pc->emulink.cmd    = -1;
			pc->emulink.status = 0;
		}
		return count;
	}
err:
	pc->emulink.cmd    = -1;
	pc->emulink.status = 0x80;
	return count;
}

/* --------------------------------------------------------------------------*/
#if TRACE_PORTS
static FIL ports_log;
#include <stdarg.h>
void debug_write(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	UINT bw;
	f_write(&ports_log, buf, n, &bw);
	f_sync(&ports_log);
}
#else
#define debug_write(...) (void)0
#endif

static __always_inline u8 _pc_io_read(void *o, int addr)
{
	cp_io_read();
	PC *pc = o;
	u8 val;

	switch(addr) {
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		val = i8259_ioport_read(pc->pic, addr);
		return val;
	case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
	case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
		val = 0xff;
		if (pc->enable_serial)
			val = u8250_reg_read(pc->serial, addr - 0x3f8);
		return val;
	case 0x2f8: case 0x2f9: case 0x2fa: case 0x2fb:
	case 0x2fc: case 0x2fd: case 0x2fe: case 0x2ff:
	case 0x2e8: case 0x2e9: case 0x2ea: case 0x2eb:
	case 0x2ec: case 0x2ed: case 0x2ee: case 0x2ef:
	case 0x3e8: case 0x3e9: case 0x3ea: case 0x3eb:
	case 0x3ec: case 0x3ed: case 0x3ee: case 0x3ef:
		return 0;
	case 0x42:
		/* read delay for PIT channel 2 */
		/* certain guest code needs it to drive pc speaker properly */
		usleep(0);
		/* fall through */
	case 0x40: case 0x41: case 0x43:
		val = i8254_ioport_read(pc->pit, addr);
		return val;
	case 0x70: case 0x71:
		val = cmos_ioport_read(pc->cmos, addr);
		return val;
	case 0x1f0: case 0x1f1: case 0x1f2: case 0x1f3:
	case 0x1f4: case 0x1f5: case 0x1f6: case 0x1f7:
		return ide_ioport_read(pc->ide, addr - 0x1f0);
	case 0x170: case 0x171: case 0x172: case 0x173:
	case 0x174: case 0x175: case 0x176: case 0x177:
		return ide_ioport_read(pc->ide2, addr - 0x170);
	case 0x3f6:
		return ide_status_read(pc->ide);
	case 0x376:
		return ide_status_read(pc->ide2);
	/* FDC ports 0x3F0-0x3F5, 0x3F7 (0x3F6 = IDE alt-status, handled above) */
	case 0x3f0: case 0x3f1: case 0x3f2: case 0x3f3:
	case 0x3f4: case 0x3f5:
	case 0x3f7:
		if (pc->fdc)
			return fdc_ioport_read(pc->fdc, addr);
		return 0xff;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de: case 0x3df:
		val = vga_ioport_read(pc->vga, addr);
		return val;
	case 0x92:
		return pc->port92 | 0x02;
	case 0x60:
		val = kbd_read_data(pc->i8042, addr);
		return val;
	case 0x64:
		val = kbd_read_status(pc->i8042, addr);
		return val;
	case 0x61:
		val = pcspk_ioport_read(pc->pcspk);
		return val;
	case 0x220: case 0x221: case 0x222: case 0x223:
	case 0x228: case 0x229:
	case 0x388: case 0x389: case 0x38a: case 0x38b:
		if (pc->adlib_enabled)
			return adlib_read(pc->adlib, addr);
		return 0xFF;
	case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
		val = i440fx_read_data(pc->i440fx, addr - 0xcfc, 0);
		return val;
	/* NE2000 networking removed */
	case 0x300: case 0x301: case 0x302: case 0x303:
	case 0x304: case 0x305: case 0x306: case 0x307:
	case 0x308: case 0x309: case 0x30a: case 0x30b:
	case 0x30c: case 0x30d: case 0x30e: case 0x30f:
	case 0x310: case 0x31f:
		return 0xff;
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
		val = i8257_read_chan(pc->isa_dma, addr - 0x00, 1);
		return val;
	case 0xf1f4: {
		/* emulink: single-byte read (BIOS probes this way too) */
		uint32_t v32 = emulink_read32(pc);
		return (u8)(v32 & 0xff);
	}
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x0c: case 0x0d: case 0x0e: case 0x0f:
		val = i8257_read_cont(pc->isa_dma, addr - 0x08, 1);
		return val;
	case 0x81: case 0x82: case 0x83: case 0x87:
		val = i8257_read_page(pc->isa_dma, addr - 0x80);
		return val;
	case 0x481: case 0x482: case 0x483: case 0x487:
		val = i8257_read_pageh(pc->isa_dma, addr - 0x480);
		return val;
	case 0xc0: case 0xc1:
		/* SN76489 is write-only; return 0xFF on read when Tandy active */
		if (pc->tandy_enabled)
			return 0xff;
		val = i8257_read_chan(pc->isa_hdma, addr - 0xc0, 1);
		return val;
	case 0xc2: case 0xc4: case 0xc6:
	case 0xc8: case 0xca: case 0xcc: case 0xce:
		val = i8257_read_chan(pc->isa_hdma, addr - 0xc0, 1);
		return val;
	case 0xd0: case 0xd2: case 0xd4: case 0xd6:
	case 0xd8: case 0xda: case 0xdc: case 0xde:
		val = i8257_read_cont(pc->isa_hdma, addr - 0xd0, 1);
		return val;
	case 0x89: case 0x8a: case 0x8b: case 0x8f:
		val = i8257_read_page(pc->isa_hdma, addr - 0x88);
		return val;
	case 0x489: case 0x48a: case 0x48b: case 0x48f:
		val = i8257_read_pageh(pc->isa_hdma, addr - 0x488);
		return val;
	case 0x225:
		if (pc->sb16_enabled) {
			return sb16_mixer_read(pc->sb16, addr);
		}
		return 0xFF;
	case 0x226: case 0x22a: case 0x22c: case 0x22d: case 0x22e: case 0x22f:
		if (pc->sb16_enabled) {
			return sb16_dsp_read(pc->sb16, addr);
		}
		return 0xFF;
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		/* Analog game port. When no joystick is configured this returns
		 * 0xF0 - axes timed out, no buttons - which is exactly what an
		 * empty adapter reads, so probing games behave as before. */
		if (pc->joystick_enabled)
			return gameport_read();
		return 0xf0;
	// MPU-401
	case 0x330:
	case 0x331:
		if (pc->mpu401_enabled)
	        return mpu401_read(addr);
		return 0xFF;

	/* --- LPT1 (0x378): принтер + Disney Sound Source ---------------------
	 * data:    защёлка (нужна для detect: write 0xAA/0x55 -> read back).
	 *          При включённом DSS dss_in(0x378) отдаёт тот же последний байт.
	 * status:  DSS использует ТОЛЬКО бит6 (FIFO full). Остальные биты
	 *          доливаем как "принтер готов", иначе INT 17h всегда timeout:
	 *          bit7 nBusy=1, bit4 Select=1, bit3 nError=1  => 0x98.
	 * control: настоящая защёлка (init = 0x04, как было раньше константой).
	 *          dss_out() ведёт свой собственный control, чтение ему не нужно.
	 */
	case 0x378: return pc->dss_enabled ? dss_in(addr) : pc->lpt_data[0];
	case 0x379: return pc->dss_enabled ? (dss_in(addr) | 0x98) : 0xD8;
	case 0x37A: return pc->lpt_ctrl[0];

	/* --- LPT2 (0x278): принтер + Covox ---------------------------------- */
	case 0x278: return pc->lpt_data[1];
	case 0x279: return 0xD8;                 /* idle: nBusy|nAck|Select|nError */
	case 0x27A: return pc->lpt_ctrl[1];
	default:
		//fprintf(stderr, "in 0x%x <= 0x%x\n", addr, 0xff);
		return 0xff;
	}
}

static u8 pc_io_read(void *o, int addr) {
	u8 r = _pc_io_read(o, addr);
	debug_write("R8: %ph <- %02Xh\n", addr, r);
	return r;
}

static __always_inline u16 _pc_io_read16(void *o, int addr)
{
	PC *pc = o;
	u16 val;

	switch(addr) {
	case 0x1ce: case 0x1cf:
		val = vbe_read(pc->vga, addr - 0x1ce);
		return val;
	/* IDE ports */
	case 0x1f0:
		return ide_data_readw(pc->ide);
	case 0x170:
		return ide_data_readw(pc->ide2);
	case 0xcf8:
		val = i440fx_read_addr(pc->i440fx, 0, 1);
		return val;
	case 0xcfc: case 0xcfe:
		val = i440fx_read_data(pc->i440fx, addr - 0xcfc, 1);
		return val;
	/* NE2000 networking removed */
	case 0x310:
		return 0xffff;
	case 0x220:
		if (pc->adlib_enabled)
			return adlib_read(pc->adlib, addr);
		return 0xFFFF;
	/* Game port. Games normally use IN AL, but a 16-bit read must not
	 * silently return 0 - that reads as "axes already timed out" and the
	 * stick looks stuck at one extreme. */
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		if (pc->joystick_enabled)
			return 0xff00u | gameport_read();
		return 0xfff0u;
	default:
		return 0;
	}
}

static u16 pc_io_read16(void *o, int addr) {
	u16 r = _pc_io_read16(o, addr);
	debug_write("R16: %ph <- %04Xh\n", addr, r);
	return r;
}

static __always_inline u32 _pc_io_read32(void *o, int addr)
{
	PC *pc = o;
	u32 val;
	switch(addr) {
	/* IDE ports */
	case 0x1f0:
		return ide_data_readl(pc->ide);
	case 0x170:
		return ide_data_readl(pc->ide2);
	case 0x3cc:
		return (get_uticks() - pc->boot_start_time) / 1000;
	case 0xcf8:
		val = i440fx_read_addr(pc->i440fx, 0, 2);
		return val;
	case 0xcfc:
		val = i440fx_read_data(pc->i440fx, 0, 2);
		return val;
	/* Emulink FDD status port */
	case 0xf1f0:
		return emulink_read32(pc);
	default:
		return 0;
	}
}

static u32 pc_io_read32(void *o, int addr) {
	u32 r = _pc_io_read32(o, addr);
	debug_write("R32: %ph <- %08Xh\n", addr, r);
	return r;
}

static int pc_io_read_string(void *o, int addr, uint32_t buf, int size, int count)
{
	debug_write("RS: %ph [%d / %d]\n", addr, size, count);
	PC *pc = o;
	switch(addr) {
	case 0x1f0:
		return ide_data_read_string(pc->ide, buf, size, count);
	case 0x170:
		return ide_data_read_string(pc->ide2, buf, size, count);
	case 0xf1f4:
		return emulink_data_read(pc, buf, size, count);
	}
	return 0;
}

#if EMULATE_LTEMS
uint8_t ems_pages[4] = {0};
uint8_t *ems_base_ptr = NULL;

inline static void out_ems(const uint16_t port, const uint8_t data) {
    ems_pages[port & 3] = data;
}
#endif

static void pc_io_write(void *o, int addr, u8 val)
{
	cp_io_write();
	debug_write("W8: %ph -> %02Xh\n", addr, val);
	PC *pc = o;
	switch(addr) {
	case 0x80: case 0xed:
		/* used by linux, for io delay */
		return;
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		i8259_ioport_write(pc->pic, addr, val);
		return;
	case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
	case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
		u8250_reg_write(pc->serial, addr - 0x3f8, val);
		return;
#if EMULATE_LTEMS
    case 0x260: case 0x261: case 0x262: case 0x263:
		out_ems(addr, val);
        return;
#endif
	case 0x2f8: case 0x2f9: case 0x2fa: case 0x2fb:
	case 0x2fc: case 0x2fd: case 0x2fe: case 0x2ff:
	case 0x2e8: case 0x2e9: case 0x2ea: case 0x2eb:
	case 0x2ec: case 0x2ed: case 0x2ee: case 0x2ef:
	case 0x3e8: case 0x3e9: case 0x3ea: case 0x3eb:
	case 0x3ec: case 0x3ed: case 0x3ee: case 0x3ef:
		return;
	case 0x40: case 0x41: case 0x42: case 0x43:
		i8254_ioport_write(pc->pit, addr, val);
		return;
	case 0x70: case 0x71:
		cmos_ioport_write(pc->cmos, addr, val);
		return;
	/* IDE ports */
	case 0x1f0: case 0x1f1: case 0x1f2: case 0x1f3:
	case 0x1f4: case 0x1f5: case 0x1f6: case 0x1f7:
		ide_ioport_write(pc->ide, addr - 0x1f0, val);
		return;
	case 0x170: case 0x171: case 0x172: case 0x173:
	case 0x174: case 0x175: case 0x176: case 0x177:
		ide_ioport_write(pc->ide2, addr - 0x170, val);
		return;
	case 0x3f6:
		ide_cmd_write(pc->ide, val);
		return;
	case 0x376:
		ide_cmd_write(pc->ide2, val);
		return;
	/* FDC ports 0x3F0-0x3F5, 0x3F7 */
	case 0x3f0: case 0x3f1: case 0x3f2: case 0x3f3:
	case 0x3f4: case 0x3f5:
	case 0x3f7:
		if (pc->fdc)
			fdc_ioport_write(pc->fdc, addr, val);
		return;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de: case 0x3df:
		vga_ioport_write(pc->vga, addr, val);
		return;
	case 0x402:
		return;
	case 0x92:
		/* Fast A20 gate is hard-wired on; preserve all other port bits. */
		pc->port92 = val | 0x02;
		cpu_set_a20(pc->cpu, 1);
		return;
	case 0x60:
		kbd_write_data(pc->i8042, addr, val);
		return;
	case 0x64:
		kbd_write_command(pc->i8042, addr, val);
		return;
	case 0x61:
		pcspk_ioport_write(pc->pcspk, val);
		return;
	case 0x220: case 0x221: case 0x222: case 0x223:
	case 0x228: case 0x229:
	case 0x388: case 0x389: case 0x38a: case 0x38b:
		if (pc->adlib_enabled)
			adlib_write(pc->adlib, addr, val);
		return;
	case 0x8900:
		switch (val) {
		case 'S': if (pc->shutdown_state == 0) pc->shutdown_state = 1; break;
		case 'h': if (pc->shutdown_state == 1) pc->shutdown_state = 2; break;
		case 'u': if (pc->shutdown_state == 2) pc->shutdown_state = 3; break;
		case 't': if (pc->shutdown_state == 3) pc->shutdown_state = 4; break;
		case 'd': if (pc->shutdown_state == 4) pc->shutdown_state = 5; break;
		case 'o': if (pc->shutdown_state == 5) pc->shutdown_state = 6; break;
		case 'w': if (pc->shutdown_state == 6) pc->shutdown_state = 7; break;
		case 'n': if (pc->shutdown_state == 7) pc->shutdown_state = 8; break;
		default : pc->shutdown_state = 0; break;
		}
		return;
	case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
		i440fx_write_data(pc->i440fx, addr - 0xcfc, val, 0);
		return;
	/* NE2000 networking removed */
	case 0x300: case 0x301: case 0x302: case 0x303:
	case 0x304: case 0x305: case 0x306: case 0x307:
	case 0x308: case 0x309: case 0x30a: case 0x30b:
	case 0x30c: case 0x30d: case 0x30e: case 0x30f:
	case 0x310: case 0x31f:
		return;
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
		i8257_write_chan(pc->isa_dma, addr - 0x00, val, 1);
		return;
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x0c: case 0x0d: case 0x0e: case 0x0f:
		i8257_write_cont(pc->isa_dma, addr - 0x08, val, 1);
		return;
	case 0x81: case 0x82: case 0x83: case 0x87:
		i8257_write_page(pc->isa_dma, addr - 0x80, val);
		return;
	case 0x481: case 0x482: case 0x483: case 0x487:
		i8257_write_pageh(pc->isa_dma, addr - 0x480, val);
		return;
	/* 0xC0/0xC1: SN76489 data port when Tandy enabled, hdma ch0 otherwise.
	 * 0xC2-0xCE: always hdma (Tandy only occupied 0xC0). */
	case 0xc0: case 0xc1:
		if (pc->tandy_enabled)
			sn76489_out(val);
		else
			i8257_write_chan(pc->isa_hdma, addr - 0xc0, val, 1);
		return;
	case 0xc2: case 0xc4: case 0xc6:
	case 0xc8: case 0xca: case 0xcc: case 0xce:
		i8257_write_chan(pc->isa_hdma, addr - 0xc0, val, 1);
		return;
	case 0xd0: case 0xd2: case 0xd4: case 0xd6:
	case 0xd8: case 0xda: case 0xdc: case 0xde:
		i8257_write_cont(pc->isa_hdma, addr - 0xd0, val, 1);
		return;
	case 0x89: case 0x8a: case 0x8b: case 0x8f:
		i8257_write_page(pc->isa_hdma, addr - 0x88, val);
		return;
	case 0x489: case 0x48a: case 0x48b: case 0x48f:
		i8257_write_pageh(pc->isa_hdma, addr - 0x488, val);
		return;
	case 0x224:
		if (pc->sb16_enabled) {
			sb16_mixer_write_indexb(pc->sb16, addr, val);
		}
		return;
	case 0x225:
		if (pc->sb16_enabled) {
			sb16_mixer_write_datab(pc->sb16, addr, val);
		}
		return;
	case 0x226: case 0x22c:
		if (pc->sb16_enabled) {
			sb16_dsp_write(pc->sb16, addr, val);
		}
		return;
	/* Tandy 3-Voice Sound (SN76489) - additional alias ports.
	 * Primary port 0xC0 is handled above.
	 * 0x1E0: Tandy 1000 SX/TX/HX data port
	 * 0x2C0: Tandy 1000 A/B mirror */
	case 0x1E0: case 0x2C0:
		if (pc->tandy_enabled)
			sn76489_out(val);
		return;
	/* Covox Speech Thing (parallel port DAC)
	 * 0x278 = LPT2 data. */
	case 0x278:
		pc->lpt_data[1] = val;              /* latch! */
		if (pc->covox_enabled) pc->covox_sample = val;
		return;
	case 0x27A:
		pc->lpt_ctrl[1] = val & 0x1F;
		return;
	// MPU-401
	case 0x330:
	case 0x331:
		if (pc->mpu401_enabled)
			mpu401_write(addr, val);
		return;
	// Disney Sound Source
	case 0x378:
		pc->lpt_data[0] = val;              /* latch! */
		if (pc->dss_enabled) dss_out(addr, val);
		return;
	case 0x37A:
		pc->lpt_ctrl[0] = val & 0x1F;
		if (pc->dss_enabled) dss_out(addr, val);
		return;
	/* Game port: any write fires the axis one-shots. The value written
	 * is irrelevant on real hardware and is ignored here too. */
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		if (pc->joystick_enabled)
			gameport_write();
		return;
	/* LPT status/control ports are read-only - writes ignored */
	case 0x379: case 0x279:
	default:
///		fprintf(stderr, "out 0x%x => 0x%x\n", val, addr);
		return;
	}
}

static void pc_io_write16(void *o, int addr, u16 val)
{
	debug_write("W16: %ph -> %04Xh\n", addr, val);
	PC *pc = o;
	switch(addr) {
	/* IDE ports */
	case 0x1f0:
		ide_data_writew(pc->ide, val);
		return;
	case 0x170:
		ide_data_writew(pc->ide2, val);
		return;
    case 0x260: case 0x261: case 0x262: case 0x263:
		pc_io_write(o, addr, (uint8_t) val);
		pc_io_write(o, addr + 1, val >> 8);
        return;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de:
		vga_ioport_write(pc->vga, addr, val & 0xff);
		vga_ioport_write(pc->vga, addr + 1, (val >> 8) & 0xff);
		return;
	case 0x1ce: case 0x1cf:
		vbe_write(pc->vga, addr - 0x1ce, val);
		return;
	case 0xcfc: case 0xcfe:
		i440fx_write_data(pc->i440fx, addr - 0xcfc, val, 1);
		return;
	/* NE2000 networking removed */
	case 0x310:
		return;
	default:
///		fprintf(stderr, "outw 0x%x => 0x%x\n", val, addr);
		return;
	}
}

static void pc_io_write32(void *o, int addr, u32 val)
{
	debug_write("W32: %ph -> %08Xh\n", addr, val);
	PC *pc = o;
	switch(addr) {
	/* IDE ports */
	case 0x1f0:
		ide_data_writel(pc->ide, val);
		return;
	case 0x170:
		ide_data_writel(pc->ide2, val);
		return;
	case 0xcf8:
		i440fx_write_addr(pc->i440fx, 0, val, 2);
		return;
	case 0xcfc:
		i440fx_write_data(pc->i440fx, 0, val, 2);
		return;
	/* Emulink FDD command/data ports */
	case 0xf1f0:
		emulink_cmd_write(pc, val);
		return;
	case 0xf1f4:
		emulink_arg_write(pc, val);
		return;
    case 0x260: case 0x261: case 0x262: case 0x263:
		pc_io_write16(o, addr, (uint16_t) val);
		pc_io_write16(o, addr + 2, val >> 16);
        return;
	default:
///		do_log(stderr, "outd 0x%x => 0x%x\n", val, addr);
		return;
	}
}

static int pc_io_write_string(void *o, int addr, uint32_t buf, int size, int count)
{
	debug_write("WS: %ph [%d / %d]\n", addr, size, count);
	PC *pc = o;
	switch(addr) {
	case 0x1f0:
		return ide_data_write_string(pc->ide, buf, size, count);
	case 0x170:
		return ide_data_write_string(pc->ide2, buf, size, count);
	case 0xf1f4:
		return emulink_data_write(pc, buf, size, count);
	}
	return 0;
}

void pc_vga_step(void *o)
{
	PC *pc = o;
	int refresh = vga_step(pc->vga);
	if (refresh) {
		vga_refresh(pc->vga, pc->redraw, pc->redraw_data, 0);
	}
}

static uint32_t pc_last_device_service;

static void __not_in_flash_func(pc_service_impl)(PC *pc)
{
    /*
     * Native ELF code runs synchronously on core0, so the outer emulation
     * loop cannot reach pc_step() until the native application yields.  This
     * is the same device tail normally executed after a CPU burst, but it is
     * deliberately separated from cpu_step(): cooperative yield must never
     * execute or advance guest CPU state.
     */
    pc_last_device_service = get_uticks();

    {
        /* Завершение отложенного INT 15h/AH=83h (см. bios_15h.c). */
        extern void bios_15h_event_wait_tick(void);
        bios_15h_event_wait_tick();
    }

    PROF_T(t_dev);
    /* reset_request is handled in main.c via load_bios_and_reset() */
    int refresh = vga_step(pc->vga);
    if (pc->enable_serial)
        u8250_update(pc->serial);
    kbd_step(pc->i8042);
    i8257_dma_run(pc->isa_dma);
    i8257_dma_run(pc->isa_hdma);
    if (pc->fdc) fdc_tick(pc->fdc);
    PROF_ADD(t_dev, devices);

    {
        PROF_T(t_poll);
        if (pc->poll) pc->poll(pc->redraw_data);
        PROF_ADD(t_poll, poll);
    }
    {
        PROF_T(t_refresh);
        if (refresh && pc->redraw) {
            vga_refresh(pc->vga, pc->redraw, pc->redraw_data,
                        pc->full_update != 0);
            if (pc->full_update == 2)
                pc->full_update = 0;
        }
        PROF_ADD(t_refresh, refresh);
    }
}

void __not_in_flash_func(pc_service)(PC *pc)
{
    pc_service_impl(pc);
}

void __not_in_flash_func(pc_step)(PC *pc, size_t max_ops)
{
    PROF_T(t_total);
    /*
     * Порядок слайса: сначала CPU-burst, затем обслуживание устройств.
     *
     * Вложенные нативные вызовы гостевого кода (bios_intcall(): CON-вывод
     * посимвольно, INT 16h-опросы) крутят pc_step() до native_done;
     * типичный вложенный обработчик - считанные инструкции (трап-страница
     * возвращает в нативный код сразу). Завершившийся установкой
     * native_done burst выходит, НЕ заходя в платформенную преамбулу
     * (USB-poll, DMA, FDC, редрав) - иначе она исполнялась бы на каждый
     * символ вывода (двухпорядковый регресс скорости нативного CON
     * против гостевого DOS).
     *
     * Ранний выход рейт-лимитирован (~1 мс): во время долгих НАТИВНЫХ
     * фаз (kernel-init, обвязка DOS) внешний цикл стоит, и устройства
     * живут только на вложенных pc_step()'ах - безусловный пропуск
     * преамбулы замораживал бы PIT-тик (0x46C) и опрос клавиатуры
     * (keycheck() конфига ждал бы вечно и клавишу, и таймаут). Раз в
     * миллисекунду преамбула проходит даже под штормом коротких
     * вложенных вызовов. Выход - только по ПЕРЕХОДУ native_done внутри
     * данного burst'а: залипший флаг внешнего потока не морит устройства.
     *
     * Задержка доставки поднятых устройствами IRQ - один слайс, как и
     * была (сдвиг фазы); редрав после burst'а показывает свежий vram.
     * Отложенный INT 15h/AH=83h завершается в девайс-блоке - тем же
     * миллисекундным гарантированным тактом.
     */
    bool was_native_done = pc->cpu->native_done;
    if (!pc->paused) {
        if (max_ops > 4096) max_ops = 4096;
        PROF_T(t_cpu);
        cpu_step(pc->cpu, max_ops);
        PROF_ADD(t_cpu, cpu);
        if (pc->cpu->native_done && !was_native_done &&
            (uint32_t)(get_uticks() - pc_last_device_service) < 1000u)
            return;
    }

    pc_service_impl(pc);

#if SUBSYS_PROFILE
    PROF_ADD(t_total, total);
    if (++g_prof.steps >= PROF_REPORT_STEPS)
        prof_report();
#endif

#if 0
    /* Dump profile every ~10M inАions */
    static uint32_t prof_dump_counter = 0;
    prof_dump_counter += 4096;
    if (prof_dump_counter >= 10000000) {
        i386_profile_dump();
        i386_profile_reset();
        prof_dump_counter = 0;
    }
#endif
}

static void raise_irq(void *o, PicState2 *s)
{
	cpu_raise_irq(o);
}

static int read_irq(void *o)
{
	PicState2 *s = o;
	return i8259_read_irq(s);
}

static void set_irq(void *o, int irq, int level)
{
	PicState2 *s = o;
	return i8259_set_irq(s, irq, level);
}

static void set_pci_vga_bar(void *opaque, int bar_num, uint32_t addr, bool enabled)
{
	PC *pc = opaque;
	if (enabled)
		pc->pci_vga_ram_addr = addr;
	else
		pc->pci_vga_ram_addr = -1;
#ifdef USEKVM
	if (enabled)
		cpukvm_register_mem(pc->cpu, 2, addr, pc->vga_mem_size,
				    pc->vga_mem);
	else
		cpukvm_register_mem(pc->cpu, 2, addr, 0,
				    NULL);
#endif
}

u8 __not_in_flash_func(iomem_read8)(void *iomem, uword addr)
{
	PC *pc = iomem;
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		addr -= vga_addr2;
		if (addr < pc->vga_mem_size)
			return pc->vga_mem[addr];
		else
			return 0;
	}
	return vga_mem_read(pc->vga, addr - 0xa0000);
}

void __not_in_flash_func(iomem_write8)(void *iomem, uword addr, u8 val)
{
	PC *pc = iomem;
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		addr -= vga_addr2;
		if (addr < pc->vga_mem_size)
			pc->vga_mem[addr] = val;
		return;
	}
	vga_mem_write(pc->vga, addr - 0xa0000, val);
}

u16 __not_in_flash_func(iomem_read16)(void *iomem, uword addr)
{
	return iomem_read8(iomem, addr) |
		((u16) iomem_read8(iomem, addr + 1) << 8);
}

void __not_in_flash_func(iomem_write16)(void *iomem, uword addr, u16 val)
{
	PC *pc = iomem;
	// fast path for vga ram
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		addr -= vga_addr2;
		if (addr + 1 < pc->vga_mem_size)
			*(uint16_t *)&(pc->vga_mem[addr]) = val;
		return;
	}
	vga_mem_write16(pc->vga, addr - 0xa0000, val);
}

u32 __not_in_flash_func(iomem_read32)(void *iomem, uword addr)
{
	return iomem_read16(iomem, addr) |
		((u32) iomem_read16(iomem, addr + 2) << 16);
}

void __not_in_flash_func(iomem_write32)(void *iomem, uword addr, u32 val)
{
	PC *pc = iomem;
	// fast path for vga ram
	uword vga_addr2 = pc->pci_vga_ram_addr;
	if (addr >= vga_addr2) {
		uword vga_addr2 = pc->pci_vga_ram_addr;
		addr -= vga_addr2;
		if (addr + 3 < pc->vga_mem_size)
			*(uint32_t *)&(pc->vga_mem[addr]) = val;
		return;
	}
	vga_mem_write32(pc->vga, addr - 0xa0000, val);
}


// Новая версия принимает host-pointer напрямую
bool __not_in_flash_func(iomem_write_string_ptr)(void *iomem, uint32_t addr, const uint8_t *buf, int len)
{
    PC *pc = iomem;
    uint32_t vga_addr2 = pc->pci_vga_ram_addr;
    if (addr >= vga_addr2) {
        addr -= vga_addr2;
        if (addr + len < pc->vga_mem_size) {
            memcpy(pc->vga_mem + addr, buf, len);
            return true;
        }
        return false;
    }
    return vga_mem_write_string(pc->vga, addr - 0xa0000, (uint8_t*)buf, len);
}

// Старая версия теперь через новую
bool __not_in_flash_func(iomem_write_string)(void *iomem, uint32_t addr, uint32_t buf, int len)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(ega128_paging_active())) {
        while (len > 0) {
            uint32_t span;
            const uint8_t *p = ega128_page_ptr(buf, &span, false);
            int n = len < (int)span ? len : (int)span;
            if (!iomem_write_string_ptr(iomem, addr, p, n)) return false;
            addr += n; buf += n; len -= n;
        }
        return true;
    }
#endif
    return iomem_write_string_ptr(iomem, addr, PC_RAM + buf, len);
}

static void pc_reset_request(void *p)
{
	PC *pc = p;
	pc->reset_request = 1;
}

extern uint8_t gfx_buffer[];

static CMOS *_pc_cmos_for_floppy = NULL;
static void cmos_floppy_update(uint8_t ta, uint8_t tb) {
    cmos_set_floppy_types(_pc_cmos_for_floppy, ta, tb);
    /* Здесь раньше безусловно писалось 0x41 и затирался equipment byte,
     * который pc_new() собирает правильно (floppy | kbd | display | FPU |
     * число FDD).  В POST-дампе это видно как CMOS 14=41 вместо 4D.
     * Трогаем только биты 7-6 (кол-во FDD) и бит 0 (наличие FDD). */
    {
        CMOS *c = _pc_cmos_for_floppy;
        uint8_t eq  = cmos_get(c, 0x14);
        uint8_t nfd = (ta ? 1 : 0) + (tb ? 1 : 0);
        eq &= (uint8_t)~0xC1;
        if (nfd)
            eq |= (uint8_t)(0x01 | ((nfd - 1) << 6));
        cmos_set(c, 0x14, eq);
    }
    cmos_update_checksum(_pc_cmos_for_floppy);
}

static PC *_pc_for_fdc = NULL;
static void fdc_mediachange_notify(int drive) {
    if (_pc_for_fdc && _pc_for_fdc->fdc)
        fdc_media_changed(_pc_for_fdc->fdc, drive);

    /* disk.c has a single media-change callback slot.  Once the hardware FDC
       callback replaces bios_13h_init()'s early callback, explicitly keep the
       BIOS change-line state in sync as well; FDOS C_MEDIACHK reaches it via
       INT 13h/AH=16h. */
    bios_13h_fdc_mediachange(drive);
}

/* CD-ROM media change callback: called by disk layer when a CD-ROM drive
 * is inserted (filename != NULL) or ejected (filename == NULL).
 *
 * drivenum = ata[] index (0..3), NOT the diskui selected_drive (0..4):
 *   ata[0] -> ide,  drive 0  (primary master)
 *   ata[1] -> ide,  drive 1  (primary slave)
 *   ata[2] -> ide2, drive 0  (secondary master)  <- DRIVE_CDROM_E via diskui
 *   ata[3] -> ide2, drive 1  (secondary slave)
 */
static PC *_pc_for_cdrom = NULL;
static void cdrom_change_notify(int drivenum, const char *filename, int was_present) {
    if (!_pc_for_cdrom) return;
    IDEIFState *ide = drivenum < 2 ? _pc_for_cdrom->ide : _pc_for_cdrom->ide2;
    int ide_drive;
    switch (drivenum) {
        case 0: ide_drive = 0; break;
        case 1: ide_drive = 1; break;
        case 2: ide_drive = 0; break;
        case 3: ide_drive = 1; break;
        default: return;
    }
    FIL *f = filename ? ata_get_file(drivenum) : NULL;
    ide_change_cd(ide, ide_drive, f, was_present);
}

PC *pc_new(SimpleFBDrawFunc *redraw, void (*poll)(void *), void *redraw_data,
	   u8 *fb, PCConfig *conf)
{
#if TRACE_PORTS
	f_open(&ports_log, "ports.log", FA_WRITE | FA_CREATE_ALWAYS);
#endif
	PC *pc = malloc(sizeof(PC));
	g_pc = pc;
	CPU_CB *cb = NULL;
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
	if (!ega128_paging_active())
#endif
	for(int i = 0; i < (conf->mem_size >> 2); ++i)
	 PC_RAM32[i] = 0;
#ifdef BUILD_ESP32
	extern char *pcram;
	extern long pcram_len;
	pcram = mem + 0xa0000;
	pcram_len = 0xc0000 - 0xa0000;
#endif
	phys_mem_size = conf->mem_size;
	pc->cpu = cpu_new(conf->cpu_gen, &cb);
	pc->fpu_enabled = conf->fpu ? 1 : 0;
	if (conf->fpu)
		enable_fpu(pc->cpu);
	pc->bios = conf->bios;
	pc->cpu->bios = pc->bios;
	if (!pc->bios) {
		cpu_install_bios_handlers(pc->cpu);
	}
	pc->vga_bios = conf->vga_bios;
	pc->cpu->native_done = false;
	pc->enable_serial = conf->enable_serial;
#if !defined(_WIN32) && !defined(__wasm__)
	if (pc->enable_serial)
		CaptureKeyboardInput();
#endif
	pc->full_update = 0;

	pc->pic = i8259_init(raise_irq, pc->cpu);
	cb->pic = pc->pic;
	cb->pic_read_irq = read_irq;

	pc->pit = i8254_init(0, pc->pic, set_irq);
	pc->serial = u8250_init(4, pc->pic, set_irq);
	pc->cmos = cmos_init(conf->mem_size, 8, pc->pic, set_irq);
	_pc_cmos_for_floppy = pc->cmos;

	/* Set up INT 13h disk handler (real mode - DOS) */
	disk_set_cpu(pc->cpu);
	disk_set_cmos_callback(cmos_floppy_update);
	bios_13h_init();

	netredirect_init(pc->cpu, conf->redirector);

	/* Set up IDE emulation (protected mode - Win95) */
	pc->ide  = ide_allocate(14, pc->pic, set_irq);
	pc->ide2 = ide_allocate(15, pc->pic, set_irq);

	/* Register CD-ROM callback BEFORE insertdisk so the callback fires
	 * correctly when insertdisk opens a configured CD image below. */
	_pc_for_cdrom = pc;
	disk_set_cdrom_change_callback(cdrom_change_notify);

	/* Attach hard disks and configured CD-ROMs.
	 * ide_attach_cd MUST come before insertdisk for CD slots: insertdisk
	 * immediately fires disk_cdrom_change_cb which calls ide_change_cd,
	 * and that requires drives[n] to already exist. */
	for (int i = 0; i < 4; i++) {
		if (!conf->ata[i] || conf->ata[i][0] == 0)
			continue;
		if (conf->iscd[i]) {
			/* Attach ATAPI slot first, then open the image */
			if (i < 2)
				ide_attach_cd(pc->ide, i);
			else
				ide_attach_cd(pc->ide2, i - 2);
			insertdisk(i, false, true, conf->ata[i]);
		} else {
			/* HDD: insertdisk opens the file, then attach */
			insertdisk(i, false, false, conf->ata[i]);
			FIL *fil = ata_get_file(i);
			if (fil)
				ide_attach_ata(i < 2 ? pc->ide : pc->ide2,
				               i < 2 ? i : i - 2,
				               fil,
				               ata_get_cyls(i),
				               ata_get_heads(i),
				               ata_get_sects(i));
		}
	}

	disk_set_raw_sd_hdd(conf->raw_sd_hdd);

	/* CD-ROM E: always present on ide2/drive0 (secondary master).
	 * Only attach if cdc= didn't already claim that slot (ata[2]). */
	if (!ide_has_drive(pc->ide2, 0))
		ide_attach_cd(pc->ide2, 0);



	ide_fill_cmos(pc->ide, pc->cmos, cmos_set);

	/* CMOS 0x10 — типы FDD (4 = 1.44M 3.5") */
	cmos_set(pc->cmos, 0x10, (uint8_t)fdds_types());

	/* CMOS 0x14 — equipment byte:
	 *   bit 0     : floppy installed
	 *   bit 1     : math coprocessor
	 *   bit 2     : keyboard present
	 *   bit 3     : display enabled
	 *   bits 5-4  : 00 = EGA/VGA
	 *   bits 7-6  : (число FDD - 1)
	 */
	uint8_t nfd = 2;                        /* эмулируем два дисковода */
	uint8_t eq  = 0x01 | 0x04 | 0x08;
	if (conf->fpu) eq |= 0x02;
	eq |= (uint8_t)((nfd - 1) << 6);
	cmos_set(pc->cmos, 0x14, eq);

	cmos_update_checksum(pc->cmos);         /* обязательно, после ide_fill_cmos */

	int piix3_devfn;
	pc->i440fx = i440fx_init(&pc->pcibus, &piix3_devfn);
	pc->pci_ide = piix3_ide_init(pc->pcibus, piix3_devfn + 1);

	cb->io = pc;
	cb->io_read8 = pc_io_read;
	cb->io_write8 = pc_io_write;
	cb->io_read16 = pc_io_read16;
	cb->io_write16 = pc_io_write16;
	cb->io_read32 = pc_io_read32;
	cb->io_write32 = pc_io_write32;
	cb->io_read_string = pc_io_read_string;
	cb->io_write_string = pc_io_write_string;

	pc->boot_start_time = 0;

	/* The build profile owns the physical video-memory size.  Normal VGA is
	 * 256 KiB; EGA128/VGA128 use 128 KiB and MCGA uses 64 KiB. */
#ifdef MCGA
	pc->vga_mem_size = 64u << 10;
#elif defined(EGA128) || defined(VGA128)
	pc->vga_mem_size = 128u << 10;
#else
	pc->vga_mem_size = 256u << 10;
#endif
	pc->vga_mem = gfx_buffer;
	memset(pc->vga_mem, 0, pc->vga_mem_size);
	pc->vga = vga_init(pc->vga_mem, pc->vga_mem_size,
			   fb, conf->width, conf->height);
	vga_set_force_8dm(pc->vga, conf->vga_force_8dm);
	pc->pci_vga = vga_pci_init(pc->vga, pc->pcibus, pc, set_pci_vga_bar);
	pc->pci_vga_ram_addr = -1;
	disk_set_vga(pc->vga);

	/* Attach floppy disks using INT 13h disk handler */
	const char **fdd = conf->fdd;
	for (int i = 0; i < 2; i++) {
		if (!fdd[i] || fdd[i][0] == 0)
			continue;
		/* Floppy drives use drivenum 0 and 1 */
		insertdisk(i, true, false, fdd[i]);
	}

	cb->iomem = pc;

	pc->redraw = redraw;
	pc->redraw_data = redraw_data;
	pc->poll = poll;

	pc->i8042 = i8042_init(&(pc->kbd), &(pc->mouse),
			       1, 12, pc->pic, set_irq,
			       pc, pc_reset_request);
	i8042_set_cpu(pc->cpu);
	pc->adlib = adlib_new();
	/* NE2000 networking removed */
	pc->isa_dma = i8257_new(0x00, 0x80, 0x480, 0);
	pc->isa_hdma = i8257_new(0xc0, 0x88, 0x488, 1);
	/* Emulink FDD – virtual floppy on ports 0xF1F0/0xF1F4 (required by BIOS) */
	memset(&pc->emulink, 0, sizeof(pc->emulink));
	pc->emulink.cmd = -1;

	/* FDC (Intel 8272A/82077AA) – port I/O 0x3F0-0x3F7, DMA ch2, IRQ 6.
	 * Created after isa_dma/pic, and floppy images already inserted above,
	 * so fdc_media_changed fires correctly on subsequent insert/eject. */
	pc->fdc = fdc_new(pc->pic, pc->isa_dma);
	_pc_for_fdc = pc;
	disk_set_fdc_mediachange_callback(fdc_mediachange_notify);
	pc->sb16 = sb16_new(0x220, 5,
			    pc->isa_dma, pc->isa_hdma,
			    pc->pic, set_irq);
	pc->pcspk = pcspk_init(pc->pit);
	sn76489_reset();

	// Audio/mouse enable flags default to enabled
	// These can be disabled via config_set_* functions at runtime
	pc->adlib_enabled = 1;
	pc->sb16_enabled = 1;
	pc->pcspk_enabled = 1;
	pc->tandy_enabled = 0;
	pc->covox_enabled = 1;
	pc->mpu401_enabled = 1;
	pc->covox_sample  = 0;
	pc->dss_enabled = 0;
	pc->mouse_enabled = 1;

	/* LPT: защёлки data/control. control=0x04 (nInit=1) — то же значение,
	 * которое раньше жёстко возвращалось из порта 0x37A. */
	pc->lpt_data[0] = pc->lpt_data[1] = 0xFF;
	pc->lpt_ctrl[0] = pc->lpt_ctrl[1] = 0x04;

	pc->port92 = 0x2;
	pc->shutdown_state = 0;
	pc->reset_request = 0;
	return pc;
}

static void rom_puts(uint32_t addr, const char *s) {
    while (*s) pstore8(addr++, (uint8_t)*s++);
    pstore8(addr, 0x00);
}

/* "Jul 12 2026" (__DATE__) -> "07/12/26" */
static void rom_bios_date(uint32_t addr) {
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;               /* "Mmm dd yyyy" */
    int m = 0;
    for (int i = 0; i < 12; ++i)
        if (!strncmp(d, mon + i * 3, 3)) { m = i + 1; break; }
    char buf[9];
    buf[0] = '0' + m / 10;  buf[1] = '0' + m % 10;  buf[2] = '/';
    buf[3] = (d[4] == ' ') ? '0' : d[4];
    buf[4] = d[5];          buf[5] = '/';
    buf[6] = d[9];          buf[7] = d[10]; buf[8] = 0;
    for (int i = 0; i < 8; ++i) pstore8(addr + i, (uint8_t)buf[i]);
}

// IRET is saved on 0xFFF06
static void point2iret(u32 intno) {
	pstore16(intno*4, 0x0006);
	pstore16(intno*4 + 2, 0xFFF0);
}
static void point2zero(u32 intno) {
	pstore16(intno*4, 0);
	pstore16(intno*4 + 2, 0);
}

static void install_dpte(int idx, uint32_t addr)
{
    bios_hdd_info_t info;
    if (!bios_hdd_get_info((uint8_t)idx, &info) || info.ata_slot < 0) {
        for (int i = 0; i < 16; i++)
            pstore8(addr + i, 0x00);
        return;
    }
	// Primary IDE: 0x1F0, Secondary: 0x170
    int8_t slot = info.ata_slot;
    uint16_t iobase  = (slot < 2) ? 0x01F0 : 0x0170;
    uint16_t ctlbase = (slot < 2) ? 0x03F6 : 0x0376;
//    uint8_t  devhead = (slot & 1) ? 0xB0 : 0xA0; // slave/master
	uint8_t  devhead = (slot & 1) ? 0xF0 : 0xE0; // With LBA

    uint8_t buf[16] = {0};
    buf[0]  = (uint8_t)(iobase & 0xFF);
    buf[1]  = (uint8_t)(iobase >> 8);
    buf[2]  = (uint8_t)(ctlbase & 0xFF);
    buf[3]  = (uint8_t)(ctlbase >> 8);
    buf[4]  = devhead;
    buf[5]  = 0x00;   // host bus/interface type: ISA-compatible ATA
    buf[6]  = 0x00;   // flags: no CHS translation/DMA assumptions
    buf[7]  = 0x02;   // PIO mode 2
    buf[8]  = 0x00;   // DMA mode: none
    buf[9]  = 0x0B;   // PIO cycle timing, conservative default
    buf[10] = 0x00;
    buf[11] = 0x00;   // DMA channel
    buf[12] = 0x00;
    buf[13] = 0x00;
    buf[14] = 0x00;
    buf[15] = 0x00;   // checksum placeholder

    // checksum: сумма байт 0..14, результат = (-sum) & 0xFF
    uint8_t sum = 0;
    for (int i = 0; i < 15; i++) sum += buf[i];
    buf[15] = (uint8_t)((-sum) & 0xFF);

    for (int i = 0; i < 16; i++)
        pstore8(addr + i, buf[i]);
}

static void install_hdd_dpt(PC *pc, int idx, uint32_t addr)
{
    // Вектор INT 41h = 0x104, INT 46h = 0x118
    uint32_t vec = (idx == 0) ? 0x41 * 4 : 0x46 * 4;
    bios_hdd_info_t info;

    if (!bios_hdd_get_info((uint8_t)idx, &info)) {
        // Нет диска — вектор указывает на нули, не на fake BIOS
        // Просто обнулить таблицу и поставить вектор
        for (int i = 0; i < 16; i++)
            pstore8(addr + i, 0x00);
        pstore16(vec, 0x0000);
        pstore16(vec + 2, 0x0000);
        return;
	} else {
        uint16_t cyls  = info.cyls;
        uint16_t heads = info.heads;
        uint16_t sects = info.sects;

        // Fixed Disk Parameter Table, 16 bytes (INT 41h/46h format)
        pstore16(addr + 0x00, cyls);          /* max cylinders */
        pstore8 (addr + 0x02, (uint8_t)heads);/* max heads */
        pstore8 (addr + 0x03, 0x00);          /* reserved (XT: starting reduced write current cyl low) */
        pstore8 (addr + 0x04, 0x00);          /* reserved (XT: starting reduced write current cyl high) */

		pstore16(addr + 0x05, 0xFFFF);                    /* write precomp: disabled */
		pstore8 (addr + 0x07, 0x0B);                      /* max ECC burst length */
		pstore8 (addr + 0x08, heads > 8 ? 0x08 : 0x00);  /* drive control */
        pstore8 (addr + 0x09, 0x00);          /* reserved */
        pstore8 (addr + 0x0A, 0x00);          /* reserved */
        pstore8 (addr + 0x0B, 0x00);          /* reserved */
		pstore16(addr + 0x0C, cyls ? (cyls - 1) : 0);     /* landing zone */
        pstore8 (addr + 0x0E, (uint8_t)sects);/* sectors per track */
        pstore8 (addr + 0x0F, 0x00);          /* reserved */
    }

    // Вектор → таблица (в любом случае, даже если нули)
    pstore16(vec,     (uint16_t)(addr - 0xF0000)); /* offset */
    pstore16(vec + 2, 0xF000);                     /* segment */	
}

static int bios_post_beep_pending;
static bool bios_cold_post_pending = true;

void pc_set_cold_post_pending(bool cold)
{
    bios_cold_post_pending = cold;
}

static void bios_post_table_rule(PC *pc, uint8_t left, uint8_t middle, uint8_t right)
{
    static char line[81];

    line[0] = (char)left;
    memset(line + 1, 0xC4, 38);
    line[39] = (char)middle;
    memset(line + 40, 0xC4, 39);
    line[79] = (char)right;
    line[80] = 0;
    bios_puts(pc->cpu, line);
}

static void bios_post_table_row(PC *pc, const char *left, const char *right)
{
    static char line[81];
    size_t n;

    memset(line, ' ', 80);
    line[0] = (char)0xB3;
    line[39] = (char)0xB3;
    line[79] = (char)0xB3;

    n = strlen(left);
    if (n > 36) n = 36;
    memcpy(line + 2, left, n);

    n = strlen(right);
    if (n > 37) n = 37;
    memcpy(line + 41, right, n);

    line[80] = 0;
    bios_puts(pc->cpu, line);
}

static const char *bios_post_fdd_type(uint8_t drive)
{
    switch ((fdds_types() >> (drive ? 4 : 0)) & 0x0F) {
    case 1: return "360 KB 5.25";
    case 2: return "1.2 MB 5.25";
    case 3: return "720 KB 3.5";
    case 4: return "1.44 MB 3.5";
    case 5: return "2.88 MB 3.5";
    default: return "none";
    }
}

static void bios_post_hdd_text(char *dst, size_t dst_size, uint8_t bios_index)
{
    bios_hdd_info_t info;

    if (!bios_hdd_get_info(bios_index, &info)) {
        dst[0] = 0;
        return;
    }

    unsigned long mib = (unsigned long)(info.total_sectors >> 11);
    if (info.raw_sd) {
        snprintf(dst, dst_size, "HDD %02Xh  : RAW-SD %lu MB",
                 0x80u + bios_index, mib);
    } else {
        unsigned controller = (unsigned)info.ata_slot >> 1;
        unsigned device = (unsigned)info.ata_slot & 1u;
        snprintf(dst, dst_size, "HDD %02Xh  : ATA%u-%u %lu MB",
                 0x80u + bios_index, controller, device, mib);
    }
}

static void bios_post_components(PC *pc, size_t psram_size)
{
    (void)psram_size;
    char left[40], right[40];
    char serial[40], parallel[40];
    char hdd[6][40];

#if I386_MODE
    snprintf(left, sizeof(left), "CPU      : 80386");
#else
    snprintf(left, sizeof(left), "CPU      : 80286");
#endif
    snprintf(right, sizeof(right), "FPU      : %s",
             pc->fpu_enabled ? "80387" : "not installed");

    bios_post_table_rule(pc, 0xDA, 0xC2, 0xBF);
    bios_post_table_row(pc, left, right);

#if defined(EGA128)
    snprintf(left, sizeof(left), "Video    : EGA 128 KB [%s]",
             SELECT_VGA ? "VGA" : "HDMI");
#elif defined(VGA128)
    snprintf(left, sizeof(left), "Video    : VGA 128 KB [%s]",
             SELECT_VGA ? "VGA" : "HDMI");
#elif defined(MCGA)
    snprintf(left, sizeof(left), "Video    : MCGA 64 KB [%s]",
             SELECT_VGA ? "VGA" : "HDMI");
#else
    snprintf(left, sizeof(left), "Video    : VGA VBE 1.2 256 KB [%s]",
             SELECT_VGA ? "VGA" : "HDMI");
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active())
        snprintf(right, sizeof(right), "%s", ega128_paging_post_label());
    else
        snprintf(right, sizeof(right), "QSPI PSRAM Up to 16 MB");
#else
    snprintf(right, sizeof(right), "QSPI PSRAM Up to 16 MB");
#endif
    bios_post_table_row(pc, left, right);
#if EMULATE_LTEMS
    if (phys_mem_size >= (4u << 20)) {
    	snprintf(left, sizeof(left), "PC RAM   : %lu KB / 2048 KB EMS",
        	     (unsigned long)((phys_mem_size - 2048) >> 10));
	} else {
		snprintf(left, sizeof(left), "PC RAM   : %lu KB",
				(unsigned long)(phys_mem_size >> 10));
	}
#else
    snprintf(left, sizeof(left), "PC RAM   : %lu KB",
             (unsigned long)(phys_mem_size >> 10));
#endif
    snprintf(right, sizeof(right), "Mouse    : %s",
             pc->mouse_enabled ? "PS/2" : "not installed");
    bios_post_table_row(pc, left, right);

    bios_post_table_rule(pc, 0xC3, 0xC5, 0xB4);

    snprintf(serial, sizeof(serial), "Serial   : %s",
             pc->enable_serial ? "03F8h" : "none");
    if (pc->dss_enabled && pc->covox_enabled)
        snprintf(parallel, sizeof(parallel), "Parallel : 0378h 0278h");
    else if (pc->dss_enabled)
        snprintf(parallel, sizeof(parallel), "Parallel : 0378h");
    else if (pc->covox_enabled)
        snprintf(parallel, sizeof(parallel), "Parallel : 0278h");
    else
        snprintf(parallel, sizeof(parallel), "Parallel : none");

    snprintf(left, sizeof(left), "FDD A    : %s", bios_post_fdd_type(0));
    bios_post_table_row(pc, left, serial);
    snprintf(left, sizeof(left), "FDD B    : %s", bios_post_fdd_type(1));
    bios_post_table_row(pc, left, parallel);

    for (uint8_t i = 0; i < 6; ++i)
        bios_post_hdd_text(hdd[i], sizeof(hdd[i]), i);

    if (!hdd[0][0]) {
        strcpy(left, "HDD      : none");
        bios_post_table_row(pc, left, right);
    } else {
        for (uint8_t i = 0; i < 6; i += 2) {
            if (!hdd[i][0] && !hdd[i + 1][0])
                break;
            bios_post_table_row(pc, hdd[i], hdd[i + 1]);
        }
    }

    bios_post_table_rule(pc, 0xC0, 0xC1, 0xD9);
}

void pc_play_pending_post_beep(PC *pc)
{
    int result = bios_post_beep_pending;
    if (!result || !pc || !pc->pcspk || !pc->pcspk_enabled)
        return;

    bios_post_beep_pending = 0;

    unsigned hz = result > 0 ? 1000u : 300u;
    unsigned ms = result > 0 ? 100u : 500u;
    uint16_t divisor = (uint16_t)(PIT_FREQ / hz);
    if (divisor == 0)
        divisor = 1;

    uint8_t old61 = (uint8_t)pcspk_ioport_read(pc->pcspk);
    i8254_ioport_write(pc->pit, 0x43, 0xB6); /* ch2, lo/hi, mode 3 */
    i8254_ioport_write(pc->pit, 0x42, divisor & 0xFF);
    i8254_ioport_write(pc->pit, 0x42, divisor >> 8);
    pcspk_ioport_write(pc->pcspk, old61 | 0x03);

    /* Audio is now serviced by core1's 44.1-kHz repeating timer, so core0 may
     * sleep while PIT2 remains connected to the speaker. */
    usleep(ms * 1000u);
    pcspk_ioport_write(pc->pcspk, old61);
}

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
static bool bios_post_paged_memory_test(PC *pc)
{
    const uint32_t total = EGA128_VIRTUAL_RAM_SIZE;
    const uint32_t progress_step = 256u << 10;

    bios_printf(pc->cpu, "RAM memory test: 00000000  %5lu / %5lu KB",
                0ul, (unsigned long)(total >> 10));

    for (uint32_t addr = 0; addr < total; addr += 4) {
        /* A0000h..BFFFFh is the VGA aperture.  C0000h..FFFFFh is backed
         * by pageable guest memory too (UMB plus native/external ROM data),
         * so it must be verified through the same pload/pstore path. */
        if (addr == 0x000A0000u)
            addr = 0x000C0000u;

        uint32_t old = pload32(addr);
        uint32_t pattern = addr ^ 0xA55AA55Au;

        pstore32(addr, pattern);
        if (pload32(addr) != pattern) {
            pstore32(addr, old);
            bios_printf(pc->cpu, "\rRAM memory test: FAILED at %08lX (%5lu KB)\n",
                        (unsigned long)addr, (unsigned long)(addr >> 10));
            return false;
        }
        pstore32(addr, old);

        uint32_t done = addr + 4;
        if ((done % progress_step) == 0 || done == total) {
            bios_printf(pc->cpu, "\rRAM memory test: %08lX  %5lu / %5lu KB",
                        (unsigned long)done, (unsigned long)(done >> 10),
                        (unsigned long)(total >> 10));
        }
    }

    bios_printf(pc->cpu, "  OK\n");
    return true;
}
#endif

static bool bios_post_psram_test(PC *pc, size_t psram_size)
{
    const size_t step = 256u << 10;
    bool ok = true;
    int size_mb = (int)(psram_size >> 20);
    int psram_freq = config_get_psram_freq();

    if (config_get_psram_size_mb() == size_mb &&
        config_get_psram_test_freq() == psram_freq)
        return true;

    psram_prepare_nondestructive_test();

    bios_printf(pc->cpu, "PSRAM memory test: 00000000  %5lu / %5lu KB",
                0ul, (unsigned long)(psram_size >> 10));

    for (size_t off = 0; off < psram_size; off += step) {
        size_t len = psram_size - off;
        if (len > step)
            len = step;
        if (!psram_test_nondestructive(off, len)) {
            ok = false;
            bios_printf(pc->cpu, "\rPSRAM memory test: FAILED at %08lX (%5lu KB)\n",
                        (unsigned long)off, (unsigned long)(off >> 10));
            break;
        }
        bios_printf(pc->cpu, "\rPSRAM memory test: %08lX  %5lu / %5lu KB",
                    (unsigned long)(off + len),
                    (unsigned long)((off + len) >> 10),
                    (unsigned long)(psram_size >> 10));
    }

    if (ok) {
        bios_printf(pc->cpu, "  OK\n");
        config_set_psram_test_cache(size_mb, psram_freq);
        (void)config_save_all();
    }
    return ok;
}

void bios_post(PC *pc) {
// POST
    const uint16_t ebda_seg = 0x9FC0;                 /* 1 KiB EBDA at 9FC00 */
    const uint32_t ebda_phys = (uint32_t)ebda_seg << 4;
    const bool cold_post = bios_cold_post_pending;
    bios_cold_post_pending = false;
    reset_umb();

    /* Match the controller-visible keyboard state left by SeaBIOS POST. */
    i8042_bios_post_init(pc->i8042);

    /* Native-режим: в F000 никто ничего не грузил -> там мусор из PSRAM,
     * который SysInfo/CheckIt показывают как "Copyright notice".
     * Чистим сегмент целиком, дальше кладём нормальный ROM-identity. */
    for (uint32_t a = 0xF0000u; a < 0x100000u; ++a)
        pstore8(a, 0x00);

	uint32_t ext_ram = phys_mem_size <= (1024 << 10) ? 0 : (phys_mem_size - (1024 << 10)) >> 10;
	if (ext_ram > 0xFFFF)
		ext_ram = 0xFFFF;
	cmos_write(pc->cpu, 0x17, (uint8_t)(ext_ram & 0xFF)); // low byte extended memory KB
	cmos_write(pc->cpu, 0x18, (uint8_t)((ext_ram >> 8) & 0xFF)); // high byte
	/* 17h/18h are inside the standard CMOS checksum range 10h..2Dh. */
	cmos_update_checksum(pc->cmos);
// fast IRET cases:
	for (int i = 0; i < 256; ++i) { // initially all them just pointed to IRET
		point2iret(i);
	}
// init BDA
	for (uint32_t a = 0x400; a < 0x500; ++a)
		pstore8(a, 0);
	/* 40:72 is a BIOS compatibility result, not the input used to choose
	 * the POST path. Cold POST leaves it clear; every later soft/warm POST
	 * publishes the standard IBM warm-boot signature. */
	pstore16(0x472, cold_post ? 0x0000 : 0x1234);
// init EDBA
	for (uint32_t a = ebda_phys; a < ebda_phys + 1024; ++a)
        pstore8(a, 0);
    pstore8(ebda_phys + 0x00, 1);                    /* EBDA size, KiB */
// Zero-valued BDA fields are omitted below: the whole BDA was cleared above.
	/* BIOS Data Area, IBM PC/AT compatible minimum. */
	pstore16(0x400, pc->enable_serial ? 0x03F8 : 0x0000); /* COM1 base */
//	pstore16(0x402, 0x0000);                              /* COM2 base */
//	pstore16(0x404, 0x0000);                              /* COM3 base */
//	pstore16(0x406, 0x0000);                              /* COM4 base */
	/* LPT1 = 0x378 (Disney Sound Source / принтер), LPT2 = 0x278 (Covox) */
	uint8_t lpt_count = 0;
	if (pc->dss_enabled)
		pstore16(0x408 + 2 * lpt_count++, 0x0378);
	if (pc->covox_enabled)
		pstore16(0x408 + 2 * lpt_count++, 0x0278);
	for (unsigned i = 0; i < lpt_count; ++i)
		pstore8(0x478 + i, 0x14);   /* printer timeout, 20 тиков */
	pstore16(0x40E, ebda_seg);                            /* EBDA segment */

	uint16_t equipment = (uint16_t)(lpt_count & 3) << 14; /* биты 15-14 LPT */
	equipment |= 0x0001;                                  /* diskette subsystem present */
	if (pc->fpu_enabled)
		equipment |= 0x0002;                          /* math coprocessor installed */
	/* Биты 5-4 (initial video mode) = 00b: дисплейный адаптер с
	   собственным BIOS (EGA/VGA). Значение 10b ("CGA 80x25") включает
	   snow-checking у детект-библиотек эпохи: equipment word читается
	   раньше INT 10h/AH=1Ah, и Norton SI пословно ждёт ретрейс 3DAh
	   на всю отрисовку (кадр на слово). Реальный POST с VGA-картой
	   ставит здесь 00b. */
	if (pc->enable_serial)
		equipment |= 0x0200;                              /* one serial port */
    if (pc->mouse_enabled)
        equipment |= 0x0004;                              /* pointing device */		
	equipment |= (1u << 6);                               /* two diskette drives, encoded count-1 */
	pstore16(0x410, equipment);

	/* SeaBIOS post.c: SET_BDA(mem_size_kb, ebda_seg / (1024/16)) - размер
	   conventional памяти ВЫВОДИТСЯ из адреса EBDA, а не берётся как 640.
	   При ebda_seg = 0x9FC0 это 639 КБ: иначе INT 12h отдаёт DOS тот КБ,
	   в котором лежит EBDA. */
	uint16_t ebda_kb = (uint16_t)(ebda_phys >> 10);      /* 0x9FC00 -> 639 */
	uint16_t conventional_kb = (uint16_t)(phys_mem_size >> 10);
	if (conventional_kb > ebda_kb)
		conventional_kb = ebda_kb;
	pstore16(0x413, conventional_kb);                    /* INT 12h value */

//	pstore8 (0x417, 0x00);                               /* keyboard flags */
//	pstore8 (0x418, 0x00);
	pstore16(0x41A, 0x001E);                             /* kbd buffer head */
	pstore16(0x41C, 0x001E);                             /* kbd buffer tail */

	/* Diskette BIOS work area.  INT 13h keeps the last FDD status here. */
//	pstore8 (0x43E, 0x00);                               /* diskette recalibration/status */
//	pstore8 (0x43F, 0x00);                               /* diskette motor status */
//	pstore8 (0x440, 0x00);                               /* diskette motor timeout */
//	pstore8 (0x441, 0x00);                               /* last diskette status */
//	for (uint32_t a = 0x442; a <= 0x448; ++a)
//		pstore8(a, 0x00);                                  /* FDC result/status bytes */
//	pstore8 (0x48B, 0x00);                               /* diskette media state 0/1 */
//	pstore8 (0x490, 0x00);                               /* diskette drive 0 media type */
//	pstore8 (0x491, 0x00);                               /* diskette drive 1 media type */
//	pstore8 (0x494, 0x00);                               /* diskette current track drive 0 */
//	pstore8 (0x495, 0x00);                               /* diskette current track drive 1 */

	pstore8 (0x449, 0x03);                               /* current video mode */
	pstore16(0x44A, 80);                                 /* columns */
	pstore16(0x44C, 0x1000);                             /* video page size (4096, IBM AT std for mode 3) */
//	pstore16(0x44E, 0x0000);                             /* active page offset */
//	for (uint32_t a = 0x450; a < 0x460; a += 2)
//		pstore16(a, 0x0000);                              /* cursor positions */
	pstore16(0x460, 0x0607);                             /* cursor shape */
//	pstore8 (0x462, 0x00);                               /* active page */
	pstore16(0x463, 0x03D4);                             /* color CRTC base */
	/* 40:65 - это video_msr (тень порта 3x8h), его проставит set-mode.
	   Значение video_ctl 0x60 пишется ниже по своему адресу 40:87. */
	pstore8 (0x465, 0x29);                               /* video_msr: mode 3 */
//	pstore8 (0x466, 0x00);                               /* CGA palette */

//	pstore8 (0x46B, 0x00);                               /* ctrl-break flag */
	/* D5. SeaBIOS clock_setup() засевает timer_counter текущим временем
	   RTC; без этого INT 1Ah/AH=00h до первого тика отдаёт 00:00, и DOS
	   стартует с полуночи. Множитель привязан к TICKS_PER_DAY (0x1800B0),
	   которым INT 08h делает rollover, поэтому результат гарантированно
	   меньше суток. От ticks_from_ms() SeaBIOS отличается не более чем на
	   4 тика (~0.2 c) из-за двойного округления вверх у эталона. */
	{
		uint8_t bh = cmos_read(pc->cpu, 0x04);
		uint8_t bm = cmos_read(pc->cpu, 0x02);
		uint8_t bs = cmos_read(pc->cpu, 0x00);
		uint32_t hh = ((bh >> 4) & 0x0F) * 10u + (bh & 0x0F);
		uint32_t mm = ((bm >> 4) & 0x0F) * 10u + (bm & 0x0F);
		uint32_t ss = ((bs >> 4) & 0x0F) * 10u + (bs & 0x0F);
		uint32_t secs = (hh * 60u + mm) * 60u + ss;
		if (secs >= 86400u) secs = 0;
		pstore32(0x46C, (uint32_t)(((uint64_t)secs * 0x1800B0u) / 86400u));
	}
	pstore8 (0x470, 0x00);                               /* midnight flag */
//	pstore8 (0x471, 0x00);                               /* break flag */
//	pstore16(0x472, 0x0000);                             /* reset flag */
//	pstore8 (0x474, 0x00);                               /* last HDD status */
	pstore8 (0x475, bios_hdd_count());                         /* fixed disk count */
	/* SeaBIOS block.c: drive_control_byte = 0xC0 | ((heads > 8) << 3). */
	{
		uint8_t ctrl = 0xC0;
        bios_hdd_info_t first_hdd;
        if (bios_hdd_get_info(0, &first_hdd) && first_hdd.heads > 8)
            ctrl |= 0x08;
		pstore8 (0x476, ctrl);                           /* HDD control byte */
	}
//	pstore8 (0x477, 0x00);                               /* HDD I/O port offset */
//	pstore8 (0x478, 0x00);                               /* LPT timeouts */
//	pstore8 (0x479, 0x00);
//	pstore8 (0x47A, 0x00);
	pstore8 (0x47C, 0x01);                               /* COM1 timeout */
//	pstore8 (0x47D, 0x00);
//	pstore8 (0x47E, 0x00);
//	pstore8 (0x47F, 0x00);
	pstore16(0x480, 0x001E);                             /* keyboard buffer start */
	pstore16(0x482, 0x003E);                             /* keyboard buffer end */
	pstore8 (0x484, 24);                                 /* rows minus one */
	pstore16(0x485, 16);                                 /* char height */
#ifdef MCGA
	pstore8 (0x487, 0x00);                               /* video_ctl: 64K */
#elif defined(EGA128) || defined(VGA128)
	pstore8 (0x487, 0x20);                               /* video_ctl: 128K */
#else
	pstore8 (0x487, 0x60);                               /* video_ctl: 256K */
#endif
	pstore8 (0x488, 0xF9);                               /* video_switches */
	pstore8 (0x489, 0x51);                               /* modeset_ctl (SeaBIOS vgainit.c:144) */
	/* 40:8E = disk_interrupt_flag, 40:8F = floppy_harddisk_info.
	   0x77 (два дисковода) относится к 40:8F - SeaBIOS block.c:297. */
	pstore8 (0x48E, 0x00);                               /* disk_interrupt_flag */
	pstore8 (0x48F, 0x77);                               /* floppy_harddisk_info */
	pstore8(0x496, 0x10);   /* KF1: bit4 = enhanced (101/102-key) keyboard */
	pstore8(0x497, 0x00);   /* LED flags */

// init PIC (i8259) — IBM PC/AT sequence
	// Master PIC: base vector 0x08 (IRQ0→INT 08h … IRQ7→INT 0Fh)
	i8259_ioport_write(pc->pic, 0x20, 0x11); // ICW1: edge, cascade, ICW4 needed
	i8259_ioport_write(pc->pic, 0x21, 0x08); // ICW2: base vector 0x08
	i8259_ioport_write(pc->pic, 0x21, 0x04); // ICW3: slave on IRQ2
	i8259_ioport_write(pc->pic, 0x21, 0x01); // ICW4: 8086 mode
	i8259_ioport_write(pc->pic, 0x21, 0x00); // OCW1: unmask all IRQs
	// Slave PIC: base vector 0x70 (IRQ8→INT 70h … IRQ15→INT 77h)
	i8259_ioport_write(pc->pic, 0xA0, 0x11); // ICW1
	i8259_ioport_write(pc->pic, 0xA1, 0x70); // ICW2: base vector 0x70
	i8259_ioport_write(pc->pic, 0xA1, 0x02); // ICW3: slave id 2
	i8259_ioport_write(pc->pic, 0xA1, 0x01); // ICW4: 8086 mode
	i8259_ioport_write(pc->pic, 0xA1, 0x00); // OCW1: unmask all IRQs

// init PIT (i8254) — channel 0: mode 3, 18.2 Hz
	// OUT 43h, 36h: channel 0, LSB/MSB, mode 3 (square wave), binary
	// OUT 40h, 00h: LSB=0
	// OUT 40h, 00h: MSB=0  → count = 0x10000 = 65536 → 1193182/65536 ≈ 18.2 Hz
	i8254_ioport_write(pc->pit, 0x43, 0x36);
	i8254_ioport_write(pc->pit, 0x40, 0x00);
	i8254_ioport_write(pc->pit, 0x40, 0x00);

// init IVT: fake processing markers: 0xFFExx
    for (uint16_t ipa = 0; ipa <= 0xFF; ++ipa) {
		pstore16(ipa*4, ipa);
		pstore16(ipa*4 + 2, 0xFFE0);
	}
 // reusable IRET
    pstore8(0xFFF06, 0xCF);
// IRQ1 INT 15h/4Fh keyboard intercept stub: 0xFFF70..0xFFF7A
//   0xFFF70 = scratch byte (scan code, written by bios_09h phase1)
//   0xFFF71 = stub entry point
    pstore8(0xFFF70, 0x00); // scratch: scan code placeholder
    pstore8(0xFFF71, 0xF9); // STC  — CF=1: do not intercept by default
    pstore8(0xFFF72, 0xCD); // INT 15h
    pstore8(0xFFF73, 0x15);
    pstore8(0xFFF74, 0xCD); // INT FFh — callback
    pstore8(0xFFF75, 0xFF);
    pstore8(0xFFF76, 0xCF); // IRET — fallback if phase2 returns false
    pstore8(0xFFF77, 0x90); // NOP
// NLS support
	static const uint8_t x86_charmap_stub[] = {
		0x3C, 0x61,       /* cmp al,'a' */
		0x72, 0x06,       /* jb done */
		0x3C, 0x7A,       /* cmp al,'z' */
		0x77, 0x02,       /* ja done */
		0x2C, 0x20,       /* sub al,20h */
		0xCB              /* retf */
	};	
	for(int i = 0; i < sizeof(x86_charmap_stub); ++i) // 11 bytes [FFF78..FFF82]
		pstore8(0xFFF78 + i, x86_charmap_stub[i]);
// INT 15h support:
    const uint32_t table = 0xFFF10;
    pstore16(table + 0x00, 0x0008); /* number of bytes following */
    pstore8 (table + 0x02, 0xFC);   /* model: IBM PC AT */
    pstore8 (table + 0x03, 0x01);   /* submodel */
    pstore8 (table + 0x04, 0x01);   /* BIOS revision */
	/*
	bit 7 = DMA channel 3 used by fixed disk BIOS
	bit 6 = второй 8259 PIC установлен
	bit 5 = RTC установлен
	bit 4 = INT 15h/AH=4Fh вызывается из INT 09h
	bit 3 = INT 15h/AH=41h wait for external event поддержан
	bit 2 = EBDA выделена
	bit 1 = Micro Channel bus вместо ISA
	bit 0 = dual bus: Micro Channel + ISA
	*/
    pstore8 (table + 0x05, 0b01111100);   /* feature byte 1: slave PIC + RTC + INT15/4Fh + INT15/41h + EDBA */
	/*
7      32-bit DMA supported
6      INT 16/AH=09h (keyboard functionality) supported (see #00585)
5      INT 15/AH=C6h (get POS data) supported
4      INT 15/AH=C7h (return memory map info) supported
3      INT 15/AH=C8h (en/disable CPU functions) supported
2      non-8042 keyboard controller
1      data streaming supported
0      reserved
	*/
    pstore8 (table + 0x06, 0b11000000);   /* feature byte 2 */
	/*
7      not used
6-5    reserved
4      POST supports ROM-to-RAM enable/disable
3      SCSI subsystem supported on system board
2      information panel installed
1      IML (Initial Machine Load) system (BIOS loaded from disk)
0      SCSI supported in IML
	*/
    pstore8 (table + 0x07, 0x00);   /* feature byte 3 */
	/*
7      IBM "private" (set on N51SX, CL57SX)
6      system has EEPROM
5-3    ABIOS presence.
001 not supported.
010 supported in ROM.
011 supported in RAM (must be loaded)
2      "private"
1      system supports memory split at/above 16M
0      POSTEXT directly supported by POST
	*/
    pstore8 (table + 0x08, 0x00);   /* feature byte 4 */
	/*
7-5    IBM "private"
4-2    reserved
1      system has enhanced mouse mode
0      flash EPROM
	*/
    pstore8 (table + 0x09, 0x00);   /* feature byte 5 */
// INT 10h support:
	bios_10h_install_rom_fonts(pc->cpu);
// INT 1Eh Diskette Parameter Table: F000:EFC7
	install_floppy_dpt();
// INT 41h/46h support: 0xFFF30-0xFFF4F
    install_hdd_dpt(pc, 0, 0xFFF30);  // INT 41h → первый HDD
    install_hdd_dpt(pc, 1, 0xFFF40);  // INT 46h → второй HDD
// 0xFFF50-0xFFF6F
	install_dpte(0, DPTE_ADDR_0);
	install_dpte(1, DPTE_ADDR_1);

// like VGA BIOS banner:
	vga_bios_baner(pc->cpu);

    {
        size_t psram_size = psram_detected_size();

        /* Hardware table is shown on every POST, cold or warm. */
        bios_post_components(pc, psram_size);

        /* Memory test and its result tone are cold-POST features only. */
        if (cold_post) {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
            bool memory_ok = ega128_paging_active()
                       ? bios_post_paged_memory_test(pc)
                       : bios_post_psram_test(pc, psram_size);
#else
            bool memory_ok = bios_post_psram_test(pc, psram_size);
#endif
            bios_post_beep_pending = memory_ok ? 1 : -1;
        } else {
            bios_post_beep_pending = 0;
        }
    }
// STEP/BREAKPOINT/TRACE etc (no DOS/BIOS support)
	point2iret(0x01);
	point2iret(0x03);
	point2iret(0x04);
//  do not trap custom timer (to be overriden by DOS)
	point2iret(0x1C);
	point2iret(0x77);
// DOS (set it later, if required):
	point2iret(0x20);
	point2iret(0x21);
	point2iret(0x28); // Idle
	point2iret(0x29);
	point2iret(0x2f);
// MS MOUSE: INT 33h + IRQ12 (INT 74h).
// Драйвер в bios/bios_33h.c, данные берёт из 8042 по IRQ12
	/*
     * Keep INT 33h pointing at a valid entry point even when mouse
	 * support is disabled.  A null IVT entry is not a callable
	 * "driver absent" stub: software which probes the vector and then
	 * calls it starts executing at 0000:0000.
	 *
	 * bios_33h() reports AX=0000h for reset/status while disabled.
	 */
	pstore16(0x33*4, 0x0033); pstore16(0x33*4 + 2, 0xFFE0);
	if (pc->mouse_enabled) {
		pstore16(0x74*4, 0x0074); pstore16(0x74*4 + 2, 0xFFE0);
	} else {
		point2iret(0x74);
	}
// IRQ14 - HARD DISK CONTROLLER OPERATION COMPLETE (AT and later)
	point2iret(0x76);

/* --- ROM identity ------------------------------------------------- */
    static const char rom_copyright[] = "Copyright (C) 2026 Murmulator Group";
    static const char rom_biosname[]  = "RP2350 ARM Cortex-M33 BIOS";

    /* Note: the identity strings are intentionally NOT placed at F000:0000
       anymore. F0000-F8FFF is handed to the guest as UMB (see umb_native[] /
       umb_select_map), so anything here would be overwritten. The copies at
       F000:C600 and F000:E000 below (outside the UMB window) keep SysInfo /
       CheckIt happy. */
    rom_puts(0xFC600, rom_biosname);                 /* F000:C600 (свободно)*/
    rom_puts(0xFC600 + 32, rom_copyright);
    rom_puts(0xFE000, rom_copyright);                /* F000:E000 — классика*/
    rom_puts(0xFE000 + 40, rom_biosname);
    rom_puts(0xFE000 + 72, "Murmulator Group MURM-386");

    rom_bios_date(0xFFFF5);                          /* F000:FFF5 "MM/DD/YY" */
    pstore8(0xFFFFD, 0x00);                          /* checksum-заглушка    */
    pstore8(0xFFFFE, 0xFC);                          /* model byte: IBM AT   */
    pstore8(0xFFFFF, 0x01);                          /* submodel/revision    */

/* --- Канонические IBM PC/AT точки входа BIOS в F000 ---------------------
 * Вектора, указывающие прямо в трап-страницу FFE0:NN, ломают программы,
 * которые трассируют прерывания через TF/INT 1 (Norton Utilities и т.п.):
 * трассировщик пошагово идёт по обработчику, пока CS:IP не попадёт в
 * "настоящий" ROM BIOS (обычно сравнивают с F000:xxxx или каноническими
 * адресами вроде INT 13h = F000:E3FE). Попадание на FFE0-страницу
 * исполняет ВЕСЬ обработчик нативно за один "шаг" - условие завершения
 * трассировки не наступает никогда, и утилита зависает в бесконечном
 * INT 1. Даём каждому классическому вектору настоящую 5-байтовую точку
 * входа JMP FAR FFE0:NN по каноническому смещению IBM AT: первый же шаг
 * трассировки видит CS=F000 и завершается, а исполнение всё равно
 * попадает в нативный диспетчер.
 *
 * INT 19h (E05Bh) намеренно пропущен: смещение занято ROM-identity
 * строками выше. INT 1Ch остаётся FFF0:0006 (каноничный F000:FF53 занят
 * DPTE-таблицами по FFF50-FFF6F).
 *
 * ВАЖНО - тень трап-страницы: диспетчер ядер перехватывает исполнение по
 * условию (phys >> 8) == 0xFFE, то есть ЛЮБОЙ линейный адрес
 * 0xFFE00-0xFFEFF. Сегмент F000 со смещениями FE00h-FEFFh отображается
 * ровно в это окно, поэтому канонические IBM-адреса INT 08h (FEA5h) и
 * INT 1Ah (FE6Eh) использовать НЕЛЬЗЯ: первый же тик таймера уходил в
 * handlers[0xA5] = no_handler. Эти два стаба смещены на страницу ниже
 * (FDA5h/FD6Eh) - для трассировщиков значим сегмент F000, а не точное
 * смещение. Никакие другие смещения таблицы в окно FExx не попадают. */
    {
        static const struct { uint8_t intno; uint16_t off; } rom_entry[] = {
            { 0x05, 0xFF54 },   /* print screen                    */
            { 0x08, 0xFDA5 },   /* IRQ0 timer (канонич. FEA5h - в тени) */
            { 0x09, 0xE987 },   /* IRQ1 keyboard                   */
            { 0x10, 0xF065 },   /* video                           */
            { 0x11, 0xF84D },   /* equipment list                  */
            { 0x12, 0xF841 },   /* memory size                     */
            { 0x13, 0xE3FE },   /* disk                            */
            { 0x14, 0xE739 },   /* serial                          */
            { 0x15, 0xF859 },   /* system services                 */
            { 0x16, 0xE82E },   /* keyboard services               */
            { 0x17, 0xEFD2 },   /* printer (сразу за DPT @ EFC7)   */
            { 0x1A, 0xFD6E },   /* time of day (канонич. FE6Eh - в тени) */
        };
        for (unsigned i = 0; i < sizeof(rom_entry)/sizeof(rom_entry[0]); ++i) {
            uint32_t phys = 0xF0000u + rom_entry[i].off;
            pstore8 (phys,     0xEA);              /* JMP FAR FFE0:00NN */
            pstore16(phys + 1, rom_entry[i].intno);
            pstore16(phys + 3, 0xFFE0);
            pstore16(rom_entry[i].intno * 4,     rom_entry[i].off);
            pstore16(rom_entry[i].intno * 4 + 2, 0xF000);
        }
    }
/* Дефолтный обработчик INT 24h ядра DOS: kernel.asm _int24_handler =
 * "mov al,FAIL; iret" (FAIL=03h). Байты живут в fake-ROM, вектор ставит
 * init_vectors() ядра (fdos/kernel.c) - как в оригинале, где байты лежат
 * в образе ядра. FFF44-FFF46 свободны (FFF50+ занят DPTE). */
    pstore8(0xFFF44, 0xB0);   /* MOV AL, imm8 */
    pstore8(0xFFF45, 0x03);   /* FAIL         */
    pstore8(0xFFF46, 0xCF);   /* IRET         */

   /* INT 33h: сброс состояния + инициализация 8042/мыши через порты.
    * Делать ПОСЛЕ инициализации PIC (выше в bios_post), иначе IRQ12 замаскирован. */
    bios_33h_install(pc->cpu, pc->mouse_enabled);

/* 1 = печатать в POST дамп ключевых полей (проверка, что патч реально в прошивке) */
#if NATIVE_POST_SELFTEST
    /* Печатается сразу после VGA-баннера. */
    {
        char cp[17];
        for (int i = 0; i < 16; ++i) {
            uint8_t c = pload8(0xFE000 + i);
            cp[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        cp[16] = 0;
        bios_printf(pc->cpu,
            "\n\nPOST: F000:E000=[%s] 40:96=%02X 40:08=%04X 40:0A=%04X EQ=%04X\n",
            cp, pload8(0x496), pload16(0x408), pload16(0x40A), pload16(0x410));
        bios_printf(pc->cpu,
            "POST: CMOS 10=%02X 12=%02X 14=%02X 2E=%02X 2F=%02X  HD=%u\n",
            cmos_read(pc->cpu, 0x10), cmos_read(pc->cpu, 0x12),
            cmos_read(pc->cpu, 0x14), cmos_read(pc->cpu, 0x2E),
            cmos_read(pc->cpu, 0x2F), (unsigned)pload8(0x475));
    }
#endif

//	bios_19h(pc->cpu);
    pstore8(0xFFFF0, 0xCD); // INT 19h - bootstrap
    pstore8(0xFFFF1, 0x19);
}

void load_bios_and_reset(PC *pc)
{
	int bios_size = 0;
	int vga_loaded = 0;
	if (pc->bios && pc->bios[0]) {
			bios_size = load_rom(PC_RAM, pc->bios, 0x100000, 1);

		// Only load VGA BIOS if main BIOS doesn't overlap with 0xC0000
		// 256KB BIOS starts at 0xC0000, so VGA BIOS would overwrite it
		int bios_start = 0x100000 - bios_size;
		if (pc->vga_bios && pc->vga_bios[0] && bios_start >= 0xC8000) {
			load_rom(PC_RAM, pc->vga_bios, 0xc0000, 0);
			vga_loaded = 1;
		} else if (pc->vga_bios && pc->vga_bios[0]) {
			printf("Skipping VGA BIOS - main BIOS overlaps at 0x%x\n", bios_start);
		}
		umb_select_map(0, (uint32_t)bios_start, vga_loaded);
	} else {
		umb_select_map(1, 0x100000u, 0);
		bios_post(pc);
	}
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
	/* Fake/native BIOS (F9000-FFFFF) and external ROM images share the
	 * pageable physical backing with UMB RAM.  There is deliberately no
	 * ROM overlay over F0000-F8FFF: that range is native-BIOS UMB. */
	extern bool ega128_paging_flush(void);
	if (ega128_paging_active() && !ega128_paging_flush())
		printf("ERROR: guest-RAM paging flush failed before reset\n");
#endif
	sn76489_reset();
	cpu_reset(pc->cpu);
}

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value)
{
	PCConfig *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	// Accept [pc], [386] and the build's own [SD_DATA_DIR] section.
	// [pc] and [386] are kept for backward compatibility with existing
	// config.ini files; SD_DATA_DIR ("286" or "386") lets a config match
	// the build without editing.
	if (SEC("pc") || SEC("386") || SEC(SD_DATA_DIR)) {
		if (NAME("bios")) {
			if (value[0] == '\0' || strcasecmp(value, "native") == 0)
				conf->bios = NULL;
			else
				conf->bios = strdup(value);
		} else if (NAME("vga_bios")) {
			conf->vga_bios = strdup(value);
		} else if (NAME("mem_size") || NAME("mem")) {
			/* Obsolete: physical PSRAM size is detected at boot. */
		} else if (NAME("cpu")) {
			conf->cpu_gen = atoi(value);
		} else if (NAME("raw_sd_hdd")) {
            conf->raw_sd_hdd = atoi(value) != 0;
		} else if (NAME("hda")) {
			conf->ata[0] = strdup(value);
			conf->iscd[0] = 0;
		} else if (NAME("hdb")) {
			conf->ata[1] = strdup(value);
			conf->iscd[1] = 0;
		} else if (NAME("hdc")) {
			conf->ata[2] = strdup(value);
			conf->iscd[2] = 0;
		} else if (NAME("hdd")) {
			conf->ata[3] = strdup(value);
			conf->iscd[3] = 0;
		} else if (NAME("cda")) {
			conf->ata[0] = strdup(value);
			conf->iscd[0] = 1;
		} else if (NAME("cdb")) {
			conf->ata[1] = strdup(value);
			conf->iscd[1] = 1;
		} else if (NAME("cdc")) {
			conf->ata[2] = strdup(value);
			conf->iscd[2] = 1;
		} else if (NAME("cdd")) {
			conf->ata[3] = strdup(value);
			conf->iscd[3] = 1;
		} else if (NAME("fda")) {
			conf->fdd[0] = strdup(value);
		} else if (NAME("fdb")) {
			conf->fdd[1] = strdup(value);
		} else if (NAME("redirector")) {
			conf->redirector = atoi(value);
		} else if (NAME("enable_serial")) {
			conf->enable_serial = atoi(value);
		} else if (NAME("vga_force_8dm")) {
			conf->vga_force_8dm = atoi(value);
		}
	} else if (SEC("display")) {
		if (NAME("width")) {
			conf->width = atoi(value);
		} else if (NAME("height")) {
			conf->height = atoi(value);
		}
	} else if (SEC("cpu")) {
		if (NAME("gen")) {
			conf->cpu_gen = atoi(value);
		} else if (NAME("fpu")) {
			conf->fpu = atoi(value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}
