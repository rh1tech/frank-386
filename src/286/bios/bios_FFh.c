#include "../cpu.h"
#include "../bios.h"

// assigned to 0xFFEFF address (FFE0: 00FF)
// used as callback, no direct vector is used for this in IVT
// in case such address in CS:IP, it means: it was restored from x86 stack,
// pushed by some bios_XXh function to return back
bool bios_FFh(CPU* cpu) { // W/A BIOS callback
    if (!cpu->ext_accessors->bios_callback) {
        cpu_err_msg(cpu, "ERROR: null bios callback defined");
while(1); // remove it
        return true; // IRET
    }
    return cpu->ext_accessors->bios_callback(cpu, cpu->ext_accessors->bios_callback_data);
}

bool bios_no_callback(CPU* cpu, void* any) {
    cpu_err_msg(cpu, "ERROR: no bios callback defined");
while(1); // remove it
    return true; // IRET
}
