#ifndef PC_H
#define PC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "i386.h"
#include "i8259.h"
#include "i8254.h"
#include "disk.h"
#include "vga.h"
#include "i8042.h"
#include "misc.h"
#include "adlib.h"
#include "i8257.h"
#include "sb16.h"
#include "pcspk.h"
#include "pci.h"
#include "ide.h"
#include "ini.h"
#include "sn76489.h"
#include "fdd.h"

/// Platform HAL
uint32_t get_uticks();
void *pcmalloc(long size);
int load_rom(void *phys_mem, const char *file, uword addr, int backward);

/// PC
#ifdef USEKVM
#include "kvm.h"
typedef CPUKVM CPU;
#else
typedef CPUI386 CPU;
#endif

typedef struct {
	CPU *cpu;
	PicState2 *pic;
	PITState *pit;
	U8250 *serial;
	CMOS *cmos;
	VGAState *vga;
	char *phys_mem;
	long phys_mem_size;
	char *vga_mem;
	int vga_mem_size;
	int64_t boot_start_time;

	SimpleFBDrawFunc *redraw;
	void *redraw_data;
	void (*poll)(void *);

	KBDState *i8042;
	PS2KbdState *kbd;
	PS2MouseState *mouse;
	AdlibState *adlib;
	I8257State *isa_dma, *isa_hdma;
	FDCState   *fdc;

	/* Emulink FDD – simple virtual floppy protocol on ports 0xF1F0/0xF1F4 */
	struct {
		uint32_t status;   /* read via 0xF1F0 */
		uint32_t cmd;      /* written via 0xF1F0 */
		uint32_t args[4];
		int      argi;
		int      dataleft;
	} emulink;
	SB16State *sb16;
	PCSpkState *pcspk;

	// Covox Speech Thing - no state object needed, just last sample + enable
	volatile uint8_t covox_sample;      /* last written DAC value */

	// Runtime enable flags for audio devices (checked in mixer_callback)
	int adlib_enabled;
	int sb16_enabled;
	int pcspk_enabled;
	int tandy_enabled;
	int covox_enabled;
	int mpu401_enabled;
	int dss_enabled;
	int mouse_enabled;

	IDEIFState *ide;
	IDEIFState *ide2;
	PCIDevice *pci_ide;

	I440FXState *i440fx;
	PCIBus *pcibus;
	PCIDevice *pci_vga;
	uword pci_vga_ram_addr;

	const char *bios;
	const char *vga_bios;

	u8 port92;
	int shutdown_state;
	int reset_request;
	int paused;  // Emulation paused (e.g., for disk UI)

	const char *linuxstart;
	const char *kernel;
	const char *initrd;
	const char *cmdline;
	int enable_serial;
	int full_update;
} PC;

typedef struct {
	const char *linuxstart;
	const char *kernel;
	const char *initrd;
	const char *cmdline;
	const char *bios;
	const char *vga_bios;
	long mem_size;
	long vga_mem_size;
	const char *disks[4];
	int iscd[4];
	const char *fdd[2];
	int fill_cmos;
	int width;
	int height;
	int cpu_gen;
	int fpu;
	int enable_serial;
	int vga_force_8dm;
} PCConfig;

PC *pc_new(SimpleFBDrawFunc *redraw, void (*poll)(void *), void *redraw_data,
	   u8 *fb, PCConfig *conf);

// XXX: still contains ESP32-specific logic
void pc_vga_step(void *o);
void pc_step(PC *pc);

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value);
void load_bios_and_reset(PC *pc);

int16_t midi_sample(void);

#endif /* PC_H */
