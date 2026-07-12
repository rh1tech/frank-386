#ifndef FDOS_FCOM_FCOM_H
#define FDOS_FCOM_FCOM_H

#include "286/cpu.h"

int fcom_is_command_com(const char *name);
void fcom_run(CPU *cpu, const char *init_tail, UBYTE start_mode, UWORD environment_seg, UBYTE own_environment);

#endif
