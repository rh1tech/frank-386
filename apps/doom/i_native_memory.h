#ifndef __DOOM_NATIVE_MEMORY_H__
#define __DOOM_NATIVE_MEMORY_H__

#ifdef ELF_MODE

/* Native software screens are ordinary allocations from the native runtime. */
#define NATIVE_SCREENS_SIZE (320ul * 200ul * 4ul)

extern void *native_screens_base;

#endif /* ELF_MODE */

#endif /* __DOOM_NATIVE_MEMORY_H__ */
