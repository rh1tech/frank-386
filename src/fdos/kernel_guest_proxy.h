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
void fdos_lol_or_version_flags(UBYTE bits);
void fdos_dos_set_current_ldt(dos_far_ptr value);

UWORD fdos_dos_cu_psp(void);
UBYTE fdos_dos_mem_access_mode(void);
void fdos_dos_set_mem_access_mode(UBYTE value);
UBYTE fdos_lol_uppermem_link(void);
UWORD fdos_lol_uppermem_root(void);
UWORD fdos_lol_first_mcb(void);
dos_far_ptr fdos_lol_sfthead(void);
ULONG fdos_sft_size(dos_far_ptr sft_ptr);
UWORD fdos_sft_count(dos_far_ptr sft_ptr);
dos_far_ptr fdos_dos_lp_cur_sft(void);
dos_far_ptr fdos_lol_dpb(void);
UBYTE fdos_lol_nblkdev(void);
dos_far_ptr fdos_dpb_next(dos_far_ptr dpb_ptr);
dos_far_ptr fdos_dpb_device(dos_far_ptr dpb_ptr);
UBYTE fdos_dpb_unit(dos_far_ptr dpb_ptr);
UBYTE fdos_dpb_subunit(dos_far_ptr dpb_ptr);
BYTE fdos_dpb_flags(dos_far_ptr dpb_ptr);
UBYTE fdos_dpb_mdb(dos_far_ptr dpb_ptr);
dos_far_ptr fdos_dhdr_next(dos_far_ptr dhdr_ptr);
UWORD fdos_dhdr_attr(dos_far_ptr dhdr_ptr);
UWORD fdos_dhdr_strategy(dos_far_ptr dhdr_ptr);
UWORD fdos_dhdr_interrupt(dos_far_ptr dhdr_ptr);
void fdos_dhdr_read_name(dos_far_ptr dhdr_ptr, BYTE *dst);

UWORD fdos_dos_crit_err_code(void);
void fdos_dos_set_crit_err_code(UWORD value);
dos_far_ptr fdos_lol_nul_next(void);
UBYTE fdos_lol_os_major(void);
UBYTE fdos_lol_os_minor(void);
void fdos_lol_set_setver(UBYTE major, UBYTE minor);
dos_far_ptr fdos_lol_syscon(void);
dos_far_ptr fdos_cds_slot(unsigned drive);
dos_far_ptr fdos_temp_cds_build(UBYTE drive_letter, unsigned drive);
UWORD fdos_sft_dec_ref_raw(dos_far_ptr sft_ptr);
UWORD fdos_sft_mode_raw(dos_far_ptr sft_ptr);
UWORD fdos_sft_flags_raw(dos_far_ptr sft_ptr);
dos_far_ptr fdos_sft_dev_raw(dos_far_ptr sft_ptr);
void fdos_sft_set_psp_raw(dos_far_ptr sft_ptr, UWORD psp);
UWORD fdos_psp_max_files(UWORD psp_seg);
dos_far_ptr fdos_psp_file_table(UWORD psp_seg);
void fdos_psp_set_parent(UWORD psp_seg, UWORD parent);
void fdos_psp_set_prev(UWORD psp_seg, dos_far_ptr prev);
void fdos_psp_set_size(UWORD psp_seg, UWORD size);
void fdos_psp_set_max_files(UWORD psp_seg, UWORD count);
void fdos_psp_set_file_table(UWORD psp_seg, dos_far_ptr table);
void fdos_psp_set_environment(UWORD psp_seg, UWORD env_seg);
void fdos_psp_set_return_version(UWORD psp_seg, UWORD version);
void fdos_psp_set_vector(UWORD psp_seg, unsigned which, dos_far_ptr vector);
dos_far_ptr fdos_psp_vector(UWORD psp_seg, unsigned which);
void fdos_psp_set_fcb_drive(UWORD psp_seg, unsigned which, UBYTE drive);
UBYTE fdos_psp_fcb_drive(UWORD psp_seg, unsigned which);
void fdos_psp_clear_fcb_name(UWORD psp_seg, unsigned which);
void fdos_psp_set_command_empty(UWORD psp_seg);
void fdos_psp_set_file_handle(UWORD psp_seg, UWORD index, UBYTE handle);
void fdos_sft_inc_ref_raw(dos_far_ptr sft_ptr);
void fdos_lol_set_network_retry(UWORD delay, UWORD retry);
ULONG fdos_sft_position(dos_far_ptr sft_ptr);
void fdos_sft_set_size(dos_far_ptr sft_ptr, ULONG value);
UWORD fdos_sft_date(dos_far_ptr sft_ptr);
UWORD fdos_sft_time(dos_far_ptr sft_ptr);
void fdos_sft_or_mode(dos_far_ptr sft_ptr, UWORD bits);
UWORD fdos_sft_psp(dos_far_ptr sft_ptr);
UWORD fdos_sfttbl_count(dos_far_ptr table_ptr);
dos_far_ptr fdos_sfttbl_next(dos_far_ptr table_ptr);
dos_far_ptr fdos_sfttbl_entry(dos_far_ptr table_ptr, UWORD index);
BOOL fdos_dpb_is_fat32(dos_far_ptr dpb_ptr);
ULONG fdos_dpb_root_cluster(dos_far_ptr dpb_ptr);
ULONG fdos_dpb_xfatsize(dos_far_ptr dpb_ptr);

#ifdef __cplusplus
}
#endif
