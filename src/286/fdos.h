#ifndef FDOS_H
#define FDOS_H

#include <stdbool.h>
#include "i386.h"

void _boot(CPU*); // like kernel/boot/boot.asm
void kernel(CPU*); // like kernel/kernel

void dos_puts(const char*);
//void execrh(request FAR * rq, struct dhdr FAR * dhp);

bool fdos_21h(CPU*); // MAIN DOS HANDLER
bool fdos_29h(CPU*); // FAST CONSOLE OUTPUT

#endif // FBIOS_H
