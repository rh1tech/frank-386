# tiny386 / frank-386 — performance handoff (2026-09-04, Claude)

Written so Codex or another session can take over mid-stream. Everything here
is measured on the attached Z2 board unless marked as an estimate.

## Board and tree state right now

- **Board (final user decision, 2026-09-04)**: `frank-386-opl-block8.elf`,
  SHA-256
  `E1CF222B8524DB5CBBB29827C15EA4A47EA9814AEF29F1F1137F1A891E5ACF93`.
  It was flashed again after the core-1 OPL experiment and verified by
  OpenOCD. Leave this image on the board unless a new experiment is explicitly
  requested.
- **Rebuilt final ELF**: `C:/Users/janbr/Documents/Codex/2026-08-31/zkous-3/frank-386-aladdin-fastprefetch-final.elf`,
  SHA-256 `00E7CD88013BF6ED511F1D58CAAAC9A0DADCCFBD17E46BD8C5CECE7668620310`.
  **The board now has `frank-386-opl-block8.elf`**, SHA-256
  `E1CF222B8524DB5CBBB29827C15EA4A47EA9814AEF29F1F1137F1A891E5ACF93`. It is
  that build plus the two Dune II
  fixes, the `translate()` fast path and the OPL block drain - i.e. everything
  from this session that measured better and nothing that did not. RAM 476,324 B
  (90.85%); the whole session cost 16 bytes of SRAM. The preceding test link
  `frank-386-aladdin-fastprefetch-v2.elf` (SHA-256
  `DCEC1884078F91BA50762A13C90A7B5BFA3C559D57D5FBA7F46564308EE50D5F`) has the
  same code/config as the "final" ELF; later source edits only improved
  comments.
- **Hardware validation**: Aladdin (boot option 3 / EMS) passes the Disney logo
  and the user confirms the v2 run is dramatically faster. Prehistorik 1 and 2
  have since passed their regression pass on this exact build. The previous fully
  validated fallback remains `frank-386-perf-nocore1-final.elf`, SHA-256
  `651B28E902A0F13D6913817016A52ECA6239A257FBD277922AA60BFB5B3C7986`.
- **Tree**: Codex's two DRACIHIS changes (`refresh_flags` mask guard,
  `in_iomem` reordering) + everything below except the unsafe true-core-1
  audio alarm pool.
- Useful ELFs kept in `build/`: `dh-core1timer.elf` (no shared guard, known
  good), `guard-sets3.elf` (shared guard + debug checks),
  `prefetch32b.elf` (equivalent to what is flashed).
- Unproven but kept: the conditional PTE dirty-bit store in
  `translate_lpgno()`. Semantically identical, measured neutral (162.5 and
  ~161 s against 159.8 / 160.4). Revert it freely if it is in the way.

## REJECTED: OPL synthesis on core 1 (Codex follow-up)

This was a measured experiment, not a new baseline. Its conditional source,
CMake option, lock and I2S FIFO were removed again before the public checkpoint
was committed. Do not recreate it for a release build without redesigning the
audio scheduling. The exact stable ELF is the preserved artifact named above.

Measured Doom results, always after a clean boot:

| build | wall | MIPS | result |
|---|---:|---:|---|
| stable `opl-block8` | 162.5 s | 1.752 | accepted baseline |
| core-1 OPL, one-frame DMA | 148.7 s | 1.899 | music and SFX stuttered |
| core-1 OPL, 8-frame DMA/ring | 147.9 s | 1.909 | still sounded worse |
| core-1 OPL, 8-frame DMA + exact 44.1 kHz timer | 148.1 s | 1.904 | about +8.9%, but user ultimately heard distortion and almost no useful difference in games |

Important findings:

- Moving the mixer itself to core 1 is wrong; it starves the time-critical I2S
  path and degrades both OPL music and SFX.
- The original `-1000000 / 44100` repeating-timer delay truncates to 22 us,
  i.e. 45,454.5 Hz. A 298x23 us + 143x22 us schedule per 441 callbacks gives
  the exact 44.1 kHz average and reduced measured OPL ring underruns from
  155,640 to 968 in the Doom run.
- Despite that correction, the direct listening comparison won: the user
  chose `opl-block8`; the synthetic +8.9% Doom result was not worth the audio
  regression and was barely noticeable in actual games.
- Experimental artifacts retained for diagnosis only:
  `frank-386-opl-core1-dmaring8.elf` SHA-256
  `88ABC88FE4A3951FCE5415F912646891E9573AA95C2EAC80A77F142E03103370` and
  `frank-386-opl-core1-dmaring8-exact44k.elf` SHA-256
  `D134B846D89567CC060A843FB678B72441F7A3E9B57246881A3710B43B5E42D6`.
- The experiment used a cross-core atomic lock around OPL production/register
  writes. A future attempt would need a timestamped register-command queue
  with sole OPL ownership on core 1, plus buffered I2S with no starvation.
  Do not simply re-enable the current experiment.

## Start here: the arena in PSRAM

This is the agreed next task and nothing of it has been started.

Why: the arena is `nj_code[NJ_CODE_BYTES/2]` in `.bss`, 8 KB, set by
`NJIT_CODE_KB` in `CMakeLists.txt:451`. Generated code runs about 50 bytes per
guest instruction, so it holds roughly **160 guest instructions** - smaller
than any game's hot loop, which is why blocks are evicted and recompiled and
why making them bigger cost 23% on Doom. SRAM has nothing left to give (the
VGA buffer was tried; see below), so the arena has to move to PSRAM and be
executed through the XIP cache, exactly as flash code already is.

What to know before starting:

- Guest physical memory is PSRAM at host `0x11000000`; `phys_mem_size` is
  8 MiB. The existing diagnostic buffers live in the hole the guest never
  touches - the VGA aperture at guest `0xa0000..0xbffff` (`NJ_V6_STOP` is at
  `0x110a8000`). That hole is only 128 KB and `pc_new()` clears guest RAM on
  every boot, so **the arena needs its own reservation, not that hole.**
  Shrinking `EMU_MEM_SIZE_MB` from 8 and putting the arena above the guest's
  memory is the clean way.
- **The hazard is cache coherency, not addressing.** After writing generated
  code, the XIP cache lines covering it must be invalidated or the core
  executes a stale line. The SDK exposes this through `hardware/xip_cache`
  (`xip_cache_clean_range` / `xip_cache_invalidate_range` in SDK 2.2.0); the
  existing `dsb sy / isb sy` in `nj_compile_*` is **not** sufficient once the
  code lives behind a cache. This failure mode looks like "it crashes
  sometimes", so build with `-DFAULT_PARK=ON` from the first flash: it stops
  the core in the fault instead of rebooting over the evidence.
- `nj_compact_code()` moves finished blocks. Anything the generated code
  branches to outside itself must materialise the address (`MOVW/MOVT` +
  `BLX`), never a PC-relative `BL` - see the traps section.
- Expect the first attempt to boot-loop. Recovery is
  `openocd ... -c "adapter speed 2000" -c init -c "reset halt" -c "program
  <known-good.elf> verify" -c "reset run"`, repeated if it times out, with a
  **Windows-style** path (OpenOCD does not understand `/c/...`).

After the arena, in order: the opcodes the compiler still refuses (`02` and the
other byte ALU forms, `ac` LODSB, `0F B6` MOVZX, `c6`, and `JGE`-class back
edges in `nj_decode_backedge`), and only then native block linking.

## REGRESSIONS RESOLVED

Hardware bisecting isolated both real regressions to the audio mixer running
from a true core-1 alarm pool:

1. **Prehistorik 1** crashed at startup with glyph garbage.
2. **Prehistorik 2** produced a loud hum until the level-select menu.

`dh-core1timer.elf`, which had neither the shared JIT guard nor the 32-byte
prefetch, reproduced them. The clean Codex baseline fixed both. Rebuilding the
complete optimized tree while changing only the mixer registration back to
`add_repeating_timer_us()` also fixed both, proving the JIT guard and prefetch
innocent. Do not restore a true core-1 mixer until SB16, PC-speaker and Covox
state have an audited cross-core handoff.

**Aladdin was not frozen.** It eventually passes the Disney logo; that sequence
is simply extremely slow. On the new stable optimized build a live 15-second
window measured 2.106 MIPS and 2.1% native coverage, versus 2.013 MIPS and 0.5%
in an earlier clean-baseline window (scene variance applies). The user also
reported it subjectively a little faster.

## ALADDIN INTERPRETER FETCH OPTIMIZATION (validated 2026-09-04)

Aladdin is not specifically an EGA problem. It uses a VGA planar/Mode-X-like
path and spends almost all its time in the interpreter. It must be tested with
**boot-menu option 3 (EMS/EMM386)**.

A no-JIT, 32-byte-bucket `PC_SAMPLE` run over the first graphics scene produced
424,104 samples (5.15% outside the 256 KB SRAM window). The two adjacent
`peek8_slow` buckets alone were 40,404 (9.53%) and 26,090 (6.15%) samples:

- `0x20001740..175f`: function entry, CS:IP linear address, ifetch page-tag
  check and beginning of the prefetch check;
- `0x20001760..177f`: prefetch hit compare/load/return;
- `sb16_getsample` was 7.10%; `cpu_exec1`, address translation and `modsib`
  made up most of the rest.

The accepted change in `src/i386.c` has two parts:

1. Test the linear-address `PREFETCH_HIT` before checking `ifetch.laddr`. This
   is safe for the same reason as the existing inlined `FAST_FETCH` path: a
   32-byte line is tagged by linear address and cannot cross a 4 KB page.
2. Keep that hit path in small `peek8_slow`, but force refill/page-walk handling
   into a `noinline` `peek8_miss`. The miss helper deliberately recomputes
   CS:IP once per 32-byte refill; passing `laddr` caused GCC to retain another
   callee-saved register on every byte fetch. The resulting hot wrapper saves
   two registers instead of six plus a local stack allocation.

This also reduced linked RAM from 480,404 B to 476,308 B (91.63% -> 90.85%).
It does **not** enable full `FAST_FETCH`, whose hundreds of inline copies cost
about 12 KB.

Controlled 20-second A/B in the same opening game scene:

| build | mean MIPS | median | min | samples below 70% median |
|---|---:|---:|---:|---:|
| v1: fast-first check, miss still in same function | 1.725 | 1.843 | 1.070 | 8/200 |
| v2: separate noinline miss helper | 1.947 | 1.904 | 1.753 | 0/199 |

Mean gain is **12.9%**, minimum interval rate improves **64%**, and the
0.8-second collapse around 1.1 MIPS disappears. A separate 15-second v2 window
measured 1.940 MIPS with only 2.4% JIT coverage, confirming that the gain is in
the interpreter. The user reports the visible game speed improvement as much
larger than the average-MIPS change because v2 removes the long stalls.

Profiler memory note: `PC_SAMPLE_SHIFT` is now a CMake cache setting. Working
sizes on Z2 were shift 11 = 2 KB buckets / 512-byte histogram, shift 7 = 128 B
/ 8 KB, and (only with `NATIVE_JIT=OFF`) shift 5 = 32 B / 32 KB. A 1 KB
histogram already left too little heap for `pc_new()` in the full JIT build.

### v2 regression check and the next target

Both regressions listed above were re-tested on the v2 build and are gone:
**Prehistorik 1 and Prehistorik 2 both pass on v2** - Prehistorik 1 starts
without the glyph garbage and Prehistorik 2 no longer hums up to the
level-select menu. Together with Aladdin, that is the accepted validation set
for the fast-prefetch change; nothing else regressed on it.

The next profiling target is **Draci historie (DRACIHIS)**. It is the scene the
"5% JIT coverage" number was measured in, so it is the workload that should say
whether the interpreter fetch path still dominates after v2, or whether the
remaining time has moved somewhere else.

## DUNE II: TWO BUGS FOUND AND FIXED (2026-09-04, validated on hardware)

Both were reported by the user against a real PC and both are now confirmed
fixed by the user on the board. They are unrelated to each other and to the
JIT, but each is the kind that survives for a long time because it looks like
"that game is just weird".

### 1. Four palette indices were unreachable on HDMI

`hdmi.c` reserves four byte values - 0xd4..0xd7 - because pixels and HDMI
control symbols index the same `conv_color[]` table. The renderer substituted
`c ^ 8` for them, which is sound reasoning about a *pair* of 4-bit colours and
nonsense in a 256-colour mode, where the byte is the palette index itself:
212..215 came out as whatever the guest happened to have at 220..223.

Dune II draws the Westwood logo **entirely** in indices 212..215. Measured off
the board: index 215 alone is 3964 of the logo's 10414 non-black pixels and
its colour is #0f38bf blue, while 223 held #ffffff. The logo rendered white.
The sparkle is an animation of those same four entries, so it was invisible.
The intro's throne room turned dark purple shadows into #9f9f9f grey speckles
(2517 pixels in one frame).

The fix chooses the substitute **by colour**: `vga_hw_set_palette()` and the
two pair-table setters call the new `hdmi_set_pixel_substitutes()` with, for
each reserved index, the nearest non-reserved entry. Palettes commonly hold
the same colour twice - Dune II's holds that blue ramp again at 227, 229 and
231 - so three of the four substitutions are *exact* and the fourth is one
step off. The search is 4 x 256 with an early exit on an exact match, next to
the ~1 ms of TMDS encoding the same function already does.

How it was diagnosed, because the method generalises: `claude_handoff/
swdshot.py` (new) renders the guest's screen out of `gfx_buffer` **with the
guest's own DAC**, read through the `vga_state` pointer, rather than the stock
mode-13h palette `swdgfx.py` substitutes. That splits the question in half - a
correct picture from it with a wrong picture on the monitor puts the fault
downstream in the HDMI encoder, which is exactly what happened.

### 2. The DMA current-count register never reached terminal count

Symptom: Dune II says "Dune" and then no digitised speech for the rest of the
session, while FM music and effects keep playing.

`SB_read_DMA()` ended with `dma_pos = (dma_pos + written) % dma_len`. A
single-cycle Sound Blaster block always ends exactly at the end of the buffer
- the DSP block size and the DMA count are programmed to the same length - so
that modulo folded the finished position back to 0. `i8257_channel_run()`
stores it in `regs[n].now[COUNT]`, so:

- `i8257_read_chan()` reported the guest's current-count register as
  `base[COUNT] - now[COUNT]` = the full block instead of 0xffff, and
- its `n == (base[COUNT]+1) << ncont` test never matched, so the terminal-count
  status bit was never set either.

Dune II's IRQ5 handler (found at 51BD:061F over SWD) decides whether the
interrupt is its own block finishing by reading that count register and
comparing against 0xffff. Getting 0x3b80 back it returns without reading
base+0x0e, so the card's interrupt line stays asserted - and the 8259 is edge
triggered, so no further Sound Blaster interrupt can ever be delivered. This is
the same trap the Supaplex comment in `reset()` describes, reached by a
different route.

The fix leaves a transfer that finishes exactly at the end of the buffer *at*
the end, and wraps on the way into the next pass instead, which is what
auto-init needs. Verified live: `now[COUNT]` now reads `base[COUNT] + 1`, so
the guest reads 0xffff, and the driver streams 4690-byte blocks back to back.

Evidence trail worth keeping: the `AUDIO_DIAG=ON AUDIO_DIAG_HOT=OFF` build
(RAM 91.63%, boots fine) plus the event ring at `FRANK_DIAG_BASE+0x11000`
showed every DSP command, DMA register write and IRQ edge on one timeline -
the probe's 4-byte block acknowledged 117 us after its interrupt, and the
15233-byte speech block interrupting at +990 ms with no acknowledge ever. Do
not set `CMAKE_C_FLAGS` on the command line to add a define; it replaces the
SDK's `-mcpu`/`-mthumb` and the build fails in confusing ways.

## DIRECTION, RE-DECIDED WITH A MEASUREMENT (2026-09-04, later)

The "arena in PSRAM" section below is still accurate about *how* to do it, but
it should no longer be the next task, and the reason is in this document
already: the JIT executes about 5% of guest instructions, so all of it is
capped at 5% of runtime. The interpreter is where tens of percent have to come
from.

So the interpreter's memory path was attacked first, and the result is worth
recording because it **closes a plausible-looking direction**.

### Baseline discipline

`claude_handoff/doombench.py <elf> <label>` from a board reset, always. A
second launch inside the same DOS session is not comparable, and a hand-typed
run is not either: `doom -timedemo demo3` typed into a warm session measured
170.7 s against 168.9 s for the same build from a clean boot.

Clean-boot baseline of the shipping build (both Dune II fixes in):
**168.9 s, 285,937,208 guest instructions, 1.693 MIPS.** Note this is 5.5%
slower than the 159.8/160.4 s recorded for prefetch32 earlier in this
document. The palette-substitute search added with the HDMI fix is not the
cause - it runs about 60 times a second for some 8000 cycles, which is 0.1% of
one core. Unexplained; most likely the layout noise this file already warns
about. Treat 168.9 s as the baseline for anything measured from here.

### Collapsing the translate call chain: +1.8%, and what that rules out

`translate()` was four nested out-of-line calls for a plain TLB hit -
translate -> segcheck, then translate_laddr -> translate_lpgno - and every
memory operand pays for all of them. It is now one function with the hit path
(segment check, paging on, TLB entry present and permitted, access inside one
page) straight-line, and a single `translate_out_of_line()` for everything
else. It does not inline into the ~200 call sites, so **RAM is unchanged at
476,308 B** and flash shrank by 2 KB.

Measured: **165.9 s against 168.9 s, 1.8%** (MIPS 1.693 -> 1.720). Real - the
instrument repeats to 0.4% - but small, and it is the answer to a question
worth having asked: `translate` + `translate_laddr` are 11% of core 0 on Doom,
and removing three of the four calls from that path bought 1.8%. **Call
overhead is not what makes the interpreter slow.** Do not spend more effort
shaving call sequences out of the memory path; the cost is in the work itself,
not in reaching it.

The change is kept (free, semantically identical, no RAM), but it is not a
direction.

### The rep movs / rep stos inner loops: nothing, and why. TRIED, REVERTED

The second thing tried, and it is worth writing down because it looks obviously
right. `MOVS_helper2` and `STOS_helper2` already hoist the address translation
to once per page-clipped run, but their loop body still called `load##BIT()`
and `store##BIT()` per element, each re-testing `in_iomem()`, the
`phys_mem_size` bound and `res->res` - all invariant for the whole run,
because the run is clipped to one page and 0xa0000 / 0xc0000 / 0xe0000000 are
page aligned. Hoisting those and calling `nj_note_write()` once for the whole
range (it already takes a length) turns the body into a load, a store and two
adds.

Measured on Draci historie's opening - the right instrument, since Doom's
renderer draws columns pixel by pixel and barely uses string instructions:

| build | MIPS | native coverage |
|---|---:|---:|
| without the change | 1.887 | 7.5% |
| with it | 1.888 | 7.4% |

**0.05%. Nothing.** On Doom it measured 168.5 s against 165.9 s, i.e. 1.6%
*worse*, which cannot be the change's own cost (its added tests run once per
run, not per element) and is the layout noise this file warns about -
`cpu_exec1` grew 926 bytes.

The reason it cannot pay is worth keeping: **games blit to 0xa0000, and that
is exactly what `in_iomem()` calls MMIO**, so the fast path declines the one
case it was written for. Screen writes already go through
`cpu->cb.iomem_write_string`, a bulk call that is optimised. Only RAM-to-RAM
copies were left for it, and there are not enough of those to see. Reverted;
it cost 926 bytes of an SRAM budget that is 90.85% full.

### The conclusion these two experiments share

Two independent attempts to remove out-of-line calls from the interpreter's
hot memory path bought 1.8% and 0.0%. **The interpreter is not slow because of
call overhead**, and further work of that shape should not be expected to pay.
Whatever the 252 host cycles per guest instruction are, they are the work
itself. Anyone picking this up should either find that out with a fresh
`PC_SAMPLE` profile at fine resolution, or go after a cost that is structural
rather than incremental - which is what the mixer item below is.

### Where the next attempt should go

Two candidates, in order:

**UPDATE - this was tried; read the CORE-1 MIXER section below before starting.**

1. **Get OPL synthesis off core 0.** It is ~23% of core 0 on Doom and all of
   it is in flash. Moving the audio mixer to a true core-1 alarm pool was
   already measured as freeing 13-15% of core 0 - the only measured
   tens-of-percent lever anyone has found - and it was reverted for
   *correctness*, not performance: Prehistorik 1 crashed at startup and
   Prehistorik 2 hummed until the level-select menu. The blocker is a
   cross-core race in SB16 / PC-speaker / Covox state, not a missing idea.
   `sb_set_irq()`'s core-1 deferral is the one hazard of that class already
   found and fixed; the rest needs the same treatment. The SWD instrumentation
   built for the Dune II work (live SB16 + 8259 + i8257 state, and the
   AUDIO_DIAG event ring) is exactly the tooling for it.

2. **The `rep movs` / `rep stos` inner loops.** Address translation is already
   hoisted per page run, but the loop body still calls `load##BIT` and
   `store##BIT` **per element**, and each of those re-tests `in_iomem`, the
   `phys_mem_size` bound and `res->res` - all loop-invariant once the run is
   known to stay inside one page. What should be a memcpy costs ~25-30 cycles
   a word. Doom's timedemo may not show it; the blit-heavy DOS games the user
   actually complains about (DRACIHIS's curtain, Aladdin, Prehistorik) should.
   Measure that one in DRACIHIS, not on Doom.

## THE CORE-1 MIXER: the regressions are fixed, and it still buys nothing

Done, measured, and the result is not what this document predicted.

### What was changed

`timer_callback0` used to do both halves of the 44.1 kHz tick - the audio mixer
and `vga_hw_process_deferred()` - so the earlier attempt to move "the mixer" to
core 1 moved the video work with it. They are now separate:

- `timer_callback` (audio only) runs on a real core-1 alarm pool, created in
  `core1_entry()` with `alarm_pool_create_with_unused_hardware_alarm()`;
- `vga_deferred_callback` (video only) stays on the default pool, i.e. core 0,
  at the same 44.1 kHz;
- the core-1 alarm's IRQ priority is set to 0xc0, below hdmi.c's scanline DMA
  interrupt (priority 0) and its render worker (0x40), so the display always
  preempts the mixer rather than the other way round.

Verified on the board rather than assumed: `core1_pool` is non-NULL, `m_timer`
carries the core-1 pool and `timer_callback`, `v_timer` carries the default
pool and `vga_deferred_callback`.

### The regressions are gone

**Prehistorik 1 and Prehistorik 2 both run correctly** with the mixer on core 1
- no glyph garbage, no hum. The user confirmed both.

The likely reason, though *not* isolated: both failures are text-mode ones, and
a text-mode palette fade rebuilds the whole 256-entry pair table through TMDS
encoding every frame - about a millisecond. On core 1 that millisecond lands
inside a timer interrupt sharing a core with a 32 us scanline interrupt. Doom
never fades a text palette, which is why its music was fine last time while
Prehistorik broke. Two things changed at once here (the split and the IRQ
priority), so if it ever matters, isolate them by putting the video half back
on core 1 while keeping the priority.

### And it is worth nothing

Doom, clean boot: **167.2 s, against 165.9 s for the same tree with the mixer
still on core 0.** That is 0.8% *worse*, not the 13-15% this document promised.

So the "frees 13-15% of core 0" figure does not turn into throughput. Taken
with the two interpreter experiments above - removing three of four calls from
the memory path bought 1.8%, and hoisting the per-element work out of the
string loops bought 0.0% - three independent attempts to give core 0 back CPU
time have produced almost nothing. The working hypothesis is now that **core 0
is not compute bound**, and that whatever the 252 host cycles per guest
instruction are, they are not cycles the CPU could be spending on something
else. That has to be established or refuted with a measurement before any
further work is planned on top of it.

The next measurement is the cheap bound: build with the mixer never registered
at all and run the demo. That is not a shippable build; it says how large the
entire audio prize is. If Doom still takes ~166 s with no mixer, this whole
branch is closed and the 13-15% figure should be struck from this document.

## SESSION SUMMARY: five experiments, two small wins, and one thing now understood

Doom, `doombench.py`, clean boot every time. The instrument repeats to 0.4%,
but this tree is also known to move up to 5% on code layout alone, so read
anything under about 2% with that in mind.

| build | wall | MIPS | verdict |
|---|---:|---:|---|
| starting point (both Dune II fixes) | 168.9 s | 1.693 | baseline |
| + `translate()` fast path | 165.9 s | 1.720 | **kept, +1.8%** |
| + `rep movs`/`stos` hoisting | 168.5 s | 1.696 | reverted, 0% |
| + mixer on a core-1 alarm pool | 167.2 s | 1.710 | reverted, -0.8% |
| mixer never registered (diagnostic only) | 143.0 s | 1.891 | **the bound: 13.8%** |
| + OPL block drain of 8 | 162.5 s | 1.752 | **kept, +2.0%** |
| + OPL render drivers into SRAM | 163.8 s | 1.740 | reverted, -0.8% |

Net for the session: **168.9 s -> 162.5 s, 3.8%.** Draci historie was used as a
second instrument (`claude_handoff/dhbench.py`, new) wherever Doom was the
wrong workload.

### What was learned, which is worth more than the 3.8%

**The audio is 13.8% of the run, and it is not where anyone thought.** Switching
the mixer off entirely takes Doom from 165.9 s to 143.0 s. But moving the mixer
callback to a real core-1 alarm pool recovered *none* of it - because the mixer
does not synthesise anything. `adlib_getsample()` only pops a ring; the
synthesis is `adlib_produce()`, reached from **`adlib_core0()`, which `pc.c`
calls sixteen times per `pc_step()` on core 0**. Turning the mixer off gains
13.8% because nothing drains the ring any more, so the producer stops.

So the prize is real, it is measured, and **the thing to move is
`adlib_core0()`, not the mixer.** That has not been tried.

**Removing call overhead from the interpreter does almost nothing.** Collapsing
four nested calls to one on every memory operand bought 1.8%; hoisting the
per-element work out of the string loops bought 0%. Whatever the 252 host
cycles per guest instruction are, they are not call sequences.

**OPL code in flash is not the problem; OPL data was.** Moving
`OPL_calc_buffer_linear` and `slot_mod_linear` (1.1 KB) into SRAM measured
0.8% worse. That is consistent with the earlier 16.6% win from `exp_table`:
data indexed near-randomly per sample thrashes the XIP cache, 35 consecutive
lines of code do not.

### The next task, with the evidence already in hand

Move OPL synthesis off core 0. The sample ring between producer and consumer
already exists and is already cross-core. What blocks it is ordering: besides
the periodic `adlib_core0()`, `adlib_write()` calls `adlib_produce(s, 1)`
synchronously on core 0 so that a register write lands at the right sample.
Two cores calling `adlib_produce()` is the race the old comment in `main.c`
warned about.

The clean design is a second ring: core 0 queues *register writes with the
sample index they take effect at*, core 1 owns `adlib_produce()` entirely and
applies queued writes as it reaches their index. Then core 0 does no synthesis
at all and the 13.8% is available. Budget for it as a real piece of work, not
a patch, and keep `adlib_underruns()` and the `g_adlib_*` counters in view -
they already report exactly the failure mode a bad handoff produces.

Two things this session established that make it safer than last time:

- **The Prehistorik regressions are understood and fixed.** Splitting the
  44.1 kHz callback so that `vga_hw_process_deferred()` stays on core 0, and
  giving the core-1 alarm an IRQ priority below the scanline interrupt, makes
  Prehistorik 1 and 2 both run correctly with the mixer on core 1. The user
  confirmed both. The code for that split is in
  `frank-386-core1-mixer.elf` and in this document's CORE-1 MIXER section; it
  was reverted only because it bought nothing on its own.
- **The audio path is now observable.** `dhbench.py`, the SB16/8259/i8257 live
  reads and the AUDIO_DIAG event ring from the Dune II work all apply directly.

## The one number that should drive the next decision

**The JIT executes about 5% of guest instructions.** Measured with
`claude_handoff/perf.py` during DRACIHIS: 5.2% native coverage, 3.3 guest
instructions per block entry. Doom's renderer is the same story - 6.3% and 3.5.

So *any* work on the JIT is capped at ~5% of total runtime until coverage rises
enormously, and coverage is limited by opcode and back-edge support, not by
anything fixed this session. **The interpreter is 95% of the work.** That is
where tens of percent have to come from, and it is where I would go next.

`PC_SAMPLE` over the DRACIHIS scene the user reports as slow (the Nosense logo
and the opening curtain, which take 48 s here against ~12 s on a real PC):

| | share of core 0 |
|---|---|
| `cpu_exec1` (decode/dispatch) | 41.4% |
| `peek8_slow` (prefetch refill) | 8.9% |
| `modsib` (effective address) | 8.2% |
| `store16` | 8.1% |
| `translate` + `translate_laddr` (3 symbols) | 9.4% |
| `spi_write_blocking` (SD) | 4.5% |
| `vga_mem_write` | 1.1% |

The interpreter costs **252 host cycles per guest instruction** (504 MHz /
2.0 MIPS). That ratio, not the JIT, is the 4x gap the user measured.

## What was done today, with numbers

| change | effect | state |
|---|---|---|
| `exp_table` → SCRATCH_Y (`slot_render.cpp`) | **+16.6%** on Doom | shipped |
| `slot_envelope_loop` 16 instantiations → 1 | **+8.7%** | shipped |
| that loop into main SRAM (`__not_in_flash_func`) | +4% | shipped |
| audio mixer onto a core-1 alarm pool | frees 13-15% of core 0, but causes Prehistorik crashes/hum | **reverted** |
| out-of-line JIT guard (`NJIT_SHARED_GUARD`) | coverage 6%→5%, Doom 163.3 s | shipped, no gain |
| JIT loop memory operands (`NJIT_LOOP_MEM`) | follows the guard | on, no gain |
| instruction prefetch 16 → 32 bytes | Doom 159.8 / 160.4 s | shipped |

The best experimental cumulative result was 228.2 s → 160.1 s, but that number
included the now-reverted core-1 mixer and must not be quoted as the stable
shipping gain. Almost all of the safe measured improvement came from the OPL
and code-placement work; the JIT contributed nothing measurable.

### The prefetch change

`cpu->prefetch` was 16 bytes filled by four `pload32`. The RP2350's XIP cache
line is 32 bytes, so the fill already brought in twice what the buffer kept and
`peek8_slow` - 8.9% of core 0 - refilled twice as often as it needed to. A
32-byte line never straddles a 4 KB page (4096 divides by 32), so no paging
logic changes.

Widening it means touching **five** places, not one: `prefetch[]` in
`i386.h`, the mask and fill in `prefetch_fill()`, `PREFETCH_HIT`, the three
`prefetch[laddr & 31]` sites, **and the separate `unsigned off = laddr & 15`
indexing inside `fetch16()` and `fetch32()`**. Missing the last two makes the
guest read a wrong instruction stream and hang on a blinking cursor with the
firmware still healthy - which is what happened here.

Rejected with evidence: a single-entry write-page cache (Doom -5%), a faster
flash clock (already at 100.8 MHz; 126 MHz hangs), a lower-latency PSRAM part
(XIP hit rate is 98.1% and the QMI is 4-lane QSPI).

## Measuring: use these, not the obvious things

- **`claude_handoff/doombench.py <elf> [label]`** - resets the board, types the
  command in ring-sized chunks, verifies it reached the prompt, and times the
  graphics-mode window against the **host's** clock. Use this, never Doom's own
  `realtics`: that is the guest's timer driven by `cpu->cycle`, which
  under-counts on native side exits, and three runs of one identical build once
  reported 7119, 7649 and 6426.
- Repeatability is **1-6%**, not the 0.4% one lucky pair suggested. Two runs
  minimum; do not call a 3% difference.
- `perf.py <elf> <secs>` - MIPS and native coverage. `jitstat.py` - JIT
  counters and the reject histogram. `ratetrace.py` - throughput over time,
  for telling a uniformly slow scene from one with stalls. `stallwatch.py` -
  core-0 unavailability via the AdLib ring gap. `pshist.py` - symbolises a
  `PC_SAMPLE` histogram; it now weighs a bucket by overlap, because
  attributing by start address once put 8.5% on `osd_render_line_vga`, a
  function that only runs while the OSD is visible.
- `swdflash.sh` - flashing on Windows. `flash.sh --swd` cannot work here; its
  probe enumeration uses macOS `ioreg`.
- **`frankelf.py`** - resolves `offsetof(CPUI386, cycle)` from the ELF's DWARF
  via gdb. The scripts used to hardcode 324; widening `prefetch` moved the
  field to 340 and they silently read the wrong address, reporting 2.6 billion
  guest instructions and 16 MIPS. Wall-clock figures were unaffected because
  they come from the host, but every MIPS and coverage number in between was
  wrong. Never hardcode a struct offset again.

## Traps that cost me cycles today

1. **`.bss` and the guest's PSRAM are cleared on every boot.** Post-mortem
   buffers there read back as the fresh boot's zeros. Twice I concluded "the
   trampolines were never built" from zeros that were simply re-initialised.
   Use `FAULT_PARK=ON`, which stops the core in the fault instead of rebooting.
2. **`nj_compact_code()` relocates finished blocks.** Branches *inside* a block
   survive because source and target move together; a `BL` to a fixed external
   address does not. This is what boot-looped the first shared-guard build -
   it ran until the arena filled, then every call into the trampolines landed
   somewhere else. Generated calls out of a block must materialise the address
   (`MOVW/MOVT` + `BLX`).
3. **Never call `nj_flush()` from inside a compile.** It resets `nj_code_ptr`
   under the emitter and the half-built block is registered in free space.
4. **Symbol addresses move between builds.** `wait2.tcl` hardcodes
   `current_mode` at 0x2002ba5c; against a newer ELF it silently never matches.
   Always resolve from the ELF being run.
5. **Profile the scene the user is complaining about.** A window 8-38 s after
   launch and one triggered on entry to graphics mode disagreed about which
   subsystem to blame.

## What I would do next, in order

0. **Done: the prefetch (was 8.9%).** See above.
1. **`translate` (9.4%).** Three constprop clones. Every guest memory access
   goes through it. Look for a cheaper fast path for "same page as last time"
   before touching the TLB structure.
2. **`modsib` (8.2%).** The note in the earlier handoff still stands: scene
   variance is high, so it needs a controlled A/B rather than a rewrite on
   intuition.
3. **`peek8_slow` (8.9%)** is the 16-byte prefetch refill. The XIP line is
   32 bytes, so a 32-byte prefetch buffer would halve the refills for the cost
   of one extra word per fill. Cheap to try, easy to measure.
4. Only then the JIT, and only with an **interpreter-vs-JIT differential
   harness** first - the user proposed it, and this session is the argument
   for it: I spent five flash cycles on three bugs in one function whose only
   symptom was a boot loop.

## The VGA buffer is not spare SRAM (tried, reverted)

Halving `EMU_VGA_MEM_SIZE_KB` from 256 to 128 frees 128 KB and drops RAM from
91.6% to 66.6% - and it does not work. Two reasons, both now fixed or
documented:

1. `GFX_BUFFER_SIZE` was hardcoded `(256 * 1024)` in `vga_hw.c` **and**
   `hdmi.c`, and `pc.c` hardcoded `pc->vga_mem_size = 256u << 10` a third
   time, all independent of `EMU_VGA_MEM_SIZE_KB`. With the buffer built at
   128 KB and that constant still 256, `pc_new()`'s `memset` ran 128 KB past
   the end and zeroed whatever followed in `.bss` - the board boot-looped
   faulting inside the SDK's `alarm_pool_irq_handler`. All three now derive
   from the one macro.
2. Even correct, 128 KB is too small. Planar modes are stored plane-
   interleaved, four host bytes per guest byte, so 64 KB of guest VGA address
   space needs 256 KB here. Wolf3D's three pages alone reach byte 133120.
   EGA and mode X/Y would go black.

`video_driver` is a separate CMake library and does **not** receive the main
target's `target_compile_definitions`; `gfx_buffer` lives there, so any VGA
sizing has to be given to that target too.

## Why block chaining is the real path, and what has to come first

The claim "the JIT is capped at 5%" is true of the JIT as it stands and false
of a fixed one. With more opcodes, native block linking and an arena that can
hold a working set, coverage could reach tens of percent at several times the
interpreter's speed - that is a multiple, not a margin.

The order matters, and it is not chaining first. The arena is 8 KB and
generated code runs about 50 bytes per guest instruction, so it holds roughly
**160 guest instructions**. Every game's hot loop is larger. Chaining makes
the transition between blocks cheap but does nothing about blocks being
evicted and recompiled, which is what cost 23% when blocks were made bigger.

So: **arena, then opcodes, then chaining.**

The only route to a bigger arena that is left, now that the VGA buffer is
ruled out and SRAM is full, is to put the arena in **PSRAM** and execute
generated code through the XIP cache, as flash code already is. Code has good
locality so it should cache well, and the arena could be hundreds of KB. The
hazard is real though: after writing generated code the affected XIP cache
lines must be invalidated or a stale line executes, and that failure mode
looks like "it crashes sometimes". Build it with `FAULT_PARK=ON` from the
start.

## Open question worth settling early

Even at high coverage the JIT's shape caps its benefit: each block loads eight
guest registers, retires ~3 instructions, and stores eight back. Native block
linking (jumping straight into the next block's body instead of returning to C
and reloading state) is the structural fix, but it is a dynarec rewrite, not an
edit. Decide whether that is in scope before spending more on JIT coverage.
