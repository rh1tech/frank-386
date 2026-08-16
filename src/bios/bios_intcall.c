#include <pico.h>
//#include <hardware/gpio.h>
#include "286/cpu.h"
#include "bios/bios.h"

const char* last_int_call = "NONE";

static bool intcall_waiter(CPU* cpu, bios_callback_params_t* params) {
    //gpio_put(PICO_DEFAULT_LED_PIN, 1);
    last_int_call = params->owner;
    if (!params->done) {
        ifl = 0; /// no more IRQ till return to normal flow
        params->done = true;
        cpu->native_done = true;
    } else {
        ifl = 1; // allow IRQ
    }
    return false; // in a loop on the same CS:IP, no IRET required there
}

extern struct PC* pc;
void pc_step(struct PC* pc, size_t max_ops);
/* proto.h (fdos) not included here - forward-declare the terminate probe. */
extern bool terminate_requested(void);

void bios_intcall(CPU* cpu, uint8_t intnum, const char* owner) {
    uint16_t entry_ax = CPU_AX;
    uint32_t wait_loops = 0;
    last_int_call = owner;
    u16 cs = CPU_CS;
    u16 ip = CPU_IP;
    /*
     * Вложенное исполнение гостевого обработчика не должно ни наследовать
     * single-step состояние прерванного потока, ни утащить своё наружу:
     * - tf прерванного потока (например, Norton трассирует через TF, а
     *   ядро посреди этого вызывает INT 28h/INT 23h) не должен пошагово
     *   трассировать вызываемый здесь обработчик;
     * - pending-трап, взведённый ПОСЛЕДНЕЙ инструкцией вложенного потока
     *   (IRET обработчика, восстановивший TF=1 из стекового образа),
     *   не должен выстрелить после первой инструкции внешнего потока.
     * cpu_intcall() сам сбрасывает tf для входа в обработчик (как INT),
     * но образ флагов в стеке хранит исходное значение - его IRET и
     * вернёт; здесь мы страхуем только межпоточную утечку состояния.
     */
    bool old_pending_trap = cpu_pending_trap();
    cpu_pending_trap_set(false);
    bios_callback_params_t params = {
        .callback = intcall_waiter,
        .expected_cs = 0xFFEF, // just default, may be changed
        .expected_ip = 0x000F, // by set_bios_callback reenter=true
        .done = false,
        .owner = owner
    };
    set_bios_callback(cpu, &params, true);
#if BIOS_DEBUG
    if (intnum != 0x1C) {
        char buf[80];
        u16 new_cs = getmem16(0, (uint16_t) intnum * 4 + 2);
        u16 new_ip = getmem16(0, (uint16_t) intnum * 4);
        int snprintf(char *s, size_t n, const char *fmt, ...);
        snprintf(buf, 79, "INT %02Xh ARM? %04X:%04X->%04X:%04X AX:%04X  ", intnum, cs, ip, params.expected_cs, params.expected_ip, CPU_AX);
        print_line(buf, 0);
    }
#endif
    // to handle IRET by intcall_waiter:
    SET_CS ( params.expected_cs ); // -> FFEFF
    SET_IP ( params.expected_ip );
    bool old_ifl = ifl;
    /* native_done делят request_terminate(), этот waiter и
       cpu_far_call(): состояние ВНЕШНЕГО потока (например, уже
       запрошенная терминация) стекуется, а не затирается. Внутри
       вложенного цикла оно обязано быть false - его выставит только
       наш intcall_waiter. */
    bool old_native_done = cpu->native_done;
    // set CS:IP/flags, prep stack, and on IRET will recover
    cpu_intcall(cpu, intnum);
    cpu->native_done = false;
    while(!params.done) {
        if (++wait_loops == 256u) {
            char buf[80];
            int snprintf(char *s, size_t n, const char *fmt, ...);
            snprintf(buf, sizeof(buf),
                     "WAIT INT %02X AX=%04X %s",
                     intnum, entry_ax, owner ? owner : "?");
            print_line(buf, 0);
        }
        /* Break the nested guest burst when a terminate is pending.
           request_terminate() (e.g. LMSW PE=1) latches terminate_flag
           and native_done, but this loop only ends on params.done -
           which our intcall_waiter sets when the guest returns to the
           trap address.  An aborted guest thread never gets there, so
           native_done stays true, i286_step() bails at the top running
           zero instructions, and the loop spins forever.  Leave
           terminate_flag set: the signal cascades up to the owning
           exec_run_process(), which performs the process teardown. */
        if (terminate_requested())
            break;
        pc_step(pc, 4096); /// TODO: a lot of?
    }
    cpu->native_done = old_native_done;
    drop_bios_callback(cpu, &params);
    ifl = old_ifl;
    cpu_pending_trap_set(old_pending_trap);
    params.done = false;
    // restore initial CS:IP
    SET_CS (cs);
    SET_IP (ip);
    //gpio_put(PICO_DEFAULT_LED_PIN, 0);
}
