#ifndef FDOS_PSRAM_LAYOUT_H
#define FDOS_PSRAM_LAYOUT_H

/* Physical guest-memory offset immediately above the HMA.  Both XMS EMBs and
 * the native application PSRAM heap start here in direct-QSPI builds. */
#define FDOS_XMS_EMB_BASE_PHYS          0x00110000ul
#define ARM_ELF_APP_PSRAM_BEGIN_OFFSET FDOS_XMS_EMB_BASE_PHYS

/* Fixed top-of-QSPI native stack reserve.  The reclaimable FatFs L2 cache must
 * stay below this arena even while no native application is running. */
#define ARM_ELF_NATIVE_STACK_ARENA_SIZE (256u * 1024u)

#endif
