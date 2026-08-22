#ifndef FDOS_FCOM_FCOM_H
#define FDOS_FCOM_FCOM_H

#include "286/cpu.h"

int fcom_is_command_com(const char *name);
UWORD fcom_create_process(const char *init_tail, UBYTE start_mode,
                          UWORD parent_psp, UWORD environment_seg);
UWORD fcom_create_process_guest_tail(dos_far_ptr command_tail,
                          UBYTE start_mode, UWORD parent_psp,
                          UWORD environment_seg);
UWORD fcom_process_stack_top(void);
UWORD fcom_process_entry_offset(void);
UBYTE fcom_process_main(CPU *cpu, UWORD command_psp);
void fcom_run(CPU *cpu, const char *init_tail, UBYTE start_mode,
              UWORD environment_seg, UBYTE own_environment);

#endif
