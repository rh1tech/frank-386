#ifndef __NATIVE_DOS_SOUND_HW_H__
#define __NATIVE_DOS_SOUND_HW_H__

#include <stdint.h>

/*
 * Audio hardware bits reported from the public PC structure.
 *
 * These reflect devices which are actually enabled in the running emulator.
 * The implementation lives in dos-api-sdtfn.c so DOOM code does not need to
 * include dos-api.h/pc.h directly (which would collide with DOOM's enum PC).
 */
#define SOUND_HW_PC_SPEAKER   (1u << 0)
#define SOUND_HW_ADLIB        (1u << 1)
#define SOUND_HW_SB16         (1u << 2)
#define SOUND_HW_TANDY        (1u << 3)
#define SOUND_HW_COVOX        (1u << 4)
#define SOUND_HW_MPU401       (1u << 5)
#define SOUND_HW_DSS          (1u << 6)

uint32_t sound_hw_mask(void);

#endif /* __NATIVE_DOS_SOUND_HW_H__ */
