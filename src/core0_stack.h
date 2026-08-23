#ifndef CORE0_STACK_H
#define CORE0_STACK_H

#include <stdbool.h>
#include <stdint.h>

extern uintptr_t core0_stack_floor_runtime;
extern uintptr_t core0_stack_top_runtime;
extern bool core0_stack_uses_gfx_buffer;

#endif
