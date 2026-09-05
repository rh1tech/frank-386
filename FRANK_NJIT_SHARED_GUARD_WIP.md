# Out-of-line JIT memory guard — work in progress, currently broken

Build flag: `-DNJIT_SHARED_GUARD=ON` (default **OFF**; the tree is safe with it off).
Debug aids added alongside: `-DFAULT_PARK=ON`, `-DNJIT_EXIT_RING=ON`.

## Why

A memory operand in a JIT block carries ~250 bytes of Thumb: the TLB walk, the
physical range test, the VGA aperture test and the code-page bitmap test. The
code arena is 8 KB and cannot be grown (SRAM is 91% used and `gfx_buffer` alone
is 256 KB). Measured consequences of that on this board:

- letting the loop compiler emit memory operands cost **23% on Doom**, purely
  through arena eviction;
- DRACIHIS's intro runs at **6.3% native coverage with 3.5-instruction blocks**,
  i.e. the JIT contributes almost nothing, and the interpreter costs 252 host
  cycles per guest instruction — which is the 4x gap the user measured against
  a real PC (12 s versus 48 s for the same phase).

Out of line, a memory operand costs `push {lr}` / `bl` / `pop {lr}` / `cmp` /
`beq` plus the address arithmetic and the access — about 50 bytes.

## What is implemented

- `nj_bl()`, `nj_push_lr()`/`nj_pop_lr()`, `nj_bx_lr()` emitters.
- `nj_guards_build()` emits six trampolines (read/write x size 1/2/4) into
  `nj_guard_code[]`, a buffer outside the compacting arena so
  `nj_compact_code()` cannot move them under an encoded BL.
- Contract: r3 = guest linear address in, host pointer out, **0 = refuse**.
  r0 is saved inside the trampoline because it carries the loop compiler's
  live iteration count and both the TLB walk and the store's bitmap check use
  it as scratch.
- `nj_v6_emit_guard_call()` is a drop-in for the old
  `linear_to_phys + mem_guard + host_ptr` trio, so all seventeen call sites and
  their rewind/`nj_v8_finish_guard()` plumbing are untouched — the three old
  functions were renamed `*_inline` and thin wrappers keep the old names.
- Trampolines are rebuilt whenever `nj_mmu_key()` changes, from
  `nj_guards_refresh()` in `nj_try_execute()` only. That is a safe point: a
  rebuild has to flush the arena, and doing it from inside a compile reset
  `nj_code_ptr` underneath the emitter.

## Symptom

The board boots, reaches the main loop, runs the guest, then takes a **precise
bus fault** and (without `FAULT_PARK`) reboots — a boot loop.

```
scratch[0] = "FALT"
scratch[1] = 0x20001c98   pload32_local inlined into peek8_slow (i386.c:216)
scratch[2] = 0x00008200   CFSR: BFARVALID | PRECISERR
scratch[3] = 0x0100ff50   BFAR
```

`phys_mem` is 0x11000000, so the faulting guest physical address is
0x0100ff50 - 0x11000000 = **0xF010FF50** — garbage. The fault is in the
*interpreter's instruction prefetch*, i.e. a block left the CPU with a bad
`next_ip` or a stale `cpu->ifetch`, and the interpreter then fetched from
nowhere.

Bisection is unambiguous: an otherwise identical image without this change
(`build/dh-core1timer.elf`) boots and runs Doom twice.

## What has been ruled out

- **Trampolines not being built.** With `FAULT_PARK` they are: entries read
  back as 0x20071b44, 0x20071b82, 0x20071bc0, 0x20071bfe, 0x20071c66,
  0x20071cce — about 62 bytes each for reads in real mode, which is right
  (the TLB walk emits nothing when paging is off).
- **Buffer overflow into `nj_guard_entry`**, which follows it in `.bss`. The
  six trampolines total ~500 bytes against 3200 available. The patch-ordering
  bug that would have made an overflow corrupt memory (patching the guard
  branches before checking `e.failed`) is fixed regardless.
- **Blocks returning nonsense.** The exit ring's last sixteen entries before
  the fault are ordinary: two blocks at 0x000f36f5/0x000f36f9 alternating,
  `done` 2-3 against `insns` 2-3.
- **Guard/key pairing.** Trampolines bake in the paging mode and CPL; they are
  now rebuilt when `nj_mmu_key()` changes, before any block can call them.

## Debugging notes for whoever continues

Both obvious post-mortem stores are useless without `FAULT_PARK`:
`nj_guard_entry` is in `.bss` and the exit ring lives in the guest's physical
memory hole at 0x110a9000, and **both are cleared on every boot** — reading
them after a reboot shows the fresh boot's zeros, which cost two wasted cycles
here. `FAULT_PARK=ON` stops the core in the fault instead, and then everything
is readable live.

The freeze marker `nj_xr_note()` tests (index 65) and the one
`frank_fault_record()` writes (index 68) do not match; the ring is not actually
frozen. Fix that before relying on it.

## Next step

Bisect by scope rather than by reasoning — enable the trampoline for one
variant at a time (reads only, then a single size) and see which one first
produces the fault. Each step is a flash cycle, but it converts "boot loop"
into "the fault appears when 1-byte writes go out of line", which is a
statement about one trampoline rather than about the whole idea.

Better still, and what the user has asked for: an interpreter-versus-JIT
differential harness, so a divergence is reported as a block and a register
rather than as a board that will not boot.
