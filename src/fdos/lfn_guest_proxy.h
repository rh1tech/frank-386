#pragma once

/* Thin guest-memory accessors for struct lfn_inode. */

#ifdef __cplusplus
extern "C" {
#endif

COUNT fdos_lfn_name_length(dos_far_ptr inode);
UBYTE fdos_lfn_sfn_checksum(dos_far_ptr inode);
void fdos_lfn_dir_to_native(dos_far_ptr inode, struct dirent *dst);
void fdos_lfn_dir_from_native(dos_far_ptr inode, const struct dirent *src);
UNICODE fdos_lfn_name_get(dos_far_ptr inode, UWORD index);
void fdos_lfn_name_set(dos_far_ptr inode, UWORD index, UNICODE value);
void fdos_lfn_set_diroff(dos_far_ptr inode, UWORD value);

#ifdef __cplusplus
}
#endif
