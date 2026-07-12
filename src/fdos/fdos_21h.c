#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"

int snprintf(char *s, size_t n, const char *fmt, ...);
static void dpb_watch_int21_checkpoint(CPU* cpu, const char *where)
{
    static char tags[16][40];
    static unsigned tag_idx;

    char *tag = tags[tag_idx++ & 15];
    snprintf(tag, 40, "INT21-%s AH=%02x AL=%02x", where, CPU_AH, CPU_AL);
    dpb_watch_check_chain(tag);
}

#ifdef NO_HANDLER_DETECTOR
static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 21H - ERROR: no handler defined ");
while(1); // remove it
    return true;
}
#endif

/*
 * Minimal critical-error backend until the original INT 24h/user-stack
 * trampoline from entry.asm is ported.
 *
 * Return FAIL to the caller without modifying the live CPU register
 * frame.  INT 21h dispatch uses a separate local register frame, while
 * INT 2Fh callers that require AL explicitly store this return value.
 */
COUNT ASMCFUNC CriticalError(COUNT nFlag, COUNT nDrive, COUNT nError,
                             struct dhdr FAR *lpDevice)
{
  UNREFERENCED_PARAMETER(nFlag);
  UNREFERENCED_PARAMETER(nDrive);
  UNREFERENCED_PARAMETER(nError);
  UNREFERENCED_PARAMETER(lpDevice);
  return FAIL;
}

/* Abort, retry or fail for character devices                   */
COUNT char_error(request * rq, struct dhdr FAR * lpDevice)
{
  /// TODO:
  internal_data->CritErrCode = (rq->r_status & S_MASK) + 0x13;
  return CriticalError(EFLG_CHAR | EFLG_ABORT | EFLG_RETRY | EFLG_IGNORE,
                       0, rq->r_status & S_MASK, lpDevice);
}

/* Abort, retry or fail for block devices                       */
COUNT block_error(request * rq, COUNT nDrive, struct dhdr FAR * lpDevice,
                  int mode)
{
  /// TODO:
  internal_data->CritErrCode = (rq->r_status & S_MASK) + 0x13;
  return CriticalError(EFLG_ABORT | EFLG_RETRY | EFLG_IGNORE |
                       (mode == DSKWRITE ? EFLG_WRITE : 0),
                       nDrive, rq->r_status & S_MASK, lpDevice);
}

/* common - call the clock driver */
void ExecuteClockDriverRequest(BYTE command)
{
  BinaryCharIO(&LoL->clock, sizeof(struct ClockRecord), linear_to_far(&internal_data->ClkRecord), command);
}

const UWORD days[2][13] = {
  {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
  {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}
};

/*
    return a pointer to an array with the days for that year
*/

const UWORD *is_leap_year_monthdays(UWORD y)
{
  /* this is correct in a strict mathematical sense   
     return ((y) & 3 ? days[0] : (y) % 100 ? days[1] : (y) % 400 ? days[0] : days[1]); */

  /* this will work until 2200 - long enough for me - and saves 0x1f bytes */

  if ((y & 3) || y == 2100)
    return days[0];

  return days[1];
}

static unsigned char DosGetDateRegs(UWORD *out_year, UBYTE *out_month, UBYTE *out_day)
{
  UWORD c;
  const UWORD *pdays;
  UWORD Year, Month;

  ExecuteClockDriverRequest(C_INPUT);

  if (CharReqHdr.r_status & S_ERROR)
    return 0;

  for (Year = 1980, c = internal_data->ClkRecord.clkDays;;)
  {
    pdays = is_leap_year_monthdays(Year);
    if (c >= pdays[12])
    {
      ++Year;
      c -= pdays[12];
    }
    else
      break;
  }

  /* c contains the days left and count the number of days for    */
  /* that year.  Use this to index the table.                     */
  Month = 1;
  while (c >= pdays[Month])
  {
    ++Month;
  }

  *out_year = Year;
  *out_month = (UBYTE)Month;
  *out_day = (UBYTE)(c - pdays[Month - 1] + 1);

  /* Day of week is simple. Take mod 7, add 2 (for Tuesday        */
  /* 1-1-80) and take mod again                                   */

  return (internal_data->ClkRecord.clkDays + 2) % 7;
}

unsigned char DosGetDate(CPU *cpu)
{
  UWORD year;
  UBYTE month, day;
  unsigned char dow = DosGetDateRegs(&year, &month, &day);

  CPU_CX = year;
  CPU_DH = month;
  CPU_DL = day;
  return dow;
}

UWORD DaysFromYearMonthDay(UWORD Year, UWORD Month, UWORD DayOfMonth)
{
  if (Year < 1980)
    return 0;

  return DayOfMonth - 1
      + is_leap_year_monthdays(Year)[Month - 1]
      + ((Year - 1980) * 365) + ((Year - 1980 + 3) / 4);

}

static int DosSetDateRegs(UWORD Year, UWORD Month, UWORD DayOfMonth)
{
  const UWORD *pdays = is_leap_year_monthdays(Year);

  if (Year < 1980 || Year > 2099
      || Month < 1 || Month > 12
      || DayOfMonth < 1
      || DayOfMonth > pdays[Month] - pdays[Month - 1])
    return DE_INVLDDATA;

  ExecuteClockDriverRequest(C_INPUT);

  internal_data->ClkRecord.clkDays = DaysFromYearMonthDay(Year, Month, DayOfMonth);

  ExecuteClockDriverRequest(C_OUTPUT);

  if (CharReqHdr.r_status & S_ERROR)
    return char_error(&CharReqHdr, (struct dhdr*)ARM_PTR(LoL->clock));
  return SUCCESS;
}

int DosSetDate(CPU *cpu)
{
  return DosSetDateRegs(CPU_CX, CPU_DH, CPU_DL);
}

static void DosGetTimeRegs(UBYTE *out_hour, UBYTE *out_minute, UBYTE *out_second, UBYTE *out_hundredth)
{
  ExecuteClockDriverRequest(C_INPUT);

  if (CharReqHdr.r_status & S_ERROR)
    return;

  *out_hour = internal_data->ClkRecord.clkHours;
  *out_minute = internal_data->ClkRecord.clkMinutes;
  *out_second = internal_data->ClkRecord.clkSeconds;
  *out_hundredth = internal_data->ClkRecord.clkHundredths;
}

void DosGetTime(CPU *cpu)
{
  UBYTE hour, minute, second, hundredth;

  DosGetTimeRegs(&hour, &minute, &second, &hundredth);
  CPU_CH = hour;
  CPU_CL = minute;
  CPU_DH = second;
  CPU_DL = hundredth;
}

static int DosSetTimeRegs(UBYTE hour, UBYTE minute, UBYTE second, UBYTE hundredth)
{
  if (hour > 23 || minute > 59 || second > 59 || hundredth > 99)
     return DE_INVLDDATA;
 
  /* for ClkRecord.clkDays */
  ExecuteClockDriverRequest(C_INPUT);

  internal_data->ClkRecord.clkHours = hour;
  internal_data->ClkRecord.clkMinutes = minute;
  internal_data->ClkRecord.clkSeconds = second;
  internal_data->ClkRecord.clkHundredths = hundredth;

  ExecuteClockDriverRequest(C_OUTPUT);

  if (CharReqHdr.r_status & S_ERROR)
    return char_error(&CharReqHdr, (struct dhdr*)ARM_PTR(LoL->clock));
  return SUCCESS;
}

int DosSetTime(CPU *cpu)
{
  return DosSetTimeRegs(CPU_CH, CPU_CL, CPU_DH, CPU_DL);
}

/*
 * Return a CDS entry by zero-based drive index without validating its
 * flags or DPB.
 *
 * This mirrors upstream FreeDOS get_cds_unvalidated(): internal DOS
 * services use it when they need the CDS slot itself, including disabled,
 * JOINed or not-yet-completely-initialized entries.
 */
struct cds FAR *get_cds_unvalidated(unsigned drive)
{
  if (drive >= LoL->lastdrive || far_is_null(LoL->CDSp))
    return NULL;

  return (struct cds FAR *)ARM_PTR(LoL->CDSp) + drive;
}

/* get current directory structure for drive
   return NULL if the CDS is not valid or the
   drive is not within range */
dos_far_ptr/*struct cds*/ get_cds(unsigned drive)
{
  if (drive >= LoL->lastdrive)
    return MK_FP(0, 0);
  if (far_is_null(LoL->CDSp))
    return MK_FP(0, 0);

  struct cds* CDSp = (struct cds*)ARM_PTR(LoL->CDSp) + drive;
  unsigned flags = CDSp->cdsFlags;
  /* Entry is disabled or JOINed drives are accessable by the path only */
  if (!(flags & CDSVALID) || (flags & CDSJOINED) != 0)
    return MK_FP(0, 0);
  if (!(flags & CDSNETWDRV) && EFFECTIVE(CDSp->cdsDpb) == 0)
    return MK_FP(0, 0);
  return x86_FAR_PTR(FP_SEG(LoL->CDSp), CDSp);
}

UBYTE DosSelectDrv(UBYTE drv)
{
  internal_data->current_ldt = get_cds(drv);

  if (FP_OFF(internal_data->current_ldt) != 0xFFFF)
    internal_data->default_drive = drv;
  
  return LoL->lastdrive;
}

static int fcb_parse_common_sep(int c)
{
  return c != 0 && strchr(":;,=+ \t", c) != NULL;
}
 
static int fcb_parse_field_sep(int c)
{
  return (unsigned char)c <= ' ' || strchr("/\\\"[]<>|.:;,=+\t", c) != NULL;
}
 
static const BYTE *fcb_parse_skip_wh(const BYTE *src)
{
  while (*src == ' ' || *src == '\t')
    ++src;
  return src;
}
 
static const BYTE *fcb_parse_name_field(const BYTE *src, BYTE *dst,
                                        COUNT field_size, BOOL *wild)
{
  COUNT i = 0;
  BYTE fill = ' ';

  while (*src != 0 && !fcb_parse_field_sep(*src) && i < field_size)
  {
    if (*src == '*')
    {
      *wild = TRUE;
      fill = '?';
      ++src;
      break;
    }
    if (*src == '?')
      *wild = TRUE;

    *dst++ = DosUpFChar(*src++);
    ++i;
  }
 
  memset(dst, fill, field_size - i);
  return src;
}

static UBYTE DosParseFilenameIntoFcbRegs(UBYTE mode, dos_far_ptr srcp, dos_far_ptr fcbp, UWORD *next_si)
{
  const BYTE *base = (const BYTE *)ARM_PTR(srcp);
  const BYTE *src = base;
  fcb *dst = (fcb *)ARM_PTR(fcbp);
  BOOL bad_drive = FALSE;
  BOOL wild_name = FALSE;
  BOOL wild_ext = FALSE;
 
  if (mode & 0x01)
    while (fcb_parse_common_sep(*src))
      ++src;

  /* MS-DOS skips spaces and tabs even without PARSE_SKIP_LEAD_SEP. */
  src = fcb_parse_skip_wh(src);

  if (!fcb_parse_field_sep(*src) && src[1] == ':')
  {
    UBYTE drive = DosUpFChar(*src) - 'A';
 
    /* Keep parsing even for an invalid drive, as DOS does. */
    dos_far_ptr cds_ = get_cds(drive);
    if (far_is_null(cds_))
      bad_drive = TRUE;
 
    dst->fcb_drive = drive + 1;
    src += 2;
  }
  else if (!(mode & 0x02))
  {
    dst->fcb_drive = 0;
  }

  /* Undocumented DOS behaviour: these two fields are always reset. */
  dst->fcb_cublock = 0;
  dst->fcb_recsiz = 0;

  if (!(mode & 0x04))
    memset(dst->fcb_fname, ' ', FNAME_SIZE);
  if (!(mode & 0x08))
    memset(dst->fcb_fext, ' ', FEXT_SIZE);

  /* Special names '.' and '..' return immediately, without extension. */
  if (*src == '.')
  {
    dst->fcb_fname[0] = '.';
    ++src;
    if (*src == '.')
     {
      dst->fcb_fname[1] = '.';
      ++src;
    }
    *next_si = FP_OFF(srcp) + (UWORD)(src - base);
    return 0;
  }
 
  src = fcb_parse_name_field(src, dst->fcb_fname, FNAME_SIZE, &wild_name);
 
  if (*src == '.')
    src = fcb_parse_name_field(src + 1, dst->fcb_fext, FEXT_SIZE, &wild_ext);
 
  *next_si = FP_OFF(srcp) + (UWORD)(src - base);
 
  if (bad_drive)
    return 0xff;
  return (wild_name || wild_ext) ? 1 : 0;
}

/* INT 21h is dispatched against a local register frame, as in upstream
 * FreeDOS int21_service().  Guest BIOS/device calls may use the live CPU as
 * scratch state, but cannot leak register changes into this frame. */
#define R_AX    regs->gprx[regax].r16
#define R_BX    regs->gprx[regbx].r16
#define R_CX    regs->gprx[regcx].r16
#define R_DX    regs->gprx[regdx].r16
#define R_SI    regs->gprx[regsi].r16
#define R_DI    regs->gprx[regdi].r16
#define R_BP    regs->gprx[regbp].r16
#define R_AL    regs->gprx[regax].r8[0]
#define R_AH    regs->gprx[regax].r8[1]
#define R_BL    regs->gprx[regbx].r8[0]
#define R_BH    regs->gprx[regbx].r8[1]
#define R_CL    regs->gprx[regcx].r8[0]
#define R_CH    regs->gprx[regcx].r8[1]
#define R_DL    regs->gprx[regdx].r8[0]
#define R_DH    regs->gprx[regdx].r8[1]
#define R_DS    regs->ds
#define R_ES    regs->es
#define R_FS    regs->fs
#define R_GS    regs->gs
#define R_CF    regs->flags.bits.CF
#define R_ZF    regs->flags.bits.ZF
#define R_FP_DS_DX MK_FP(R_DS, R_DX)
#define R_FP_DS_SI MK_FP(R_DS, R_SI)
#define R_FP_ES_DI MK_FP(R_ES, R_DI)

#ifdef WITHFAT32
static COUNT int21_fat32_regs(CPU_regs *regs)
{
  COUNT rc;

  switch (R_AL)
  {
    /* Get extended drive parameter block */
    case 0x02:
    {
      struct xdpbdata FAR *xddp;

      if (R_CX < sizeof(struct xdpbdata))
        return DE_INVLDBUF;

      dos_far_ptr _dpb = GetDriveDPB(R_DL, &rc);
      if (rc != SUCCESS)
        return rc;

      struct dpb* dpb = (struct dpb*)ARM_PTR(_dpb);
      flush_buffers(dpb->dpb_unit);
      dpb->dpb_flags = M_CHANGED;

      if (media_check_tagged(_dpb, "INT21/7302/GetDriveDPB") < 0)
        return DE_INVLDDRV;

      xddp = (struct xdpbdata FAR *)ARM_PTR(R_FP_ES_DI);
      memcpy(&xddp->xdd_dpb, dpb, sizeof(struct dpb));
      xddp->xdd_dpbsize = sizeof(struct dpb);

      if (!ISFAT32(dpb) && dpb->dpb_xsize != dpb->dpb_size)
      {
        xddp->xdd_dpb.dpb_nfreeclst_un.dpb_nfreeclst_st.dpb_nfreeclst_hi =
          (dpb->dpb_nfreeclst == 0xFFFF ? 0xFFFF : 0);
        dpb16to32(&xddp->xdd_dpb);
        xddp->xdd_dpb.dpb_xfatsize = dpb->dpb_fatsize;
        xddp->xdd_dpb.dpb_xcluster =
          (dpb->dpb_cluster == 0xFFFF ? 0xFFFFFFFFuL : dpb->dpb_cluster);
      }
      break;
    }

    /* Get extended free drive space */
    case 0x03:
    {
      struct xfreespace FAR *xfsp = (struct xfreespace FAR *)ARM_PTR(R_FP_ES_DI);

      if (R_CX < sizeof(struct xfreespace))
        return DE_INVLDBUF;

      rc = DosGetExtFree((BYTE FAR *)ARM_PTR(R_FP_DS_DX), xfsp);
      if (rc != SUCCESS)
        return rc;
      break;
    }

    /* Set DPB to use for formatting */
    case 0x04:
    {

      if (R_CX < sizeof(struct xdpbforformat))
        return DE_INVLDBUF;

      dos_far_ptr _dpb = GetDriveDPB(R_DL, &rc);
      if (rc != SUCCESS)
        return rc;

      struct xdpbforformat FAR *xdffp = (struct xdpbforformat FAR *)ARM_PTR(R_FP_ES_DI);
      xdffp->xdff_datasize = sizeof(struct xdpbforformat);
      xdffp->xdff_version.actual = 0;

      struct dpb* dpb = (struct dpb*)ARM_PTR(_dpb);
      switch ((UWORD)xdffp->xdff_function)
      {
        case 0x00:
        {
          ULONG nfreeclst = xdffp->xdff_f.setdpbcounts.nfreeclst;
          ULONG cluster = xdffp->xdff_f.setdpbcounts.cluster;
          if (ISFAT32(dpb))
          {
            if ((dpb->dpb_xfsinfosec == 0xffff && (nfreeclst != 0 || cluster != 0))
                || nfreeclst == 1 || nfreeclst > dpb->dpb_xsize
                || cluster == 1 || cluster > dpb->dpb_xsize)
              return DE_INVLDPARM;
            dpb->dpb_xnfreeclst = nfreeclst;
            dpb->dpb_xcluster = cluster;
            write_fsinfo(dpb);
          }
          else
          {
            if ((unsigned)nfreeclst == 1 || (unsigned)nfreeclst > dpb->dpb_size ||
                (unsigned)cluster == 1 || (unsigned)cluster > dpb->dpb_size)
              return DE_INVLDPARM;
            dpb->dpb_nfreeclst = (UWORD)nfreeclst;
            dpb->dpb_cluster = (UWORD)cluster;
          }
          break;
        }

        case 0x01:
        {
          ddt *pddt = getddt(R_DL);
          memcpy(&pddt->ddt_bpb, ARM_PTR(xdffp->xdff_f.rebuilddpb.bpbp), sizeof(bpb));
        }
        /* fall through */
        case 0x02:
rebuild_dpb:
          flush_buffers(dpb->dpb_unit);
          dpb->dpb_flags = M_CHANGED;
          if (media_check_tagged(_dpb, "INT21/7304/GetDriveDPB") < 0)
            return DE_INVLDDRV;
          break;

        case 0x03:
        case 0x04:
        {
          ULONG value;
          if (!ISFAT32(dpb))
            return DE_INVLDPARM;

          value = xdffp->xdff_f.setget.new;
          if ((UWORD)xdffp->xdff_function == 0x03)
          {
            if (value != 0xFFFFFFFFUL && (value & ~(0xf | 0x80)))
              return DE_INVLDPARM;
            xdffp->xdff_f.setget.old = dpb->dpb_xflags;
          }
          else
          {
            if (value != 0xFFFFFFFFUL && (value < 2 || value > dpb->dpb_xsize))
              return DE_INVLDPARM;
            xdffp->xdff_f.setget.old = dpb->dpb_xrootclst;
          }

          if (value != 0xFFFFFFFFUL)
          {
            bpb FAR *bpbp;
            struct buffer FAR *bp = getblock(1, dpb->dpb_unit);
            bp->b_flag &= ~(BFR_DATA | BFR_DIR | BFR_FAT);
            bp->b_flag |= BFR_VALID | BFR_DIRTY;
            bpbp = (bpb FAR *)&bp->b_buffer[BT_BPB];
            if ((UWORD)xdffp->xdff_function == 0x03)
              bpbp->bpb_xflags = (UWORD)value;
            else
              bpbp->bpb_xrootclst = value;
          }
          goto rebuild_dpb;
        }

        default:
          return DE_INVLDFUNC;
      }
      break;
    }

    /* Extended absolute disk read/write */
    case 0x05:
    {
      BYTE FAR *SectorBlock = (BYTE FAR *)ARM_PTR(MK_FP(R_DS, R_BX));
      ULONG blkno;
      UWORD nblks;
      dos_far_ptr bufp;
      UBYTE mode;

      if (R_CX != 0xffff || (R_SI & ~0x6001))
        return DE_INVLDPARM;

      if (R_DL > LoL->lastdrive || R_DL == 0)
        return -0x207;

      blkno =  (ULONG)SectorBlock[0]
             | ((ULONG)SectorBlock[1] << 8)
             | ((ULONG)SectorBlock[2] << 16)
             | ((ULONG)SectorBlock[3] << 24);
      nblks =  (UWORD)SectorBlock[4]
             | ((UWORD)SectorBlock[5] << 8);
      bufp = MK_FP((UWORD)(SectorBlock[8] | ((UWORD)SectorBlock[9] << 8)),
                   (UWORD)(SectorBlock[6] | ((UWORD)SectorBlock[7] << 8)));
        
      mode = ((R_SI & 1) == 0) ? DSKREADINT25 : DSKWRITEINT26;

      R_AX = dskxfer(R_DL - 1, blkno, bufp, nblks, mode);

      if (mode == DSKWRITEINT26 && R_AX == 0)
        setinvld(R_DL - 1);

      if (R_AX > 0)
        return -0x20c;
      break;
    }

    default:
      return DE_INVLDFUNC;
  }

  return SUCCESS;
}

static COUNT int21_fat32(void)
{
  CPU_regs regs;
  COUNT rc;

  cpu_save_regs(cpu, &regs);
  rc = int21_fat32_regs(&regs);
  cpu_restore_regs(cpu, &regs);
  return rc;
}
#endif

/*
DOS 1+ - main DOS handler
*/
bool fdos_21h(CPU* _cpu) {
    COUNT rc;
    CPU_regs lr;
    CPU_regs *regs = &lr;
    UWORD entry_ss, entry_sp;

    cpu = _cpu;
    entry_ss = CPU_SS;
    entry_sp = CPU_SP;
    cpu_save_regs(_cpu, regs);
    uint16_t flags_on_stack = readw86(((uint32_t)entry_ss << 4) + entry_sp + 4);
    regs->flags.value = (regs->flags.value & ~0x0041u) | (flags_on_stack & 0x0041u);
    internal_data->Int21AX = R_AX;
    ++internal_data->InDOS;
    dpb_watch_int21_checkpoint(cpu, "entry");
    /* STI: real DOS re-enables interrupts first thing in its INT 21h
       entry stub (FreeDOS entry.asm does "sti" right after the stack
       switch), because the INT dispatch itself cleared IF. This port
       must do the same: while this native handler runs, any x86
       stepping it triggers (bios_intcall() polling INT 16h from the
       CON device, execrh()/cpu_far_call() into loaded drivers, and
       exec_run_child() for AH=4Bh) inherits the live IF flag - and
       with IF=0, IRQ1 is never delivered, bios_09h never runs, the
       BDA keyboard buffer never fills, and every INT 16h AH=01h poll
       reports "empty" forever (dead keyboard at the COMMAND.COM
       prompt). The caller's own IF is untouched: it is restored from
       flags_on_stack by the final IRET, exactly as on real hardware. */
    ifl = 1;
dispatch:                       /* re-entry point for AH=5Dh AL=00h
                                   (remote server call), matching the
                                   original inthndlr.c dispatch: label */
    switch (R_AH) {
      /* Read Keyboard With Echo                                      */
      case 0x01:
      DOS_01:
        R_AL = read_char_stdin(TRUE);
        write_char_stdout(R_AL);
        break;

      case 0x02:
        write_char_stdout(R_AL);
        R_AL = (R_DL == HT) ? ' ' : R_DL;
        break;

      /* Auxiliary Input                                              */
      case 0x03:
      {
        int sft_idx = get_sft_idx(STDAUX);
        R_AL = read_char(sft_idx, sft_idx, TRUE);
      }
        break;

      /* Auxiliary Output                                             */
      case 0x04:
        write_char(R_DL, get_sft_idx(STDAUX));
        break;

      /* Print Character                                              */
      case 0x05:
        write_char(R_DL, get_sft_idx(STDPRN));
        break;

      /* Direct Console I/O                                           */
      case 0x06:
      DOS_06:
        if (R_DL != 0xff)
        {
          R_AL = R_DL;
          write_char_stdout(R_AL);
          break;
        }
        R_AL = 0x00;
        R_ZF = 1;
        if (StdinBusy())
        {
          DosIdle_int();
          break;
        }
        R_ZF = 0;
        /* fall through */

      /* Direct Console Input                                         */
      case 0x07:
      DOS_07:
        R_AL = read_char_stdin(FALSE);
        break;

      /* Read Keyboard Without Echo                                   */
      case 0x08:
      DOS_08:
        R_AL = read_char_stdin(TRUE);
        break;

      /* Buffered Keyboard Input                                      */
      case 0x0a:
      DOS_0A:
        read_line(get_sft_idx(STDIN), get_sft_idx(STDOUT), (keyboard *)ARM_PTR(R_FP_DS_DX));
        break;

      /* Check Stdin Status                                           */
      case 0x0b:
        R_AL = 0xFF;
        if (StdinBusy())
          R_AL = 0x00;
        break;

      /* Flush Buffer, Read Keyboard                                  */
      case 0x0c:
      {
        dos_far_ptr dev = sft_to_dev((sft*) ARM_PTR ( get_sft(STDIN) ) );
        if (FP_SEG(dev) || FP_OFF(dev))
          con_flush(&dev);
        switch (R_AL)
        {
          case 0x01: goto DOS_01;
          case 0x06: goto DOS_06;
          case 0x07: goto DOS_07;
          case 0x08: goto DOS_08;
          case 0x0a: goto DOS_0A;
        }
        R_AL = 0x00;
      }
        break;

      /* Display String                                               */
      case 0x09:
        {
          unsigned char c;
          unsigned char FAR *bp = ARM_PTR( R_FP_DS_DX );

          while ((c = *bp++) != '$')
            write_char_stdout(c);

          R_AL = c;
        }
        break;

      case 0x0E: // set drive
        R_AL = DosSelectDrv(R_DL);
        break;

        /* Get default drive                                           */
      case 0x19:
        R_AL = internal_data->default_drive;
        break;

      case 0x1A: // set DTA
        internal_data->dta = R_FP_DS_DX;
        break;

      case 0x29: /* DOS 1+ - PARSE FILENAME INTO FCB */
        R_AL = DosParseFilenameIntoFcbRegs(R_AL, MK_FP(R_DS, R_SI), MK_FP(R_ES, R_DI), &R_SI);
        break;

        /* Set Interrupt Vector                                         */
      case 0x25:
      {
        /* AL = interrupt number, DS:DX = new handler. */
        pstore16((uint32_t)R_AL * 4u, R_DX);
        pstore16((uint32_t)R_AL * 4u + 2u, R_DS);
        R_CF = 0;
      }
        break;

        /* Get Date                                                     */
      case 0x2a:
        R_AL = DosGetDateRegs(&R_CX, &R_DH, &R_DL);
        break;

        /* Set Date                                                     */
      case 0x2b:
        R_AL = DosSetDateRegs(R_CX, R_DH, R_DL) == SUCCESS ? 0 : 0xFF;
        break;

        /* Get Time                                                     */
      case 0x2c:
        DosGetTimeRegs(&R_CH, &R_CL, &R_DH, &R_DL);
        break;

        /* Set Time                                                     */
      case 0x2d:
        R_AL = DosSetTimeRegs(R_CH, R_CL, R_DH, R_DL) == SUCCESS ? 0 : 0xFF;
        break;
        // get DTA
      case 0x2f:
        R_BX = FP_OFF(internal_data->dta);
        R_ES = (FP_SEG(internal_data->dta));
        break;

      /* Get (editable) DOS Version                                   */
      case 0x30:
      {
        if (R_AL == 1) /* from RBIL, if AL=1 then return version_flags */
            R_BH = LoL->version_flags;
        else
            R_BH = OEM_ID;
        psp *p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
        UWORD ver = p->ps_retdosver;

        /* PSP мог быть создан не через child_psp() (нативный загрузчик FreeCOM)
           — тогда поле нулевое. Фолбэк на реальную версию ядра. */
        if ((ver & 0x00FF) == 0)
            ver = ((UWORD)LoL->os_setver_minor << 8) | LoL->os_setver_major;

        R_BH = (R_AL == 1) ? LoL->version_flags : OEM_ID;
        R_AX = ver;
        R_BL = REVISION_SEQ;
        R_CX = 0; /* do not set this to a serial number!
                      32RTM won't like non-zero values   */

        if (ReturnAnyDosVersionExpected)
        {
          /* TE for testing purpose only and NOT
            to be documented:
            return programs, who ask for version == XX.YY
            exactly this XX.YY.
            this makes most MS programs more happy.
          */
          UBYTE FAR *retp = ARM_PTR ( MK_FP(CPU_CS, CPU_IP) );

          if (retp[0] == 0x3d &&  /* cmp ax, xxyy */
              (retp[3] == 0x75 || retp[3] == 0x74))       /* je/jne error    */
          {
            R_AL = retp[1];
            R_AH = retp[2];
          }
          else if (retp[0] == 0x86 &&     /* xchg al,ah   */
                  retp[1] == 0xc4 && retp[2] == 0x3d &&  /* cmp ax, xxyy */
                  (retp[5] == 0x75 || retp[5] == 0x74))  /* je/jne error    */
          {
            R_AL = retp[4];
            R_AH = retp[3];
          }

        }
      }
      break;
        /* Get Interrupt Vector                                         */
      case 0x35:
      {
        /* AL = interrupt number.  Return current vector in ES:BX.
           Upstream handles this in the re-entrant INT 21h front path as
           p = getvec(AL); ES = FP_SEG(p); BX = FP_OFF(p). */
        uint32_t vec = (uint32_t)R_AL * 4u;
        R_BX = pload16(vec);
        R_ES = (pload16(vec + 2u));
        R_CF = 0;
      }
        break;

      case 0x37: /* DOS 2+ - SWITCHAR - GET/SET SWITCH CHARACTER */
        switch (R_AL) {
        case 0x00:              /* get switch character */
          R_DL = internal_data->switchar;
          R_AL = 0x00;
          break;
        case 0x01:              /* set switch character */
          internal_data->switchar = R_DL;
          R_AL = 0x00;
          break;
        default:
          R_AL = 0xff;
          break;
        }
        break;
/// TODO: ensure
#if 1
        /* Get/Set Country Info                                         */
      case 0x38:
        {
          UWORD cntry = R_AL;

          if (cntry == 0xff)
            cntry = R_BX;

          if (0xffff == R_DX)
          {
            /* Set Country Code */
            rc = DosSetCountry(cntry);
          }
          else
          {
            if (cntry == 0)
              cntry--;
            /* Get Country Information */
            rc = DosGetCountryInformation(cntry, ARM_PTR ( R_FP_DS_DX ) );
            if (rc >= SUCCESS)
            {
              if (cntry == (UWORD) - 1) {
                struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
                cntry = ((struct nlsPackage *)ARM_PTR(nlsInfo->actPkg))->cntry;
              }
              R_AX = R_BX = cntry;
            }
          }
          goto short_check;
        }
#endif
      /* Create file                                                  */
      /* AH=3Ch had no dispatcher entry at all -
         and until dos_open()'s O_CREAT/O_TRUNC branches were implemented
         (see fatfs.c), wiring it wouldn't have helped anyway. Now that
         both exist, this mirrors inthndlr.c's "case 0x3c" exactly,
         same long_check convention as case 0x3d below. */
      case 0x3c:
      {
        long result = DosOpen(R_FP_DS_DX, O_LEGACY | O_RDWR | O_CREAT | O_TRUNC, R_CL);
        if (result < SUCCESS)
        {
          R_CF = 1;
          R_AX = (UWORD)(-result);
        }
        else
        {
          R_CF = 0;
          R_AX = (UWORD)result;
        }
      }
        break;

      case 0x3d: // DOS 2+ - OPEN - OPEN EXISTING FILE
      {
        /* DS:DX = ASCIIZ pathname, AL = access mode.
           Migrated from inthndlr.c's "case 0x3d" (DosOpen(R_FP_DS_DX,
           O_LEGACY | O_OPEN | lr.AL, 0)). On success: CF=0, AX=handle.
           On failure: CF=1, AX=DOS error code (negated, per the
           kernel-wide convention - see init_DosOpen()/dup2() above,
           which already expect this). */
        long result = DosOpen(R_FP_DS_DX, O_LEGACY | O_OPEN | R_AL, 0);
        if (result < SUCCESS)
        {
          R_CF = 1;
          R_AX = (UWORD)(-result);
        }
        else
        {
          R_CF = 0;
          R_AX = (UWORD)result;
        }
      }
        break;

      case 0x3e: // DOS 2+ - CLOSE - CLOSE FILE
      {
        /* BX = file handle. Migrated from inthndlr.c's "case 0x3e"
           (DosClose(lr.BX), "short_check": AX=-rc, CF=1 on error;
           CF=0, AX unchanged on success - DosClose() itself doesn't
           return a value the caller cares about on success). */
        int result = DosClose(R_BX);
        if (result < SUCCESS) {
          R_CF = 1;
          R_AX = (UWORD)(-result);
        } else {
          R_CF = 0;
        }
      }
        break;

     case 0x3f: // DOS 2+ - READ - READ FROM FILE OR DEVICE
      {
        /* BX = file handle, CX = byte count, DS:DX = buffer.
           Migrated from inthndlr.c's "case 0x3f" (DosRead(lr.BX,
           lr.CX, R_FP_DS_DX)), same long_check convention as case 0x3d
           above: CF=0/AX=bytes-read on success, CF=1/AX=-rc on
           error. */
        long result = DosRead(R_BX, R_CX, R_FP_DS_DX);
        if (result < SUCCESS)
        {
          R_CF = 1;
          R_AX = (UWORD)(-result);
        }
        else
        {
          R_CF = 0;
          R_AX = (UWORD)result;
        }
      }
        break;

      case 0x40: // DOS 2+ - WRITE - WRITE TO FILE OR DEVICE
      {
        /* BX = file handle, CX = byte count, DS:DX = buffer.
           Migrated from inthndlr.c's "case 0x40" (DosWrite(lr.BX,
           lr.CX, R_FP_DS_DX)), same long_check convention as case 0x3f:
           CF=0/AX=bytes-written on success, CF=1/AX=-rc on error. */
        long result = DosWrite(R_BX, R_CX, R_FP_DS_DX);
        if (result < SUCCESS)
        {
          R_CF = 1;
          R_AX = (UWORD)(-result);
        }
        else
        {
          R_CF = 0;
          R_AX = (UWORD)result;
        }
      }
        break;
 
      /* Make directory                                                */
      /* Remove directory                                               */
      /* classic top-level entry points were missing;
         DosMkRmdir() already exists and is used by the AH=43h/AL=FF path -
         see inthndlr.c "case 0x39: case 0x3a: rc = DosMkRmdir(R_FP_DS_DX, lr.AH);" */
      case 0x39:
      case 0x3a:
        rc = DosMkRmdir(R_FP_DS_DX, R_AH);
        goto short_check;

      /* Rename file (classic entry point) */
      /* DosRename() already exists (used by the
         AH=43h/AL=FF/CL=56h path); wire the standard AH=56h entry point too -
         see inthndlr.c "case 0x56: rc = DosRename(R_FP_DS_DX, R_FP_ES_DI);" */
      case 0x56:
        rc = DosRename(R_FP_DS_DX, R_FP_ES_DI);
        goto short_check;

      /* Change directory                                             */
      /* DosChangeDir() was declared but never
         implemented in this port at all - AH=3Bh had no backend. */
      case 0x3b:
        rc = DosChangeDir(R_FP_DS_DX);
        goto short_check;

      /* Delete file                                                  */
      /* DosDelete() was declared but never
         implemented in this port at all - AH=41h had no backend. */
      case 0x41:
        rc = DosDelete(R_FP_DS_DX, D_ALL);
        goto short_check;

      /* Find first matching file                                     */
      case 0x4e:
        rc = DosFindFirst(R_CX, R_FP_DS_DX);
        goto short_check;

      /* Find next matching file                                      */
      case 0x4f:
        rc = DosFindNext();
        goto short_check;
        
      /* Get/Set File Attributes                                      */
      case 0x43:
        switch (R_AL)
        {
          case 0x00:
            rc = DosGetFattr(R_FP_DS_DX);
            if (rc >= SUCCESS)
              R_CX = rc;
            break;

          case 0x01:
            rc = DosSetFattr(R_FP_DS_DX, R_CX);
            R_AX = R_CX;
            break;

          case 0xff: /* DOS 7.20 (w98) extended name (128 char length) functions */
          {
            switch(R_CL)
            {
                  /* Dos Create Directory                                         */
                  case 0x39:
                  /* Dos Remove Directory                                         */
                  case 0x3a:
                    rc = DosMkRmdir(R_FP_DS_DX, R_CL);
                    goto short_check;

                  /* Dos rename file */
                  case 0x56:
                    rc = DosRename(R_FP_DS_DX, R_FP_ES_DI);
                    goto short_check;

                /* fall through to goto error_invaid */
            }
          }
          default:
            goto error_invalid;
        }
        goto short_check;
        /* Device I/O Control                                           */
      case 0x44:
      {
        CPU_regs live_saved;

        /* DosDevIOctl() still has the historical port-specific API that
         * reads and writes live CPU_* registers.  Isolate that API here;
         * the rest of INT 21h uses the local frame. */
        cpu_save_regs(_cpu, &live_saved);
        cpu_restore_regs(_cpu, regs);
        rc = DosDevIOctl();      /* can set critical error code! */
        cpu_save_regs(_cpu, regs);
        cpu_restore_regs(_cpu, &live_saved);

        if (rc < SUCCESS)
        {
          R_AX = -rc;
          if (rc != DE_DEVICE && rc != DE_ACCESS)
            internal_data->CritErrCode = R_AX;
          goto error_carry;
        }
        R_CF = 0;
      }
        break;

      case 0x45: /* DOS 2+ - DUP - DUPLICATE FILE HANDLE */
      {
        unsigned old_hndl = R_BX;
        psp *p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
        UBYTE *filetab = (UBYTE *)ARM_PTR(p->ps_filetab);
        dos_far_ptr old_sft;
        unsigned new_hndl;

        if (old_hndl >= p->ps_maxfiles || filetab[old_hndl] == 0xff)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        old_sft = idx_to_sft(filetab[old_hndl]);
        if (far_is_end(old_sft) ||
            (((sft *)ARM_PTR(old_sft))->sft_mode & O_NOINHERIT))
        {
          cf = 1;
          CPU_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        for (new_hndl = 0; new_hndl < p->ps_maxfiles; new_hndl++)
        {
          if (filetab[new_hndl] == 0xff)
            break;
        }

        if (new_hndl >= p->ps_maxfiles)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_TOOMANY);
          break;
        }

        filetab[new_hndl] = filetab[old_hndl];
        ((sft *)ARM_PTR(old_sft))->sft_count++;

        R_AX = (UWORD)new_hndl;
        R_CF = 0;
      }
        break;

      case 0x46: // DOS 2+ - DUP2, FORCEDUP - FORCE DUPLICATE FILE HANDLE
      // BX = existing handle (old), CX = handle to redirect (new)
      {
        unsigned old_hndl = R_BX;
        unsigned new_hndl = R_CX;
        psp *p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
        UBYTE *filetab = (UBYTE *) ARM_PTR(p->ps_filetab);

        if (old_hndl >= p->ps_maxfiles || filetab[old_hndl] == 0xff)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }
        if (new_hndl >= p->ps_maxfiles)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }
        if (new_hndl != old_hndl)
        {
          /* close new handle if open */
          if (filetab[new_hndl] != 0xff)
          {
            COUNT close_rc = DosClose(new_hndl);
            if (close_rc < SUCCESS)
            {
              cf = 1;
              CPU_AX = (UWORD)(-close_rc);
              break;
            }
          }
          /* copy SFT index and bump ref count */
          filetab[new_hndl] = filetab[old_hndl];
          {
            dos_far_ptr old_sft = idx_to_sft(filetab[old_hndl]);
            if (far_is_end(old_sft))
            {
              cf = 1;
              CPU_AX = (UWORD)(-DE_INVLDHNDL);
              break;
            }
            ((sft *)ARM_PTR(old_sft))->sft_count++;
          }
        }
        R_CF = 0;
      }
        break;

      case 0x47: /* DOS 2+ - CWD - GET CURRENT DIRECTORY */
        rc = DosGetCuDir(R_DL, MK_FP(R_DS, R_SI));
        goto short_check;

        /* Set PSP                                                      */
      case 0x50:
        internal_data->cu_psp = R_BX;
        break;

      case 0x52: { // DOS 2+ internal - SYSVARS - GET LIST OF LISTS -> ES:BX -> DOS list of lists (see #01627)
          /*
          * DOS 2+ GET LIST OF LISTS.
          *
          * Return ES:BX pointing at LoL->DPBp (MARK0026H).
          * Documented consumers access:
          *
          *   ES:[BX-2] = first MCB segment
          *
          * Do not duplicate the fixed-data and structure offsets here.
          */
          R_ES = (FP_SEG(x86_FIXED_DATA));
          R_BX = FP_OFF(x86_FIXED_DATA) + offsetof(struct lol, DPBp); // see MARK0026H
        }
        break;
// 53h — Translate BIOS
        /* Get PSP                                                      */
      case 0x51: // DOS 2+ internal - GET CURRENT PROCESS ID (GET PSP ADDRESS)
      case 0x62: // DOS 3.0+ - GET CURRENT PSP ADDRESS
        R_BX = internal_data->cu_psp;
        break;

      case 0x60: /* DOS 3+ - TRUENAME - canonicalize filename/path */
        rc = DosTruename(MK_FP(R_DS, R_SI), R_FP_ES_DI);
        R_AX = rc;
        goto short_check;

        /* Extended country information / NLS functions                */
      case 0x65:
        switch (R_AL)
        {
          case 0x20:             /* upcase single character */
            R_DL = DosUpChar(R_DL);
            R_CF = 0;
            break;
          case 0x21:             /* upcase memory area */
            DosUpMem(ARM_PTR(R_FP_DS_DX), R_CX);
            R_CF = 0;
            break;
          case 0x22:             /* upcase ASCIZ */
            DosUpString((char FAR *)ARM_PTR(R_FP_DS_DX));
            R_CF = 0;
            break;
          case 0xA0:             /* upcase single filename character */
            R_DL = DosUpFChar(R_DL);
            R_CF = 0;
            break;
          case 0xA1:             /* upcase filename memory area */
            DosUpFMem(ARM_PTR(R_FP_DS_DX), R_CX);
            R_CF = 0;
            break;
          case 0xA2:             /* upcase filename ASCIZ */
            DosUpFString((char FAR *)ARM_PTR(R_FP_DS_DX));
            R_CF = 0;
            break;
          case 0x23:             /* check Yes/No response */
            R_AX = DosYesNo(R_DL);
            CfgDbgPrintf(("INT21/65%02x YESNO in DL=%02x -> AX=%04x\n",
                          R_AL, R_DL, R_AX));            
            R_CF = 0;
            break;
          default: {
            #if DEBUG
            UBYTE subfct = R_AL;
            UWORD in_bx = R_BX;
            UWORD in_dx = R_DX;
            UWORD in_cx = R_CX;
            UWORD in_es = R_ES;
            UWORD in_di = R_DI;
            #endif
            rc = DosGetData(R_AL, R_BX, R_DX, R_CX, ARM_PTR(R_FP_ES_DI));
            #if DEBUG
            CfgDbgPrintf(("INT21/65%02x GetData bx=%04x dx=%04x cx=%04x es:di=%04x:%04x -> rc=%d ax=%04x R_CF=%d\n",
                          subfct, in_bx, in_dx, in_cx, in_es, in_di,
                          rc, R_AX, R_CF));
            #endif
            goto short_check;
          }
        }
        break;

        /* Dos Seek                                                     */
      case 0x42: // DOS 2+ - LSEEK - MOVE FILE POINTER
      {
        ULONG result;

        /* BX = file handle, CX:DX = signed 32-bit offset, AL = origin:
           0 = start, 1 = current position, 2 = end of file.

           Upstream FreeDOS does the AL range check in the INT 21h
           dispatcher before calling DosSeek(); keep that behaviour here
           instead of relying only on SftSeek2(), so invalid modes report
           DE_INVLDFUNC through the normal INT 21h error path. */
        if (R_AL > 2)
          goto error_invalid;

        result = DosSeek(R_BX,
                         (LONG)(((UDWORD)R_CX << 16) | R_DX),
                         R_AL,
                         &rc);
        if (rc == SUCCESS) {
          R_DX = (UWORD)(result >> 16);
          R_AX = (UWORD)result;
        }
        goto short_check;
      }


        /* Terminate process (old-style, CP/M-compatible; same as
           INT 20h - see fdos_20h() - and equivalent to AH=4Ch with
           AL=0) */
      case 0x00:
        request_terminate(0, 0);
        R_CF = 0;
        break;

        /* Terminate process with return code                          */
      case 0x4c:
        request_terminate(R_AL, 0);
        R_CF = 0;
        break;

        /* Get return code (ERRORLEVEL)                                 */
      case 0x4d:
        R_AX = DosGetRetCode();
        R_CF = 0;
        break;

        /* EXEC - load and/or execute a program                        */
      case 0x4b: {
          exec_blk *ep = (exec_blk *) ARM_PTR(MK_FP(R_ES, R_BX));
          BYTE *lp = (BYTE *) ARM_PTR(R_FP_DS_DX);
          rc = DosExec(R_AL, ep, lp);
          dpb_watch_check_chain("0x4b");
          if (rc < SUCCESS)
          {
            R_AX = (UWORD) (-rc);
            R_CF = 1;
          }
          else
            R_CF = 0;
        }
        break;

        /* Allocate memory                                              */
      case 0x48: {
          seg para;
          UWORD asize = 0;

          rc = DosMemAlloc(R_BX, internal_data->mem_access_mode, &para, &asize);
#ifdef INT21_DIAG
          printf("MEM 48 by %04x:%04x bx=%04x -> rc=%d seg=%04x max=%04x\n",
                 readw86((CPU_SS << 4) + CPU_SP + 2),
                 readw86((CPU_SS << 4) + CPU_SP),
                 R_BX, rc, (UWORD)(para + 1), asize);
#endif
          if (rc < SUCCESS)
          {
            R_BX = asize;
            R_AX = (UWORD) (-rc);
            R_CF = 1;
          }
          else
          {
            R_AX = para + 1;  /* segment of the usable block, not the MCB itself */
            R_CF = 0;
          }
        }
        break;

        /* Free memory                                                  */
      case 0x49:
        rc = DosMemFree(R_ES - 1);
#ifdef INT21_DIAG
        printf("MEM 49 by %04x:%04x es=%04x -> rc=%d\n",
               readw86((CPU_SS << 4) + CPU_SP + 2),
               readw86((CPU_SS << 4) + CPU_SP), R_ES, rc);
#endif
        if (rc < SUCCESS)
        {
          /*
           * Upstream verifies the complete MCB chain when a free fails.
           * Report structural corruption as DE_MCBDESTRY instead of
           * returning the less specific invalid-MCB error.
           */
          if (DosMemCheck() != SUCCESS)
            rc = DE_MCBDESTRY;

          R_AX = (UWORD) (-rc);
          R_CF = 1;
        }
        else
          R_CF = 0;
        break;

        /* Resize (grow/shrink) an allocated memory block               */
      case 0x4a: {
          UWORD maxsize = 0;
          /*
           * Upstream checks the chain before resizing.  A damaged chain
           * must not be modified further; return the normal DOS MCB
           * corruption error rather than entering the original panic path.
           */
          rc = DosMemCheck();
          if (rc < SUCCESS)
          {
            R_BX = 0;
            R_AX = (UWORD)(-rc);
            R_CF = 1;
            break;
          }

          rc = DosMemChange(R_ES, R_BX, &maxsize);
#ifdef INT21_DIAG
          printf("MEM 4A by %04x:%04x es=%04x bx=%04x -> rc=%d max=%04x\n",
                 readw86((CPU_SS << 4) + CPU_SP + 2),
                 readw86((CPU_SS << 4) + CPU_SP),
                 R_ES, R_BX, rc, maxsize);
#endif
          if (rc < SUCCESS)
          {
            R_BX = maxsize;
            R_AX = (UWORD) (-rc);
            R_CF = 1;
          }
          else
            R_CF = 0;
        }
        break;
      /* Get/Set File Date and Time                                   */
      case 0x57:
        switch (R_AL)
        {
          case 0x00:
            rc = DosGetFtime((COUNT)R_BX, (ddate*)&R_DX, (dtime*)&R_CX);
            break;

          case 0x01:
            rc = DosSetFtime((COUNT)R_BX, (ddate)R_DX, (dtime)R_CX);
            break;

          default:
            rc = DE_INVLDFUNC;
        }
        goto short_check;

        /* Get/Set memory allocation strategy, get/set UMB link state   */
      case 0x58:
        switch (R_AL)
        {
          case 0x00:            /* get allocation strategy */
            R_AX = internal_data->mem_access_mode;
            R_CF = 0;
            break;
          case 0x01:            /* set allocation strategy */
#ifdef INT21_DIAG
            printf("STRAT 5801 bl=%02x by %04x:%04x\n", R_BL,
                   readw86((CPU_SS << 4) + CPU_SP + 2),
                   readw86((CPU_SS << 4) + CPU_SP));
#endif
            if (R_BL != FIRST_FIT && R_BL != BEST_FIT && R_BL != LAST_FIT &&
                R_BL != FIRST_FIT_UO && R_BL != BEST_FIT_UO && R_BL != LAST_FIT_UO &&
                R_BL != FIRST_FIT_U && R_BL != BEST_FIT_U && R_BL != LAST_FIT_U)
            {
              rc = DE_INVLDFUNC;
              R_AX = (UWORD) (-rc);
              R_CF = 1;
            }
            else
            {
              internal_data->mem_access_mode = R_BL;
              R_CF = 0;
            }
            break;
          case 0x02:            /* get UMB link state */
            R_AL = LoL->uppermem_link;
            R_CF = 0;
            break;
          case 0x03:            /* set UMB link state */
#ifdef INT21_DIAG
            printf("LINK 5803 bx=%04x (was %u, root=%04x) by %04x:%04x\n",
                   R_BX, LoL->uppermem_link & 1, LoL->uppermem_root,
                   readw86((CPU_SS << 4) + CPU_SP + 2),
                   readw86((CPU_SS << 4) + CPU_SP));
#endif
            /*
             * FreeDOS accepts only BX=0 (unlink) and BX=1 (link).
             * Do not silently normalize every non-zero value to 1.
             */
            if (R_BX > 1 || LoL->uppermem_root == 0xffff)
            {
              R_AX = (UWORD)-DE_INVLDFUNC;
              R_CF = 1;
            }
            else
            {
              DosUmbLink(R_BX);
              R_CF = 0;
            }
#ifdef INT21_DIAG
            printf("LINK done: link=%u\n", LoL->uppermem_link & 1);
            if (R_BX)
              mcb_dump_chain();
#endif
            break;
          default:
            R_AX = (UWORD)-DE_INVLDFUNC;
            R_CF = 1;
        }
        break;
      /* UNDOCUMENTED: Double byte and korean tables                  */
    case 0x63:
      {
#if 0
        /* not really supported, but will pass.                 */
        lr.AL = 0x00;           /*jpp: according to interrupt list */
        /*Bart: fails for PQDI and WATCOM utilities:
           use the above again */
#endif
        switch (R_AL)
        {
          case 0: {
            dos_far_ptr p = DosGetDBCS();
            R_DS = ( FP_SEG(p) );
            R_SI = FP_OFF(p) + 2;
            break;
          }
          case 1: /* set Korean Hangul input method to DL 0/1 */
            R_AL = 0xff;       /* flag error (AL would be 0 if okay) */
            break;
          case 2: /* get Korean Hangul input method setting to DL */
            R_AL = 0xff;       /* flag error, do not set DL */
            break;
          default:      /* is this the proper way to handle invalid AL? */
            rc = -1;
            goto error_exit;
        }
        break;
      }
        /* Windows95 / EDR-DOS long filename API.
           Ported from original kernel/inthndlr.c case 0x71.

           Most LFN subfunctions in the original kernel fall through to
           unsupp: AL=00, CF=1.  AX therefore becomes 7100h.

           AL=42h and AL=A6h are special: original validates the DOS file
           handle and, for shared/network SFT entries, gives the redirector
           INT 2Fh/AH=11h a chance to handle the request.  Local filesystem
           support is still reported as unsupported. */
       case 0x71:
        switch (R_AL)
        {
#ifdef WITHLFNAPI
          case 0x0d:
          case 0x39:
          case 0x3a:
          case 0x3b:
          case 0x41:
          case 0x43:
          case 0x47:
          case 0x4e:
          case 0x4f:
          case 0xa0:
          case 0xa1:
          case 0xa2:
          case 0xa8:
          case 0xaa:
            goto lfn_unsupp;

          case 0x56:
          case 0x60:
            switch (R_CL)
            {
              case 0x00:
              case 0x01:
              case 0x02:
                goto lfn_unsupp;
              default:
                goto lfn_unsupp;
            }

          case 0xa9:
          case 0x6c:
            goto lfn_unsupp;

          case 0xa7:
            switch (R_BL)
            {
              case 0x00:
              case 0x01:
                goto lfn_unsupp;
              default:
                goto lfn_unsupp;
            }
#endif
          default:
            goto lfn_unsupp;
        }
        break;

#ifdef WITHLFNAPI
        /* Win95 beta LFN - find close */
      case 0x72:
        goto lfn_unsupp;
#endif

#ifdef WITHFAT32
        /* DOS 7.0+ FAT32 extended functions */
      case 0x73:
        R_CF = 0;
        internal_data->CritErrCode = SUCCESS;
        rc = int21_fat32_regs(regs);
        goto short_check;
#endif

#ifdef WITHLFNAPI
        /* FreeDOS LFN helper API functions */
      case 0x74:
        switch (R_AL)
        {
          case 0x01:
            rc = lfn_allocate_inode();
            break;
          case 0x02:
            rc = lfn_free_inode(R_BX);
            break;
          case 0x03:
            rc = lfn_setup_inode(R_BX, MK_ULONG(R_CX, R_DX), MK_ULONG(R_SI, R_DI));
            break;
          case 0x04:
            rc = lfn_create_entries(R_BX, (lfn_inode_ptr)ARM_PTR(R_FP_DS_DX));
            break;
          case 0x05:
            rc = lfn_dir_read(R_BX, (lfn_inode_ptr)ARM_PTR(R_FP_DS_DX));
            break;
          case 0x06:
            rc = lfn_dir_write(R_BX);
            break;
          default:
            goto error_invalid;
        }
        R_AX = rc;
        R_CF = 0;
        goto short_check;
#endif
      /* ------------------------------------------------------------------
         Block G (ported from kernel/inthndlr.c): the FCB layer
         (fcbfns.c). AH=29h (parse) was already implemented separately.
         ------------------------------------------------------------------ */

      case 0x0f:
        R_AL = FcbOpen(R_FP_DS_DX, O_FCB | O_LEGACY | O_OPEN | O_RDWR);
        break;

      case 0x10:
        R_AL = FcbClose(R_FP_DS_DX);
        break;

      case 0x11:
        R_AL = FcbFindFirstNext(R_FP_DS_DX, TRUE);
        break;

      case 0x12:
        R_AL = FcbFindFirstNext(R_FP_DS_DX, FALSE);
        break;

      case 0x13:
        R_AL = FcbDelete(R_FP_DS_DX);
        break;

      case 0x14:
        /* FCB read */
        R_AL = FcbReadWrite(R_FP_DS_DX, 1, XFR_READ);
        break;

      case 0x15:
        /* FCB write */
        R_AL = FcbReadWrite(R_FP_DS_DX, 1, XFR_WRITE);
        break;

      case 0x16:
        R_AL = FcbOpen(R_FP_DS_DX, O_FCB | O_LEGACY | O_CREAT | O_TRUNC | O_RDWR);
        break;

      case 0x17:
        R_AL = FcbRename(R_FP_DS_DX);
        break;

      /* Random read using FCB: fields not updated
         (XFR_RANDOM should not be used here) */
      case 0x21:
        R_AL = FcbRandomIO(R_FP_DS_DX, XFR_READ);
        break;

      /* Random write using FCB */
      case 0x22:
        R_AL = FcbRandomIO(R_FP_DS_DX, XFR_WRITE);
        break;

      /* Get file size in records using FCB */
      case 0x23:
        R_AL = FcbGetFileSize(R_FP_DS_DX);
        break;

      /* Set random record field in FCB */
      case 0x24:
        FcbSetRandom(R_FP_DS_DX);
        break;

      /* Read random record(s) using FCB */
      case 0x27:
      {
        UWORD nrec = R_CX;
        R_AL = FcbRandomBlockIO(R_FP_DS_DX, &nrec, XFR_READ | XFR_FCB_RANDOM);
        R_CX = nrec;
        break;
      }

      /* Write random record(s) using FCB */
      case 0x28:
      {
        UWORD nrec = R_CX;
        R_AL = FcbRandomBlockIO(R_FP_DS_DX, &nrec, XFR_WRITE | XFR_FCB_RANDOM);
        R_CX = nrec;
        break;
      }

      /* ------------------------------------------------------------------
         Block F (ported from kernel/inthndlr.c): terminate and stay
         resident.
         ------------------------------------------------------------------ */

      /* Keep Program (Terminate and stay resident) */
      case 0x31:
        /* Resize the process's block to DX paragraphs (min 6, per the
           original) BEFORE terminating; environment block and open
           handles are kept - exec_run_child() skips DosClose()/
           FreeProcessMem() for term type 3. Return code AH=03h is
           what the parent's INT 21h AH=4Dh will report (original:
           return_code = AL | 0x300). Errors from DosMemChange() are
           deliberately ignored, exactly like the original. */
        DosMemChange(internal_data->cu_psp,
                     R_DX < 6 ? 6 : R_DX, NULL);
        request_terminate(R_AL, 3);
        R_CF = 0;
        break;

      /* ------------------------------------------------------------------
         Blocks D & E (ported from kernel/inthndlr.c): server/network
         (5Dh/5Eh/5Fh, redirector permanently stubbed - see
         network_redirector_mx() in kernel.c) and NLS codepage (66h).
         ------------------------------------------------------------------ */

      case 0x5d:
        switch (R_AL)
        {
            /* Remote Server Call: DS:DX -> DOS parameter list holding
               the register frame AX,BX,CX,DX,SI,DI,DS,ES (original:
               fmemcpy(&lr, R_FP_DS_DX, sizeof(lregs)); goto dispatch).
               Load DS/ES last - reading the frame uses the old DS.   */
          case 0x00:
          {
            uint32_t frame = (R_DS << 4) + R_DX;
            UWORD new_ds, new_es;
            R_AX = readw86(frame + 0);
            R_BX = readw86(frame + 2);
            R_CX = readw86(frame + 4);
            R_DX = readw86(frame + 6);
            R_SI = readw86(frame + 8);
            R_DI = readw86(frame + 10);
            new_ds = readw86(frame + 12);
            new_es = readw86(frame + 14);
            R_DS = ( new_ds );
            R_ES = ( new_es );
            goto dispatch;
          }

            /* Get address of SDA: DS:SI -> internal_data (the
               ErrorMode byte), CX = bytes to swap while InDOS,
               DX = bytes to swap always. The original's asm labels:
               _swap_always sits right before Int21AX (offset 1Ah),
               _swap_indos marks the end of all kernel data ("we don't
               know precisely what needs to be swapped, so set it
               here") - the port's equivalent end is the end of
               struct dos_data.                                       */
          case 0x06:
          {
            char *sda_base = (char *)ARM_PTR(MK_FP(DOS_PSP, 0));
            R_DS = ( DOS_PSP );
            R_SI = (UWORD)((char *)&internal_data->ErrorMode - sda_base);
            R_CX = (UWORD)((char *)(internal_data + 1) -
                             (char *)&internal_data->ErrorMode);
            R_DX = (UWORD)((char *)&internal_data->Int21AX -
                             (char *)&internal_data->ErrorMode);
            R_CF = 0;
            break;
          }

          case 0x07:
          case 0x08:
          case 0x09:
            rc = (int)network_redirector_mx(REM_PRINTREDIR,
                     NULL, (void *)(intptr_t)internal_data->Int21AX);
            R_CF = 0;
            if (rc != SUCCESS)
              goto error_exit;
            break;

            /* Set Extended Error: DS:DX -> lregs frame
               (AX=0 BX=2 CX=4 DX=6 SI=8 DI=10 DS=12 ES=14)           */
          case 0x0a:
          {
            uint32_t er = (R_DS << 4) + R_DX;
            internal_data->CritErrCode   = readw86(er + 0);
            internal_data->CritErrDev    = MK_FP(readw86(er + 14),
                                                 readw86(er + 10));
            internal_data->CritErrLocus  = read86(er + 5);   /* CH */
            internal_data->CritErrClass  = read86(er + 3);   /* BH */
            internal_data->CritErrAction = read86(er + 2);   /* BL */
            R_CF = 0;
            break;
          }

          default:
            internal_data->CritErrCode = SUCCESS;
            goto error_invalid;
        }
        break;

      case 0x5e:
        switch (R_AL)
        {
          case 0x00:
            R_CX = get_machine_name(R_FP_DS_DX);
            break;

          case 0x01:
            set_machine_name(R_FP_DS_DX, R_CX);
            break;

          default:
            rc = (int)network_redirector_mx(REM_PRINTSET, NULL,
                     (void *)(intptr_t)internal_data->Int21AX);
            goto short_check;
        }
        break;

      case 0x5f:
        if (R_AL == 7 || R_AL == 8)
        {
          if (R_DL < LoL->lastdrive)
          {
            struct cds *cdsp =
                (struct cds *)ARM_PTR(LoL->CDSp) + R_DL;
            if (FP_OFF(cdsp->cdsDpb))   /* letter of physical drive?  */
            {
              cdsp->cdsFlags &= ~CDSPHYSDRV;
              if (R_AL == 7)
                cdsp->cdsFlags |= CDSPHYSDRV;
              break;
            }
          }
          rc = DE_INVLDDRV;
          goto error_exit;
        }
        else
        {
          /* original: network_redirector_mx(REM_DOREDIRECT, &lr, ...)
             manipulates the caller's register frame directly and
             leaves via real_exit (AX = -rc, CF on failure, registers
             otherwise untouched). With the permanent stub rc is always
             DE_INVLDFUNC; the register-frame side effects don't exist. */
          rc = (int)network_redirector_mx(REM_DOREDIRECT, NULL,
                   (void *)(intptr_t)internal_data->Int21AX);
          if (rc != SUCCESS)
          {
            internal_data->CritErrCode = -rc;   /* Maybe set */
            R_CF = 1;
          }
          R_AX = -rc;
          break;
        }

      /* Get/Set global code page                                     */
      case 0x66:
        switch (R_AL)
        {
          case 1:
          {
            UWORD act, sys;
            rc = DosGetCodepage(&act, &sys);
            R_BX = act;
            R_DX = sys;
            break;
          }
          case 2:
            rc = DosSetCodepage(R_BX, R_DX);
            break;

          default:
            goto error_invalid;
        }
        if (rc != SUCCESS)
          goto error_exit;
        R_CF = 0;
        break;

      /* ------------------------------------------------------------------
         Block C (ported from kernel/inthndlr.c): file extensions.
         Long results follow the original's long_check convention: on
         failure rc = (COUNT)lrc -> error_exit (which also latches
         CritErrCode), on success CF=0 and AX = low word.
         ------------------------------------------------------------------ */

      /* Create Temporary File                                        */
      case 0x5a:
      {
        long lrc = DosMkTmp(R_FP_DS_DX, R_CX);
        if (lrc < SUCCESS)
        {
          rc = (COUNT)lrc;
          goto error_exit;
        }
        R_CF = 0;
        R_AX = (UWORD)lrc;
        break;
      }

      /* Create New File (fails with DE_FILEEXISTS if it exists)      */
      case 0x5b:
      {
        long lrc = DosOpen(R_FP_DS_DX, O_LEGACY | O_RDWR | O_CREAT, R_CX);
        if (lrc < SUCCESS)
        {
          rc = (COUNT)lrc;
          goto error_exit;
        }
        R_CF = 0;
        R_AX = (UWORD)lrc;
        break;
      }

      /* Lock/unlock file access (added for SHARE - Ron Cemer)        */
      case 0x5c:
        rc = DosLockUnlock(R_BX,
                           ((LONG)R_CX << 16) | R_DX,
                           ((LONG)R_SI << 16) | R_DI,
                           R_AL != 0);
        if (rc != SUCCESS)
          goto error_exit;
        R_CF = 0;
        break;

      /* Set Max file handle count                                    */
      case 0x67:
        rc = SetJFTSize(R_BX);
        goto short_check;

      /* Flush file buffer -- COMMIT FILE                             */
      case 0x68:
      case 0x6a:
        rc = DosCommit(R_BX);
        goto short_check;

      /* Extended Open/Create                                         */
      case 0x6c:
      {
        long lrc;
        /* high nibble must be <= 1, low nibble must be <= 2 */
        if ((R_DL & 0xef) > 0x2)
          goto error_invalid;
        lrc = DosOpen(MK_FP(R_DS, R_SI),
                      (R_BX & 0x70ff) | ((R_DL & 3) << 8) |
                      ((R_DL & 0x10) << 6), R_CL);
        if (lrc < SUCCESS)
        {
          rc = (COUNT)lrc;
          goto error_exit;
        }
        /* action taken */
        R_CX = (UWORD)(lrc >> 16);
        R_CF = 0;
        R_AX = (UWORD)lrc;
        break;
      }

      /* ------------------------------------------------------------------
         Block B (ported from kernel/inthndlr.c): disk / DPB information.
         ------------------------------------------------------------------ */

      /* Get Default Drive Data                                       */
      case 0x1b:
        R_DL = 0;
        /* fall through */
      /* Get Drive Data                                               */
      case 0x1c:
      {
        UBYTE spc;
        UWORD bps, nc;
        dos_far_ptr p = FatGetDrvData(R_DL, &spc, &bps, &nc);
        if (!far_is_null(p))
        {
          R_AL = spc;
          R_CX = bps;
          R_DX = nc;
          R_DS = ( FP_SEG(p) );
          R_BX = FP_OFF(p);
        }
        else
          R_AL = 0xff;  /* return 0xff on invalid drive */
        break;
      }

      /* Get default DPB                                              */
      case 0x1f:
      /* Get DPB                                                      */
      case 0x32:
      /* r->DL is NOT changed by MS 6.22 */
      /* INT21/32 is documented to reread the DPB */
      {
        int drv = (R_DL == 0 || R_AH == 0x1f)
                    ? internal_data->default_drive : R_DL - 1;
        dos_far_ptr dpbp_x86 = get_dpb(drv);
        struct dpb *dpbp;

        if (far_is_null(dpbp_x86))
        {
          internal_data->CritErrCode = -DE_INVLDDRV;
          R_AL = 0xFF;
          break;
        }
        dpbp = (struct dpb *) ARM_PTR (dpbp_x86);
        /* hazard: no error checking! */
        flush_buffers(dpbp->dpb_unit);
        dpbp->dpb_flags = M_CHANGED;  /* force flush and reread of drive BPB/DPB */

#ifdef WITHFAT32
        if (media_check(dpbp_x86) < 0 || ISFAT32(dpbp))
#else
        if (media_check(dpbp_x86) < 0)
#endif
        {
          R_AL = 0xff;
          internal_data->CritErrCode = -DE_INVLDDRV;
          break;
        }
        R_DS = ( FP_SEG(dpbp_x86) );
        R_BX = FP_OFF(dpbp_x86);
        R_AL = 0;
        break;
      }

      /* Dos Get Disk Free Space                                      */
      case 0x36:
      {
        UWORD navc, bps, nc;
        R_AX = DosGetFree(R_DL, &navc, &bps, &nc);
        if (R_AX != 0xffff)
        {
          /* original copies its whole reg frame back, leaving the
             outputs untouched on error; only assign on success */
          R_BX = navc;
          R_CX = bps;
          R_DX = nc;
        }
        break;
      }

      /* DOS 2+ internal - TRANSLATE BIOS PARAMETER BLOCK TO DRIVE
         PARAM BLOCK: DS:SI -> BPB, ES:BP -> DPB to fill              */
      case 0x53:
#ifdef WITHFAT32
        bpb_to_dpb((bpb *) ARM_PTR (MK_FP(R_DS, R_SI)),
                   (struct dpb *) ARM_PTR (MK_FP(R_ES, R_BP)),
                   (R_CX == 0x4558 && R_DX == 0x4152));
#else
        bpb_to_dpb((bpb *) ARM_PTR (MK_FP(R_DS, R_SI)),
                   (struct dpb *) ARM_PTR (MK_FP(R_ES, R_BP)));
#endif
        break;

      /* Get/Set disk serial number: original wraps generic IOCTL
         44h/0Dh with CX=0866h (get) / 0846h (set); the drive travels
         in BL, which DosDevIOctl's 0Dh path reads directly.          */
      case 0x69:
      {
        int drv = (R_BL == 0 ? internal_data->default_drive : R_BL - 1);
        if (R_AL < 2)
        {
          if (far_is_null(get_cds(drv)))
          {
            rc = DE_INVLDDRV;
            goto error_exit;
          }
          if (!far_is_null(get_dpb(drv)))
          {
            UWORD saveCX = R_CX;
            R_CX = (R_AL == 0) ? 0x0866 : 0x0846;
            R_AL = 0x0d;
            rc = DosDevIOctl();
            R_CX = saveCX;
            goto short_check;
          }
        }
        goto error_invalid;
      }

      /* ------------------------------------------------------------------
         Block A (ported from kernel/inthndlr.c): trivial functions whose
         state already exists in the port's SDA/LoL.
         ------------------------------------------------------------------ */

      /* Disk Reset: flush all dirty buffers                          */
      case 0x0d:
        flush();
        break;

      /* Create New PSP: parent template is the CALLER's CS, exactly
         as in the original (new_psp(lr.DX, r->CS)). The caller's CS
         sits on the INT frame: SS:SP -> IP, CS, FLAGS.               */
      case 0x26:
        new_psp(R_DX, readw86((CPU_SS << 4) + CPU_SP + 2));
        break;

      /* Set Verify Flag                                              */
      case 0x2e:
        internal_data->verify_ena = R_AL & 1;
        break;

      /* DosVars - get/set dos variables (original: int21_syscall).
         Does not touch carry.                                        */
      case 0x33:
        switch (R_AL)
        {
          /* Set Ctrl-C flag; returns DL = break_ena. break_ena is the
             SDA byte at internal_data+17h - the single source of truth
             (guest programs peek it directly), see kernel.c.          */
          case 0x01:
            internal_data->break_ena = R_DL & 1;
            /* fall through so DL only low bit (as in MS-DOS) */
          /* Get Ctrl-C flag                                          */
          case 0x00:
            R_DL = internal_data->break_ena;
            break;
          case 0x02:            /* get/set extended control break     */
          {
            UBYTE tmp = internal_data->break_ena;
            internal_data->break_ena = R_DL & 1;
            R_DL = tmp;
            break;
          }
          /* Get Boot Drive                                           */
          case 0x05:
            R_DL = LoL->BootDrive;
            break;
          /* Get (real) DOS-C version                                 */
          case 0x06:
            R_BL = LoL->os_major;
            R_BH = LoL->os_minor;
            R_DL = 0;                    /* revision, remaining 0   */
            R_DH = LoL->version_flags;   /* bit3: ROM, bit4: HMA    */
            break;
          /* FreeDOS extension: CPU family. Both emulator cores are at
             least 286-class; keep the conservative answer.           */
          case 0xfa:
            R_AL = 2;
            break;
          /* FreeDOS extension: set version returned by INT 21h/30h   */
          case 0xfc:
            LoL->os_setver_major = R_BL;
            LoL->os_setver_minor = R_BH;
            break;
          /* FreeDOS extension: get release string pointer in DX:AX   */
          case 0xff:
            R_DX = DOS_PSP;
            R_AX = (UWORD)((char *)LoL->os_release_str -
                             (char *)ARM_PTR(MK_FP(DOS_PSP, 0)));
            break;
          default:              /* set AL=0xFF as error, NOT carry    */
            R_AL = 0xff;
            break;
        }
        break;

      /* Get InDOS flag address                                       */
      case 0x34:
        R_ES = ( DOS_PSP );
        R_BX = (UWORD)((char *)&internal_data->InDOS -
                         (char *)ARM_PTR(MK_FP(DOS_PSP, 0)));
        break;

      /* Get Verify Flag                                              */
      case 0x54:
        R_AL = internal_data->verify_ena;
        break;

      /* UNDOCUMENTED: create child PSP at DX, memory top in SI       */
      case 0x55:
        child_psp(R_DX, internal_data->cu_psp, R_SI);
        /* copy command line from the parent (required for some device
           loaders) */
        fmemcpy(MK_FP(R_DX, 0x80), MK_FP(internal_data->cu_psp, 0x80), 128);
        internal_data->cu_psp = R_DX;
        break;

      /* Get Extended Error information                               */
      case 0x59:
        R_AX = internal_data->CritErrCode;
        R_CH = internal_data->CritErrLocus;
        R_BH = internal_data->CritErrClass;
        R_BL = internal_data->CritErrAction;
        R_DI = FP_OFF(internal_data->CritErrDev);
        R_ES = ( FP_SEG(internal_data->CritErrDev) );
        break;

      /* DOS 5+ internal (set driver lookahead): original rejects it  */
      case 0x64:
        goto error_invalid;

      case 0xDD: // Novell NetWare - WORKSTATION - SET NetWare ERROR MODE
        goto error_invalid;

      /*
       * CP/M compatibility functions: genuine no-ops in the original
       * kernel.  These explicitly return AL=0 and leave carry unchanged.
       */
      case 0x18:
      case 0x1d:
      case 0x1e:
      case 0x20:
      case 0x61:
      case 0x6b:
        R_AL = 0;
        break;

      default:
#ifdef NO_HANDLER_DETECTOR
        no_handler(_cpu);
#endif
        break;
    }
    goto exit_dispatch;

short_check:
    if (rc < SUCCESS)
        goto error_exit;
    R_CF = 0;
    goto exit_dispatch;

lfn_unsupp:
    R_AL = 0x00;
    R_CF = 1;
    goto exit_dispatch;

error_invalid:
    rc = DE_INVLDFUNC;

error_exit:
    R_AX = (UWORD)(-rc);
    if (internal_data->CritErrCode == SUCCESS)
        internal_data->CritErrCode = R_AX;      /* Maybe set */
    R_CF = 1;
    goto exit_dispatch;

error_carry:
    R_CF = 1;

exit_dispatch:
    flags_on_stack = (flags_on_stack & ~0x0041u)
                   | (regs->flags.value & 0x0041u);

    /* Upstream copies its local lregs frame back only after dispatch.
     * Do the same here, then patch CF/ZF in the caller's IRET frame. */
    cpu_restore_regs(_cpu, regs);
    dpb_watch_int21_checkpoint(cpu, "exit");
    writew86(((uint32_t)entry_ss << 4) + entry_sp + 4, flags_on_stack);
    dpb_watch_int21_checkpoint(cpu, "after-flags-write");
    --internal_data->InDOS;
    return true;
}

#undef R_AX
#undef R_BX
#undef R_CX
#undef R_DX
#undef R_SI
#undef R_DI
#undef R_BP
#undef R_AL
#undef R_AH
#undef R_BL
#undef R_BH
#undef R_CL
#undef R_CH
#undef R_DL
#undef R_DH
#undef R_DS
#undef R_ES
#undef R_FS
#undef R_GS
#undef R_CF
#undef R_ZF
#undef R_FP_DS_DX
#undef R_FP_DS_SI
#undef R_FP_ES_DI

UCOUNT res_read(CPU* cpu, int fd, dos_far_ptr buf, UCOUNT count) {
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x3F;
    CPU_BX = fd;
    CPU_CX = count;
    SET_DS ( FP_SEG(buf) );
    CPU_DX = FP_OFF(buf);
    bios_intcall(cpu, 0x21, "RES READ 21h");
    int res = cf ? (UCOUNT)-1 : CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

int init_switchar(int ch) {
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AX = 0x3701;
    CPU_DL = (BYTE)ch;
    bios_intcall(cpu, 0x21, "INIT SWITCHER 21h");
    int res = CPU_AL == 0x00 ? 0 : -1;
    cpu_restore_regs(cpu, &saved);
    return res;
}


/* INT 20h - old-style (CP/M-compatible) terminate: no return code.
   Equivalent to INT 21h AH=00h/4Ch AL=0 - see request_terminate() in
   task.c. Kept separate from fdos_21h() because it's a different
   interrupt vector (handlers[0x20], registered in 286/cpu.c), not a
   sub-function of INT 21h. */
bool fdos_20h(CPU* _cpu) {
    cpu = _cpu;
    request_terminate(0, 0);
    return true;
}
