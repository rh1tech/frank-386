#pragma once

/* Include after hdrs.h/portab.h so legacy DOS types are already defined. */

#ifdef __cplusplus
extern "C" {
#endif

void fdos_ioctl_set_network_retry(UWORD delay, UWORD retry, BOOL set_retry);
UBYTE fdos_ioctl_default_drive(void);
UWORD fdos_ioctl_sft_flags(dos_far_ptr sft_ptr);
void fdos_ioctl_sft_set_flags_lo(dos_far_ptr sft_ptr, UBYTE value);
dos_far_ptr fdos_ioctl_sft_dev(dos_far_ptr sft_ptr);
ULONG fdos_ioctl_sft_position(dos_far_ptr sft_ptr);
ULONG fdos_ioctl_sft_size(dos_far_ptr sft_ptr);
UBYTE fdos_ioctl_dpb_subunit(dos_far_ptr dpb_ptr);
dos_far_ptr fdos_ioctl_dpb_device(dos_far_ptr dpb_ptr);
BYTE fdos_ioctl_dpb_flags(dos_far_ptr dpb_ptr);
UWORD fdos_ioctl_dhdr_attr(dos_far_ptr dhdr_ptr);
BOOL fdos_ioctl_cds_flags(unsigned drive, UWORD *flags);
void fdos_ioctl_set_crit_err_code(UWORD value);

#ifdef __cplusplus
}
#endif
