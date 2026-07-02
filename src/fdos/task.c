#include "hdrs.h"

#define ExeHeader (*(exe_header *)(SecPathName + 0))
#define TempExeBlock (*(exec_blk *)(SecPathName + sizeof(exe_header)))
#define Shell (SecPathName + sizeof(exe_header) + sizeof(exec_blk))

/* Paragraphs per chunk when loading a multi-segment device driver
   image: 32256 bytes (== 2016 paragraphs), same chunk size upstream
   FreeDOS's DosExeLoader() uses. Reading (and, for EXE images,
   relocating) in paragraph-sized chunks and advancing the *segment*
   between chunks - rather than reading the whole image in one shot
   at a growing offset - keeps every transfer's destination safely
   inside one 64K segment, however large the driver image is. */
#define DEVLOAD_CHUNK_PARAS (32256 / 16)

/*
    DosExec() - COUNT DosExec(COUNT mode, exec_blk FAR *ep, BYTE FAR *lp)

    Scope note (this port): only mode==EXEC_OVERLAY (AL=3, "load
    overlay") is implemented here.

    That is deliberate, not an oversight: EXEC_OVERLAY is the only
    mode CONFIG.SYS's DEVICE=/DEVICEHIGH=/INSTALL= driver loading
    needs (see LoadDevice() in config.c) - DOS always loads device
    drivers with AL=3: load the raw image (applying EXE relocations,
    if any) at a caller-supplied segment, and return. No PSP, no
    environment block, no transfer of control. This mirrors upstream
    FreeDOS's DosComLoader()/DosExeLoader() OVERLAY path.

    mode==EXEC_LOADNGO/EXEC_LOAD (0/1: full EXEC - allocate memory,
    build a PSP and environment, and for LOADNGO actually transfer
    control to the loaded program) is *not* implemented: that needs a
    process/task-switch layer (child_psp()/patchPSP()/ExecMemAlloc()/
    exec_user()/return_user() in upstream FreeDOS) that does not exist
    anywhere in this port yet - see the "res_DosExec()" TODO in P_0()
    below, which is what actually starts COMMAND.COM. Rather than
    silently perform a partial (and wrong) EXEC, callers asking for
    those modes get DE_INVLDFMT back. EXEC_LOADNGO/EXEC_LOAD/
    EXEC_OVERLAY are defined in hdr/process.h, shared with config.c's
    LoadDevice()/InstallExec().
*/
COUNT DosExec(COUNT mode, exec_blk FAR * ep, BYTE FAR * lp)
{
  int sft_idx;
  long openresult;
  long rc;
  dos_far_ptr x86_lp;
  UWORD load_seg;

  if (mode != EXEC_OVERLAY)
    return DE_INVLDFMT;                /* see scope note above */

  x86_lp = linear_to_far((const BYTE *)lp);
  load_seg = ep->load.load_seg;
  
  memcpy(&TempExeBlock, ep, sizeof(exec_blk));

  openresult = DosOpenSft(x86_lp, O_LEGACY | O_OPEN | O_RDONLY, 0);
  if (openresult < SUCCESS)
    return (COUNT) openresult;
  sft_idx = (int)(openresult & 0xffff);

  rc = DosRWSft(sft_idx, sizeof(exe_header), linear_to_far((BYTE FAR *)&ExeHeader), XFR_READ);
 
  if (rc == sizeof(exe_header) && (ExeHeader.exSignature == MAGIC || ExeHeader.exSignature == OLD_MAGIC))
  {
    /* MZ/EXE-style image: skip the header, load the body verbatim at
       load_seg:0, then patch every relocation table entry by adding
       ep->load.reloc (== load_seg, as set by LoadDevice()) to the
       word already there - exactly upstream's OVERLAY relocation
       semantics, just without ever allocating memory of our own. */
    UWORD paras_left = (ExeHeader.exPages << 5) - ExeHeader.exHeaderSize;
    UWORD sp = load_seg;
    int i;

    if (SftSeek(sft_idx, (LONG) ExeHeader.exHeaderSize * 16UL, SEEK_SET) < SUCCESS)
      goto baddata;

    while (paras_left)
    {
      UWORD chunk = paras_left > DEVLOAD_CHUNK_PARAS ? DEVLOAD_CHUNK_PARAS : paras_left;
      if (DosRWSft(sft_idx, (size_t) chunk * 16u, MK_FP(sp, 0), XFR_READ)
            != (long)chunk * 16)
        goto baddata;
      sp += chunk;
      paras_left -= chunk;
    }

    if (SftSeek(sft_idx, (LONG) ExeHeader.exRelocTable, SEEK_SET) < SUCCESS)
      goto baddata;

    for (i = 0; i < ExeHeader.exRelocItems; i++)
    {
      UWORD FAR *reloc = (UWORD FAR *)((BYTE FAR *)&ExeHeader + sizeof(exe_header));
      UWORD FAR *spot;

      if (DosRWSft(sft_idx, 2 * sizeof(UWORD), linear_to_far((BYTE FAR *)reloc), XFR_READ) != 2 * sizeof(UWORD))
        goto baddata;

      spot = (UWORD FAR *) ARM_PTR(MK_FP(reloc[1] + load_seg, reloc[0]));
      *spot += ep->load.reloc;
    }
  }
  else if (rc > 0)
  {
    /* COM-style: no header, no relocations - the whole file is the
       image, loaded verbatim at load_seg:0. Real device drivers (and
       COM files in general) are always well under 64K, so a single
       transfer is enough; matches DosComLoader()'s OVERLAY path. */
    if (SftSeek(sft_idx, 0, SEEK_SET) < SUCCESS)
      goto baddata;
    if (DosRWSft(sft_idx, 0xfffe, MK_FP(load_seg, 0), XFR_READ) < 0)
      goto baddata;
  }
  else
  {
    goto baddata;
  }

  DosCloseSft(sft_idx, FALSE);
  memcpy(ep, &TempExeBlock, sizeof(exec_blk));
  return SUCCESS;

baddata:
  DosCloseSft(sft_idx, FALSE);
  return DE_INVLDDATA;
}

/* start process 0 (the shell) */
VOID P_0(CPU* cpu, struct config FAR *Config)
{
  BYTE *tailp, *endp;
  exec_blk exb;
  UBYTE mode = Config->cfgP_0_startmode;

  /* build exec block and save all parameters here as init part will vanish! */
  exb.exec.fcb_1 = exb.exec.fcb_2 = (fcb FAR *)-1L;
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
    exb.exec.cmd_line = (CommandTail *)(tailp + 1);
    exb.exec.cmd_line->ctCount = endp - tailp - 2;
#ifdef DEBUG
    DebugPrintf(("Process 0 starting: %s%s\n\n", Shell, tailp + 2));
#endif
/// TODO:    res_DosExec(mode, &exb, Shell);
    if (is_guest_ptr(Shell)) {
        put_string("Bad or missing Command Interpreter: "); /* failure _or_ exit */
        put_string(Shell);
        put_string(tailp + 2);
        put_string(" Enter the full shell command line: ");
        endp = Shell + res_read(cpu, STDIN, linear_to_far(Shell), NAMEMAX);
        *endp = '\0';                             /* terminate string for strchr */
    } else {
        printf("PANIC: Shell pointer is native...\n");
        while(1);
    }
  }
  __unreachable();
}
