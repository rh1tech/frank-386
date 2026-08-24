#pragma once

/* Include after hdrs.h/portab.h so legacy DOS types are already defined. */

#ifdef __cplusplus
extern "C" {
#endif

UWORD fdos_cds_flags(dos_far_ptr cds_ptr);
dos_far_ptr fdos_cds_dpb(dos_far_ptr cds_ptr);
WORD fdos_cds_backslash_offset(dos_far_ptr cds_ptr);
void fdos_cds_copy_current_path(dos_far_ptr cds_ptr, char *dst, size_t dst_size);
void fdos_cds_current_path_byte(dos_far_ptr cds_ptr, unsigned index, UBYTE value);
UBYTE fdos_dos_default_drive(void);
UBYTE fdos_dos_lastdrive(void);
void fdos_dos_set_current_ldt(dos_far_ptr value);

UWORD fdos_dos_cu_psp(void);
UBYTE fdos_dos_mem_access_mode(void);
void fdos_dos_set_mem_access_mode(UBYTE value);
UBYTE fdos_lol_uppermem_link(void);
UWORD fdos_lol_uppermem_root(void);
UWORD fdos_lol_first_mcb(void);
ULONG fdos_sft_size(dos_far_ptr sft_ptr);

#ifdef __cplusplus
}
#endif
