#ifndef FDOS_H
#define FDOS_H

#include <stdbool.h>
#include "i386.h"

void _boot(CPU*); // like kernel/boot/boot.asm
void kernel(CPU*); // like kernel/kernel

#endif // FBIOS_H
