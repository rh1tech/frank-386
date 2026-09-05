#ifndef AUDIODIAG_H
#define AUDIODIAG_H

#include <stdint.h>
#include <hardware/timer.h>

/*
 * Live audio-path instrumentation, read out over SWD.
 *
 * The question this exists to answer is what a game actually does to the
 * sound hardware, at what rate, and how much of that the emulator delivers.
 * Counters alone cannot answer it: a stream of OPL writes at 8 kHz and a
 * stream at 700 Hz produce the same totals over a second if the second one
 * is bursty.  So this keeps ring buffers with microsecond timestamps - every
 * OPL register write, every IRQ0 edge handed to the PIC, and every Sound
 * Blaster and DMA event - and the analysis happens on the host from the dump.
 *
 * Where it lives: the guest's 0xA0000-0xBFFFF VGA aperture is redirected to
 * gfx_buffer on RP2350, so the PSRAM behind it is never read or written by
 * the guest.  adlib.c already borrows its first 2 KB for the OPL3 bank-1
 * shadow.  Everything here sits further in, which means the diagnostics cost
 * zero SRAM - and that matters: this firmware has repeatedly stopped booting
 * after much smaller .bss and heap changes.  pc_new() memsets the whole
 * 8 MB before the guest starts, so every counter begins at zero on boot.
 *
 * Enable with -DAUDIO_DIAG=ON.  With it off, every macro below compiles to
 * nothing and no code changes.
 */

#ifndef FRANK_AUDIO_DIAG
#define FRANK_AUDIO_DIAG 0
#endif

#if FRANK_AUDIO_DIAG

#define FRANK_DIAG_MAGIC   0x47414944u   /* "DIAG" */
#define FRANK_DIAG_BASE    (0x11000000u + 0x000a1000u)

/*
 * The instruction-pointer ring.
 *
 * The CS ring above sees only far transfers, and the failure being chased
 * starts with a *near* one: control leaves the game's code inside its own
 * segment, runs through whatever bytes it finds for thousands of
 * instructions, wraps IP past 0xffff and only then trips an invalid opcode.
 * Nothing in a far-transfer log can show where that began.
 *
 * So this records every instruction pointer - two bytes, one store per
 * instruction - and freezes with everything else.  32768 entries cover about
 * 16 ms of guest time, comfortably more than the 7.5 ms measured between the
 * bad branch and the first #UD.  It takes the space of the OPL, IRQ and
 * event rings, which is why those stop recording while it is on; their
 * counters keep working.
 */
#define FRANK_IP_TRACE     1

/*
 * The ring has to live inside the borrowed VGA aperture, and that is a hard
 * limit rather than a choice.  Moving it above the guest's memory looked
 * obvious - a million entries would be half a second of history instead of a
 * few milliseconds - but CMakeLists.txt gives the guest EMU_MEM_SIZE_MB=8 and
 * the part is 8 MB, so there is no "above": every byte of PSRAM is guest
 * memory.  Writing a ring at 0x11400000 put it in the middle of the guest's
 * RAM and the machine went into a boot loop.
 *
 * A probe that wrote a pattern at 0x11500000 and read it back intact while
 * the emulator ran is *not* evidence that the region is free; it only means
 * the guest had not touched that byte in that second.  Check the configured
 * memory size, not the memory.
 */
#define FRANK_IP_RING      ((volatile uint32_t *)(FRANK_DIAG_BASE + 0x1000u))
#define FRANK_IP_N         16384u        /* 64 KB at BASE+0x1000, a power of two */
#define FRANK_IP_MASK      (FRANK_IP_N - 1u)

#define FRANK_OPL_TRACE_N  2048u         /* 16 KB at BASE+0x1000 */
#define FRANK_IRQ_TRACE_N  4096u         /* 16 KB at BASE+0x6000 */
#define FRANK_EV_TRACE_N   2048u         /* 24 KB at BASE+0xc000 */

typedef struct {
    uint32_t t;      /* time_us_32() at the write */
    uint16_t port;   /* I/O port it arrived on */
    uint8_t  reg;    /* selected OPL register (data writes only) */
    uint8_t  val;    /* byte written */
} FrankOplEvent;

/*
 * One ring for everything the digital side does, because the whole point is
 * to see the *order* of it: a DSP command, the DMA registers that were
 * programmed before it, the transfers it produced and the interrupt it did
 * or did not raise, all on one timeline.
 */
enum {
    FRANK_EV_DSP_W  = 1,   /* a = port offset,  c = value          */
    FRANK_EV_DSP_R  = 2,   /* a = port offset,  c = value          */
    FRANK_EV_MIX_W  = 3,   /* a = mixer index,  c = value          */
    FRANK_EV_MIX_R  = 4,   /* a = mixer index,  c = value          */
    FRANK_EV_IRQ    = 5,   /* a = irq number,   c = level          */
    FRANK_EV_DMA_IO = 6,   /* a = port,         c = value          */
    FRANK_EV_DMA_RUN= 7,   /* a = channel, b = written, c = dma_pos*/
    FRANK_EV_DSP_CMD= 8,   /* a = command, b = block_size, c = freq*/
};

typedef struct {
    uint32_t t;
    uint8_t  kind;
    uint8_t  a;
    uint16_t b;
    uint32_t c;
} FrankEvent;

typedef struct {
    uint32_t magic;
    uint32_t opl_sel;         /* index-port writes */
    uint32_t opl_data;        /* data-port writes */
    uint32_t opl_status;      /* status-port reads */
    uint32_t irq0_edges;      /* IRQ0 pulses handed to the PIC */
    uint32_t pit_polls;       /* i8254_update_irq() calls */
    uint32_t pc_steps;        /* pc_step() calls */
    uint32_t port61;          /* PC speaker port writes */
    uint32_t sb_dsp;          /* Sound Blaster DSP command writes */
    uint32_t covox;           /* parallel-port DAC writes */
    uint32_t pit_ch0_count;   /* channel 0 reload as last latched */
    uint32_t uticks;          /* wall clock at the last pc_step() */
    uint32_t opl_head;        /* next slot in the OPL ring */
    uint32_t irq_head;        /* next slot in the IRQ0 ring */
    uint32_t opl_gap[24];     /* log2(us) histogram of OPL data-write gaps */
    uint32_t last_opl_us;
    uint32_t ev_head;         /* next slot in the digital-side ring */
    uint32_t ev_total;        /* events ever recorded, ring or not */
    uint32_t ev_freeze;       /* 1 = stop at the end of the ring, keep the start */
    uint16_t reghist[256];    /* data writes per OPL register */

    /*
     * Core-0 cycle accounting, in DWT cycles.
     *
     * SUBSYS_PROFILE already answers this question, but enabling it pushes
     * SRAM from 91.8% to 94.9% and the board then comes up with a black
     * screen - pc_new() runs out of heap long before the profiler prints
     * anything.  These accumulators live in the same PSRAM block as the rest
     * of the diagnostics, so they cost no SRAM at all and the build that
     * measures is the build that runs.
     */
    uint64_t prof_total;      /* whole pc_step()                            */
    uint64_t prof_cpu;        /* cpui386_step() - the interpreter           */
    uint64_t prof_adlib;      /* adlib_core0()  - OPL2 synthesis            */
    uint64_t prof_dev;        /* PIT/CMOS/8042/DMA/SB poll/FDC/vga_step     */
    uint32_t prof_steps;
    uint32_t prof_pad;

    /*
     * First CPU exception, and the control-flow trail that led to it.
     *
     * Prehistorik 2 wedges by running off into unwritten BIOS ROM at
     * F000:2D2C, where the 0xff filler decodes as an invalid opcode: #UD
     * vectors to INT 6, the BIOS handler there is a bare IRET, and the IRET
     * returns to the same address forever.  The loop is stable, so the
     * faulting address is easy to read live - what is not is how control got
     * there the first time, because by then the trail is long gone.
     *
     * So the first exception latches everything and freezes the trail behind
     * it.  The trail is sampled once per cpui386_step() call and only when
     * the code segment actually changes, which costs a compare in a function
     * that already runs every few hundred instructions and compresses a
     * second of execution into a handful of rows.
     */
    uint32_t ud_hit;          /* 0 until the first exception is latched */
    uint32_t ud_cs;
    uint32_t ud_ip;
    uint32_t ud_excno;
    uint32_t ud_excerr;
    uint32_t ud_flags;
    uint32_t trace_head;
    uint32_t trace_last_cs;
    uint32_t cs_head;         /* next slot in the CS-load ring */
    uint32_t exc_hist[32];    /* every exception, counted by number */

    /*
     * Why the recorder stopped, and the guest stack at that moment.
     * 1 = the first #UD, 2 = a control transfer that repeated until its
     * counter saturated, which is what a wedged guest looks like from here.
     */
    uint32_t ud_reason;       /* +0x374 */
    uint32_t ud_ssp;          /* +0x378  SS base + SP, linear */
    uint32_t ud_stk0;         /* +0x37c  the four bytes at SS:SP */
    uint32_t ud_stk1;         /* +0x380  and the next four       */
    uint32_t ip_head;         /* +0x384  next slot in the IP ring */
    uint32_t ip_last_cs;      /* +0x388  CS base as the ring last saw it */

    /*
     * Write watch.
     *
     * Prehistorik 2 does not take a bad branch: something overwrites its
     * code, and the branch that follows is simply the first instruction to
     * read a byte that changed under it.  So the question is who writes
     * there, and the emulator is the one party that can answer it exactly -
     * every guest store passes through pstore8/16/32.
     *
     * The range is physical and is written over SWD once the level is up,
     * because the loader legitimately writes the code segment on its way in.
     * Zero in wp_lo leaves the whole thing switched off.
     */
    uint32_t wp_lo;           /* +0x38c  first physical address watched */
    uint32_t wp_hi;           /* +0x390  one past the last              */
    uint32_t wp_head;         /* +0x394  next slot in the hit ring      */
    uint32_t wp_total;        /* +0x398  hits ever, ring or not         */

    /*
     * Which code segment the near-RET trap applies to.  DOS itself
     * returns to tiny offsets during boot - COMMAND.COM does it at
     * 215F:0096 - so an unqualified trap fires long before the game
     * starts.  Written over SWD once the level is up; zero disables.
     */
    uint32_t ret_cs;          /* +0x39c  CS base the trap watches */

    /*
     * Shadow copy of a slice of guest memory.
     *
     * The write watch above only sees stores made by the emulated CPU, and
     * the corruption being chased has already happened once with that watch
     * armed and silent - so whoever writes it is not the guest's own code
     * but something else in the machine: a DMA transfer, a disk read, a
     * driver on the other core.  A shadow copy compared periodically sees
     * all of them, because it asks what changed rather than who changed it.
     *
     * shadow_base is a physical guest address, written over SWD once the
     * level is up; the next check captures the shadow and every check after
     * that compares against it.  Zero disables.
     */
    uint32_t shadow_base;     /* +0x3a0 */
    uint32_t shadow_armed;    /* +0x3a4  1 = shadow captured, comparing */
    uint32_t shadow_tick;     /* +0x3a8 */
    uint32_t shadow_addr;     /* +0x3ac  where they first differed */
    uint32_t shadow_was;      /* +0x3b0  the byte the shadow held */
    uint32_t shadow_now;      /* +0x3b4  and the byte found there */

    /*
     * Ranges the write watch ignores.
     *
     * The game keeps variables inside its own code segment - a clean run of
     * the level changes 10BB:1D42 and 10BB:2680..2695 and nothing else - so
     * watching the whole segment without these would freeze on the first
     * frame every time.  Four pairs, written over SWD, lo inclusive and hi
     * exclusive; a pair with lo == hi is unused.
     */
    uint32_t wp_skip_lo[4];   /* +0x3b8 */
    uint32_t wp_skip_hi[4];   /* +0x3c8 */

    /* Hardware data watchpoint: the physical address to watch, and whether
     * the main loop has programmed the comparator for it yet. */
    uint32_t mon_addr;        /* +0x3d8 */
    uint32_t mon_armed;       /* +0x3dc  core 0 has programmed its comparator */

    /*
     * Second round on the watchpoint.
     *
     * The first version was one shot and armed by hand over SWD once the
     * level was up, because arming earlier would have spent the single shot
     * on the loader writing the code segment in.  That made the measurement
     * a race: the byte can turn over during the second it takes to poke
     * mon_addr, and then there is nothing left to catch.
     *
     * So it stays armed instead and the handler re-arms itself, keeping the
     * last 32 hits in a ring.  A legitimate write - the loader - simply
     * shows up as a row with a PC inside the interpreter, which is the proof
     * that the instrument works at all.  Only a hit that leaves mon_bad
     * behind in the byte freezes everything.
     *
     * The comparator is per core and the DWT unit is too, so core 1 arms its
     * own; mon_c1 says it has.  That half was never covered before, and
     * video and audio both live there.
     *
     * mon_nowp is the interesting outcome: the poll below finds the byte
     * changed while mon_hits is still zero.  That would mean no instruction
     * on either core wrote it, which leaves a bus master or the memory
     * itself - and no amount of further work on the emulator would find it.
     */
    uint32_t mon_bad;         /* +0x3e0  value that means corrupted; 0 = record only */
    uint32_t mon_hits;        /* +0x3e4  watchpoint hits, ring or not */
    uint32_t mon_head;        /* +0x3e8  next slot in the hit ring */
    uint32_t mon_c1;          /* +0x3ec  1 once core 1 armed its comparator */
    uint32_t mon_polls;       /* +0x3f0  main-loop reads of the byte */
    uint32_t mon_seen;        /* +0x3f4  what the last poll read there */
    uint32_t mon_nowp;        /* +0x3f8  1 = it changed with no hit recorded */
    uint32_t mon_pad;         /* +0x3fc */

    /*
     * Freeze when the guest reaches one exact instruction.
     *
     * Supaplex does not crash: pressing a cursor key in a level makes it walk
     * back to its copy-protection screen deliberately, and the wait loop it
     * ends in is at a known address.  Nothing already here can catch that -
     * there is no exception to latch, no bad control transfer, and no write
     * to watch - so the trigger has to be the arrival itself.  The
     * instruction ring behind it then holds the sixteen thousand steps that
     * led there, which is the decision being looked for.
     *
     * trap_cs is the CS base and trap_ip the offset; both must match, and
     * zero in trap_cs disables it.  Arm it only once the level is up:
     * getting into a level goes *through* the very loop being trapped.
     */
    uint32_t trap_cs;         /* +0x400 */
    uint32_t trap_ip;         /* +0x404 */

    /* Ordered I/O trace; see frank_diag_port().  Off until armed, because it
     * is only wanted around one event and the ring is small. */
    uint32_t portlog_on;      /* +0x408 */
    uint32_t portlog_head;    /* +0x40c */
    uint32_t portlog_total;   /* +0x410 */
} FrankDiag;

#define FRANK_DIAG     ((volatile FrankDiag *)FRANK_DIAG_BASE)
#define FRANK_OPL_RING ((volatile FrankOplEvent *)(FRANK_DIAG_BASE + 0x1000u))
#define FRANK_IRQ_RING ((volatile uint32_t *)(FRANK_DIAG_BASE + 0x6000u))
#if FRANK_IP_TRACE
/*
 * The IP ring owns BASE+0x1000..0x11000, so the event ring moves into the
 * port histograms' space rather than going dark.  Having both at once is the
 * point: the Supaplex failure is a decision the guest makes, and reading it
 * needs the instruction trail *and* what the Sound Blaster was doing at that
 * instant on one timeline.  The write histogram at BASE+0x14000 is lost
 * while this is on; nothing currently reads it.
 */
/* BASE+0x11000..0x14000 is the only genuinely free gap: the IP ring ends at
 * 0x11000 and the port write histogram starts at 0x14000.  Putting the event
 * ring on top of that histogram - the first thing I tried - let
 * frank_diag_port() scribble counters through it, and the dump came back with
 * impossible rows (an interrupt "level" of 131072, event kinds that do not
 * exist).  Instrument collisions look exactly like the fault you are hunting. */
#define FRANK_EV_RING  ((volatile FrankEvent *)(FRANK_DIAG_BASE + 0x11000u))
#define FRANK_EV_N     1024u             /* 12 KB, exactly the free gap */
#else
#define FRANK_EV_RING  ((volatile FrankEvent *)(FRANK_DIAG_BASE + 0xc000u))
#define FRANK_EV_N     FRANK_EV_TRACE_N
#endif

/*
 * Every guest port access, counted per port.
 *
 * A ring is the wrong shape for a trap storm: when the guest is taking
 * thousands of faults a second, 2048 entries cover a few milliseconds and
 * say nothing about which port is being hammered.  Two 4096-entry
 * histograms answer that directly and compactly - ports below 0x1000 cover
 * the DMA controllers, the PICs, the PIT, the VGA registers and the Sound
 * Blaster, which is everything a V86 monitor traps.
 *
 * 8 KB each, at the top of the borrowed VGA aperture: 0x110b5000 and
 * 0x110b7000, ending at 0x110b9000, clear of both the event ring below and
 * the guest's own RAM above 0xC0000.
 */
#define FRANK_PORTW_HIST ((volatile uint16_t *)(FRANK_DIAG_BASE + 0x14000u))
#define FRANK_PORTR_HIST ((volatile uint16_t *)(FRANK_DIAG_BASE + 0x16000u))
#define FRANK_PORT_HIST_N 4096u

/* Control-flow trail: 512 entries of 16 bytes at BASE+0x18000, clear of the
 * two port histograms below it and of the guest's RAM above 0xC0000. */
typedef struct {
    uint32_t cs_base;
    uint32_t ip;
    uint32_t t;
    uint32_t seq;
} FrankTraceEnt;

#define FRANK_TRACE_RING ((volatile FrankTraceEnt *)(FRANK_DIAG_BASE + 0x18000u))
#define FRANK_TRACE_N    512u

/*
 * Every load of CS, with the address it was loaded from.
 *
 * The coarse trail above samples once per cpui386_step() call, so it sees
 * that the guest ended up somewhere impossible but not the transfer that
 * put it there.  CS changes only on a far jump, call, return, interrupt or
 * task switch, so hooking the one place that writes it catches every such
 * transfer exactly, with the source address still in cpu->ip.
 *
 * 512 entries of 32 bytes at BASE+0x1b000 - that is guest 0xbc000..0xc0000,
 * the last free space in the borrowed VGA aperture.  Each entry also carries
 * the stack the transfer saw, because the question a bad far transfer raises
 * is always the same: was the pointer it took already wrong in memory, or did
 * the CPU get a good pointer and land somewhere else anyway.
 */
typedef struct {
    uint32_t t;
    uint32_t from_base;   /* CS base the transfer came from */
    uint32_t from_ip;     /* and the offset within it       */
    uint32_t to_sel;      /* selector in the low half, repeat count in the high */
    uint32_t ssp;         /* SS base + SP at the transfer, linear */
    uint32_t stk0;        /* the four bytes the stack held there */
    uint32_t stk1;        /* and the four above them             */
    uint32_t flags;       /* guest flags, so V86 and IF are readable */
} FrankCsEnt;

#define FRANK_CS_RING ((volatile FrankCsEnt *)(FRANK_DIAG_BASE + 0x1b000u))
#define FRANK_CS_N    512u

static inline void frank_diag_cs(uint32_t from_base, uint32_t from_ip,
                                 uint32_t to_sel, uint32_t ssp,
                                 uint32_t flags, const uint8_t *stk)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (d->ud_hit) return;              /* frozen behind the trigger */

    /*
     * Collapse a repeat into a counter - and look back four entries, not one.
     *
     * A guest that has run off the rails does not repeat a single transfer.
     * It alternates: the invalid opcode vectors through INT 6, the handler
     * IRETs straight back, and the pair repeats forever - two distinct
     * entries that never match their immediate predecessor.  An idle guest
     * behaves the same way with a three-entry timer-tick cycle.  Comparing
     * only against the previous entry therefore collapsed nothing and the
     * ring filled with the aftermath in a few seconds, every time, which is
     * exactly why four captures in a row caught the wedge instead of the
     * transfer that caused it.  Four slots of lookback fold any cycle up to
     * that length into fixed entries and leave the history behind it intact.
     */
    for (uint32_t k = 1u; k <= 4u; k++) {
        uint32_t q = (d->cs_head + FRANK_CS_N - k) % FRANK_CS_N;
        if (d->cs_head < k) break;
        if (FRANK_CS_RING[q].from_base != from_base ||
            (FRANK_CS_RING[q].to_sel & 0xffffu) != (to_sel & 0xffffu))
            continue;
        FRANK_CS_RING[q].t = time_us_32();
        uint32_t n = FRANK_CS_RING[q].to_sel >> 16;
        if (n < 0xffffu) {
            FRANK_CS_RING[q].to_sel += 0x10000u;
        } else if (!d->ud_hit) {
            /*
             * 65535 repetitions of one transfer is not a loop the guest
             * means to run.  Freeze here too: the variants that wedge
             * without ever raising #UD leave no other trigger, and waiting
             * for a human to notice and write ud_hit over SWD took about a
             * second - long enough for the ring to lose everything.
             */
            d->ud_reason = 2u;
            d->ud_cs = from_base;
            d->ud_ip = from_ip;
            d->ud_flags = flags;
            d->ud_ssp = ssp;
            d->ud_stk0 = stk ? *(const uint32_t *)stk : 0u;
            d->ud_stk1 = stk ? *(const uint32_t *)(stk + 4) : 0u;
            d->ud_hit = 1u;
        }
        return;
    }

    uint32_t h = d->cs_head;
    FRANK_CS_RING[h].t         = time_us_32();
    FRANK_CS_RING[h].from_base = from_base;
    FRANK_CS_RING[h].from_ip   = from_ip;
    FRANK_CS_RING[h].to_sel    = to_sel;
    FRANK_CS_RING[h].ssp       = ssp;
    FRANK_CS_RING[h].stk0      = stk ? *(const uint32_t *)stk : 0u;
    FRANK_CS_RING[h].stk1      = stk ? *(const uint32_t *)(stk + 4) : 0u;
    FRANK_CS_RING[h].flags     = flags;
    d->cs_head = (h + 1u) % FRANK_CS_N;
}

typedef struct {
    uint32_t addr;      /* physical address written */
    uint32_t val;       /* the value, as wide as the store was */
    uint32_t cs_base;   /* and the guest instruction that did it */
    uint32_t ip;
} FrankWpEnt;

#define FRANK_WP_RING ((volatile FrankWpEnt *)(FRANK_DIAG_BASE + 0x11000u))
#define FRANK_WP_N    64u

static inline void frank_diag_wp(uint32_t addr, uint32_t val,
                                 uint32_t cs_base, uint32_t ip)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (!d->wp_lo || addr < d->wp_lo || addr >= d->wp_hi) return;
    for (uint32_t k = 0; k < 4u; k++)
        if (d->wp_skip_lo[k] != d->wp_skip_hi[k] &&
            addr >= d->wp_skip_lo[k] && addr < d->wp_skip_hi[k]) return;

    uint32_t h = d->wp_head;
    if (h < FRANK_WP_N) {
        FRANK_WP_RING[h].addr    = addr;
        FRANK_WP_RING[h].val     = val;
        FRANK_WP_RING[h].cs_base = cs_base;
        FRANK_WP_RING[h].ip      = ip;
        d->wp_head = h + 1u;
    }
    d->wp_total++;

    /* Freeze on the first one: the instruction ring behind it is the whole
     * point, and it is only worth having for the write that came first. */
    if (!d->ud_hit) {
        d->ud_reason = 3u;
        d->ud_cs = cs_base;
        d->ud_ip = ip;
        d->ud_hit = 1u;
    }
}

/*
 * A near return that lands on offset zero.
 *
 * That is how Prehistorik 2 dies: a subroutine pops its four saved registers
 * cleanly and then RET takes the guest to offset 0x0000 of its own code
 * segment, where the data there decodes as a far call and control leaves for
 * good.  The interesting question is whether the stack slot held the wrong
 * value or SP pointed at the wrong slot, so this keeps 64 bytes centred on
 * the stack pointer: the four pushed registers should be sitting right above
 * the return address, and if they are, SP was fine and something overwrote
 * the slot.
 */
#define FRANK_RET_DUMP ((volatile uint32_t *)(FRANK_DIAG_BASE + 0x11400u))

static inline void frank_diag_ret(uint32_t newip, uint32_t cs_base,
                                  uint32_t ip, uint32_t ssp,
                                  const uint8_t *stk)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (newip >= 0x10u || d->ud_hit) return;
    if (!d->ret_cs || cs_base != d->ret_cs) return;
    d->ud_reason = 4u;
    d->ud_cs = cs_base;
    d->ud_ip = ip;
    d->ud_ssp = ssp;
    d->ud_stk0 = newip;
    if (stk) {
        const uint32_t *w = (const uint32_t *)stk;   /* ssp - 32 */
        for (int i = 0; i < 16; i++) FRANK_RET_DUMP[i] = w[i];
    }
    d->ud_hit = 1u;
}

#define FRANK_SHADOW      ((volatile uint8_t *)(FRANK_DIAG_BASE + 0x11800u))
/*
 * 7 KB, sized and placed by measurement rather than guesswork: a clean run
 * of the level changes exactly six bytes of the game's code segment, all of
 * them variables the game keeps at 10BB:1D42 and 10BB:2680..2695, so the
 * window runs from 10BB:0100 up to just below the first of them.  Every
 * other byte in it is code that nothing should ever touch.
 */
#define FRANK_SHADOW_N    7168u

static inline void frank_diag_shadow(const uint8_t *phys_mem)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (!d->shadow_base || d->ud_hit) return;

    /* Once every 8192 instructions: 2 KB compared per 8192 steps costs a
     * eighth of a byte per instruction, which does not move g_mips. */
    if ((++d->shadow_tick & 0x3fffu) != 0u) return;

    const uint8_t *p = phys_mem + d->shadow_base;
    if (!d->shadow_armed) {
        for (uint32_t i = 0; i < FRANK_SHADOW_N; i++) FRANK_SHADOW[i] = p[i];
        d->shadow_armed = 1u;
        return;
    }
    for (uint32_t i = 0; i < FRANK_SHADOW_N; i++) {
        if (FRANK_SHADOW[i] == p[i]) continue;
        d->shadow_addr = d->shadow_base + i;
        d->shadow_was = FRANK_SHADOW[i];
        d->shadow_now = p[i];
        d->ud_reason = 5u;
        d->ud_hit = 1u;
        return;
    }
}

/* Block writes - a string I/O read, a DMA burst - reported as one hit on the
 * first byte that falls inside the window. */
static inline void frank_diag_wp_range(uint32_t lo, uint32_t len,
                                       uint32_t cs_base, uint32_t ip)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (!d->wp_lo || lo + len <= d->wp_lo || lo >= d->wp_hi) return;
    frank_diag_wp(lo < d->wp_lo ? d->wp_lo : lo, len, cs_base, ip);
}

static inline void frank_diag_ip(uint32_t ip, uint32_t cs_base, uint32_t sp)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (d->ud_hit) return;              /* frozen behind the trigger */

    /* Arrival at one exact instruction, for a fault that is a decision
     * rather than a crash.  Checked before the ring is written so the
     * trapped instruction is the last thing in it. */
    if (d->trap_cs && cs_base == d->trap_cs && ip == d->trap_ip) {
        d->ud_reason = 9u;
        d->ud_cs = cs_base;
        d->ud_ip = ip;
        d->ud_hit = 1u;
        return;
    }

    uint32_t h = d->ip_head;

    /*
     * An offset alone is ambiguous, and the ambiguity is not academic: the
     * first reading of this ring was disassembled against the wrong segment
     * and produced instruction boundaries that did not exist.  A change of
     * code segment therefore writes a two-word marker - 0xffff, then the
     * segment - and IP 0xffff itself is given the same treatment so the
     * reader never mistakes one for the other.
     */
    if (cs_base != d->ip_last_cs) {
        d->ip_last_cs = cs_base;
        FRANK_IP_RING[h] = 0xffffffffu;
        h = (h + 1u) & FRANK_IP_MASK;
        FRANK_IP_RING[h] = cs_base;
        h = (h + 1u) & FRANK_IP_MASK;
    }

    /*
     * SP rides along in the top half.  The failure this is chasing is a
     * stack pointer that ends up two bytes high while the stack contents
     * are intact, and an offset-only trace cannot show which instruction
     * failed to move it; beside every instruction, the drift is obvious.
     */
    FRANK_IP_RING[h] = (ip & 0xffffu) | (sp << 16);
    d->ip_head = (h + 1u) & FRANK_IP_MASK;
}

static inline void frank_diag_trace(uint32_t cs_base, uint32_t ip)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (d->ud_hit) return;                      /* frozen behind the fault */
    if (cs_base == d->trace_last_cs) return;    /* same segment, nothing new */
    d->trace_last_cs = cs_base;
    uint32_t h = d->trace_head;
    FRANK_TRACE_RING[h].cs_base = cs_base;
    FRANK_TRACE_RING[h].ip = ip;
    FRANK_TRACE_RING[h].t = time_us_32();
    FRANK_TRACE_RING[h].seq = h;
    d->trace_head = (h + 1u) % FRANK_TRACE_N;
}

static inline void frank_diag_exc(uint32_t cs_base, uint32_t ip, uint32_t no,
                                  uint32_t err, uint32_t flags)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (no < 32u) d->exc_hist[no]++;
    /*
     * Latch only on #UD.  Ordinary DOS boot raises exceptions the emulator
     * handles perfectly well - a general protection fault on a trapped I/O
     * under EMM386, a page fault - and latching on the first of those would
     * freeze the trail long before the game ever runs.  Six is the one that
     * wedges the machine, so six is the one that stops the recorder.
     */
    if (no != 6u || d->ud_hit) return;
    d->ud_reason = 1u;
    d->ud_cs = cs_base;
    d->ud_ip = ip;
    d->ud_excno = no;
    d->ud_excerr = err;
    d->ud_flags = flags;
    d->ud_hit = 1u;
}

/*
 * A ring of the I/O accesses that matter, in order.
 *
 * The histograms say a port was touched forty thousand times but not when or
 * in what order, and the Sound Blaster event ring only sees 0x22x.  Chasing
 * why Supaplex's driver gives up needs the sequence: which register it polls,
 * what it does between polls, and what it stops doing.
 *
 * The filter is what makes it fit.  Everything the guest hammers for its own
 * reasons - the VGA status port, the PIT, the IDE data register - is left
 * out, and what remains is the DMA controller (0x00-0x1f, including the
 * 8237's own alias), its page registers, both interrupt controllers and the
 * Sound Blaster.  1024 entries of 8 bytes take the read histogram's space,
 * which nothing reads any more.
 */
typedef struct {
    uint32_t t;
    uint16_t port;
    uint16_t flags;      /* bit 0: write */
} FrankPortEnt;

#define FRANK_PORTLOG      ((volatile FrankPortEnt *)(FRANK_DIAG_BASE + 0x16000u))
#define FRANK_PORTLOG_N    1024u

static inline int frank_port_of_interest(uint32_t port)
{
    return port <= 0x1fu                       /* DMA 1 and its alias      */
        || (port >= 0x80u && port <= 0x8fu)    /* DMA page registers       */
        || port == 0x20u || port == 0x21u      /* master PIC               */
        || port == 0xa0u || port == 0xa1u      /* slave PIC                */
        || (port >= 0x220u && port <= 0x22fu); /* Sound Blaster            */
}

static inline void frank_diag_port(uint32_t port, int is_write)
{
    if (port < FRANK_PORT_HIST_N) {
        volatile uint16_t *h = is_write ? FRANK_PORTW_HIST : FRANK_PORTR_HIST;
        if (h[port] != 0xffffu) h[port]++;
    }

    volatile FrankDiag *d = FRANK_DIAG;
    if (d->ud_hit || !d->portlog_on || !frank_port_of_interest(port)) return;

    uint32_t h2 = d->portlog_head;
    /* Collapse a spin: a driver polling one register writes the same row
     * thousands of times and pushes everything else out of the ring. */
    if (h2) {
        uint32_t prev = (h2 - 1u) % FRANK_PORTLOG_N;
        if (FRANK_PORTLOG[prev].port == (uint16_t)port &&
            (FRANK_PORTLOG[prev].flags & 1u) == (uint32_t)(is_write != 0)) {
            FRANK_PORTLOG[prev].flags += 2u;   /* repeat count in bits 15..1 */
            FRANK_PORTLOG[prev].t = time_us_32();
            d->portlog_total++;
            return;
        }
    }
    FRANK_PORTLOG[h2 % FRANK_PORTLOG_N].t = time_us_32();
    FRANK_PORTLOG[h2 % FRANK_PORTLOG_N].port = (uint16_t)port;
    FRANK_PORTLOG[h2 % FRANK_PORTLOG_N].flags = (uint16_t)(is_write ? 1u : 0u);
    d->portlog_head = h2 + 1u;
    d->portlog_total++;
}

/* DWT / DEMCR, ARMv8-M debug block. The counter is per-core and everything
 * timed here runs on core 0. */
#define FRANK_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define FRANK_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define FRANK_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

static inline void frank_diag_arm(void)
{
    FRANK_DIAG->magic = FRANK_DIAG_MAGIC;
    FRANK_DEMCR |= (1u << 24);          /* TRCENA */
    FRANK_DWT_CYCCNT = 0;
    FRANK_DWT_CTRL |= (1u << 0);        /* CYCCNTENA */
}

/* Differences are taken in uint32 so a wrap inside one interval is still
 * right; the accumulators are 64-bit so the totals are too. */
#define FRANK_PROF_T(v)       uint32_t v = FRANK_DWT_CYCCNT
#define FRANK_PROF_ADD(v, f)  FRANK_DIAG->f += (uint32_t)(FRANK_DWT_CYCCNT - (v))

static inline void frank_diag_opl_write(uint32_t port, uint8_t reg,
                                        uint8_t val, int is_data)
{
    volatile FrankDiag *d = FRANK_DIAG;
    uint32_t now = time_us_32();

    if (is_data) {
        d->opl_data++;
        d->reghist[reg]++;
        uint32_t gap = now - d->last_opl_us;
        d->last_opl_us = now;
        /* log2 bucket, so a steady 8 kHz stream lands in one column and a
         * bursty one spreads across several */
        uint32_t b = 0;
        while (gap >>= 1) b++;
        if (b > 23) b = 23;
        d->opl_gap[b]++;
    } else {
        d->opl_sel++;
    }

#if !FRANK_IP_TRACE
    uint32_t h = d->opl_head;
    FRANK_OPL_RING[h].t    = now;
    FRANK_OPL_RING[h].port = (uint16_t)port;
    FRANK_OPL_RING[h].reg  = reg;
    FRANK_OPL_RING[h].val  = val;
    d->opl_head = (h + 1u) % FRANK_OPL_TRACE_N;
#endif
}

static inline void frank_diag_irq0(void)
{
    volatile FrankDiag *d = FRANK_DIAG;
#if !FRANK_IP_TRACE
    uint32_t h = d->irq_head;
    FRANK_IRQ_RING[h] = time_us_32();
    d->irq_head = (h + 1u) % FRANK_IRQ_TRACE_N;
#endif
    d->irq0_edges++;
}

static inline void frank_diag_ev(uint8_t kind, uint8_t a, uint16_t b,
                                 uint32_t c)
{
    volatile FrankDiag *d = FRANK_DIAG;
    if (d->ud_hit) return;              /* frozen with everything else */
    uint32_t h = d->ev_head;

    /*
     * Collapse a spin loop into a single row.  A driver waiting on the DSP
     * status port issues forty thousand identical reads a second, which
     * pushes everything that matters out of the ring within milliseconds -
     * the first capture of Tyrian's failure held 2013 reads of 0x22c and
     * exactly one of everything else.  Repeats of the same port and value
     * just bump a counter in b.
     */
    if (kind == FRANK_EV_DSP_R || kind == FRANK_EV_DSP_W) {
        uint32_t p = (h + FRANK_EV_N - 1u) % FRANK_EV_N;
        if (d->ev_total && FRANK_EV_RING[p].kind == kind &&
            FRANK_EV_RING[p].a == a && FRANK_EV_RING[p].c == c) {
            FRANK_EV_RING[p].t = time_us_32();
            if (FRANK_EV_RING[p].b != 0xffffu)
                FRANK_EV_RING[p].b++;
            d->ev_total++;
            return;
        }
    }

    /* Freeze keeps the beginning of a sequence instead of the end, which is
     * what a detection handshake needs: clear ev_head/ev_total over SWD, set
     * this, and the ring holds the first thing the guest did. */
    if (d->ev_freeze && h >= FRANK_EV_N - 1u)
        return;

    FRANK_EV_RING[h].t    = time_us_32();
    FRANK_EV_RING[h].kind = kind;
    FRANK_EV_RING[h].a    = a;
    FRANK_EV_RING[h].b    = b;
    FRANK_EV_RING[h].c    = c;
    d->ev_head = (h + 1u) % FRANK_EV_N;
    d->ev_total++;
}

/*
 * The two hooks that cost SRAM, separated so they can be left out.
 *
 * Everything else here lives in PSRAM and is nearly free, but these two are
 * inlined into code that must itself live in SRAM: frank_diag_wp() sits in
 * pstore8/16/32 and inflates store32 into a 3.5 KB RAM-resident function of
 * its own, and frank_diag_ip() sits in the interpreter loop and grows
 * cpu_exec1.  Together they are about 8 KB, which is exactly what stops the
 * native JIT and the diagnostics fitting in SRAM at the same time - and
 * pc_new() then fails its allocation and the board comes up to a black
 * screen.
 *
 * With FRANK_DIAG_HOT set to 0 the control-flow ring, the port trace, the
 * traps, the counters and the display state all remain, which is what
 * graphics work needs; only the per-store watch and the per-instruction
 * trail go away.  Turn it back on when chasing something that needs them.
 */
#ifndef FRANK_DIAG_HOT
#define FRANK_DIAG_HOT 1
#endif
#if !FRANK_DIAG_HOT
#define frank_diag_wp(addr, val, cs, ip)      do { } while (0)
#define frank_diag_wp_range(lo, len, cs, ip)  do { } while (0)
#define frank_diag_ip(ip, cs, sp)             do { } while (0)
#define frank_diag_trace(cs_base, ip)         do { } while (0)
#define frank_diag_shadow(mem)                do { } while (0)
#endif

#define FRANK_DIAG_COUNT(field)  do { FRANK_DIAG->field++; } while (0)
#define FRANK_DIAG_SET(field, v) do { FRANK_DIAG->field = (v); } while (0)

#else /* !FRANK_AUDIO_DIAG */

#define frank_diag_arm()                              do { } while (0)
#define frank_diag_opl_write(port, reg, val, is_data) do { } while (0)
#define frank_diag_irq0()                             do { } while (0)
#define frank_diag_ev(kind, a, b, c)                  do { } while (0)
#define frank_diag_port(port, is_write)               do { } while (0)
#define frank_diag_trace(cs_base, ip)                 do { } while (0)
#define frank_diag_cs(from_base, from_ip, to_sel, ssp, flags, stk) \
                                                      do { } while (0)
#define frank_diag_exc(cs, ip, no, err, flags)        do { } while (0)
#define frank_diag_ip(ip, cs, sp)                     do { } while (0)
#define frank_diag_ret(nip, cs, ip, ssp, stk)         do { } while (0)
#define frank_diag_shadow(pm)                         do { } while (0)
#define frank_diag_wp(addr, val, cs, ip)              do { } while (0)
#define frank_diag_wp_range(lo, len, cs, ip)          do { } while (0)
#define FRANK_PROF_T(v)                               do { } while (0)
#define FRANK_PROF_ADD(v, f)                          do { } while (0)
#define FRANK_DIAG_COUNT(field)                       do { } while (0)
#define FRANK_DIAG_SET(field, v)                      do { } while (0)

#endif /* FRANK_AUDIO_DIAG */

#endif /* AUDIODIAG_H */
