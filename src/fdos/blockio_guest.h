#pragma once

#ifdef __cplusplus
extern "C" {
#endif

dos_far_ptr fdos_buffer_first(void);
void fdos_buffer_first_set(dos_far_ptr p);
UWORD fdos_buffer_count(void);
UWORD fdos_buffer_next(dos_far_ptr p);
UWORD fdos_buffer_prev(dos_far_ptr p);
void fdos_buffer_next_set(dos_far_ptr p, UWORD v);
void fdos_buffer_prev_set(dos_far_ptr p, UWORD v);
BYTE fdos_buffer_unit(dos_far_ptr p);
void fdos_buffer_unit_set(dos_far_ptr p, BYTE v);
BYTE fdos_buffer_flag(dos_far_ptr p);
void fdos_buffer_flag_set(dos_far_ptr p, BYTE v);
ULONG fdos_buffer_blkno(dos_far_ptr p);
void fdos_buffer_blkno_set(dos_far_ptr p, ULONG v);
UBYTE fdos_buffer_copies(dos_far_ptr p);
UWORD fdos_buffer_offset(dos_far_ptr p);
dos_far_ptr fdos_buffer_dpbp(dos_far_ptr p);

#ifdef __cplusplus
}
#endif
