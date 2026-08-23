#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void fdos_media_request_prepare(dos_far_ptr dpb, UBYTE command);
dos_far_ptr fdos_media_request_far(void);
UWORD fdos_media_request_status(void);
BYTE fdos_media_request_mcretcode(void);
dos_far_ptr fdos_media_request_bpptr(void);
BOOL fdos_bpb_is_fat32(dos_far_ptr bpbp);
UBYTE fdos_dpb_unit(dos_far_ptr dpb);
ULONG fdos_dpb_xfatsize(dos_far_ptr dpb);
UBYTE fdos_dpb_subunit(dos_far_ptr dpb);
UWORD fdos_dpb_secsize(dos_far_ptr dpb);
UWORD fdos_dpb_dirents(dos_far_ptr dpb);
UWORD fdos_dpb_dirstrt(dos_far_ptr dpb);
UBYTE fdos_dpb_clsmask(dos_far_ptr dpb);
ULONG fdos_dpb_root_cluster(dos_far_ptr dpb);
ULONG fdos_dpb_clus2phys(dos_far_ptr dpb, CLUSTER cluster);
BYTE fdos_dpb_flags(dos_far_ptr dpb);
dos_far_ptr fdos_dpb_device(dos_far_ptr dpb);
void fdos_bpb_to_dpb_guest(dos_far_ptr bpbp, dos_far_ptr dpbp, BOOL extended);

#ifdef __cplusplus
}
#endif
