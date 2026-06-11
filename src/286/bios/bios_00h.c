#include "i286.h"
#include "bios.h"

/**
 * INT 00 - CPU-generated - DIVIDE ERROR

Desc:	generated if the divisor of a DIV or IDIV instruction is zero or the
	  quotient overflows the result register; DX and AX will be unchanged.
Notes:	on an 8086/8088, the return address points to the following instruction
	on an 80286+, the return address points to the divide instruction
	an 8086/8088 will generate this interrupt if the result of a division
	  is 80h (byte) or 8000h (word)
 */
bool bios_00h() {
    // TODO: detect division by sero, etc
    //
    return true;
}
