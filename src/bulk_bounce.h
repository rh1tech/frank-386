#ifndef BULK_BOUNCE_H
#define BULK_BOUNCE_H

#include <stdint.h>

/* Shared 512-byte bounce buffer for synchronous core0 FatFs <-> guest bulk I/O. */
#define GUEST_BULK_BUF_SIZE 512u
extern uint8_t guest_bulk_buf[GUEST_BULK_BUF_SIZE];

#endif /* BULK_BOUNCE_H */
