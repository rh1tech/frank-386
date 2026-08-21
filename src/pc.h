#ifndef PC_H
#define PC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
#include "ini.h"
#include "sn76489.h"
#include "fdd.h"

/// Platform HAL
uint32_t get_uticks();
void *pcmalloc(long size);
int load_rom(void *phys_mem, const char *file, uword addr, int backward);

typedef struct IDEIFState IDEIFState;

/* Public PC ABI: field layout is fixed to 4-byte packing on both sides. */
#pragma pack(push, 4)
typedef struct PC {
	CPU *cpu;
	PicState2 *pic;
	PITState *pit;
	U8250 *serial;
	CMOS *cmos;
	VGAState *vga;
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
	int joystick_enabled;   /* analog game port at 0x201 */

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

	int enable_serial;
	int fpu_enabled;       /* conf->fpu, needed by bios_post() */
	int full_update;

	uint8_t lpt_data[2];   /* защёлки data-регистров LPT1(0x378)/LPT2(0x278) */
	uint8_t lpt_ctrl[2];   /* защёлки control-регистров 0x37A/0x27A  	*/
} PC;

/* Select the first native BIOS POST path from the platform reset cause.
 * bios_post() consumes the cold state; later resets in the same run are warm. */
void pc_set_cold_post_pending(bool cold);

/* Play the native POST result tone once core1 audio service is running. */
void pc_play_pending_post_beep(PC *pc);

/* ARM32 ABI guards for the public PC layout. */
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(offsetof(PC, boot_start_time) == 32, "PC.boot_start_time ABI offset");
_Static_assert(offsetof(PC, redraw) == 40, "PC.redraw ABI offset");
_Static_assert(offsetof(PC, emulink) == 80, "PC.emulink ABI offset");
_Static_assert(offsetof(PC, sb16) == 112, "PC.sb16 ABI offset");
_Static_assert(offsetof(PC, covox_sample) == 120, "PC.covox_sample ABI offset");
_Static_assert(offsetof(PC, adlib_enabled) == 124, "PC.adlib_enabled ABI offset");
_Static_assert(offsetof(PC, ide) == 160, "PC.ide ABI offset");
_Static_assert(offsetof(PC, bios) == 188, "PC.bios ABI offset");
_Static_assert(offsetof(PC, port92) == 196, "PC.port92 ABI offset");
_Static_assert(offsetof(PC, shutdown_state) == 200, "PC.shutdown_state ABI offset");
_Static_assert(offsetof(PC, lpt_data) == 224, "PC.lpt_data ABI offset");
_Static_assert(offsetof(PC, lpt_ctrl) == 226, "PC.lpt_ctrl ABI offset");
_Static_assert(sizeof(PC) == 228, "PC ABI size");
#endif

#pragma pack(pop)

typedef struct {
	const char *bios;
	const char *vga_bios;
	long mem_size;
	long vga_mem_size;
	const char *ata[4];
	int iscd[4];
	int raw_sd_hdd;
	const char *fdd[2];
	int redirector;
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
void pc_step(PC *pc, size_t max_ops);
/* Service emulated devices without executing guest CPU instructions.
 * Used by cooperative native-ELF yield. */
void pc_service(PC *pc);

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value);
void load_bios_and_reset(PC *pc);
void bios_post(PC *pc);

int16_t midi_sample(void);

#endif /* PC_H */
