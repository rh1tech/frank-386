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
- [ ] **dsk.c:341 (r_bpptr)**: still needs the ddt signature change. The ddt
      lives in the MCB arena (DynAlloc, arbitrary segment), so thread its real
      dos_far_ptr into blk_bldbpb()/getbpb() rather than guessing a segment.
- [ ] **nls.c:33 (call_nls buf)**: thread the original dos_far_ptr through
      call_nls()/muxGo()/muxBufGo() rather than reconstructing from a native
      pointer that came from ARM_PTR(ES:DI).
- [ ] **fdos_21h.c:105 (lpDevice)**: defensible as-is (is_guest_ptr-guarded, no
      canonical segment available, only published as SDA CritErrDev). Can drop
      once CriticalError() is passed a dos_far_ptr instead of a native dhdr*.
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
- [ ] **NATIVE_PTR convention for native drivers**: for the (few) native
      drivers, redefine their dos_far_ptr to mean high16:low16 of a 32-bit
      native address (NATIVE_PTR(seg,off) = ((u32)seg<<16)|off) rather than
      seg:off, and make those paths use NATIVE_PTR instead of ARM_PTR. Track
      every such site carefully. This gives a clean, checkable way to carry a
      native pointer inside a dos_far_ptr where no guest seg:off exists (e.g.
      the CriticalError lpDevice case, and possibly the DPB/ddt cases).
- [ ] **Relocate static buffers out of SRAM** into spare guest RAM: unused HMA
      region and the fake ROM area can host buffers currently eating the
      520 KB SRAM. Candidates: the DiskTransferBuffer, deblock buffer, any
      large static scratch. Frees precious SRAM.

## Housekeeping tasks noticed
- [ ] src/fdos/fcom/ contains a pair of proposed patches (against an OLDER
      base). Analyse for relevance; if still meaningful, rebase onto the
      current tree and apply.
- [ ] src/diag.c is an unwired runtime-diagnostics utility. Consider adapting
      it and gating it behind a CMake switch.
