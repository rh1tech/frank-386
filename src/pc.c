#include "pc.h"
#include "mem.h"
#include "ide.h"
#include "dss.h"
#include "misc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <hardware/watchdog.h>

#include "mpu401.c.inl"
void netredirect_init(CPU *cpu, int enable);

unsigned long phys_mem_size = 8l << 20;
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
        uint8_t buf[512];
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
        uint8_t buf[512];
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
		return pc->port92;
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
	case 0x201:
		/* Gameport / Joystick - return "no joystick" state
		 * Bits 7-4: buttons (1 = not pressed)
		 * Bits 3-0: axes timeout (0 = timed out, no joystick)
		 * Return 0xF0 to indicate axes have timed out (no joystick present) */
		return 0xf0;
	case 0x27A: // Covox Speech Thing
		return 0;
	// MPU-401
	case 0x330:
	case 0x331:
		if (pc->mpu401_enabled)
	        return mpu401_read(addr);
		return 0xFF;
	// Disney Sound Source
	case 0x378:
	case 0x379:
		if (pc->dss_enabled)
			return dss_in(addr);
		return 0xFF;
	/* LPT data ports (Covox): write-only DAC, reads return 0xFF */
	case 0x278:
		return 0xff;
	/* LPT status ports: bit7=nBusy(1=ready), bits6..3=1 (idle/ready) */
	case 0x279:
		return 0xf8;
	/* LPT control ports */
	case 0x37a:
		return 0x04;
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

inline static void out_ems(const uint16_t port, const uint8_t data) {
    ems_pages[port & 3] = data;
}
#endif

static void pc_io_write(void *o, int addr, u8 val)
{
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
		pc->port92 = val;
		cpu_set_a20(pc->cpu, (val >> 1) & 1);
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
		if (pc->covox_enabled)
			pc->covox_sample = val;
		return;
	// MPU-401
	case 0x330:
	case 0x331:
		if (pc->mpu401_enabled)
			mpu401_write(addr, val);
		return;
	// Disney Sound Source
    case 0x378:
    case 0x37A:
		if (pc->dss_enabled)
			dss_out(addr, val);
		return;
	/* LPT status/control ports are read-only - writes ignored */
	case 0x379: case 0x279:
	case 0x27a:
		return;
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

void __not_in_flash_func(pc_step)(PC *pc)
{
	/* reset_request is handled in main.c via load_bios_and_reset() */
	int refresh = vga_step(pc->vga);
	i8254_update_irq(pc->pit);
	cmos_update_irq(pc->cmos);
	if (pc->enable_serial)
		u8250_update(pc->serial);
	kbd_step(pc->i8042);
	i8257_dma_run(pc->isa_dma);
	i8257_dma_run(pc->isa_hdma);
	if (pc->fdc) fdc_tick(pc->fdc);
	if (pc->poll) pc->poll(pc->redraw_data);
	if (refresh && pc->redraw) {
		vga_refresh(pc->vga, pc->redraw, pc->redraw_data,
			    pc->full_update != 0);
		if (pc->full_update == 2)
			pc->full_update = 0;
	}
	if (pc->adlib_enabled) {
		for (int i = 0; i < 409; ++i) {
			cpu_step(pc->cpu, 10);
			adlib_core0(pc->adlib);
		}
	} else {
		cpu_step(pc->cpu, 4096);
	}

#if 0
	/* Dump profile every ~10M instructions */
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
    return iomem_write_string_ptr(iomem, addr, PC_RAM + buf, len);
}

static void pc_reset_request(void *p)
{
	PC *pc = p;
	pc->reset_request = 1;
}

extern uint8_t gfx_buffer[256ul << 10];

static CMOS *_pc_cmos_for_floppy = NULL;
static void cmos_floppy_update(uint8_t ta, uint8_t tb) {
    cmos_set_floppy_types(_pc_cmos_for_floppy, ta, tb);
    cmos_set(_pc_cmos_for_floppy, 0x14, 0x41); // 0x14 <= 0x41 (2 fdds)
    cmos_update_checksum(_pc_cmos_for_floppy);
}

static PC *_pc_for_fdc = NULL;
static void fdc_mediachange_notify(int drive) {
    if (_pc_for_fdc && _pc_for_fdc->fdc)
        fdc_media_changed(_pc_for_fdc->fdc, drive);
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
	if (conf->fpu)
		enable_fpu(pc->cpu);
	pc->bios = conf->bios;
	pc->vga_bios = conf->vga_bios;
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

	/* CD-ROM E: always present on ide2/drive0 (secondary master).
	 * Only attach if cdc= didn't already claim that slot (ata[2]). */
	if (!ide_has_drive(pc->ide2, 0))
		ide_attach_cd(pc->ide2, 0);



	ide_fill_cmos(pc->ide, pc->cmos, cmos_set);

	/* we have emulation for 2 FDDs (CMOS 0x10):
		биты 7-4 = тип A:
		биты 3-0 = тип B:
		значение 4 = 1.44MB 3.5"
	*/
//	cmos_set(pc->cmos, 0x10, 0x44); // A: = 1.44MB, B: = 1.44MB
//	cmos_set(pc->cmos, 0x14, 0x41); // бит 0 = флоппи есть, биты 7-6 = 01 = два дисковода
	/* Checksum ПОСЛЕ всех записей в диапазон 0x10-0x2D */
//	cmos_update_checksum(pc->cmos);

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

	/* gfx_buffer is always 256 KB — always use exactly that, ignoring
	 * whatever vga_mem the config says.  This ensures Wolf3D's three video
	 * pages (dword offsets 0 / 16640 / 33280, up to byte 133120) are never
	 * dropped.  Old SD-card configs with vga_mem=128K would otherwise leave
	 * the third page zeroed (black) due to the size check in vga_mem_write. */
	pc->vga_mem_size = 256u << 10;   /* fixed: gfx_buffer is always 256 KB */
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

	pc->port92 = 0x2;
	pc->shutdown_state = 0;
	pc->reset_request = 0;
	return pc;
}

#ifndef I386_MODE
#include "286/bios.h"
#include "286/fdos.h"
// IRET is saved on 0xFFF06
static void point2iret(u32 intno) {
	pstore16(intno*4, 0x0006);
	pstore16(intno*4 + 2, 0xFFF0);
}

void bios_post(PC *pc) {
// POST
	// TODO:
	uint32_t ext_ram = phys_mem_size <= (1024 << 10) ? 0 : (phys_mem_size - (1024 << 10)) >> 10;
///	cmos_write(0x17, (uint8_t)(ext_ram & 0xFF)); // low byte extended memory KB
///	cmos_write(0x18, (uint8_t)((ext_ram >> 8) & 0xFF)); // high byte
// init BDA
	for (uint32_t a = 0x400; a < 0x500; ++a)
		pstore8(a, 0);

	/* BIOS Data Area, IBM PC/AT compatible minimum. */
	pstore16(0x400, pc->enable_serial ? 0x03F8 : 0x0000); /* COM1 base */
	pstore16(0x402, 0x0000);                              /* COM2 base */
	pstore16(0x404, 0x0000);                              /* COM3 base */
	pstore16(0x406, 0x0000);                              /* COM4 base */
	pstore16(0x408, 0x0000);                              /* LPT1 base */
	pstore16(0x40A, 0x0000);                              /* LPT2 base */
	pstore16(0x40C, 0x0000);                              /* LPT3 base */
	pstore16(0x40E, 0x0000);                              /* EBDA segment: none */

	uint16_t equipment = 0x0000;
	equipment |= 0x0001;                                  /* diskette subsystem present */
	equipment |= 0x0020;                                  /* initial video: 80x25 color */
	if (pc->enable_serial)
		equipment |= 0x0200;                              /* one serial port */
	equipment |= (1u << 6);                               /* two diskette drives, encoded count-1 */
	pstore16(0x410, equipment);

	uint16_t conventional_kb = phys_mem_size > 640u * 1024u
		? 640u
		: (uint16_t)(phys_mem_size >> 10);
	pstore16(0x413, conventional_kb);                    /* INT 12h value */

	pstore8 (0x417, 0x00);                               /* keyboard flags */
	pstore8 (0x418, 0x00);
	pstore16(0x41A, 0x001E);                             /* kbd buffer head */
	pstore16(0x41C, 0x001E);                             /* kbd buffer tail */

	/* Diskette BIOS work area.  INT 13h keeps the last FDD status here. */
	pstore8 (0x43E, 0x00);                               /* diskette recalibration/status */
	pstore8 (0x43F, 0x00);                               /* diskette motor status */
	pstore8 (0x440, 0x00);                               /* diskette motor timeout */
	pstore8 (0x441, 0x00);                               /* last diskette status */
	for (uint32_t a = 0x442; a <= 0x448; ++a)
		pstore8(a, 0x00);                                  /* FDC result/status bytes */

	pstore8 (0x449, 0x03);                               /* current video mode */
	pstore16(0x44A, 80);                                 /* columns */
	pstore16(0x44C, 0x1000);                             /* video page size (4096, IBM AT std for mode 3) */
	pstore16(0x44E, 0x0000);                             /* active page offset */
	for (uint32_t a = 0x450; a < 0x460; a += 2)
		pstore16(a, 0x0000);                              /* cursor positions */
	pstore16(0x460, 0x0607);                             /* cursor shape */
	pstore8 (0x462, 0x00);                               /* active page */
	pstore16(0x463, 0x03D4);                             /* color CRTC base */
	pstore8 (0x465, 0x60);                               /* video_ctl: cursor emulation on, no blink */
	pstore8 (0x466, 0x00);                               /* CGA palette */

	pstore8 (0x46B, 0x00);                               /* ctrl-break flag */
	pstore32(0x46C, 0x00000000);                         /* timer ticks */
	pstore8 (0x470, 0x00);                               /* midnight flag */
	pstore8 (0x471, 0x00);                               /* break flag */
	pstore16(0x472, 0x0000);                             /* reset flag */
	pstore8 (0x474, 0x00);                               /* last HDD status */
	pstore8 (0x475, hdcount > 0 ? hdcount : 0);           /* fixed disk count */
	pstore8 (0x476, 0xC0);                               /* HDD control byte */
	pstore8 (0x477, 0x00);                               /* HDD I/O port offset */
	pstore8 (0x478, 0x00);                               /* LPT timeouts */
	pstore8 (0x479, 0x00);
	pstore8 (0x47A, 0x00);
	pstore8 (0x47C, 0x01);                               /* COM1 timeout */
	pstore8 (0x47D, 0x00);
	pstore8 (0x47E, 0x00);
	pstore8 (0x47F, 0x00);
	pstore16(0x480, 0x001E);                             /* keyboard buffer start */
	pstore16(0x482, 0x003E);                             /* keyboard buffer end */
	pstore8 (0x484, 24);                                 /* rows minus one */
	pstore16(0x485, 16);                                 /* char height */
	pstore8 (0x487, 0x00);                               /* video control */
	pstore8 (0x488, 0xF9);                               /* switches */
	pstore8 (0x489, 0x00);                               /* VGA flags */
	pstore8 (0x48E, 0x77);                               /* fdd */
	pstore8 (0x496, 0x10);                               // enhanced keyboard present

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
// IRQ0: Timer specific W/A
    pstore8(0xFFF00, 0xCD); // INT 1Ch
    pstore8(0xFFF01, 0x1C);
    pstore8(0xFFF02, 0xB0); // MOV AL, 20h
    pstore8(0xFFF03, 0x20);
    pstore8(0xFFF04, 0xE6); // OUT 20h, AL  <- EOI
    pstore8(0xFFF05, 0x20);
    pstore8(0xFFF06, 0xCF); // IRET 0xFFF0:0006 - reusable IRET
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
/*
	point2iret(0x00); // CPU-generated - DIVIZION BY ZERO
	point2iret(0x01); // CPU-generated - SINGLE STEP
	point2iret(0x02); // NMI
	point2iret(0x03); // Breakpoin
	point2iret(0x04); // INTO overflow
	point2iret(0x05); // CPU-generated - BOUND EXCEPTION / PRINT SCREEN (TODO: more strickt impl.)
	point2iret(0x06); // Invalid opcode
	point2iret(0x07); // Coprocessor not available
	point2iret(0x0A); // IRQ2
	point2iret(0x0B); // IRQ3
	point2iret(0x0C); // IRQ4
	point2iret(0x0D); // IRQ5
	point2iret(0x0E); // IRQ6
	point2iret(0x0F); // IRQ7 cascade no EOI, if reimpl.
	point2iret(0x1C); // INT 1Ch: user timer tick hook — no-op until replaced by a TSR 
	point2iret(0x21); // No DOS functions support on BIOS level
	point2iret(0x29); // No DOS functions support on BIOS level
	point2iret(0x2A); // No NETWORK functions support on BIOS level
	point2iret(0x2F); // No DOS functions support on BIOS level
	point2iret(0x70); // IRQ8 ? RTC
	point2iret(0x71); // IRQ9
	point2iret(0x72); // IRQ10
	point2iret(0x73); // IRQ11
	point2iret(0x74); // IRQ12
	point2iret(0x75); // IRQ13
	point2iret(0x76); // IRQ14
// TODO: IRQ1 flow already uses it, to be fixed /	point2iret(0x77); // IRQ15
*/
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
    pstore8 (table + 0x05, 0b01111000);   /* feature byte 1: slave PIC + RTC + INT 15h/AH=4Fh + INT 15h/AH=41h  */
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
///    install_hdd_dpt(pc, 0, 0xFFF30);  // INT 41h → первый HDD
///    install_hdd_dpt(pc, 1, 0xFFF40);  // INT 46h → второй HDD
// 0xFFF50-0xFFF6F
///	install_dpte(0, DPTE_ADDR_0);
///	install_dpte(1, DPTE_ADDR_1);

// like VGA BIOS banner:
	vga_bios_baner(pc->cpu);
//  do not trap custom timer (to be overriden by DOS)
	point2iret(0x1C);
	point2iret(0x77);

//	bios_19h(pc->cpu);
    pstore8(0xFFFF0, 0xCD); // INT 19h - bootstrap
    pstore8(0xFFFF1, 0x19);
}
#else
void bios_post(PC *pc) {}
#endif

void load_bios_and_reset(PC *pc)
{
	int bios_size = 0;
	if (pc->bios && pc->bios[0])
		bios_size = load_rom(PC_RAM, pc->bios, 0x100000, 1);

	// Only load VGA BIOS if main BIOS doesn't overlap with 0xC0000
	// 256KB BIOS starts at 0xC0000, so VGA BIOS would overwrite it
	int bios_start = 0x100000 - bios_size;
	if (pc->vga_bios && pc->vga_bios[0] && bios_start >= 0xC8000) {
		load_rom(PC_RAM, pc->vga_bios, 0xc0000, 0);
	} else if (pc->vga_bios && pc->vga_bios[0]) {
		printf("Skipping VGA BIOS - main BIOS overlaps at 0x%x\n", bios_start);
	}
	sn76489_reset();

#ifndef I386_MODE
// fast IRET cases:
	for (int i = 0; i < 256; ++i) { // initially all them just pointed to IRET
		point2iret(i);
	}
#endif

	cpu_reset(pc->cpu);
}

static long parse_mem_size(const char *value)
{
	int len = strlen(value);
	long a = atol(value);
	if (len) {
		switch (value[len - 1]) {
		case 'G': a *= 1024 * 1024 * 1024; break;
		case 'M': a *= 1024 * 1024; break;
		case 'K': a *= 1024; break;
		}
	}
	return a;
}

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value)
{
	PCConfig *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	// Support both [pc] and [386] sections for compatibility
	if (SEC("pc") || SEC("386")) {
		if (NAME("bios")) {
			conf->bios = strdup(value);
		} else if (NAME("vga_bios")) {
			conf->vga_bios = strdup(value);
		} else if (NAME("mem_size") || NAME("mem")) {
			conf->mem_size = parse_mem_size(value);
		} else if (NAME("cpu")) {
			conf->cpu_gen = atoi(value);
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
