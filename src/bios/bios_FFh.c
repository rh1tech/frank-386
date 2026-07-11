#include "286/cpu.h"
#include "bios.h"

#define printf(...) bios_printf(cpu, __VA_ARGS__)

 // default callback for the bios_FFh
static bool bios_no_callback(CPU* cpu, bios_callback_params_t* any) {
#ifdef NO_HANDLER_DETECTOR
    cpu_err_msg(cpu, "ERROR: no bios callback defined");
while(1); // remove it
#endif
    return true; // IRET
}

static bios_callback_params_t root = {
    .callback = bios_no_callback,
    .owner = "ROOT"
};

#if PDB_DEBUG
void dpb_watch_check_chain(const char *tag);
static void dpb_watch_ff_checkpoint(CPU* cpu, const char *where,
                                    bios_callback_params_t *node)
{
    static char tags[16][80];
    static unsigned tag_idx;
    int snprintf(char *s, size_t n, const char *fmt, ...);
    char *tag = tags[tag_idx++ & 15];
    snprintf(tag, sizeof(tags[0]),
             "FF-%s CS:IP=%04x:%04x AX=%04x cb=%s done=%u",
             where, CPU_CS, CPU_IP, CPU_AX,
             node ? node->owner : "?",
             node ? node->done : 0);
    dpb_watch_check_chain(tag);
}
#else
#define dpb_watch_native_checkpoint(...)
#define dpb_watch_ff_checkpoint(...)
#endif

// assigned to 0xFFEFF address (FFE0: 00FF)
// used as callback, no direct vector is used for this in IVT
// in case such address in CS:IP, it means: it was restored from x86 stack,
// pushed by some bios_XXh function to return back
bool bios_FFh(CPU* cpu) { // W/A BIOS callback
    u16 cs = CPU_CS;
    u16 ip = CPU_IP;
    bios_callback_params_t* node = &root;
    dpb_watch_ff_checkpoint(cpu, "entry", node);
    while(node->chain) {
        node = node->chain;
        dpb_watch_ff_checkpoint(cpu, "scan", node);
        if (cs == node->expected_cs && ip == node->expected_ip) {
            dpb_watch_ff_checkpoint(cpu, "before-callback", node);
            bool rc = node->callback(cpu, node);
            dpb_watch_ff_checkpoint(cpu, "after-callback", node);
            return rc;
        }
    }
    printf("no callback found, let try default\n");
    dpb_watch_ff_checkpoint(cpu, "before-no-callback", &root);
    return bios_no_callback(cpu, &root);
}

bool set_bios_callback(CPU* cpu, bios_callback_params_t* params, bool reenter) {
    (void)cpu;
    bios_callback_params_t* node = &root;
    while(node->chain) {
        if (node->chain == params) {
            // already installed, so just ignore
            return true;
        }
        if (node->chain->expected_cs == params->expected_cs && node->chain->expected_ip == params->expected_ip) {
            if (reenter) {
                params->expected_ip += 0x10;
                --params->expected_cs;
                return set_bios_callback(cpu, params, reenter);
            }
            printf("[set_bios_callback] WARN: replaces handler on %04x:%04x\n", params->expected_cs, params->expected_ip);
            bios_callback_params_t* old = node->chain;
            params->chain = old->chain;
            old->chain = NULL;
            old->data = NULL;
            node->chain = params;
            return true;
        }
        node = node->chain;
    }
    ///printf("[set_bios_callback] new BIOS handler on %04x:%04x\n", params->expected_cs, params->expected_ip);
    node->chain = params;
    return true;
}

bool drop_bios_callback(CPU* cpu, bios_callback_params_t* params) {
    (void)cpu;
    if (!params) {
        return false;
    }
    bios_callback_params_t* node = &root;
    while (node->chain) {
        if (node->chain == params) {
            node->chain = params->chain;
            params->chain = NULL;
            params->data = NULL;
            return true;
        }
        node = node->chain;
    }
    return false;
}
