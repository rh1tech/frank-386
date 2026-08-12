#ifndef __DOOM_NATIVE_MEMORY_H__
#define __DOOM_NATIVE_MEMORY_H__

#ifdef ELF_MODE

/*
 * Native DOOM PSRAM layout.
 *
 * 0x000000..0x10ffff belongs to the x86 guest.  This includes the normal
 * 1-MiB real-mode address space plus the 64-KiB HMA/A20 extension.  Native
 * ARM code must never use this range for private buffers.
 *
 * Keep the software screens immediately above the guest-owned range and put
 * the DOOM zone at the next convenient 64-KiB boundary.  Both V_Init() and
 * I_ZoneBase() use these constants so the two allocations cannot drift into
 * one another.
 */
#define NATIVE_GUEST_RESERVED_SIZE  (0x00110000ul)
#define NATIVE_SCREENS_OFFSET       (NATIVE_GUEST_RESERVED_SIZE)
#define NATIVE_SCREENS_SIZE         (320ul * 200ul * 4ul)
#define NATIVE_ZONE_OFFSET          (0x00150000ul)

#if NATIVE_SCREENS_OFFSET + NATIVE_SCREENS_SIZE > NATIVE_ZONE_OFFSET
#error Native DOOM PSRAM screen reservation overlaps the DOOM zone
#endif

#endif /* ELF_MODE */

#endif /* __DOOM_NATIVE_MEMORY_H__ */
