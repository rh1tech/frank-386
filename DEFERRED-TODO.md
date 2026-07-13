# Deferred items (raised during review, parked for a later stage)

## Stage 4-ish (memory-manager correctness)
- [ ] **1 MB border in pstore/pload**: CHECK_RAM_BORDER only clamps the whole
      PSRAM (6-8 MB), not the 1 MB conventional line. HMA/EMB can be silently
      corrupted by an out-of-range access that stage4a now blocks at the XMS
      layer, but the emulator memory layer itself is still permissive.
      (User confirmed: fix later.)
- [ ] **XMS HMA functions (07h/08h) absent** from xms_handler switch. Kernel
      takes HMA directly (DOS=HIGH), not via XMS, so not critical, but a guest
      XMS client asking for HMA falls into default. Add for completeness.

## Versioning / build
- [ ] **Bump build version 1.04 -> release version** (it is the release build,
      keep it from being confused with an early one). XMS also reports HIMEM
      2.06 - unrelated, that one is correct.
- [ ] **README.md is outdated** - especially the memory map, which is from a
      very early port. Rewrite to match current layout.
- [ ] **CMake always names output "386"**. Add a top-level switch selecting
      base name 286 vs 386, setting I386_MODE=1 and making path constants
      (e.g. "386/") depend on it.

## Comment hygiene (found during audits)
- [ ] RELEASE_UMB still labelled "// Stub" though it really frees the chain.
- [ ] to_physical_offset macro body lacks fully defensive parenthesisation.

## Stage 5 follow-up (linear_to_far endgame)
stage5a cut linear_to_far from 26 live call sites to 4. The remaining 4 cannot
be converted mechanically and are each documented in-place:
- [x] **fattab.c:91 (b_dpbp)**: DONE in stage6a. link_fat/next_cluster/
      is_free_cluster/dos_free/getFATblock/wipe_out_clusters now take
      dos_far_ptr and derive the native view internally; callers pass the
      dos_far_ptr they already held (fnp->f_dpb / cdsDpb / _dpbp). Net removal
      of ARM_PTR round-trips.
- [x] **dsk.c:341 (r_bpptr)**: DONE in stage6b. Added getddt_far() (the ddt's
      genuine guest pointer, computed from the same far base getddt() uses);
      blk_bldbpb() now sets r_bpptr = ADD_OFF(getddt_far(dev), offsetof(ddt,
      ddt_bpb)). dev is recovered as (pddt - getddt(0)) so the dispatch-table
      signature stays uniform. Verified ARM_PTR(getddt_far(d))==getddt(d).
- [ ] **nls.c:33 (call_nls buf)**: thread the original dos_far_ptr through
      call_nls()/muxGo()/muxBufGo() rather than reconstructing from a native
      pointer that came from ARM_PTR(ES:DI).
- [x] **fdos_21h.c:105 (lpDevice)**: DONE in stage7a. CriticalError()/
      char_error()/block_error() now take dos_far_ptr; every caller already
      held the guest dhdr pointer (dpb_device / LoL->clock / BP:SI) and was
      throwing it away via ARM_PTR. The 0000:0000 sentinel doubles as the
      old NULL "no device". linear_to_far() removed from this path.
Only after all 4 are gone can linear_to_far() itself be deleted from kernel.c.

## Regression postmortem (stage5b)
stage5a wrongly converted two EXEC-path native pointers with
x86_FAR_PTR(DOS_PSP, ...). Both were DosExec()'s "lp" = ARM_PTR(guest DS:DX),
which lives in the CALLER's segment (FreeCOM's PSP), not DOS_PSP. The wrong
offset (proven: linear 0x20100 -> 0x10100, 64K off) made truename() and
DosOpenSft() fail, so every external command gave "Bad command or filename".
Reverted both to linear_to_far() in stage5b.
LESSON: x86_FAR_PTR(DOS_PSP, p) is ONLY valid when p is a static in the DOS
data segment (internal_data / SecPathName / LoL). For any pointer that is a
function parameter, trace it to its origin FIRST - if it is ARM_PTR(guest reg),
it is caller-segment and must NOT be re-anchored on DOS_PSP.

## New direction from review (reshapes the linear_to_far endgame)
- [~] **NATIVE_PTR convention**: vocabulary landed in stage7a - NATIVE_PTR()/
      NATIVE_ARM_PTR() macros (verified to round-trip any 32-bit address, which
      x86_FAR_PTR/ARM_PTR cannot), plus documentary typedefs native_ptr (packed
      native only) and mixed_ptr (either kind; gate on a discriminator). KEY
      FACT recorded in portab.h: packed-native and guest seg:off are ambiguous
      BY VALUE (a native 0x11xxxxxx packs to "segment" 0x11xx, a legal guest
      segment), so discrimination must always come from context (ATTR_NATIVE),
      never the bits. dh_next is now typed mixed_ptr. Still TODO: actually wire
      a genuinely-native external driver load path that uses NATIVE_PTR for
      dh_next / dh_interrupt (the ATTR_NATIVE external-driver item below).
- [~] **Relocate static buffers out of SRAM** into spare guest RAM. Audit
      done: DiskTransferBuffer/deblock_buf/local_buffer are ALREADY in guest
      RAM (internal_data). The remaining native-SRAM arrays are either small
      (<=256B init/debug scratch: kernel.c buf[256], config commandbuffer[256])
      that never cross to the guest - moving them frees SRAM but removes no
      linear_to_far - or fnode[2] (136B) which IS a dual-benefit target but
      touches every f_node_ptr site (high regression risk; defer as its own
      stage). ctrl_c_text was moved to guest stack in stage5a. Net: the easy
      dual-benefit wins were the linear_to_far holdouts (ddt done in 6b);
      fnode[] relocation is the remaining sizeable SRAM win, to be done
      carefully on its own.

## Housekeeping tasks noticed
- [ ] src/fdos/fcom/ contains a pair of proposed patches (against an OLDER
      base). Analyse for relevance; if still meaningful, rebase onto the
      current tree and apply.
- [ ] src/diag.c is an unwired runtime-diagnostics utility. Consider adapting
      it and gating it behind a CMake switch.
