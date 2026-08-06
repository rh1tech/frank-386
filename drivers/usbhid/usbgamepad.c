/**
 * frank-386 — USB HID gamepad decoding. See usbgamepad.h.
 *
 * Maps ported from FRANK NES (murmnes).
 */
#include "usbgamepad.h"

#include <string.h>

#ifndef CFG_TUH_HID
#define CFG_TUH_HID 4
#endif

/*
 * Different HID gamepads lay out their reports differently, so each
 * entry picks a byte index and bitmask per logical button. D-pad
 * decoding has three shapes:
 *   DPAD_AXIS: two analog bytes, 0x00 = min, 0x7F = centre, 0xFF = max
 *   DPAD_HAT:  8-way hat in the low nibble of one byte
 *   DPAD_BITS: four bits in byte 0
 */
typedef enum {
    DPAD_AXIS = 0,
    DPAD_HAT,
    DPAD_BITS,
} dpad_mode_t;

typedef struct { uint8_t byte; uint8_t mask; } btn_bit_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    dpad_mode_t dpad_mode;
    uint8_t  dpad_x;
    uint8_t  dpad_y;
    btn_bit_t a, b, x, y, l, r, start, select;
} gamepad_map_t;

// Known HID gamepad maps (ported from murmsnes/scripts/gen_gamepad_maps.py output).
static const gamepad_map_t known_hid_maps[] = {
    // gamepad_0079_0006
    {
        .vid = 0x0079, .pid = 0x0006,
        .dpad_mode = DPAD_AXIS, .dpad_x = 0, .dpad_y = 1,
        .a      = { .byte = 5, .mask = 0x20 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x10 },
        .y      = { .byte = 5, .mask = 0x80 },
        .l      = { .byte = 6, .mask = 0x01 },
        .r      = { .byte = 6, .mask = 0x02 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_046D_C219
    {
        .vid = 0x046D, .pid = 0xC219,
        .dpad_mode = DPAD_HAT, .dpad_x = 5, .dpad_y = 0,
        .a      = { .byte = 5, .mask = 0x20 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x10 },
        .y      = { .byte = 5, .mask = 0x80 },
        .l      = { .byte = 6, .mask = 0x01 },
        .r      = { .byte = 6, .mask = 0x02 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_081F_E401 (common cheap SNES-clone pad)
    {
        .vid = 0x081F, .pid = 0xE401,
        .dpad_mode = DPAD_AXIS, .dpad_x = 0, .dpad_y = 1,
        .a      = { .byte = 5, .mask = 0x20 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x10 },
        .y      = { .byte = 5, .mask = 0x80 },
        .l      = { .byte = 6, .mask = 0x01 },
        .r      = { .byte = 6, .mask = 0x02 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_11FF_3331
    {
        .vid = 0x11FF, .pid = 0x3331,
        .dpad_mode = DPAD_AXIS, .dpad_x = 0, .dpad_y = 1,
        .a      = { .byte = 5, .mask = 0x80 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x20 },
        .y      = { .byte = 5, .mask = 0x10 },
        .l      = { .byte = 6, .mask = 0x04 },
        .r      = { .byte = 6, .mask = 0x08 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_2563_0575
    {
        .vid = 0x2563, .pid = 0x0575,
        .dpad_mode = DPAD_HAT, .dpad_x = 2, .dpad_y = 0,
        .a      = { .byte = 0, .mask = 0x04 },
        .b      = { .byte = 0, .mask = 0x02 },
        .x      = { .byte = 0, .mask = 0x08 },
        .y      = { .byte = 0, .mask = 0x01 },
        .l      = { .byte = 0, .mask = 0x10 },
        .r      = { .byte = 0, .mask = 0x20 },
        .start  = { .byte = 1, .mask = 0x02 },
        .select = { .byte = 1, .mask = 0x01 },
    },
    // gamepad_FEED_2320
    {
        .vid = 0xFEED, .pid = 0x2320,
        .dpad_mode = DPAD_HAT, .dpad_x = 5, .dpad_y = 0,
        .a      = { .byte = 6, .mask = 0x01 },
        .b      = { .byte = 6, .mask = 0x02 },
        .x      = { .byte = 6, .mask = 0x08 },
        .y      = { .byte = 6, .mask = 0x04 },
        .l      = { .byte = 6, .mask = 0x10 },
        .r      = { .byte = 6, .mask = 0x20 },
        .start  = { .byte = 7, .mask = 0x08 },
        .select = { .byte = 7, .mask = 0x04 },
    },
};

// Fallback layout for unknown HID pads — same bits as the 0x081F/0xE401
// SNES-clone layout, since most cheap pads look like that.
static const gamepad_map_t fallback_hid_map = {
    .vid = 0, .pid = 0,
    .dpad_mode = DPAD_AXIS, .dpad_x = 3, .dpad_y = 4,
    .a      = { .byte = 5, .mask = 0x20 },
    .b      = { .byte = 5, .mask = 0x40 },
    .x      = { .byte = 5, .mask = 0x10 },
    .y      = { .byte = 5, .mask = 0x80 },
    .l      = { .byte = 6, .mask = 0x01 },
    .r      = { .byte = 6, .mask = 0x02 },
    .start  = { .byte = 6, .mask = 0x20 },
    .select = { .byte = 6, .mask = 0x10 },
};

/*
 * XInput (Xbox) pads are deliberately not handled here. They are not HID
 * devices - they need a separate TinyUSB host class driver
 * (xinput_host.c in murmnes, ~620 lines) plus tusb_config changes. That
 * is a self-contained follow-up rather than something to smuggle into a
 * change that must not disturb the working keyboard path.
 */

static const gamepad_map_t *find_hid_map(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < sizeof(known_hid_maps) / sizeof(known_hid_maps[0]); i++) {
        if (known_hid_maps[i].vid == vid && known_hid_maps[i].pid == pid)
            return &known_hid_maps[i];
    }
    return &fallback_hid_map;
}


// Gamepad state — two slots for two USB gamepads

static inline uint16_t eval_btn(const btn_bit_t *b, const uint8_t *report,
                                uint16_t len, uint16_t out_mask) {
    if (b->byte >= len) return 0;
    return (report[b->byte] & b->mask) ? out_mask : 0;
}

/* One entry per HID instance. A pad is only "connected" once it has
 * actually delivered a Joystick/Gamepad report - mounting alone is not
 * enough, since composite devices mount interfaces we never decode. */
typedef struct {
    uint16_t vid, pid;
    const gamepad_map_t *map;
    uint8_t dpad;        /* bit0 U, 1 D, 2 L, 3 R */
    uint16_t buttons;    /* canonical: 0x01 A, 0x02 B, ... */
    uint8_t active;
} gp_slot_t;

static gp_slot_t gp_slots[CFG_TUH_HID];

void usbgamepad_set_ids(uint8_t instance, uint16_t vid, uint16_t pid) {
    if (instance >= CFG_TUH_HID) return;
    gp_slots[instance].vid = vid;
    gp_slots[instance].pid = pid;
    gp_slots[instance].map = find_hid_map(vid, pid);
}

void usbgamepad_umount(uint8_t instance) {
    if (instance >= CFG_TUH_HID) return;
    memset(&gp_slots[instance], 0, sizeof(gp_slots[instance]));
}

void usbgamepad_report(uint8_t instance, const uint8_t *report, uint16_t len) {
    if (instance >= CFG_TUH_HID || report == NULL || len < 2) return;

    gp_slot_t *gp = &gp_slots[instance];
    const gamepad_map_t *m = gp->map ? gp->map : &fallback_hid_map;
    gp->active = 1;

    uint8_t dpad = 0;
    switch (m->dpad_mode) {
    case DPAD_AXIS:
        if (m->dpad_x < len && m->dpad_y < len) {
            if (report[m->dpad_x] < 0x40) dpad |= 0x04;   /* Left  */
            if (report[m->dpad_x] > 0xC0) dpad |= 0x08;   /* Right */
            if (report[m->dpad_y] < 0x40) dpad |= 0x01;   /* Up    */
            if (report[m->dpad_y] > 0xC0) dpad |= 0x02;   /* Down  */
        }
        break;
    case DPAD_HAT:
        if (m->dpad_x < len) {
            /* 0=U 1=UR 2=R 3=DR 4=D 5=DL 6=L 7=UL 8=neutral */
            switch (report[m->dpad_x] & 0x0F) {
            case 0: dpad = 0x01; break;
            case 1: dpad = 0x01 | 0x08; break;
            case 2: dpad = 0x08; break;
            case 3: dpad = 0x02 | 0x08; break;
            case 4: dpad = 0x02; break;
            case 5: dpad = 0x02 | 0x04; break;
            case 6: dpad = 0x04; break;
            case 7: dpad = 0x01 | 0x04; break;
            default: dpad = 0; break;
            }
        }
        break;
    case DPAD_BITS:
        if (report[0] & 0x01) dpad |= 0x01;
        if (report[0] & 0x02) dpad |= 0x02;
        if (report[0] & 0x04) dpad |= 0x04;
        if (report[0] & 0x08) dpad |= 0x08;
        break;
    }
    gp->dpad = dpad;

    uint16_t buttons = 0;
    buttons |= eval_btn(&m->a,      report, len, 0x0001);
    buttons |= eval_btn(&m->b,      report, len, 0x0002);
    buttons |= eval_btn(&m->x,      report, len, 0x0004);
    buttons |= eval_btn(&m->y,      report, len, 0x0008);
    buttons |= eval_btn(&m->l,      report, len, 0x0010);
    buttons |= eval_btn(&m->r,      report, len, 0x0020);
    buttons |= eval_btn(&m->start,  report, len, 0x0040);
    buttons |= eval_btn(&m->select, report, len, 0x0080);
    gp->buttons = buttons;
}

int usbgamepad_connected(void) {
    for (int i = 0; i < CFG_TUH_HID; i++)
        if (gp_slots[i].active) return 1;
    return 0;
}

void usbgamepad_get(int *x, int *y, uint8_t *buttons) {
    uint8_t dpad = 0;
    uint16_t btn = 0;
    for (int i = 0; i < CFG_TUH_HID; i++) {
        if (!gp_slots[i].active) continue;
        dpad |= gp_slots[i].dpad;
        btn  |= gp_slots[i].buttons;
    }

    int jx = 0, jy = 0;
    if (dpad & 0x04) jx = -1;
    if (dpad & 0x08) jx =  1;
    if (dpad & 0x01) jy = -1;
    if (dpad & 0x02) jy =  1;

    /* A/B are what a two-button DOS stick expects; X/Y alias onto them
     * so either pair works on pads that have four face buttons. */
    uint8_t jb = 0;
    if (btn & (0x0001 | 0x0008)) jb |= 0x01;   /* A or Y */
    if (btn & (0x0002 | 0x0004)) jb |= 0x02;   /* B or X */

    if (x) *x = jx;
    if (y) *y = jy;
    if (buttons) *buttons = jb;
}
