# Advisory: unbounded guest memory access (pstore/pload)

NOT a patch. This is an emulator-core decision that needs your call on the
perf/safety tradeoff; I did not touch the hot path unilaterally.

## The exposure
`CHECK_RAM_BOARDER_ENABLED` is `0` (src/mem.h:15), so EVERY bound check in the
memory layer is compiled out. With it off:

    pstore8(addr, val):  PC_RAM[addr] = val;   // addr is raw uint32_t, no clamp

and in i386.c the store path is `pstore8(res->addr1, val)` where, with paging
off, `res->addr1 = laddr` is the guest's raw 32-bit linear address with NO mask
to phys_mem_size (i386.c ~749). So a 386 guest in protected/unreal/paged mode
can read or write `0x11000000 + addr` for any 32-bit addr - up to ~4 GB past
the 6-8 MB PC_RAM buffer. That is arbitrary host memory corruption from guest
code, not just guest-visible breakage.

Correction to my own earlier note (stage4a): I claimed the whole-PSRAM check
was a backstop under the XMS MOVE bound. It is NOT - it is compiled out. The
stage4a 1 MB bound on XMS real-mode moves is therefore load-bearing, not
defence-in-depth. Every OTHER path into pstore/pload is still unbounded.

## Why it is off (best understanding)
- It sits on the CPU core's hottest path (every memory-touching instruction).
- phys_mem_size is a runtime global, and PSRAM can be 6 MB (NOT a power of
  two), so it cannot be reduced to a cheap `addr & MASK` in general - it is a
  compare-and-branch against a global on every access.

## Options, cheapest first
1. Do nothing at the core; rely on per-feature bounds (like stage4a's XMS one).
   Cheapest, but leaves the general hole open - anything that reaches pstore
   with an attacker-controlled address is exposed.
2. Enable CHECK_RAM_BOARDER_ENABLED only for the STRING/block ops
   (rep movs/stos, XMS/EMS block moves) - the paths that move attacker-chosen
   lengths. These are already not the per-byte hot path, so the compare is
   negligible there. Leaves single-access instructions unchecked (a guest can
   still poke one byte out of range, but not sweep memory).
3. Force phys_mem_size to a power of two for the mask (round 6 MB up to 8 MB of
   reserved address space, or down to 4 MB of usable), then mask unconditionally
   with `addr &= (phys_mem_size - 1)`. One AND, no branch, on every access.
   Changes usable RAM sizing - a board-config decision.
4. Enable it globally as-is (compare+branch everywhere). Simplest, safest,
   measurable perf cost on the emulator's hottest path - measure before shipping.

## Recommendation
Option 2 as the immediate safety win (covers the memory-SWEEP primitives, which
are what turn a bug into an exploit, at ~zero cost), and treat 3 vs 4 as a
separate perf-measured decision if single-access checking is wanted too.

## Refinement after tracing i386.c
The string-op IO-accelerator path (i386.c ~2974/3038, rep ins/outs to an IO
callback) ALREADY bounds `memld.addr1/memls.addr1` against phys_mem_size
UNCONDITIONALLY - those two checks are not under the #if. So the accelerated
device-string path is safe.

The unbounded paths are:
  - the per-access store8/store16/store32 / load* used by ordinary
    instructions (i386.c store8 -> pstore8(res->addr1) with no check), and
  - the FALLBACK per-element loop of rep movs/stos when the accelerator does
    not apply (plain guest RAM -> guest RAM moves).

So option 2 is narrower than first stated: the device-IO sweep is already
covered; what remains open is (a) single ordinary accesses and (b) RAM-to-RAM
rep movs/stos. (b) is the real memory-sweep primitive and the priority.

This is a CPU-core change on the emulator's hottest path with a real perf
tradeoff and 6-vs-8 MB (non-power-of-two) sizing implications. It should be
made by the core author with a benchmark, not blindly enabled. Flagging, not
patching.
