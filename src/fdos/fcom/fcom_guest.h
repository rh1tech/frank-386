#ifndef FDOS_FCOM_GUEST_H
#define FDOS_FCOM_GUEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FCOM_GUEST_NULL UINT32_MAX

uint32_t fcom_guest_linear(uint16_t segment, uint16_t offset);

uint8_t fcom_guest_read8(uint32_t addr);
void fcom_guest_write8(uint32_t addr, uint8_t value);
uint16_t fcom_guest_read16(uint32_t addr);
void fcom_guest_write16(uint32_t addr, uint16_t value);

void fcom_guest_read(uint32_t addr, void *dst, size_t len);
void fcom_guest_write(uint32_t addr, const void *src, size_t len);
void fcom_guest_fill(uint32_t addr, uint8_t value, size_t len);
void fcom_guest_copy(uint32_t dst, uint32_t src, size_t len);

/* Native shadow for FCOM persistent process data.  Guest addresses inside
 * the active range are routed to the shadow; explicit sync keeps the DOS
 * PSP:offset mirror coherent around guest interrupt execution. */
int fcom_guest_shadow_enter(uint32_t base, void *storage, size_t size);
void fcom_guest_shadow_leave(void);
void fcom_guest_shadow_sync_to_guest(void);
void fcom_guest_shadow_sync_from_guest(void);
void *fcom_guest_shadow_ptr(uint32_t addr, size_t len);

size_t fcom_guest_strnlen(uint32_t addr, size_t maxlen);
int fcom_guest_env_name_matches(uint32_t entry, size_t entry_len,
                                const char *name, size_t name_len);

uint16_t fcom_guest_psp_environment(uint16_t psp_seg);
void fcom_guest_psp_set_environment(uint16_t psp_seg, uint16_t env_seg);

uint16_t fcom_guest_current_psp(void);
uint8_t fcom_guest_lol_uppermem_link(void);
uint16_t fcom_guest_lol_first_mcb(void);
uint8_t fcom_guest_mem_access_mode(void);
void fcom_guest_set_mem_access_mode(uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
