#ifndef __NATIVE_DOS_VECT_H__
#define __NATIVE_DOS_VECT_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native replacement for the DOS getvect/setvect ownership pattern.
 *
 * The implementation lives in dos-api-sdtfn.c so applications do not pull
 * CPU/PC internals or system-table details in through this public header.
 */
typedef bool (*dos_native_vector_handler_t)(void *cpu);

typedef struct dos_native_vector {
    uint16_t intno;
    uint16_t old_off;
    uint16_t old_seg;
    dos_native_vector_handler_t old_handler;
    bool installed;
} dos_native_vector_t;

bool dos_native_setvect(dos_native_vector_t *state,
                        unsigned intno,
                        dos_native_vector_handler_t handler);
void dos_native_restorevect(dos_native_vector_t *state);

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_VECT_H__ */
