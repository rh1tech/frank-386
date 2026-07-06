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
  sft *s = idx_to_sft(sft_idx);

  if (s == (sft *) - 1)
    return DE_INVLDHNDL;

  return s->sft_size;
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
   allocate memory for and copy the current process's env to a new
   child environment. Returns the segment of the env's *MCB* (not the
   env block itself) in *pChildEnvSeg.

   Simplification vs upstream: no SETVER database support (upstream's
   SetverGetVersion()/SetverCompareFilename(), consulted from
   patchPSP() below) - faking a DOS version for specific program names
   is a rarely-needed compatibility knob, not something programs need
   in order to run at all, so it's left out of this pass entirely.
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
  if ((RetCode = truename(linear_to_far((const BYTE *) pathname), PriPathName,
                          CDS_MODE_SKIP_PHYSICAL)) < SUCCESS)
    return RetCode;
  strcpy(pDest, PriPathName);

  return SUCCESS;
}

/* base PSP setup shared by every child: copy the parent PSP wholesale
   (matching upstream's new_psp()), then fix up the fields that must
   differ. */
STATIC void new_psp(seg para, seg cur_psp)
{
  psp *p = (psp *) ARM_PTR(MK_FP(para, 0));

  memcpy(p, ARM_PTR(MK_FP(cur_psp, 0)), sizeof(psp));

  p->ps_isv22 = getvec(0x22);
  p->ps_isv23 = getvec(0x23);
  p->ps_isv24 = getvec(0x24);
}

STATIC void child_psp(seg para, seg cur_psp, int psize)
{
  psp *p = (psp *) ARM_PTR(MK_FP(para, 0));
  psp *q = (psp *) ARM_PTR(MK_FP(cur_psp, 0));
  UBYTE *q_filetab = (UBYTE *) ARM_PTR(q->ps_filetab);
  int i;

  new_psp(para, cur_psp);

  p->ps_parent = cur_psp;
  p->ps_prevpsp = MK_FP(cur_psp, 0);

  p->ps_size = psize;

  p->ps_maxfiles = 20;
  memset(p->ps_files, 0xff, 20);
  p->ps_filetab = linear_to_far(p->ps_files);

  /* Inherit all of the parent's open handles.

     Simplification vs upstream: CloneHandle() (which checks a
     per-handle "don't inherit across EXEC" bit, set via IOCTL) isn't
     implemented, so every open handle is always inherited - the
     common case; the "don't inherit" bit is a rarely-used opt-out
     most programs never set. */
  for (i = 0; i < 20; i++)
  {
    if (q_filetab[i] != 0xff)
    {
      p->ps_files[i] = q_filetab[i];
      idx_to_sft(p->ps_files[i])->sft_count++;
    }
  }

  p->ps_fcb1.fcb_drive = 0;
  memset(p->ps_fcb1.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);
  p->ps_fcb2.fcb_drive = 0;
  memset(p->ps_fcb2.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);

  p->ps_cmd.ctCount = 0;
  p->ps_cmd.ctBuffer[0] = 0xd;
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

  memcpy(&p->ps_cmd, ARM_PTR(exb->exec.cmd_line), sizeof(CommandTail));
  if (!far_is_end(exb->exec.fcb_1))
  {
    memcpy(&p->ps_fcb1, ARM_PTR(exb->exec.fcb_1), 16);
    memcpy(&p->ps_fcb2, ARM_PTR(exb->exec.fcb_2), 16);
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
  cpu_setflags(cpu, s->flags, 0xFFFF);
}

/* Called synchronously from INT 20h and INT 21h AH=00h/4Ch (see
   fdos_21h.c/fdos_20h() below). exit_type: 0=normal, 1=Ctrl-Break,
   2=critical error abort, 3=TSR (INT 21h AH=31h - not wired up in
   this pass, so exit_type is currently always 0). */
void request_terminate(UBYTE exit_code, UBYTE exit_type)
{
  term_exit_code = exit_code;
  term_exit_type = exit_type;
  terminate_flag = true;
}

/* Returns AX-packed AL=last exit code, AH=exit type, matching INT 21h
   AH=4Dh. Simplification vs upstream: not "consumed" on read (real
   DOS clears it after one read) - harmless, since callers only ever
   read it once, right after EXEC returns, before starting anything
   else. */
UWORD DosGetRetCode(void)
{
  return term_exit_code | ((UWORD) term_exit_type << 8);
}

static COUNT exec_run_child(dos_far_ptr entry, dos_far_ptr stack,
                            UWORD dses, UWORD ax_bx, UWORD child_psp_seg)
{
  struct saved_cpu_ctx parent_ctx;
  UWORD saved_cu_psp = internal_data->cu_psp;
  dos_far_ptr saved_dta = internal_data->dta;
  bool saved_terminate_flag = terminate_flag;

  save_ctx(cpu, &parent_ctx);

  internal_data->cu_psp = child_psp_seg;
  internal_data->dta = MK_FP(child_psp_seg, offsetof(psp, ps_cmd));

  SET_SS(FP_SEG(stack));  CPU_SP = FP_OFF(stack);
  SET_CS(FP_SEG(entry));  SET_IP(FP_OFF(entry));
  SET_DS(dses);            SET_ES(dses);
  CPU_AX = CPU_BX = ax_bx;
  CPU_CX = 0xff;
  CPU_SI = FP_OFF(entry);
  CPU_DI = FP_OFF(stack);
  CPU_BP = 0x091e;               /* matches upstream: some programs
                                     expect 0x09 in BP's high byte */
  ifl = 1; /* IF=1, everything else clear (including CF - the child starts "successful") */

  terminate_flag = false;
  while (!terminate_flag)
    pc_step(pc, 4096);
  terminate_flag = saved_terminate_flag;

  /* --- child terminated: free its resources (return_user()'s
     equivalent, minus upstream's real-hardware vector-restore dance -
     see the function-level comment above for why it isn't needed
     here) --- */
  if (term_exit_type != 3)       /* not a TSR (not wired up yet, always
                                     false for now, kept for parity
                                     with upstream/future INT21 31h) */
  {
    psp *p = (psp *) ARM_PTR(MK_FP(child_psp_seg, 0));
    int i;

    for (i = 0; i < p->ps_maxfiles; i++)
      DosClose(i);
    FreeProcessMem(child_psp_seg);
  }

  internal_data->cu_psp = saved_cu_psp;
  internal_data->dta = saved_dta;
  restore_ctx(cpu, &parent_ctx);

  return SUCCESS;
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

      if (DosRWSft(fd, sizeof(UWORD) * 2, linear_to_far((BYTE *) reloc),
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

    child_psp(mem, internal_data->cu_psp, mem + asize);
    fcbcode = patchPSP(mem - 1, env, exp, namep);
    exp->exec.stack = MK_FP(ExeHeader.exInitSS + start_seg, ExeHeader.exInitSP);
    exp->exec.start_addr = MK_FP(ExeHeader.exInitCS + start_seg, ExeHeader.exInitIP);
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
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

  if ((mode & 0x7f) > EXEC_OVERLAY || (mode & 0x7f) == 2)
    return DE_INVLDFMT;

  memcpy(&TempExeBlock, ep, sizeof(exec_blk));

  x86_lp = linear_to_far((const BYTE *) lp);

  if (IsDevice(lp) ||           /* don't try to "execute" e.g. C:\NUL */
      (openresult = DosOpenSft(x86_lp, O_LEGACY | O_OPEN | O_RDONLY, 0)) < SUCCESS)
    return DE_FILENOTFND;
  fd = (COUNT) (openresult & 0xffff);

  rc = (int) DosRWSft(fd, sizeof(exe_header), linear_to_far((BYTE *) &ExeHeader),
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
  return DosExec(mode, ep, lp);
}

/* start process 0 (the shell) */
VOID P_0(CPU * cpu_, struct config FAR *Config)
{
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

  for ( ; ; )   /* endless shell load loop - reboot or shut down to exit it! */
  {
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
      exb.exec.cmd_line = linear_to_far(ct);
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
    endp = Shell + res_read(cpu_, STDIN, linear_to_far(Shell), NAMEMAX);
    *endp = '\0';                             /* terminate string for strchr */
  }
  __unreachable();
}
