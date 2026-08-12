#ifndef __PC_H__
#define __PC_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "cpu.h"

typedef struct IDEIFState IDEIFState;
typedef struct PicState2 PicState2;
typedef struct PITState PITState;
typedef struct U8250 U8250;
typedef struct CMOS CMOS;
typedef struct VGAState VGAState;
typedef struct SimpleFBDrawFunc SimpleFBDrawFunc;
typedef struct KBDState KBDState;
typedef struct PS2MouseState PS2MouseState;
typedef struct PS2KbdState PS2KbdState;
typedef struct AdlibState AdlibState;
typedef struct I8257State I8257State;
typedef struct FDCState FDCState;
typedef struct SB16State SB16State;
typedef struct PCSpkState PCSpkState;
typedef struct PCIDevice PCIDevice;
typedef struct I440FXState I440FXState;
typedef struct PCIBus PCIBus;

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

	/* Emulink FDD - simple virtual floppy protocol on ports 0xF1F0/0xF1F4 */
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

	uint8_t lpt_data[2];   /* LPT1(0x378)/LPT2(0x278) */
	uint8_t lpt_ctrl[2];   /* control 0x37A/0x27A  	*/
} PC;

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

#endif /* __PC_H__ */
