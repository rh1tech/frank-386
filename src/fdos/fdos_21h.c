#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"

static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 21H - ERROR: no handler defined");
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
  BinaryCharIO(&LoL->clock, sizeof(struct ClockRecord), &internal_data->ClkRecord, command);
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

/*
DOS 1+ - main DOS handler
*/
bool fdos_21h(CPU* _cpu) {
    cpu = _cpu;
    internal_data->Int21AX = CPU_AX;
    switch (CPU_AH) {
      case 0x0E: // set drive
        CPU_AL = DosSelectDrv(CPU_DL);
        break;

      case 0x1A: // set DTA
        internal_data->dta = FP_DS_DX;
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

      case 0x46: // DOS 2+ - DUP2, FORCEDUP - FORCE DUPLICATE FILE HANDLE
      /// TODO:
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
      default:
        no_handler(_cpu);
    }
    return true;
}

UCOUNT res_read(CPU* cpu, int fd, dos_far_ptr buf, UCOUNT count) {
    CPU_AH = 0x3F;
    CPU_BX = fd;
    CPU_CX = count;
    SET_DS ( FP_SEG(buf) );
    CPU_DX = FP_OFF(buf);
    fdos_21h(cpu);
    return cf ? (UCOUNT)-1 : CPU_AX;
}
