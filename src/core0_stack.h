#ifndef CORE0_STACK_H
#define CORE0_STACK_H

#include <stdbool.h>
#include <stdint.h>

extern uintptr_t core0_stack_floor_runtime;
extern uintptr_t core0_stack_top_runtime;
extern bool core0_stack_uses_gfx_buffer;

/* After CONFIG.SYS/init is finished, reclaim CORE0_STACK_EXT too and resize
 * the services that already use the relocated old stack SRAM. No-op unless
 * core0 actually moved into GFX_BUFFER. */
void core0_expand_relocated_stack_services(void);

#endif
