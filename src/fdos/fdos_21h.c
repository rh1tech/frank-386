#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"

static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 21H - ERROR: no handler defined ");
while(1); // remove it
    return true;
}

COUNT ASMCFUNC CriticalError(COUNT nFlag, COUNT nDrive, COUNT nError, struct dhdr FAR * lpDevice) {
/// TODO: entry.asm
  CPU_AL = FAIL;
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

unsigned char DosGetDate(CPU* cpu)
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

  CPU_CX = Year;
  CPU_DH = Month;
  CPU_DL = c - pdays[Month - 1] + 1;

  /* Day of week is simple. Take mod 7, add 2 (for Tuesday        */
  /* 1-1-80) and take mod again                                   */

  return (internal_data->ClkRecord.clkDays + 2) % 7;
}

UWORD DaysFromYearMonthDay(UWORD Year, UWORD Month, UWORD DayOfMonth)
{
  if (Year < 1980)
    return 0;

  return DayOfMonth - 1
      + is_leap_year_monthdays(Year)[Month - 1]
      + ((Year - 1980) * 365) + ((Year - 1980 + 3) / 4);

}

int DosSetDate(CPU* cpu)
{
  UWORD Year = CPU_CX;
  UWORD Month = CPU_DH;
  UWORD DayOfMonth = CPU_DL;
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

void DosGetTime(CPU* cpu)
{
  ExecuteClockDriverRequest(C_INPUT);

  if (CharReqHdr.r_status & S_ERROR)
    return;

  CPU_CH = internal_data->ClkRecord.clkHours;
  CPU_CL = internal_data->ClkRecord.clkMinutes;
  CPU_DH = internal_data->ClkRecord.clkSeconds;
  CPU_DL = internal_data->ClkRecord.clkHundredths;
}

int DosSetTime(CPU* cpu)
{
  if (CPU_CH > 23 || CPU_CL > 59 || CPU_DH > 59 || CPU_DL > 99)
     return DE_INVLDDATA;
 
  /* for ClkRecord.clkDays */
  ExecuteClockDriverRequest(C_INPUT);

  internal_data->ClkRecord.clkHours = CPU_CH;
  internal_data->ClkRecord.clkMinutes = CPU_CL;
  internal_data->ClkRecord.clkSeconds = CPU_DH;
  internal_data->ClkRecord.clkHundredths = CPU_DL;

  ExecuteClockDriverRequest(C_OUTPUT);

  if (CharReqHdr.r_status & S_ERROR)
    return char_error(&CharReqHdr, (struct dhdr*)ARM_PTR(LoL->clock));
  return SUCCESS;
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

  if (EFFECTIVE(internal_data->current_ldt))
    internal_data->default_drive = drv;
  
  return LoL->lastdrive;
}

#ifdef WITHFAT32
static COUNT int21_fat32(void)
{
  COUNT rc;

  switch (CPU_AL)
  {
    /* Get extended drive parameter block */
    case 0x02:
    {
      struct dpb FAR *dpb;
      struct xdpbdata FAR *xddp;

      if (CPU_CX < sizeof(struct xdpbdata))
        return DE_INVLDBUF;

      dpb = GetDriveDPB(CPU_DL, &rc);
      if (rc != SUCCESS)
        return rc;

      flush_buffers(dpb->dpb_unit);
      dpb->dpb_flags = M_CHANGED;

      if (media_check(dpb) < 0)
        return DE_INVLDDRV;

      xddp = (struct xdpbdata FAR *)ARM_PTR(FP_ES_DI);
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
      struct xfreespace FAR *xfsp = (struct xfreespace FAR *)ARM_PTR(FP_ES_DI);

      if (CPU_CX < sizeof(struct xfreespace))
        return DE_INVLDBUF;

      rc = DosGetExtFree((BYTE FAR *)ARM_PTR(FP_DS_DX), xfsp);
      if (rc != SUCCESS)
        return rc;
      break;
    }

    /* Set DPB to use for formatting */
    case 0x04:
    {
      struct xdpbforformat FAR *xdffp = (struct xdpbforformat FAR *)ARM_PTR(FP_ES_DI);
      struct dpb FAR *dpb;

      if (CPU_CX < sizeof(struct xdpbforformat))
        return DE_INVLDBUF;

      dpb = GetDriveDPB(CPU_DL, &rc);
      if (rc != SUCCESS)
        return rc;

      xdffp->xdff_datasize = sizeof(struct xdpbforformat);
      xdffp->xdff_version.actual = 0;

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
          ddt *pddt = getddt(CPU_DL);
          memcpy(&pddt->ddt_bpb, ARM_PTR(xdffp->xdff_f.rebuilddpb.bpbp), sizeof(bpb));
        }
        /* fall through */
        case 0x02:
rebuild_dpb:
          flush_buffers(dpb->dpb_unit);
          dpb->dpb_flags = M_CHANGED;
          if (media_check(dpb) < 0)
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
      BYTE FAR *SectorBlock = (BYTE FAR *)ARM_PTR(MK_FP(CPU_DS, CPU_BX));
      ULONG blkno;
      UWORD nblks;
      dos_far_ptr bufp;
      UBYTE mode;

      if (CPU_CX != 0xffff || (CPU_SI & ~0x6001))
        return DE_INVLDPARM;

      if (CPU_DL > LoL->lastdrive || CPU_DL == 0)
        return -0x207;

      blkno =  (ULONG)SectorBlock[0]
             | ((ULONG)SectorBlock[1] << 8)
             | ((ULONG)SectorBlock[2] << 16)
             | ((ULONG)SectorBlock[3] << 24);
      nblks =  (UWORD)SectorBlock[4]
             | ((UWORD)SectorBlock[5] << 8);
      bufp = MK_FP((UWORD)(SectorBlock[8] | ((UWORD)SectorBlock[9] << 8)),
                   (UWORD)(SectorBlock[6] | ((UWORD)SectorBlock[7] << 8)));
        
      mode = ((CPU_SI & 1) == 0) ? DSKREADINT25 : DSKWRITEINT26;

      CPU_AX = dskxfer(CPU_DL - 1, blkno, bufp, nblks, mode);

      if (mode == DSKWRITEINT26 && CPU_AX == 0)
        setinvld(CPU_DL - 1);

      if (CPU_AX > 0)
        return -0x20c;
      break;
    }

    default:
      return DE_INVLDFUNC;
  }

  return SUCCESS;
}
#endif


/*
DOS 1+ - main DOS handler
*/
bool fdos_21h(CPU* _cpu) {
    COUNT rc;
    cpu = _cpu;
    internal_data->Int21AX = CPU_AX;
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    switch (CPU_AH) {
      case 0x02:
        CPU_AL = CPU_DL;
        write_char_stdout(CPU_AL);
        break;
      /* Display String                                               */
      case 0x09:
        {
          unsigned char c;
          unsigned char FAR *bp = ARM_PTR( FP_DS_DX );

          while ((c = *bp++) != '$')
            write_char_stdout(c);

          CPU_AL = c;
        }
        break;

      case 0x0E: // set drive
        CPU_AL = DosSelectDrv(CPU_DL);
        break;

        /* Get default drive                                           */
      case 0x19:
        CPU_AL = internal_data->default_drive;
        break;

      case 0x1A: // set DTA
        internal_data->dta = FP_DS_DX;
        break;

        /* Set Interrupt Vector                                         */
      case 0x25:
      {
        /* AL = interrupt number, DS:DX = new handler. */
        pstore16((uint32_t)CPU_AL * 4u, CPU_DX);
        pstore16((uint32_t)CPU_AL * 4u + 2u, CPU_DS);
        cf = 0;
      }
        break;

        /* Get Date                                                     */
      case 0x2a:
        CPU_AL = DosGetDate(_cpu);
        break;

        /* Set Date                                                     */
      case 0x2b:
        CPU_AL = DosSetDate (_cpu) == SUCCESS ? 0 : 0xFF;
        break;

        /* Get Time                                                     */
      case 0x2c:
        DosGetTime(cpu);
        break;

        /* Set Time                                                     */
      case 0x2d:
        CPU_AL = DosSetTime (_cpu) == SUCCESS ? 0 : 0xFF;
        break;
        // get DTA
      case 0x2f:
        CPU_BX = FP_OFF(internal_data->dta);
        SET_ES(FP_SEG(internal_data->dta));
        break;

      /* Get (editable) DOS Version                                   */
      case 0x30:
      {
        if (CPU_AL == 1) /* from RBIL, if AL=1 then return version_flags */
            CPU_BH = LoL->version_flags;
        else
            CPU_BH = OEM_ID;
        CPU_AX = ((psp*)ARM_PTR(x86_PSP))->ps_retdosver;
        CPU_BL = REVISION_SEQ;
        CPU_CX = 0; /* do not set this to a serial number!
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
            CPU_AL = retp[1];
            CPU_AH = retp[2];
          }
          else if (retp[0] == 0x86 &&     /* xchg al,ah   */
                  retp[1] == 0xc4 && retp[2] == 0x3d &&  /* cmp ax, xxyy */
                  (retp[5] == 0x75 || retp[5] == 0x74))  /* je/jne error    */
          {
            CPU_AL = retp[4];
            CPU_AH = retp[3];
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
        uint32_t vec = (uint32_t)CPU_AL * 4u;
        CPU_BX = pload16(vec);
        SET_ES(pload16(vec + 2u));
        cf = 0;
      }
        break;

      case 0x37: /* DOS 2+ - SWITCHAR - GET/SET SWITCH CHARACTER */
        switch (CPU_AL) {
        case 0x00:              /* get switch character */
          CPU_DL = internal_data->switchar;
          CPU_AL = 0x00;
          break;
        case 0x01:              /* set switch character */
          internal_data->switchar = CPU_DL;
          CPU_AL = 0x00;
          break;
        default:
          CPU_AL = 0xff;
          break;
        }
        break;
/// TODO: ensure
#if 1
        /* Get/Set Country Info                                         */
      case 0x38:
        {
          UWORD cntry = CPU_AL;

          if (cntry == 0xff)
            cntry = CPU_BX;

          if (0xffff == CPU_DX)
          {
            /* Set Country Code */
            rc = DosSetCountry(cntry);
          }
          else
          {
            if (cntry == 0)
              cntry--;
            /* Get Country Information */
            rc = DosGetCountryInformation(cntry, ARM_PTR ( FP_DS_DX ) );
            if (rc >= SUCCESS)
            {
              if (cntry == (UWORD) - 1) {
                struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
                cntry = ((struct nlsPackage *)ARM_PTR(nlsInfo->actPkg))->cntry;
              }
              CPU_AX = CPU_BX = cntry;
            }
          }
          goto short_check;
        }
#endif
      case 0x3d: // DOS 2+ - OPEN - OPEN EXISTING FILE
      {
        /* DS:DX = ASCIIZ pathname, AL = access mode.
           Migrated from inthndlr.c's "case 0x3d" (DosOpen(FP_DS_DX,
           O_LEGACY | O_OPEN | lr.AL, 0)). On success: CF=0, AX=handle.
           On failure: CF=1, AX=DOS error code (negated, per the
           kernel-wide convention - see init_DosOpen()/dup2() above,
           which already expect this). */
        long result = DosOpen(FP_DS_DX, O_LEGACY | O_OPEN | CPU_AL, 0);
        if (result < SUCCESS)
        {
          cf = 1;
          CPU_AX = (UWORD)(-result);
        }
        else
        {
          cf = 0;
          CPU_AX = (UWORD)result;
        }
      }
        break;

      case 0x3e: // DOS 2+ - CLOSE - CLOSE FILE
      {
        /* BX = file handle. Migrated from inthndlr.c's "case 0x3e"
           (DosClose(lr.BX), "short_check": AX=-rc, CF=1 on error;
           CF=0, AX unchanged on success - DosClose() itself doesn't
           return a value the caller cares about on success). */
        int result = DosClose(CPU_BX);
        if (result < SUCCESS) {
          cf = 1;
          CPU_AX = (UWORD)(-result);
        } else {
          cf = 0;
        }
      }
        break;

     case 0x3f: // DOS 2+ - READ - READ FROM FILE OR DEVICE
      {
        /* BX = file handle, CX = byte count, DS:DX = buffer.
           Migrated from inthndlr.c's "case 0x3f" (DosRead(lr.BX,
           lr.CX, FP_DS_DX)), same long_check convention as case 0x3d
           above: CF=0/AX=bytes-read on success, CF=1/AX=-rc on
           error. */
        long result = DosRead(CPU_BX, CPU_CX, FP_DS_DX);
        if (result < SUCCESS)
        {
          cf = 1;
          CPU_AX = (UWORD)(-result);
        }
        else
        {
          cf = 0;
          CPU_AX = (UWORD)result;
        }
      }
        break;

      case 0x40: // DOS 2+ - WRITE - WRITE TO FILE OR DEVICE
      {
        /* BX = file handle, CX = byte count, DS:DX = buffer.
           Migrated from inthndlr.c's "case 0x40" (DosWrite(lr.BX,
           lr.CX, FP_DS_DX)), same long_check convention as case 0x3f:
           CF=0/AX=bytes-written on success, CF=1/AX=-rc on error. */
        long result = DosWrite(CPU_BX, CPU_CX, FP_DS_DX);
        if (result < SUCCESS)
        {
          cf = 1;
          CPU_AX = (UWORD)(-result);
        }
        else
        {
          cf = 0;
          CPU_AX = (UWORD)result;
        }
      }
        break;

      /* Get/Set File Attributes                                      */
      case 0x43:
        switch (CPU_AL)
        {
          case 0x00:
            rc = DosGetFattr(FP_DS_DX);
            if (rc >= SUCCESS)
              CPU_CX = rc;
            break;

          case 0x01:
            rc = DosSetFattr(FP_DS_DX, CPU_CX);
            CPU_AX = CPU_CX;
            break;

          case 0xff: /* DOS 7.20 (w98) extended name (128 char length) functions */
          {
            switch(CPU_CL)
            {
                  /* Dos Create Directory                                         */
                  case 0x39:
                  /* Dos Remove Directory                                         */
                  case 0x3a:
                    rc = DosMkRmdir(FP_DS_DX, CPU_CL);
                    goto short_check;

                  /* Dos rename file */
                  case 0x56:
                    rc = DosRename(FP_DS_DX, FP_ES_DI);
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
        rc = DosDevIOctl();      /* can set critical error code! */

        if (rc < SUCCESS)
        {
          CPU_AX = -rc;
          if (rc != DE_DEVICE && rc != DE_ACCESS)
            internal_data->CritErrCode = CPU_AX;
          goto error_carry;
        }
        cf = 0;
        break;

      case 0x46: // DOS 2+ - DUP2, FORCEDUP - FORCE DUPLICATE FILE HANDLE
      // BX = existing handle (old), CX = handle to redirect (new)
      {
        unsigned old_hndl = CPU_BX;
        unsigned new_hndl = CPU_CX;
        psp *p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
        UBYTE *filetab = (UBYTE *) ARM_PTR(p->ps_filetab);

        if (old_hndl >= p->ps_maxfiles || filetab[old_hndl] == 0xff)
        {
          cf = 1;
          CPU_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }
        if (new_hndl >= p->ps_maxfiles)
        {
          cf = 1;
          CPU_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }
        if (new_hndl != old_hndl)
        {
          /* close new handle if open */
          if (filetab[new_hndl] != 0xff)
            DosClose(new_hndl);

          /* copy SFT index and bump ref count */
          filetab[new_hndl] = filetab[old_hndl];
          idx_to_sft(filetab[new_hndl])->sft_count++;
        }
        cf = 0;
      }
        break;

        /* Set PSP                                                      */
      case 0x50:
        internal_data->cu_psp = CPU_BX;
        break;

      case 0x52: { // DOS 2+ internal - SYSVARS - GET LIST OF LISTS -> ES:BX -> DOS list of lists (see #01627)
          SET_ES (DOS_PSP);
          CPU_BX = 0x08F0 + 0x26; // see MARK0026H
        }
        break;
// 53h — Translate BIOS
        /* Get PSP                                                      */
      case 0x51: // DOS 2+ internal - GET CURRENT PROCESS ID (GET PSP ADDRESS)
      case 0x62: // DOS 3.0+ - GET CURRENT PSP ADDRESS
        CPU_BX = internal_data->cu_psp;
        break;

      case 0x60: /* DOS 3+ - TRUENAME - canonicalize filename/path */
        rc = DosTruename(MK_FP(CPU_DS, CPU_SI), FP_ES_DI);
        CPU_AX = rc;
        goto short_check;

        /* Extended country information / NLS functions                */
      case 0x65:
        switch (CPU_AL)
        {
          case 0x20:             /* upcase single character */
            CPU_DL = DosUpChar(CPU_DL);
            cf = 0;
            break;
          case 0x21:             /* upcase memory area */
            DosUpMem(ARM_PTR(FP_DS_DX), CPU_CX);
            cf = 0;
            break;
          case 0x22:             /* upcase ASCIZ */
            DosUpString((char FAR *)ARM_PTR(FP_DS_DX));
            cf = 0;
            break;
          case 0xA0:             /* upcase single filename character */
            CPU_DL = DosUpFChar(CPU_DL);
            cf = 0;
            break;
          case 0xA1:             /* upcase filename memory area */
            DosUpFMem(ARM_PTR(FP_DS_DX), CPU_CX);
            cf = 0;
            break;
          case 0xA2:             /* upcase filename ASCIZ */
            DosUpFString((char FAR *)ARM_PTR(FP_DS_DX));
            cf = 0;
            break;
          case 0x23:             /* check Yes/No response */
            CPU_AX = DosYesNo(CPU_DL);
            CfgDbgPrintf(("INT21/65%02x YESNO in DL=%02x -> AX=%04x\n",
                          CPU_AL, CPU_DL, CPU_AX));            
            cf = 0;
            break;
          default: {
            UBYTE subfct = CPU_AL;
            UWORD in_bx = CPU_BX;
            UWORD in_dx = CPU_DX;
            UWORD in_cx = CPU_CX;
            UWORD in_es = CPU_ES;
            UWORD in_di = CPU_DI;
            rc = DosGetData(CPU_AL, CPU_BX, CPU_DX, CPU_CX, ARM_PTR(FP_ES_DI));
            CfgDbgPrintf(("INT21/65%02x GetData bx=%04x dx=%04x cx=%04x es:di=%04x:%04x -> rc=%d ax=%04x cf=%d\n",
                          subfct, in_bx, in_dx, in_cx, in_es, in_di,
                          rc, CPU_AX, cf));
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
        if (CPU_AL > 2)
          goto error_invalid;

        result = DosSeek(CPU_BX,
                         (LONG)(((UDWORD)CPU_CX << 16) | CPU_DX),
                         CPU_AL,
                         &rc);
        if (rc == SUCCESS) {
          CPU_DX = (UWORD)(result >> 16);
          CPU_AX = (UWORD)result;
        }
        goto short_check;
      }


        /* Terminate process (old-style, CP/M-compatible; same as
           INT 20h - see fdos_20h() - and equivalent to AH=4Ch with
           AL=0) */
      case 0x00:
        request_terminate(0, 0);
        cf = 0;
        break;

        /* Terminate process with return code                          */
      case 0x4c:
        request_terminate(CPU_AL, 0);
        cf = 0;
        break;

        /* Get return code (ERRORLEVEL)                                 */
      case 0x4d:
        CPU_AX = DosGetRetCode();
        cf = 0;
        break;

        /* EXEC - load and/or execute a program                        */
      case 0x4b: {
          exec_blk *ep = (exec_blk *) ARM_PTR(MK_FP(CPU_ES, CPU_BX));
          BYTE *lp = (BYTE *) ARM_PTR(FP_DS_DX);

          rc = DosExec(CPU_AL, ep, lp);
          if (rc < SUCCESS)
          {
            CPU_AX = (UWORD) (-rc);
            cf = 1;
          }
          else
            cf = 0;
        }
        break;

        /* Allocate memory                                              */
      case 0x48: {
          seg para;
          UWORD asize = 0;

          rc = DosMemAlloc(CPU_BX, internal_data->mem_access_mode, &para, &asize);
          if (rc < SUCCESS)
          {
            CPU_BX = asize;
            CPU_AX = (UWORD) (-rc);
            cf = 1;
          }
          else
          {
            CPU_AX = para + 1;  /* segment of the usable block, not the MCB itself */
            cf = 0;
          }
        }
        break;

        /* Free memory                                                  */
      case 0x49:
        rc = DosMemFree(CPU_ES - 1);
        if (rc < SUCCESS)
        {
          CPU_AX = (UWORD) (-rc);
          cf = 1;
        }
        else
          cf = 0;
        break;

        /* Resize (grow/shrink) an allocated memory block               */
      case 0x4a: {
          UWORD maxsize = 0;

          rc = DosMemChange(CPU_ES, CPU_BX, &maxsize);
          if (rc < SUCCESS)
          {
            CPU_BX = maxsize;
            CPU_AX = (UWORD) (-rc);
            cf = 1;
          }
          else
            cf = 0;
        }
        break;

        /* Get/Set memory allocation strategy, get/set UMB link state   */
      case 0x58:
        switch (CPU_AL)
        {
          case 0x00:            /* get allocation strategy */
            CPU_AX = internal_data->mem_access_mode;
            cf = 0;
            break;
          case 0x01:            /* set allocation strategy */
            if (CPU_BL != FIRST_FIT && CPU_BL != BEST_FIT && CPU_BL != LAST_FIT &&
                CPU_BL != FIRST_FIT_UO && CPU_BL != BEST_FIT_UO && CPU_BL != LAST_FIT_UO &&
                CPU_BL != FIRST_FIT_U && CPU_BL != BEST_FIT_U && CPU_BL != LAST_FIT_U)
            {
              rc = DE_INVLDFUNC;
              CPU_AX = (UWORD) (-rc);
              cf = 1;
            }
            else
            {
              internal_data->mem_access_mode = CPU_BL;
              cf = 0;
            }
            break;
          case 0x02:            /* get UMB link state */
            CPU_AL = LoL->uppermem_link & 1;
            cf = 0;
            break;
          case 0x03:            /* set UMB link state */
            DosUmbLink(CPU_BX ? 1 : 0);
            cf = 0;
            break;
          default:
            rc = DE_INVLDFUNC;
            CPU_AX = (UWORD) (-rc);
            cf = 1;
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
        switch (CPU_AL)
        {
          case 0: {
            dos_far_ptr p = DosGetDBCS();
            SET_DS ( FP_SEG(p) );
            CPU_SI = FP_OFF(p) + 2;
            break;
          }
          case 1: /* set Korean Hangul input method to DL 0/1 */
            CPU_AL = 0xff;       /* flag error (AL would be 0 if okay) */
            break;
          case 2: /* get Korean Hangul input method setting to DL */
            CPU_AL = 0xff;       /* flag error, do not set DL */
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
        switch (CPU_AL)
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
            switch (CPU_CL)
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
            switch (CPU_BL)
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
        cf = 0;
        internal_data->CritErrCode = SUCCESS;
        rc = int21_fat32();
        goto short_check;
#endif

#ifdef WITHLFNAPI
        /* FreeDOS LFN helper API functions */
      case 0x74:
        switch (CPU_AL)
        {
          case 0x01:
            rc = lfn_allocate_inode();
            break;
          case 0x02:
            rc = lfn_free_inode(CPU_BX);
            break;
          case 0x03:
            rc = lfn_setup_inode(CPU_BX, MK_ULONG(CPU_CX, CPU_DX), MK_ULONG(CPU_SI, CPU_DI));
            break;
          case 0x04:
            rc = lfn_create_entries(CPU_BX, (lfn_inode_ptr)ARM_PTR(FP_DS_DX));
            break;
          case 0x05:
            rc = lfn_dir_read(CPU_BX, (lfn_inode_ptr)ARM_PTR(FP_DS_DX));
            break;
          case 0x06:
            rc = lfn_dir_write(CPU_BX);
            break;
          default:
            goto error_invalid;
        }
        CPU_AX = rc;
        cf = 0;
        goto short_check;
#endif
      case 0xDD: // Novell NetWare - WORKSTATION - SET NetWare ERROR MODE
        goto error_invalid;

      default:
        no_handler(_cpu);
    }
    goto exit_dispatch;

short_check:
    if (rc < SUCCESS)
        goto error_exit;
    cf = 0;
    goto exit_dispatch;

lfn_unsupp:
    CPU_AL = 0x00;
    cf = 1;
    goto exit_dispatch;

error_invalid:
    rc = DE_INVLDFUNC;

error_exit:
    CPU_AX = (UWORD)(-rc);
    if (internal_data->CritErrCode == SUCCESS)
        internal_data->CritErrCode = CPU_AX;      /* Maybe set */
    cf = 1;
    goto exit_dispatch;

error_carry:
    cf = 1;

exit_dispatch:
    flags_on_stack = (flags_on_stack & ~0x0041) // reset ZF, CF
                   | (cpu_getflags(cpu) & 0x0041); // set them back from CPU
    writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack);
    return true;
}

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
