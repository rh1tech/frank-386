/****************************************************************/
/*                                                              */
/*                           task.c                             */
/*                                                              */
/*                 Task Manager for DOS Processes               */
/*                                                              */
/*  Ported from upstream FreeDOS kernel/task.c. Algorithms      */
/*  (ChildEnv/child_psp/patchPSP/DosComLoader/DosExeLoader/      */
/*  ExecMemAlloc/ExecMemLargest) are largely unchanged; what's   */
/*  entirely new is exec_run_child() below, replacing upstream's */
/*  exec_user()/return_user()/user_r real-hardware task-switch   */
/*  primitives (a fixed register-frame on DOS's own internal     */
/*  stack, restored via a manual IRETD-equivalent) with this     */
/*  port's own mechanism: a nested C call plus a blocking        */
/*  pc_step() loop, matching the pattern already used by         */
/*  cpu_far_call() (kernel.c) for calling into loaded device      */
/*  drivers - see exec_run_child()'s own comment for the full    */
/*  rationale.                                                  */
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include <ctype.h>
#include "fcom/fcom.h"

/* pc_step()/pc - same declaration bios/bios_intcall.c and
   kernel.c's cpu_far_call() use; there is no shared header for it. */
extern struct PC* pc;
void pc_step(struct PC* pc, size_t max_ops);

#define ExeHeader (*(exe_header *)(SecPathName + 0))
#define TempExeBlock (*(exec_blk *)(SecPathName + sizeof(exe_header)))
#define Shell (SecPathName + sizeof(exe_header) + sizeof(exec_blk))

/* Scratch pair of UWORDs used only while reading EXE relocation table
   entries (DosExeLoader()). Reuses the same SecPathName bytes as
   Shell: safe, because by the time DosExeLoader() runs, "lp" (which
   may itself have been Shell, e.g. for the running shell re-EXEC'ing
   itself) has already been fully consumed by DosOpenSft() at the very
   start of DosExec(), and Shell isn't touched again until well after
   DosExec() returns (see P_0() below) - same "recycle SecPathBuffer"
   approach already used for ExeHeader/TempExeBlock. */
#define RelocBuf ((UWORD *)(SecPathName + sizeof(exe_header) + sizeof(exec_blk)))

#define DEVLOAD_CHUNK_PARAS (32256 / 16)       /* also used by EXEC_OVERLAY loads */
#define CHUNK           32256                  /* bytes per DosExeLoader() read */
#define MAXENV          32768u
#define ENV_KEEPFREE    0x83   /* sizeof(PriPathBuffer)+3: 2 bytes "extra
                                   strings" count, 0x80 bytes max absolute
                                   filename, 1 byte '\0' - see ChildEnv() */

_Static_assert(sizeof(((struct dos_data *) 0)->PriPathBuffer) + 3 == ENV_KEEPFREE,
               "ENV_KEEPFREE must track sizeof(PriPathBuffer)+3, see ChildEnv()");

#define LOAD_HIGH 0x80          /* mode bit: try UMB first (see DosUmbLink()) */

ULONG SftGetFsize(int sft_idx)
{
  dos_far_ptr s = idx_to_sft(sft_idx);
  if (far_is_end(s))
    return DE_INVLDHNDL;
  return ((sft*)ARM_PTR(s))->sft_size;
}

/* dsk: 0 = current default drive, 1 = A:, 2 = B:, ... (FCB drive-byte
   convention). Returns NULL if invalid, exactly like get_cds(). */
struct cds *get_cds1(unsigned dsk)
{
  dos_far_ptr p;

  if (dsk == 0)
    dsk = internal_data->default_drive + 1;
  if (dsk == 0)
    return NULL;

  p = get_cds(dsk - 1);
  if (far_is_null(p))
    return NULL;
  return (struct cds *) ARM_PTR(p);
}

/*
 * Compare two SETVER filename fields case-insensitively.
 *
 * This is the original FreeDOS helper adapted only for native pointers:
 * both strings have already been mapped from guest memory by the caller.
 */
STATIC WORD SetverCompareFilename(const BYTE *m1, const BYTE *m2, COUNT count)
{
  while (count--)
  {
    if (toupper((unsigned char)*m1) != toupper((unsigned char)*m2))
      return (WORD)((unsigned char)*m1 - (unsigned char)*m2);

    ++m1;
    ++m2;
  }

  return 0;
}

/*
 * Look up a program basename in the guest SETVER table.
 *
 * Table records are encoded exactly as in FreeDOS:
 *   length byte, filename bytes, minor byte, major byte
 * and the list ends with a zero length byte.
 */
STATIC UWORD SetverGetVersion(dos_far_ptr table_ptr, const BYTE *name)
{
  BYTE *table;
  COUNT name_len;

  if (far_is_null(table_ptr) || name == NULL)
    return 0;

  table = (BYTE *)ARM_PTR(table_ptr);
  name_len = (COUNT)strlen((const char *)name);

  while (*table != 0)
  {
    BYTE len = *table;

    if (len == name_len &&
        SetverCompareFilename(name, table + 1, len) == 0)
      return *(UWORD *)(table + len + 1);

    table += len + 3;
  }

  return 0;
}

/*
   allocate memory for and copy the current process's env to a new
   child environment. Returns the segment of the env's *MCB* (not the
   env block itself) in *pChildEnvSeg.
*/
STATIC COUNT ChildEnv(exec_blk * exp, UWORD * pChildEnvSeg, char *pathname)
{
  BYTE *pSrc;
  BYTE *pDest;
  UWORD nEnvSize;
  COUNT RetCode;
  psp *ppsp = (psp *) ARM_PTR(MK_FP(internal_data->cu_psp, 0));

  *pChildEnvSeg = 0;             /* prevent freeing a random address on
                                     errors by callers of ChildEnv() */

  /* copy parent's environment if exec.env_seg == 0 */
  pSrc = exp->exec.env_seg ?
    (BYTE *) ARM_PTR(MK_FP(exp->exec.env_seg, 0)) :
    (ppsp->ps_environ ? (BYTE *) ARM_PTR(MK_FP(ppsp->ps_environ, 0)) : NULL);
  
  ///printf("ChildEnv\n");
  nEnvSize = 1;
  if (pSrc)
  {                              /* if no environment is available, one
                                     byte is required */
    for (nEnvSize = 0;; nEnvSize++)
    {
      if (nEnvSize >= MAXENV - ENV_KEEPFREE)
        return DE_INVLDENV;

      /* loop until first double terminator '\0\0' found */
      if (*(UWORD *) (pSrc + nEnvSize) == 0)
        break;
    }
    nEnvSize += 2;                /* account for trailing \0\0 */
  }

  /* allocate enough space for env + path (rounding up to nearest
     paragraph); at least 1 paragraph for an empty environment, plus
     ENV_KEEPFREE for argv[0] (the fully-qualified program name) */
  if ((RetCode = DosMemAlloc((nEnvSize + ENV_KEEPFREE + 15) / 16,
                             internal_data->mem_access_mode,
                             pChildEnvSeg, NULL)) < SUCCESS)
    return RetCode;

  pDest = (BYTE *) ARM_PTR(MK_FP(*pChildEnvSeg + 1, 0));      /* skip past MCB */

  if (pSrc)
  {
    memcpy(pDest, pSrc, nEnvSize);
    pDest += nEnvSize;
  }
  else
    *pDest++ = '\0';             /* empty environment */

  /* "extra strings" count (DOS 3.0+: argv[0] follows the env block) */
  *((UWORD *) pDest) = 1;
  pDest += sizeof(UWORD);

  /* copy the fully-qualified program name */
  /* pathname is DosExec()'s "lp": a NATIVE pointer that INT 21h/AH=4Bh built
     as ARM_PTR(guest DS:DX). That guest pointer belongs to the CALLER's
     segment (e.g. FreeCOM's PSP), NOT DOS_PSP - so it must be turned back
     into a far pointer by its true linear address, not re-anchored on
     DOS_PSP. Using x86_FAR_PTR(DOS_PSP, ...) here computes a wrong offset,
     truename() then fails to find the file, and every external command dies
     with "Bad command or filename". This is the one native->far conversion in
     the EXEC path that genuinely needs linear_to_far() until DosExec()/
     ChildEnv() are changed to carry a dos_far_ptr end to end. */
  if ((RetCode = truename(linear_to_far((const BYTE *) pathname),
                          PriPathName, CDS_MODE_SKIP_PHYSICAL)) < SUCCESS) {
    dpb_watch_check_chain("ChildEnv 1");
    return RetCode;
  }
  dpb_watch_check_chain("ChildEnv 2");
  strcpy(pDest, PriPathName);
  dpb_watch_check_chain("ChildEnv 3");

  return SUCCESS;
}

/*
 * Base PSP setup shared by every child: copy the parent PSP wholesale,
 * then replace the fields that must not be inherited.
 *
 * In particular, ps_retdosver starts from the current global DOS version.
 * A SETVER match for the child may override it later in patchPSP(); the
 * parent's own per-program fake version must not leak into the child.
 */
void new_psp(seg para, seg cur_psp)   /* exported: INT 21h AH=26h */
{
  psp *p = (psp *) ARM_PTR(MK_FP(para, 0));

  memcpy(p, ARM_PTR(MK_FP(cur_psp, 0)), sizeof(psp));

  p->ps_isv22 = getvec(0x22);
  p->ps_isv23 = getvec(0x23);
  p->ps_isv24 = getvec(0x24);
  p->ps_retdosver =
      ((UWORD)LoL->os_setver_minor << 8) | LoL->os_setver_major;
}

void child_psp(seg para, seg cur_psp, int psize)   /* exported: INT 21h AH=55h */
{
  psp *p = (psp *) ARM_PTR(MK_FP(para, 0));
  psp *q = (psp *) ARM_PTR(MK_FP(cur_psp, 0));
  /* Parent's JFT. NULL if the parent corrupted its own ps_filetab: the
     child then simply inherits no handles (its own table is already
     filled with 0xff below) instead of us reading 20 bytes out of the
     guest IVT and treating them as SFT indices. */
  UBYTE *q_filetab = jft_of(q);
  int i;

  new_psp(para, cur_psp);

  p->ps_parent = cur_psp;
  p->ps_prevpsp = MK_FP(cur_psp, 0);

  p->ps_size = psize;

  p->ps_maxfiles = 20;
  memset(p->ps_files, 0xff, 20);
  /* Canonical far pair <psp_seg>:0018h. NOT linear_to_far(p->ps_files):
     that normalises to (lin>>4):(lin&0xF), i.e. (psp_seg+1):0008h - the same
     LINEAR address, but a different seg:off pair. Programs and TSRs test the
     pair itself to decide whether the JFT is still the default one inside the
     PSP (that is what SetJFTSize() moves), so the normalised form reads to
     them as "JFT already relocated". Build it from the segment we know. */
  p->ps_filetab = MK_FP(para, offsetof(psp, ps_files));

  /*
   * Inherit the parent's first 20 handles, matching upstream
   * CloneHandle(): handles whose SFT has O_NOINHERIT are deliberately
   * omitted from the child JFT and their SFT reference count is not
   * incremented.
   */
  for (i = 0; q_filetab != NULL && i < 20; i++)
  {
    if (q_filetab[i] != 0xff)
    {
      dos_far_ptr sft_ptr = idx_to_sft(q_filetab[i]);
      if (!far_is_end(sft_ptr))
      {
        sft *entry = (sft *)ARM_PTR(sft_ptr);
        if (!(entry->sft_mode & O_NOINHERIT))
        {
          p->ps_files[i] = q_filetab[i];
          entry->sft_count++;
        }
      }
    }
  }

  p->ps_fcb1.fcb_drive = 0;
  memset(p->ps_fcb1.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);
  p->ps_fcb2.fcb_drive = 0;
  memset(p->ps_fcb2.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);

  p->ps_cmd.ctCount = 0;
  p->ps_cmd.ctBuffer[0] = 0xd;
}

/*
    exec_caller_return_addr() - the address the current INT 21h call will
    return to, i.e. upstream's user_r->CS:IP.

    DosComLoader()/DosExeLoader() publish this as the child's terminate
    vector (INT 22h, mirrored into the child PSP at +0Ah). Upstream reads it
    from the saved INT 21h register frame; the port was reading the LIVE
    CPU_CS:CPU_IP instead, which is not the same thing - by the time a loader
    runs, CS:IP no longer point at the caller's return site.

    internal_data->user_r already holds exactly the frame we need: fdos_21h()
    publishes the guest-visible iregs at PSP:2Eh on every INT 21h and points
    user_r at it (see fdos_21h.c). Read cs/ip back out of it.

    Falls back to the live CS:IP if there is no frame (a loader invoked from
    kernel init rather than from a guest INT 21h).
*/
static dos_far_ptr /* -> caller's return address */ exec_caller_return_addr(void)
{
  dos_far_ptr /* -> struct int21_guest_iregs */ fr = internal_data->user_r;

  if (!far_is_null(fr) && !far_is_end(fr))
  {
    const struct int21_guest_iregs *r =
        (const struct int21_guest_iregs *)ARM_PTR(fr);
    return MK_FP(r->cs, r->ip);
  }
  return MK_FP(CPU_CS, CPU_IP);
}

STATIC UWORD patchPSP(UWORD pspseg, UWORD envseg, exec_blk * exb, BYTE * fnam)
{
  psp *p;
  mcb *pspmcb;
  int i;
  BYTE *np;

  pspmcb = (mcb *) ARM_PTR(MK_FP(pspseg, 0));
  ++pspseg;
  p = (psp *) ARM_PTR(MK_FP(pspseg, 0));

  /* cmd_line/fcb_1/fcb_2 are guest far pointers out of the exec block, so a
     128-byte command tail placed near a segment end must wrap rather than
     read on past it. */
  guest_read(&p->ps_cmd, exb->exec.cmd_line, sizeof(CommandTail));
  /* "No FCBs" is signalled by an OFFSET of FFFFh - the segment is not part
     of the sentinel. Upstream tests exactly that (task.c: "if
     (FP_OFF(exb->exec.fcb_1) != 0xffff)"), and a guest is entitled to pass
     e.g. DS:FFFF. Testing the full FFFF:FFFF pair instead (far_is_end())
     would miss those and memcpy() 32 bytes of whatever ARM_PTR(DS:FFFF)
     lands on straight into the child's PSP FCBs. */
  if (FP_OFF(exb->exec.fcb_1) != 0xFFFF)
  {
    guest_read(&p->ps_fcb1, exb->exec.fcb_1, 16);
    guest_read(&p->ps_fcb2, exb->exec.fcb_2, 16);
  }

  pspmcb->m_psp = pspseg;
  if (envseg)
  {
    ((mcb *) ARM_PTR(MK_FP(envseg, 0)))->m_psp = pspseg;
    envseg++;
  }
  p->ps_environ = envseg;

  /* use the file name less extension, path, and drive */
  np = fnam;
  for (;;)
  {
    switch (*fnam++)
    {
      case '\0':
        goto set_name;
      case ':':
      case '/':
      case '\\':
        np = fnam;
    }
  }
set_name:
  for (i = 0; i < 8 && np[i] != '.' && np[i] != '\0'; i++)
    pspmcb->m_name[i] = toupper((unsigned char) np[i]);
  if (i < 8)
    pspmcb->m_name[i] = '\0';

  /* Per-program DOS version faking (SETVER). Upstream does this here and
     new_psp()/DosExec() both already claim we do too - but the block was
     dropped in the port, leaving SetverGetVersion() with no caller at all
     (-Wunused-function flags it). Restore it.

     LoL->setverPtr is still 0000:0000 until something publishes a SETVER
     table, and SetverGetVersion() returns 0 for a null table, so this is a
     no-op today - but the code path is live again and the comments are no
     longer lying. */
  {
    UWORD fakever = SetverGetVersion(LoL->setverPtr, np);
    if (fakever != 0)
      p->ps_retdosver = fakever;
  }

  /* AX value to be passed to the child, based on FCB drive validity -
     matches upstream's INT21/4B return convention (some old programs
     check this instead of parsing their own command line). */
  return (get_cds1(p->ps_fcb1.fcb_drive) ? 0 : 0xff) |
    (get_cds1(p->ps_fcb2.fcb_drive) ? 0 : 0xff00);
}

/*
   exec_run_child() - block the caller and run the freshly-built child
   process until it terminates, then resume the caller exactly where
   it left off. This is the architectural replacement for upstream's
   exec_user()/return_user()/user_r: those rely on a single, fixed,
   real-hardware "current user register frame" plus a manual IRETD to
   jump between processes, because upstream itself has no other way to
   suspend and resume execution contexts.

   This port doesn't need any of that: since fdos_21h() (and therefore
   every DOS call, including this one) already runs as a plain,
   synchronous, *nested* C function call from inside the CPU core's
   own pc_step() loop (see cpu_far_call()/execrh() in kernel.c for the
   same pattern used to call into loaded device drivers), "suspend the
   caller, run something else, then resume the caller" is just:
     1. save every CPU register (the C call stack itself keeps this
        call's local variables alive - no separate "user stack" is
        needed);
     2. point the CPU at the child's own initial register state and
        keep calling pc_step() until the child signals termination
        (see request_terminate(), called synchronously from the new
        INT 20h/INT 21h AH=00h/4Ch handlers - "synchronously" meaning:
        by the time those handlers run, CS:IP has not yet been pushed
        anywhere, so they can simply set a flag and return, and this
        loop notices it on the very next pc_step());
     3. restore every register saved in step 1 and return.

   Nesting (a running child EXEC-ing a further grandchild) works for
   free: each nested EXEC is just another nested call to this same
   function, and terminate_flag only ever needs to be checked by
   whichever call is currently the innermost one - which is always
   exactly the call whose pc_step() loop is presently executing.
*/
static volatile bool terminate_flag;
static UBYTE term_exit_code, term_exit_type;

struct saved_cpu_ctx
{
  UWORD ax, bx, cx, dx, si, di, bp, sp;
  UWORD cs, ds, es, ss, ip;
  UWORD flags;
};

static void save_ctx(CPU * cpu, struct saved_cpu_ctx *s)
{
  s->ax = CPU_AX; s->bx = CPU_BX; s->cx = CPU_CX; s->dx = CPU_DX;
  s->si = CPU_SI; s->di = CPU_DI; s->bp = CPU_BP; s->sp = CPU_SP;
  s->cs = CPU_CS; s->ds = CPU_DS; s->es = CPU_ES; s->ss = CPU_SS;
  s->ip = CPU_IP; s->flags = cpu_getflags(cpu);
}

static void restore_ctx(CPU * cpu, struct saved_cpu_ctx *s)
{
  SET_SS(s->ss); CPU_SP = s->sp;
  SET_CS(s->cs); SET_IP(s->ip);
  SET_DS(s->ds); SET_ES(s->es);
  CPU_AX = s->ax; CPU_BX = s->bx; CPU_CX = s->cx; CPU_DX = s->dx;
  CPU_SI = s->si; CPU_DI = s->di; CPU_BP = s->bp;
  /* Restore FLAGS exactly. cpu_setflags() is (set_mask, clear_mask)
     applied in that order in both cores, so (s->flags, 0xFFFF) would
     zero everything: bits are set first, then the full clear wipes
     them. Masked in practice only because the final IRET of the
     parent's INT 21h re-pops the real flags - fix it anyway. */
  cpu_setflags(cpu, s->flags, (uword)~s->flags);
}

/* Called synchronously from INT 20h and INT 21h AH=00h/4Ch (see
   fdos_21h.c/fdos_20h() below). exit_type: 0=normal, 1=Ctrl-Break,
   2=critical error abort, 3=TSR (INT 21h AH=31h: the resident block
   was already resized by DosMemChange() in the 31h handler;
   exec_run_child() below keeps the process's memory and open
   handles). */
void request_terminate(UBYTE exit_code, UBYTE exit_type)
{
  term_exit_code = exit_code;
  term_exit_type = exit_type;
  terminate_flag = true;
  /* CRITICAL: stop the innermost pc_step() batch *immediately*, the
     same way intcall_waiter()/cpu_far_call_waiter() do. Without this,
     the CPU core IRETs back into the just-terminated program (to the
     byte right after its INT 20h / INT 21h AH=00h/4Ch) and keeps
     executing whatever garbage follows - for up to the remaining
     ~4095 instructions of the current pc_step(pc, 4096) batch -
     because exec_run_child() only checks terminate_flag *between*
     batches. Programs place the terminate call at the very end of
     their code, so those bytes are data/nothing, and execution
     deterministically walks off into the weeds. Both CPU cores
     (286/cpu.c i286_step() and i386.c) test native_done at the top of
     every instruction iteration and break out at once. */
  cpu->native_done = true;
}

/*
 * Наблюдатель terminate_flag для путей, где upstream-код стоит ПОСЛЕ
 * noreturn-вызова (spawn_int23() в chario.c): порт не может развернуть
 * нативный C-стек прыжком в int21_handler, поэтому вызывающие циклы
 * обязаны сами прекратить I/O, как только терминация запрошена.
 */
bool terminate_requested(void)
{
  return terminate_flag;
}

/*
 * Return AX-packed AL=last exit code, AH=exit type for INT 21h/AH=4Dh.
 *
 * The status is consumed by the read, matching upstream FreeDOS:
 * a second AH=4Dh call returns 0000h until another child terminates.
 */
UWORD DosGetRetCode(void)
{
  UWORD result = term_exit_code | ((UWORD)term_exit_type << 8);
  term_exit_code = 0;
  term_exit_type = 0;
  return result;
}

struct exec_child_context
{
  struct saved_cpu_ctx cpu;
  UWORD cu_psp;
  dos_far_ptr dta;
  UBYTE indos;
  UBYTE error_mode;
  bool terminate;
  bool native_done;
};

static void exec_enter_child(struct exec_child_context *saved,
                             UWORD child_psp_seg, dos_far_ptr stack,
                             UWORD dses)
{
  save_ctx(cpu,&saved->cpu);
  saved->cu_psp=internal_data->cu_psp; saved->dta=internal_data->dta;
  saved->indos=internal_data->InDOS;
  saved->error_mode=internal_data->ErrorMode;
  saved->terminate=terminate_flag;
  /* native_done is a shared signalling channel between three users:
     request_terminate(), bios_intcall()'s waiter and cpu_far_call()'s
     waiter. When this EXEC is entered from inside an OUTER pc_step()
     loop (a guest parent - e.g. a file manager - spawning a native
     COMMAND), the outer level may have its own pending state; it must
     be part of this stack frame, not destroyed by the blanket clears
     that used to live on both the enter and leave paths. */
  saved->native_done=cpu->native_done;
  internal_data->cu_psp=child_psp_seg;
  internal_data->dta=MK_FP(child_psp_seg,offsetof(psp,ps_cmd));
  SET_SS(FP_SEG(stack)); CPU_SP=FP_OFF(stack);
  SET_DS(dses); SET_ES(dses);
  terminate_flag=false;
  /* term_exit_code/term_exit_type are intentionally NOT cleared and NOT
     part of this per-level context: upstream FreeDOS keeps the AH=4Dh
     return status in a single kernel global (the SDA), so starting a
     new child must not erase the status a previous sibling left for our
     caller, and the status the child leaves at its termination must
     survive exec_leave_child() for the parent to read via AH=4Dh. */
  cpu->native_done=false;
  if (internal_data->InDOS != 0) --internal_data->InDOS;
}

static void exec_release_child(UWORD child_psp_seg)
{
  psp *p=(psp *)ARM_PTR(MK_FP(child_psp_seg,0));
  setvec(0x22,p->ps_isv22); setvec(0x23,p->ps_isv23); setvec(0x24,p->ps_isv24);
  if (term_exit_type != 3) {
    int i; for(i=0;i<p->ps_maxfiles;i++) DosClose(i);
    FcbCloseAll(); FreeProcessMem(child_psp_seg);
  }
}

static void exec_leave_child(struct exec_child_context *saved,
                             UWORD child_psp_seg)
{
  cpu->native_done = saved->native_done;
  terminate_flag = saved->terminate;
  internal_data->InDOS = saved->indos;
  /* Match return_user(): suppress recursive critical-error aborts
     while vectors, handles, FCBs and process memory are released. */
  internal_data->abort_progress = (UBYTE)-1;
  exec_release_child(child_psp_seg);
  internal_data->cu_psp = saved->cu_psp;
  internal_data->dta = saved->dta;
  internal_data->abort_progress = 0;
  internal_data->ErrorMode = saved->error_mode;
  restore_ctx(cpu, &saved->cpu);
}

enum exec_process_kind
{
  EXEC_PROCESS_GUEST,
  EXEC_PROCESS_NATIVE_COMMAND
};

struct exec_process_start
{
  dos_far_ptr entry;
  dos_far_ptr stack;
  UWORD dses;
  UWORD ax_bx;
  UWORD child_psp;
  enum exec_process_kind kind;
};

static void exec_set_initial_registers(const struct exec_process_start *start)
{
  SET_CS(FP_SEG(start->entry));
  SET_IP(FP_OFF(start->entry));
  CPU_AX = CPU_BX = start->ax_bx;
  CPU_CX = 0x00ff;
  CPU_DX = start->dses;
  CPU_SI = FP_OFF(start->entry);
  CPU_DI = FP_OFF(start->stack);
  CPU_BP = 0x091e;
  cpu_setflags(cpu, 0x0200, (uword)~0x0200u);
}

static COUNT exec_run_process(const struct exec_process_start *start)
{
  struct exec_child_context saved;

  exec_enter_child(&saved, start->child_psp,
                   start->stack, start->dses);
  exec_set_initial_registers(start);

  if (start->kind == EXEC_PROCESS_NATIVE_COMMAND)
  {
    UBYTE exit_code = fcom_process_main(cpu, start->child_psp);

    /* The native COMMAND has no pc_step() loop of its own, so
       request_terminate() would be a category error here: its
       cpu->native_done = true targets "the innermost ACTIVE pc_step()
       batch" - which at this point is the OUTER loop of a guest
       parent (if any), producing a spurious stop/clear pulse inside
       someone else's CPU loop. All the native process needs is what
       return_user() records for the parent's AH=4Dh: the exit status.
       exec_release_child() below reads term_exit_type for the
       keep-resident decision, so it must be set on this path too. */
    term_exit_code = exit_code;
    term_exit_type = 0;
  }
  else
  {
    while (!terminate_flag)
      pc_step(pc, 4096);
  }

  exec_leave_child(&saved, start->child_psp);
  return SUCCESS;
}

COUNT exec_run_native_command(UWORD child_psp_seg, UWORD fcbcode)
{
  struct exec_process_start start;

  start.entry = MK_FP(child_psp_seg, fcom_process_entry_offset());
  start.stack = MK_FP(child_psp_seg, fcom_process_stack_top());
  start.dses = child_psp_seg;
  start.ax_bx = fcbcode;
  start.child_psp = child_psp_seg;
  start.kind = EXEC_PROCESS_NATIVE_COMMAND;

  return exec_run_process(&start);
}

static COUNT exec_run_child(dos_far_ptr entry, dos_far_ptr stack,
                            UWORD dses, UWORD ax_bx, UWORD child_psp_seg)
{
  struct exec_process_start start;

  start.entry = entry;
  start.stack = stack;
  start.dses = dses;
  start.ax_bx = ax_bx;
  start.child_psp = child_psp_seg;
  start.kind = EXEC_PROCESS_GUEST;

  return exec_run_process(&start);
}

STATIC int load_transfer(UWORD ds, exec_blk * exp, UWORD fcbcode, COUNT mode)
{
  psp *p = (psp *) ARM_PTR(MK_FP(ds, 0));

  p->ps_parent = internal_data->cu_psp;
  p->ps_prevpsp = MK_FP(internal_data->cu_psp, 0);

  if (mode == EXEC_LOADNGO) {
    CfgDbgPrintf(("LOAD psp=%04x entry=%04x:%04x stack=%04x:%04x ds=%04x ax=%04x\n",
                  ds,
                  FP_SEG(exp->exec.start_addr), FP_OFF(exp->exec.start_addr),
                  FP_SEG(exp->exec.stack), FP_OFF(exp->exec.stack),
                  ds, fcbcode));
    return exec_run_child(exp->exec.start_addr, exp->exec.stack, ds, fcbcode, ds);
  }

  /* mode == EXEC_LOAD: don't run it, just hand the caller back the
     entry point/stack we computed (exp->exec.start_addr/stack) plus
     fcbcode pushed onto that stack, matching INT21/4B AL=1. */
  exp->exec.stack.offset -= 2;
  *((UWORD *) ARM_PTR(exp->exec.stack)) = fcbcode;
  return SUCCESS;
}

/* Find out how many paragraphs are available, considering a
   threshold, trying HIGH then LOW memory. */
STATIC int ExecMemLargest(UWORD * asize, UWORD threshold)
{
  int rc;

  if (internal_data->mem_access_mode & 0x80)
  {
    internal_data->mem_access_mode &= ~0x80;
    internal_data->mem_access_mode |= 0x40;
    rc = DosMemLargest(asize);
    internal_data->mem_access_mode &= ~0x40;
    if (rc != SUCCESS || *asize < threshold)
      rc = DosMemLargest(asize);
    internal_data->mem_access_mode |= 0x80;
  }
  else
    rc = DosMemLargest(asize);

  return (*asize < threshold ? DE_NOMEM : rc);
}

STATIC int ExecMemAlloc(UWORD size, seg * para, UWORD * asize)
{
  int rc = DosMemAlloc(size, internal_data->mem_access_mode, para, asize);

  if (rc != SUCCESS)
  {
    if (rc == DE_NOMEM)
    {
      rc = DosMemAlloc(0, LARGEST, para, asize);
      if ((internal_data->mem_access_mode & 0x80) && (rc != SUCCESS))
      {
        internal_data->mem_access_mode &= ~0x80;
        rc = DosMemAlloc(0, LARGEST, para, asize);
        internal_data->mem_access_mode |= 0x80;
      }
    }
  }
  else
    *asize = size;

  if (rc == SUCCESS && *asize < size)
  {
    DosMemFree(*para);
    return DE_NOMEM;
  }
  return rc;
}

COUNT DosComLoader(BYTE * namep, exec_blk * exp, COUNT mode, COUNT fd)
{
  UWORD mem;
  UWORD env = 0, asize = 0;

  {
    UWORD com_size;
    ULONG com_size_long = SftGetFsize(fd);

    /* max 64K - 256 bytes stack - 256 bytes PSP */
    com_size = ((UWORD) min(com_size_long, 0xfe00u) >> 4) + 0x10;

    if ((mode & 0x7f) != EXEC_OVERLAY)
    {
      COUNT rc;
      UBYTE UMBstate = LoL->uppermem_link;
      UBYTE orig_mem_access = internal_data->mem_access_mode;

      if (mode & LOAD_HIGH)
      {
        internal_data->mem_access_mode |= 0x80;
        DosUmbLink(1);
      }

      rc = ChildEnv(exp, &env, namep);

      /* COM files always load into the largest available block */
      if (rc == SUCCESS)
        rc = ExecMemLargest(&asize, com_size);
      if (rc == SUCCESS)
        rc = ExecMemAlloc(asize, &mem, &asize);
      if (rc != SUCCESS)
        DosMemFree(env);

      if (mode & LOAD_HIGH)
      {
        DosUmbLink(UMBstate);
        internal_data->mem_access_mode = orig_mem_access;
        mode &= 0x7f;
      }

      if (rc != SUCCESS)
        return rc;

      ++mem;
    }
    else
      mem = exp->load.load_seg;
  }

  {
    dos_far_ptr sp;

    if (mode == EXEC_OVERLAY)
      sp = MK_FP(mem, 0);
    else
      sp = MK_FP(mem, sizeof(psp));

    /* DOS always loads only the first 64K - sizeof(psp) bytes */
    SftSeek(fd, 0, SEEK_SET);
    DosRWSft(fd, (mode == EXEC_OVERLAY) ? 0xfffeU : 0xff00U, sp, XFR_READ);
    DosCloseSft(fd, FALSE);
  }

  if (mode == EXEC_OVERLAY)
    return SUCCESS;

  {
    UWORD fcbcode;
    psp *p;
    // termination vector (not used by the kernel, but may be used by gues process)
    setvec(0x22, exec_caller_return_addr());
    child_psp(mem, internal_data->cu_psp, mem + asize);
    fcbcode = patchPSP(mem - 1, env, exp, namep);

    if (asize > 0x1000)
      asize = 0x1000;
    if (asize < 0x11)
      return DE_NOMEM;
    asize -= 0x11;

    /* CP/M compatibility: far-call-to-0:00C0h stub encoding the
       segment size, at PSP+5 */
    p = (psp *) ARM_PTR(MK_FP(mem, 0));
    p->ps_reentry = MK_FP(0xc - asize, asize << 4);
    asize <<= 4;
    asize += 0x10e;
    exp->exec.stack = MK_FP(mem, asize);
    exp->exec.start_addr = MK_FP(mem, 0x100);
    *((UWORD *) ARM_PTR(MK_FP(mem, asize))) = 0;
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

COUNT DosExeLoader(BYTE * namep, exec_blk * exp, COUNT mode, COUNT fd)
{
  UWORD mem, env = 0, start_seg, asize = 0;
  UWORD exe_size;
  UWORD image_size;

  image_size = (ExeHeader.exPages << 5) - ExeHeader.exHeaderSize;

  if ((mode & 0x7f) != EXEC_OVERLAY)
  {
    UBYTE UMBstate = LoL->uppermem_link;
    UBYTE orig_mem_access = internal_data->mem_access_mode;
    COUNT rc;

    image_size += sizeof(psp) / 16;
    exe_size = image_size + ExeHeader.exMinAlloc;

    if (exe_size < image_size)   /* overflow: exMinAlloc==0xffff etc. */
      return DE_NOMEM;

    if (mode & LOAD_HIGH)
    {
      DosUmbLink(1);
      internal_data->mem_access_mode |= 0x80;
    }

    rc = ChildEnv(exp, &env, namep);

    if (rc == SUCCESS)
      rc = ExecMemLargest(&asize, exe_size);

    exe_size = image_size + ExeHeader.exMaxAlloc;
    if (exe_size > asize || exe_size < image_size)
      exe_size = asize;

    /* exMinAlloc==exMaxAlloc==0: allocate the largest possible block
       and load the image as high in it as possible */
    if ((ExeHeader.exMinAlloc | ExeHeader.exMaxAlloc) == 0)
      exe_size = asize;

    if (rc == SUCCESS)
      rc = ExecMemAlloc(exe_size, &mem, &asize);
    if (rc != SUCCESS)
      DosMemFree(env);

    if (mode & LOAD_HIGH)
    {
      internal_data->mem_access_mode = orig_mem_access;
      DosUmbLink(UMBstate);
    }
    if (rc != SUCCESS)
      return rc;

    mode &= 0x7f;
    ++mem;
  }
  else
    mem = exp->load.load_seg;

  if (SftSeek(fd, (LONG) ExeHeader.exHeaderSize * 16UL, SEEK_SET) < SUCCESS)
  {
    if (mode != EXEC_OVERLAY)
    {
      DosMemFree(--mem);
      DosMemFree(env);
    }
    return DE_INVLDDATA;
  }

  start_seg = mem;
  exe_size = image_size;
  if (mode != EXEC_OVERLAY)
  {
    exe_size -= sizeof(psp) / 16;
    start_seg += sizeof(psp) / 16;
    if (exe_size > 0 && (ExeHeader.exMinAlloc | ExeHeader.exMaxAlloc) == 0)
    {
      mcb *mp = (mcb *) ARM_PTR(MK_FP(mem - 1, 0));
      start_seg += mp->m_size - image_size;
    }
  }

  /* read the image in CHUNK-sized (paragraph-aligned) pieces,
     advancing the *segment* between reads - see DEVLOAD_CHUNK_PARAS's
     comment in DosExec()'s file-level note for why */
  {
    int nBytesRead, toRead = CHUNK;
    seg sp = start_seg;

    for (;;)
    {
      if (exe_size < CHUNK / 16)
        toRead = exe_size * 16;
      nBytesRead = (int) DosRWSft(fd, toRead, MK_FP(sp, 0), XFR_READ);
      if (nBytesRead < toRead || exe_size <= CHUNK / 16)
        break;
      sp += CHUNK / 16;
      exe_size -= CHUNK / 16;
    }
  }

  {
    COUNT i;
    UWORD *reloc = RelocBuf;

    SftSeek(fd, (LONG) ExeHeader.exRelocTable, SEEK_SET);
    for (i = 0; i < ExeHeader.exRelocItems; i++)
    {
      UWORD *spot;

      if (DosRWSft(fd, sizeof(UWORD) * 2, x86_FAR_PTR(DOS_PSP, reloc) /* -> UWORD[] */,
                   XFR_READ) != sizeof(UWORD) * 2)
      {
        if (mode != EXEC_OVERLAY)
        {
          DosMemFree(--mem);
          DosMemFree(env);
        }
        return DE_INVLDDATA;
      }
      if (mode == EXEC_OVERLAY)
      {
        spot = (UWORD *) ARM_PTR(MK_FP(reloc[1] + mem, reloc[0]));
        *spot += exp->load.reloc;
      }
      else
      {
        spot = (UWORD *) ARM_PTR(MK_FP(reloc[1] + start_seg, reloc[0]));
        *spot += start_seg;
      }
    }
  }

  DosCloseSft(fd, FALSE);

  if (mode == EXEC_OVERLAY)
    return SUCCESS;

  {
    UWORD fcbcode;

    setvec(0x22, exec_caller_return_addr());
    // termination vector (not used by the kernel, but may be used by gues process)
    child_psp(mem, internal_data->cu_psp, mem + asize);
    fcbcode = patchPSP(mem - 1, env, exp, namep);
    exp->exec.stack = MK_FP(ExeHeader.exInitSS + start_seg, ExeHeader.exInitSP);
    exp->exec.start_addr = MK_FP(ExeHeader.exInitCS + start_seg, ExeHeader.exInitIP);
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

static void fcom_copy_exec_tail(char *dst, size_t dst_size, const CommandTail *tail)
{
  size_t count;

  if (dst_size == 0)
    return;

  dst[0] = '\0';
  if (tail == NULL)
    return;

  count = tail->ctCount;
  if (count >= dst_size)
    count = dst_size - 1;

  memcpy(dst, tail->ctBuffer, count);
  dst[count] = '\0';
}

/*
    DosExec() - COUNT DosExec(COUNT mode, exec_blk FAR *ep, BYTE FAR *lp)

    mode: EXEC_LOADNGO (0) - load, build a PSP+environment, and run
    the program, blocking until it terminates (see exec_run_child()).
    EXEC_LOAD (1) - load and build a PSP+environment, but don't run it
    (the caller gets exp->exec.stack/start_addr back and is expected
    to transfer control itself - no caller in this port actually uses
    this mode yet, but it's implemented for completeness/parity with
    the DOS API). EXEC_OVERLAY (3) - load a raw image at a
    caller-supplied segment, no PSP, no execution (this is what
    CONFIG.SYS's DEVICE=/DEVICEHIGH= uses - see LoadDevice() in
    config.c).
*/
COUNT DosExec(COUNT mode, exec_blk * ep, BYTE * lp)
{
  COUNT rc;
  COUNT fd;
  long openresult;
  dos_far_ptr x86_lp;

  if ((mode & 0x7f) == EXEC_LOADNGO &&
      fcom_is_command_com((const char *)lp))
  {
    char tail[sizeof(((CommandTail *)0)->ctBuffer) + 1];
    const CommandTail *command_tail = NULL;
    UWORD child_env_mcb = 0;
    COUNT env_rc;

    if (!far_is_null(ep->exec.cmd_line) &&
        !far_is_end(ep->exec.cmd_line))
      command_tail =
          (const CommandTail *)ARM_PTR(ep->exec.cmd_line);

    /*
     * Match ordinary EXEC semantics: ChildEnv() copies either the explicit
     * EPB environment or, for env_seg == 0, the current process environment,
     * and appends argv[0].  FCOM owns that copy for its whole lifetime.
     */
    env_rc = ChildEnv(ep, &child_env_mcb, (char *)lp);
    if (env_rc < SUCCESS)
      return env_rc;

    {
      UWORD command_psp;
      fcom_copy_exec_tail(tail,sizeof(tail),command_tail);
      UWORD fcbcode;

      command_psp=fcom_create_process(tail,mode & LOAD_HIGH,
                                      internal_data->cu_psp,
                                      child_env_mcb + 1);
      if (command_psp == 0) {
        DosMemFree(child_env_mcb);
        return DE_NOMEM;
      }

      /*
       * A native COMMAND is still an ordinary EXEC child.  patchPSP()
       * installs the exact caller-supplied command tail and FCBs, transfers
       * environment ownership, applies SETVER, and sets the canonical MCB
       * process name.  The first process-model pass skipped this entire
       * loader stage.
       */
      fcbcode=patchPSP(command_psp - 1,child_env_mcb,ep,lp);

      return exec_run_native_command(command_psp,fcbcode);
    }
  }
  
  if ((mode & 0x7f) > EXEC_OVERLAY || (mode & 0x7f) == 2)
    return DE_INVLDFMT;

  memcpy(&TempExeBlock, ep, sizeof(exec_blk));

  /* Same as ChildEnv()/truename() above: "lp" is ARM_PTR(guest DS:DX) from
     INT 21h/AH=4Bh, so it lives in the CALLER's segment (FreeCOM's PSP for an
     external command), not DOS_PSP. Re-anchoring it on DOS_PSP produces a
     bogus offset and DosOpenSft() below then fails to open the executable. */
  x86_lp = linear_to_far((const BYTE *) lp);
  dos_far_ptr x86_dhp = IsDevice(lp);
  if (EFFECTIVE(x86_dhp) ||           /* don't try to "execute" e.g. C:\NUL */
      (openresult = DosOpenSft(x86_lp, O_LEGACY | O_OPEN | O_RDONLY, 0)) < SUCCESS) {
    dpb_watch_check_chain("DosExec err");
    return DE_FILENOTFND;
  }
  dpb_watch_check_chain("DosExec");
  fd = (COUNT) (openresult & 0xffff);

  rc = (int) DosRWSft(fd, sizeof(exe_header),
                      x86_FAR_PTR(DOS_PSP, &ExeHeader) /* -> exe_header */,
                      XFR_READ);

  if (rc == sizeof(exe_header) &&
      (ExeHeader.exSignature == MAGIC || ExeHeader.exSignature == OLD_MAGIC))
    rc = DosExeLoader(lp, &TempExeBlock, mode, fd);
  else if (rc != 0)
    rc = DosComLoader(lp, &TempExeBlock, mode, fd);
  else
  {
    DosCloseSft(fd, FALSE);
    return DE_INVLDFMT;
  }

  if (mode == EXEC_LOAD && rc == SUCCESS)
    memcpy(ep, &TempExeBlock, sizeof(exec_blk));

  return rc;
}

/* res_DosExec() - what a running process calls to EXEC a child
   (P_0() below, for the shell). Upstream's version is a tiny asm
   shim that sets AH=4Bh and self-issues "int 21h" (DOS recursively
   calling its own handler - the same trick init_DosExec() used to
   rely on for CONFIG.SYS's DEVICE= loading, before DosExec() became
   directly callable - see config.c). There's no need for that
   indirection here: DosExec() is an ordinary C function. */
COUNT res_DosExec(COUNT mode, exec_blk * ep, BYTE * lp)
{
  COUNT res = DosExec(mode, ep, lp);
  dpb_watch_check_chain("res_DosExec");
  return res;
}

/* start process 0 (the shell) */
VOID P_0(CPU * cpu_, struct config FAR *Config)
{
  for ( ; ; )   /* endless shell load loop - reboot or shut down to exit it! */
  {
#if GUEST_SHELL
  BYTE *tailp, *endp;
  exec_blk exb;
  UBYTE mode = Config->cfgP_0_startmode;

  /* build exec block and save all parameters here as init part will vanish! */
  exb.exec.fcb_1 = exb.exec.fcb_2 = MK_FP(0xffff, 0xffff);  /* "no FCBs" - see
                                                                far_is_end()/
                                                                patchPSP() */
  exb.exec.env_seg = DOS_PSP + 8;
  fstrcpy(Shell, Config->cfgInit);
  /* join name and tail */
  fstrcpy(Shell + strlen(Shell), Config->cfgInitTail);
  endp =  Shell + strlen(Shell);

    BYTE *p;
    /* if there are no parameters, point to end without "\r\n" */
    if((tailp = strchr(Shell,'\t')) == NULL &&
       (tailp = strchr(Shell, ' ')) == NULL)
        tailp = endp - 2;
    /* shift tail to right by 2 to make room for '\0', ctCount */
    for (p = endp - 1; p >= tailp; p--)
      *(p + 2) = *p;
    /* terminate name and tail */
    *tailp =  *(endp + 2) = '\0';
    /* ctCount: just past '\0' do not count the "\r\n" */
    {
      CommandTail *ct = (CommandTail *)(tailp + 1);
      ct->ctCount = endp - tailp - 2;
      exb.exec.cmd_line = x86_FAR_PTR(DOS_PSP, ct) /* -> CommandTail */;
    }
    CfgDbgPrintf(("EXEC file='%s' tail='%s'\n", Shell, tailp + 2));
    res_DosExec(mode, &exb, Shell);
    /* only reached once the shell terminates (or couldn't be
       started at all) - matches upstream: P_0's loop always falls
       through here and reprompts, exactly like real DOS does if
       COMMAND.COM itself exits. */
    put_string("Bad or missing Command Interpreter: "); /* failure _or_ exit */
    put_string(Shell);
    put_string(tailp + 2);
    put_string(" Enter the full shell command line: ");
    endp = Shell + res_read(cpu_, STDIN, x86_FAR_PTR(DOS_PSP, Shell) /* -> char[] */, NAMEMAX);
    *endp = '\0';                             /* terminate string for strchr */
#else
    /*
     * One fcom_run() call represents one COMMAND process lifetime.
     * If it exits, recreate process 0 just as the original P_0 loop
     * recreated COMMAND.COM.
     * Config->cfgP_0_startmode - ignored, try HMA/UMB anytime
     */
    fcom_run(cpu_, (const char *)Config->cfgInitTail, 0x80, DOS_PSP + 8, 0);
    put_string("Native COMMAND.COM restarting as process #0 with parameters: ");
    put_string((BYTE *)Config->cfgInitTail);
#endif
  }
  __unreachable();
}
