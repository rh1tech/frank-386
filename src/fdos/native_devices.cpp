extern "C" {
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"
#include "i8254.h"
#include "hdrs.h"
}
#include "guest_ref.hpp"
#include "native_devices.h"

using fdos_guest::dhdr_ref;
using fdos_guest::request_ref;

/* dsk.cpp */
void blk_driver(CPU *cpu, request_ref &rq);

namespace {

__attribute__((always_inline)) inline uint32_t guest_linear(dos_far_ptr p)
{
    return (static_cast<uint32_t>(FP_SEG(p)) << 4) + FP_OFF(p);
}

__attribute__((always_inline)) inline void rq_done(request_ref &rq)
{
    rq.status(S_DONE);
}

__attribute__((always_inline)) inline void rq_error(request_ref &rq, UBYTE err)
{
    rq.status(S_ERROR | S_DONE | err);
}

static BYTE con_scan_code;
UWORD DaysSinceEpoch = 0;

} // namespace

extern "C" int fdos_native_execrh(dos_far_ptr dhp, dos_far_ptr rq_far,
                                     UWORD *status)
{
    const dhdr_ref dh(dhp);
    if ((dh.attr() & ATTR_NATIVE) == 0)
        return 0;

    const auto intr = dh.native_interrupt();
    if (intr == nullptr) {
        request_ref rq(rq_far);
        rq_error(rq, E_FAILURE);
        *status = rq.status();
        return 1;
    }

    intr(rq_far);
    *status = request_ref(rq_far).status();
    return 1;
}

extern "C" void fdos_x86_dhdr_entries(dos_far_ptr dhp,
                                         UWORD *strategy,
                                         UWORD *interrupt)
{
    const dhdr_ref dh(dhp);
    *strategy = dh.strategy();
    *interrupt = dh.interrupt();
}

extern "C" void ConIntr(dos_far_ptr rq_far)
{
    request_ref rq(rq_far);
    CPU_regs saved;

    switch (static_cast<UBYTE>(rq.command())) {
    case C_INIT:
        con_scan_code = 0;
        kbdType = read86(0x496) & 0x10;
        rq_done(rq);
        break;

    case C_IFLUSH:
        con_scan_code = 0;
        cpu_save_regs(cpu, &saved);
        while (1) {
            CPU_AH = (BYTE)(0x01 + kbdType);
            bios_intcall(cpu, 0x16, "CON INTR");
            if (zf)
                break;
            CPU_AH = kbdType;
            bios_intcall(cpu, 0x16, "CON INTR");
        }
        cpu_restore_regs(cpu, &saved);
        rq_done(rq);
        break;

    case C_NDREAD:
        if (con_scan_code) {
            rq.ndbyte(con_scan_code);
            rq_done(rq);
            break;
        }

        cpu_save_regs(cpu, &saved);
        CPU_AH = (BYTE)(0x01 + kbdType);
        bios_intcall(cpu, 0x16, "C_NDREAD");
        if (zf || CPU_AX == 0) {
            rq.status(S_DONE | S_BUSY);
        } else {
            BYTE ch = CPU_AL;
            if (ch == 0xE0 && CPU_AH != 0)
                ch = 0;
            rq.ndbyte(ch);
            rq_done(rq);
        }
        cpu_restore_regs(cpu, &saved);
        break;

    case C_ISTAT:
        if (con_scan_code) {
            rq_done(rq);
            break;
        }

        cpu_save_regs(cpu, &saved);
        CPU_AH = (BYTE)(0x01 + kbdType);
        bios_intcall(cpu, 0x16, "C_ISTAT");
        rq.status(zf ? (S_DONE | S_BUSY) : S_DONE);
        cpu_restore_regs(cpu, &saved);
        break;

    case C_INPUT: {
        cpu_save_regs(cpu, &saved);
        const UWORD want = rq.count();
        const dos_far_ptr trans = rq.trans();
        UWORD done = 0;

        if (want != 0 && EFFECTIVE(trans)) {
            const uint32_t dst = guest_linear(trans);
            while (done < want) {
                BYTE ch;
                if (con_scan_code) {
                    ch = con_scan_code;
                    con_scan_code = 0;
                } else {
                    do {
                        CPU_AH = kbdType;
                        bios_intcall(cpu, 0x16, "C_INPUT");
                    } while (CPU_AX == 0);

                    if (CPU_AX == 0x7200) {
                        ch = 0x10;
                    } else {
                        ch = CPU_AL;
                        if (ch == 0xE0 && CPU_AH != 0)
                            ch = 0;
                        if (ch == 0 && CPU_AH != 0)
                            con_scan_code = CPU_AH;
                    }
                }
                pstore8(dst + done, ch);
                ++done;
            }
        }

        rq.count(done);
        cpu_restore_regs(cpu, &saved);
        rq_done(rq);
        break;
    }

    case C_OUTPUT:
    case C_OUTVFY: {
        cpu_save_regs(cpu, &saved);
        const dos_far_ptr trans = rq.trans();
        const uint32_t src = guest_linear(trans);
        UWORD cnt = rq.count();
        UWORD off = 0;
        while (cnt--) {
            CPU_AH = 0x0E;
            CPU_AL = pload8(src + off++);
            CPU_BX = 0x0007;
            bios_intcall(cpu, 0x10, "C_OUTVFY/C_OUTPUT");
        }
        cpu_restore_regs(cpu, &saved);
        rq_done(rq);
        break;
    }

    default:
        rq_error(rq, E_CMD);
        break;
    }
}

extern "C" void PrnIntr(dos_far_ptr rq_far)
{
    request_ref rq(rq_far);
    switch (static_cast<UBYTE>(rq.command())) {
    case C_INIT:
        rq_done(rq);
        break;
    default:
        rq_error(rq, E_CMD);
        break;
    }
}

extern "C" void AuxIntr(dos_far_ptr rq_far)
{
    request_ref rq(rq_far);
    switch (static_cast<UBYTE>(rq.command())) {
    case C_INIT:
        rq_done(rq);
        break;
    default:
        rq_error(rq, E_CMD);
        break;
    }
}

extern "C" void Lpt1Intr(dos_far_ptr rq) { PrnIntr(rq); }
extern "C" void Lpt2Intr(dos_far_ptr rq) { PrnIntr(rq); }
extern "C" void Lpt3Intr(dos_far_ptr rq) { PrnIntr(rq); }
extern "C" void Com2Intr(dos_far_ptr rq) { AuxIntr(rq); }
extern "C" void Com3Intr(dos_far_ptr rq) { AuxIntr(rq); }
extern "C" void Com4Intr(dos_far_ptr rq) { AuxIntr(rq); }

extern "C" void ClkEntry(dos_far_ptr rq_far)
{
    request_ref rq(rq_far);

    switch (static_cast<UBYTE>(rq.command())) {
    case C_INIT:
        rq.nunits(0);
        rq_done(rq);
        break;

    case C_OFLUSH:
    case C_IFLUSH:
        rq_done(rq);
        break;

    case C_INPUT: {
        if (sizeof(ClockRecord) != static_cast<UWORD>(rq.count())) {
            rq_error(rq, E_LENGTH);
            break;
        }

        uint32_t bios_ticks = pload32(0x46C);
        uint32_t hundredths =
            (uint32_t)(((uint64_t)bios_ticks * 100u * 65536u) / PIT_FREQ);
        hundredths %= 24u * 60u * 60u * 100u;

        const UBYTE hours = hundredths / (60u * 60u * 100u);
        hundredths %= 60u * 60u * 100u;
        const UBYTE minutes = hundredths / (60u * 100u);
        hundredths %= 60u * 100u;
        const UBYTE seconds = hundredths / 100u;
        const UBYTE hs = hundredths % 100u;

        const uint32_t dst = guest_linear(rq.trans());
        pstore16(dst + offsetof(ClockRecord, clkDays), DaysSinceEpoch);
        pstore8(dst + offsetof(ClockRecord, clkMinutes), minutes);
        pstore8(dst + offsetof(ClockRecord, clkHours), hours);
        pstore8(dst + offsetof(ClockRecord, clkHundredths), hs);
        pstore8(dst + offsetof(ClockRecord, clkSeconds), seconds);
        rq_done(rq);
        break;
    }

    case C_OUTPUT: {
        if (sizeof(ClockRecord) != static_cast<UWORD>(rq.count())) {
            rq_error(rq, E_LENGTH);
            break;
        }

        const uint32_t src = guest_linear(rq.trans());
        DaysSinceEpoch = pload16(src + offsetof(ClockRecord, clkDays));
        const uint32_t minutes = pload8(src + offsetof(ClockRecord, clkMinutes));
        const uint32_t hours = pload8(src + offsetof(ClockRecord, clkHours));
        const uint32_t hs = pload8(src + offsetof(ClockRecord, clkHundredths));
        const uint32_t seconds = pload8(src + offsetof(ClockRecord, clkSeconds));

        uint32_t hundredths = hours * 60u * 60u * 100u
                            + minutes * 60u * 100u
                            + seconds * 100u + hs;
        hundredths %= 24u * 60u * 60u * 100u;
        const uint32_t bios_ticks =
            (uint32_t)(((uint64_t)hundredths * PIT_FREQ) / (100u * 65536u));
        pstore32(0x46C, bios_ticks);
        pstore8(0x470, 0);
        rq_done(rq);
        break;
    }

    default:
        rq_error(rq, E_FAILURE);
        break;
    }
}

extern "C" void BlkEntry(dos_far_ptr rq_far)
{
    request_ref rq(rq_far);
    blk_driver(cpu, rq);
}

extern "C" void NulIntr(dos_far_ptr rq_far)
{
    request_ref rq(rq_far);
    switch (static_cast<UBYTE>(rq.command())) {
    case C_INIT:
        rq_done(rq);
        break;
    default:
        rq.count(0);
        rq_done(rq);
        break;
    }
}
