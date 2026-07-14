# Deferred items (raised during review, parked for a later stage)

## Stage 4-ish (memory-manager correctness)
- [ ] **1 MB border in pstore/pload**: CHECK_RAM_BORDER only clamps the whole
      PSRAM (6-8 MB), not the 1 MB conventional line. HMA/EMB can be silently
      corrupted by an out-of-range access that stage4a now blocks at the XMS
      layer, but the emulator memory layer itself is still permissive.
      (User confirmed: fix later.)
- [x] **XMS HMA functions**: CORRECTED + FIXED (stage9c). My note was wrong -
      07h (QUERY_A20) and 08h (QUERY_EMB) were already present, as were HMA
      01h/02h and A20 03h-06h. The real bug: REQUEST_HMA/RELEASE_HMA were
      always-succeed STUBS with no ownership tracking. On a DOS=HIGH system a
      guest REQUEST_HMA was told it owned the HMA the KERNEL was running in ->
      corruption. Fixed: single-ownership state machine that consults the
      kernel's own claim (DosLoadedInHMA, read live), returns 91h "in use" /
      93h "not yours" per spec; the kernel's resident HMA can never be handed
      out or released by a guest. Also: QUERY_A20 now clears BL on success
      (was leaving caller's BL); XMS_VERSION's BL left alone on purpose (it is
      the low byte of the BX version return). DosLoadedInHMA given a proper
      extern in init-mod.h (was a bare global). Verified the ownership state
      machine on the host across all request/release orderings.

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
- [x] **nls.c (call_nls buf)**: RESOLVED in stage7b, but NOT by threading.
      Investigated the full spine and found threading would be net-negative:
      several intermediate callers (kernel.c boot upcase, SFT name) hold only
      native pointers, so a dos_far_ptr parameter would just relocate the same
      native->guest recovery to each of them - more sites, more risk, no
      removal. Instead: proved the path is only reached via external NLSFUNC
      (built-in pkg has NLS_FLAG_HARDCODED -> all-native), that buf there is
      always ARM_PTR(guest reg), guarded the single recovery with
      is_guest_ptr(), and documented the whole analysis in place.
- [x] **fdos_21h.c:105 (lpDevice)**: DONE in stage7a. CriticalError()/
      char_error()/block_error() now take dos_far_ptr; every caller already
      held the guest dhdr pointer (dpb_device / LoL->clock / BP:SI) and was
      throwing it away via ARM_PTR. The 0000:0000 sentinel doubles as the
      old NULL "no device". linear_to_far() removed from this path.
RESULT (after stages 5a/6a/6b/7a/7b): 26 live calls -> 3, and each of the 3 is
now GUARDED and justified in place rather than incidental:
  - task.c:204 / task.c:1066 (EXEC path): correct - lp is ARM_PTR(guest DS:DX)
    from a caller segment that is genuinely not knowable any other way.
  - nls.c:56 (external NLSFUNC path): is_guest_ptr-guarded, full analysis in
    the comment.
These are irreducible: each recovers a real guest-window address whose
canonical segment is not otherwise available. linear_to_far() therefore STAYS
(it is the correct tool for exactly this), but it is no longer a latent hazard -
every remaining caller is audited. Deleting it is no longer a goal.

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

## Stage 9a (dead code from -Wall) - DONE
- [x] int21_fat32(): the FAT32 API itself is LIVE (case 0x73 -> int21_fat32_regs
      on the guest frame). Only a leftover int21_fat32(void) wrapper was dead -
      and it was a trap: it did cpu_save_regs/worker/cpu_restore_regs on the
      LIVE registers, so restore would have wiped every result the worker wrote.
      Removed, with a note saying why not to reintroduce it.
- [x] init_oem(): superseded (ram_top now reads BIOS 0040:0013 directly instead
      of INT 12h). Removed; rationale moved to the use site.
- [x] fcom_which_candidate / fcom_set_file_attr / report_file_error: NOT dead -
      three complete, unwired FreeCOM helpers (PATH/exec search, ATTRIB,
      per-file error reporting). Kept and marked KEEP_UNUSED with what each is
      waiting for. New KEEP_UNUSED convention added to portab.h, explicitly
      warning that it must NOT be used to silence accidental orphans (the
      SetverGetVersion case), only for deliberately parked complete code.

## Still open
- [ ] Wire up the three parked FreeCOM helpers (ATTRIB built-in, route the
      executable search through fcom_which_candidate, use report_file_error in
      COPY/DEL/MOVE error paths).
- [x] fcom/ proposed patches: ANALYSED (stage10a).
      * native_process_common_runner.patch: already applied to the current base
        (exec_run_process/exec_set_initial_registers/exec_run_native_command and
        the 2-arg fcom_process_main are all present). Obsolete - no action.
      * port_cpm_call5_entry.patch: relevant and UNAPPLIED - it fixed a live bug.
        PSPInit already advertised the CP/M CALL-5 gateway (ps_farcall=9Ah,
        ps_reentry=0000:00C0) but nothing populated 0000:00C0 and there was no
        handlers[0x30], so any program using CALL 5 far-jumped into an unwritten
        IVT slot. Reworked onto the current base and applied: rewrote all stack
        addressing through stk_lin() (the proposal's raw (SS<<4)+sp+n would
        mis-address a caller with SP near 0xFFFF - the stage2a bug), dropped the
        stale handlers[0x20/21/29] re-adds (already present), kept the 0000:00C0
        JMP FAR FFE0:0030 gateway and handlers[0x30]=fdos_30h. Verified the
        frame reconstruction (int_sp==entry_sp, return_sp=entry_sp+6, IRET to
        near caller) on the host.
- [x] src/diag.c: WIRED (stage10b) behind `option(DIAG_ENABLED ... OFF)`. It is
      a self-contained core0 fault/stack-overflow/hang catcher that writes
      straight to the guest B8000h text framebuffer (no debugger/UART needed).
      Verified its externs still exist (last_int_call in bios_intcall.c;
      __StackBottom/__StackTop are pico-sdk linker symbols). Three hooks in
      main.c (diag_init after init_hardware, diag_heartbeat in the pc_step loop,
      diag_core1_poll in core1's idle loop), all under #ifdef DIAG_ENABLED so
      the release build is byte-unchanged and diag.c is not even compiled. Used
      #ifdef (not #if) to match the I386_PROFILE convention and stay -Wundef
      clean.
- [ ] Housekeeping: build version bump, README memory map, 286/386 CMake switch.
- [~] RAM border in pstore/pload: INVESTIGATED (stage9b). Worse than thought -
      CHECK_RAM_BOARDER_ENABLED is 0, so ALL bound checks in mem.h are compiled
      out, and with paging off i386.c sets res->addr1 = raw guest laddr with no
      mask. A 386 guest can thus write arbitrary host memory past the 6-8 MB
      PC_RAM buffer. NOT patched: it is a CPU-core hot-path perf/safety tradeoff
      (and 6 MB PSRAM is non-power-of-two, so no cheap mask) that the core
      author should decide with a benchmark. Full write-up + costed options in
      ADVISORY-ram-border.md. The device-string IO path is already bounded; the
      open primitive is RAM-to-RAM rep movs/stos + single ordinary accesses.
      Corrects my stage4a claim that this was a backstop - it is not; the
      stage4a XMS bound is load-bearing.
- [ ] XMS HMA functions 07h/08h absent from xms_handler.
