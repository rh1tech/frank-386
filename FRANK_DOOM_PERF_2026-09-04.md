# Doom performance, 2026-09-04 — where the cycles actually go

Board: Z2, 504 MHz, PSRAM 166, I2S, `--usb-hid`.
Benchmark: `doom -timedemo demo3` in `C:\HRY\DOOM1`, from a clean boot, output
redirected to `t.txt` (Doom clears the screen on exit).

| build | demo wall seconds | emulated MIPS | |
|---|---|---|---|
| baseline | 228.2 | 1.314 | |
| `exp_table` → SCRATCH_Y | 190.3, 189.9 | 1.53 | |
| the same, `AUDIO_DIAG` off | 199.5 | 1.467 | not taken on its own |
| + `slot_envelope_loop` collapsed to one instantiation | 173.7 | 1.654 | |
| **+ that loop moved into main SRAM** | **166.8, 169.4** | **1.69-1.71** | **shipped, ~35% faster** |
| + the cold OPL wrappers moved too | 170.0 | 1.686 | no evidence it helps, reverted |

Run-to-run repeatability is **1-2%**, not the 0.4% an early lucky pair suggested
(189.9/190.7), so differences under about 3% are not decidable in one run each.
The 166.8/169.4 pair is the same binary measured twice.

Audio was checked by ear after the OPL changes and is unaffected.

**Do not use Doom's `realtics`.** It is the guest's own timer, driven by the
emulated PIT off `cpu->cycle`, and `cpu->cycle` under-counts whenever a native
block side-exits mid-body. Three runs of one identical build reported 7119,
7649 and 6426 realtics — a 19% spread on a deterministic demo, entirely
instrument noise. The table above is `claude_handoff/doombench.py`: the board
is reset, the command is typed and verified on the prompt, and the
graphics-mode window is timed against the **host's** clock. Two clean runs of
one build agree to 0.4%.

Compare clean-boot runs only. A second launch inside the same DOS session runs
materially faster on a warm cache.

At this precision, code layout is itself worth several percent: disabling
`AUDIO_DIAG`, which removes a volatile read from every guest store and frees
4 KB of SRAM, measured *slower*. Before believing a timing result, look for a
mechanism-level measurement that agrees with it.

## The three changes that won

### 1. `exp_table` into SCRATCH_Y

One line. `exp_table[256]` in `src/emu8950/slot_render.cpp` now carries
`SLOT_RENDER_DATA_Y` (`__scratch_y`).

Every OPL operator, on every sample, evaluates
`exp_table[logsin_table[phase] + env]`. `logsin_table` had already been placed
in SCRATCH_X; `exp_table` was left behind, and because nothing ever writes it
the compiler put it in `.rodata` — flash, reached through the XIP cache that
the guest's own PSRAM traffic is missing 900k times a second. The lookup index
is essentially random across 512 bytes, so it is the worst possible access
pattern for a line that keeps getting evicted.

SCRATCH_Y is the right home: a separate SRAM bank, so it costs nothing in the
main region where `pc_new()` has under a kilobyte of headroom. Main RAM is
byte-for-byte unchanged at 481048; SCRATCH_Y went 2080 → 2592.

Measured with `SUBSYS_PROFILE` while Doom rendered:

| | before | after |
|---|---|---|
| `adlib` share of core 0 | 33.0% | 23.2% |
| `adlib` cycles/s | 166 M | 117 M |
| guest throughput | 1.032 MIPS | 1.392 MIPS |
| interpreter | 314 cyc/insn | 266 cyc/insn |

That is a direct cycle measurement of the OPL rather than a wall-clock one,
which is what makes the 16.6% credible rather than a layout accident.

The interpreter got faster too, because the OPL stopped evicting its cache
lines. A build with **no JIT at all** plus this fix beat the JIT-enabled
baseline (7214 vs 7875), which says how little the JIT was contributing here.

### 2. Collapsing `slot_envelope_loop` from sixteen instantiations to one

It was `template <int F_NUM, typename F>`, instantiated once per (operator
kernel, PM) pair: sixteen copies of 1200 bytes, **19 KB**. They were
near-identical, because `F_NUM` is used exactly once in the compiled path — an
`if (F_NUM < 8)` in the per-call setup — and the kernel is reached through a
call rather than being inlined. The other use of `F_NUM` is inside
`#if EMU8950_ASM`, which this build does not enable.

Making `F_NUM` a runtime argument and `fn` a plain function pointer leaves one
1380-byte function. Total OPL hot text went from **23,584 bytes to 4,632**. The
cost is one branch per call — per slot per buffer, not per sample.

That is worth 8.7% on its own (190.3 → 173.7 s), before any placement change,
purely because 19 KB of setup code cannot stay resident in a 16 KB XIP cache
that the guest is also streaming through.

### 3. The collapsed loop into SRAM

Now that it is a single plain `static` function, `__not_in_flash_func()` works
on it — GCC ignores a section attribute on a template, which is why this was
impossible before. 173.7 → 166.8 s.

It does **not** go in SCRATCH_Y, even though 1380 bytes fit in the 1504 free
there. SCRATCH_Y also holds core 0's stack, and the linker reserves only
`PICO_STACK_SIZE` (2048) for it; the rest of the bank being empty was quietly
absorbing stack overflow. Placing code under the stack turned that into
corrupted instructions and the guest rebooted as soon as Doom started.

The main region has the space only because `AUDIO_DIAG` is now off, which frees
about 4 KB. That is a deliberate trade: the diagnostic write-watchpoint costs
nothing measurable, but its SRAM buys the placement. Re-enabling it means
giving the placement back.

### 4. One hot per-sample kernel into SRAM

The flash-window sampler had identified `mod_am1_fb0_fn<false>` as the hottest
small operator kernel. GCC ignores a section attribute on its template
instantiation, so `OPL_HOT_KERNEL_RAM` uses one concrete 92-byte wrapper and
forces the template body into it. The original flash instance disappears.

Two clean Doom runs were 166.0 and 166.2 seconds (average 166.1), versus 166.8
and 169.4 seconds for the prior build (average 168.1): about 1.2% faster. The
linker remains at 476,948 bytes / 90.97% RAM and the pre-`pc_new()` heap remains
47,632 bytes, because the wrapper fits in the 236 bytes below the next heap
page boundary. This option is therefore on by default.

Moving all sixteen kernels was rejected. With a 6 KB JIT arena it measured
167.3 seconds (neutral); with the original 8 KB arena it consumed another 4 KB
RAM, left only 43,536 bytes before `pc_new()`, and produced unstable 164.6 and
187.6 second runs. Adding the likely paired 100-byte `alg0_am0_fn<false>` while
staying below the same page was also rejected after inconsistent 164.7 and
172.8 second runs. The single-wrapper version is the only retained placement.

## Where the rest of the time goes

`PC_SAMPLE` at 10 kHz, 300k samples during the renderer. Two runs, one with the
histogram over the RAM text window and one over the flash text window (new
`PS_WINDOW_FLASH` option; `g_ps_trigger` is a new word a debug probe can poke,
because Win+F10 is only reachable from the USB-HID keyboard path).

| | share |
|---|---|
| `cpu_exec1` | 27.2% |
| OPL synthesis (all in flash) | ~23% |
| `translate` + `translate_laddr` | 10.6% |
| `fetch16` + `peek8_slow` | 14.4% |
| `store32` | 7.0% |
| `call_isr` | 4.3% |
| everything else | ~13% |

`peek8_slow` is not a bug: `FAST_FETCH` inlines the hit path and this is the
16-byte prefetch refill.

## Dead ends, with the evidence that closed them

**Loop-compiler memory operands (−23%).** Kept behind `NJIT_LOOP_MEM`, default
off; full notes in `FRANK_NATIVE_JIT_LOOP_MEM_NOTES.md`. The short version:
over ten seconds inside the renderer `NJCH_BRK_NOT_SINGLE` did not advance at
all, so not one loop block executed there — the feature never even ran. What
cost 23% was `nj_compile_loop()` asking `nj_make_code_room()` for 1280 bytes
instead of 512 before *every* compile attempt, evicting live trace blocks out
of an 8 KB arena ninety times a second.

**A lower-latency PSRAM part.** The XIP cache hit rate during rendering is
98.1% (`XIP_CTR_HIT`/`XIP_CTR_ACC`, which saturate at 0xffffffff and must be
cleared before a window). Only 1.9% of accesses reach the part at all, and the
RP2350's QMI is four-lane QSPI, so octal PSRAM or HyperRAM cannot be attached
at full speed regardless. `prof_mem_bench()` puts a PSRAM miss at 188 cycles
against 8 for SRAM.

**A faster flash clock.** The flash already runs at 100.8 MHz (CLKDIV 5) in
steady state. An earlier reading of CLKDIV 8 was a transient: `vga_hw.c` calls
`set_flash_timings()` again during HDMI bring-up, so the register must be read
after the board reaches DOS. CLKDIV 4 (126 MHz) hangs at `g_diag_stage == 0`
with `POWMAN_CHIP_RESET` = `HAD_POR`, i.e. a clean power-on and a real timing
failure. A build-time frequency floor is also misleading, because that second
caller passes `HDMI_SYS_CLOCK_MHZ` rather than the live clock and produced
CLKDIV 3.

**Moving the OPL synthesis code to SRAM.** 23.5 KB of `slot_render`
instantiations — `slot_envelope_loop<N>` alone is sixteen copies of 1200 bytes.
The eight per-sample operator kernels are small enough (~1 KB total, and
`mod_am1_fb0_fn<false>` holds 6.13% of core 0 in 92 bytes), but they are
reached through a function reference and GCC ignores a `section` attribute on a
template, so placing them needs sixteen explicit wrappers — about 1.7 KB into
the 1504 bytes SCRATCH_Y has left. Not attempted.

## What is still open

The OPL is ~23% of core 0 and all of it executes from flash. Making it cheaper
is the largest remaining lever and the honest ways in are algorithmic rather
than placement: it is a genuine dual-OPL2 workload (the guest really does drive
the second register bank — `g_opl3_bank1_writes` is non-zero), so it cannot
simply be halved.

On the interpreter side, `translate` at 10.6% and `call_isr` at 4.3% have not
been looked at at all.
