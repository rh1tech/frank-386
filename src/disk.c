/*
 * Disk management - adapted from pico-286
 * Selects platform-specific implementation
 */
#ifdef RP2350_BUILD
#include "disks-rp2350.c.inl"
#else
#include "disks-win32.c.inl"
#endif
