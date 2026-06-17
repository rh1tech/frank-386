#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"

#include "hdr/kconfig.h"
#include "hdr/portab.h"
#include "hdr/error.h"
#include "hdr/clock.h"
#include "hdr/device.h"
#include "hdr/sft.h"
#include "hdr/kbd.h"
#include "hdr/fcb.h"
#include "hdr/fat.h"
#include "hdr/pcb.h"
#include "hdr/dirmatch.h"
#include "hdr/fnode.h"
#include "hdr/mcb.h"
#include "hdr/lol.h"
#include "hdr/tail.h"
#include "hdr/process.h"
#include "proto.h"
#include "globals.h"

static CPU* cpu; /// TODO: refactoring

UWORD ASM Int21AX;
seg ASM cu_psp;

dos_far_ptr dta;

//#include <stdio.h>
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
  CritErrCode = (rq->r_status & S_MASK) + 0x13;
  return CriticalError(EFLG_CHAR | EFLG_ABORT | EFLG_RETRY | EFLG_IGNORE,
                       0, rq->r_status & S_MASK, lpDevice);
}

STATIC int CharRequest(struct dhdr FAR **pdev, unsigned command)
{
  struct dhdr FAR *dev = *pdev;
  CharReqHdr.r_command = command;
  CharReqHdr.r_unit = 0;
  CharReqHdr.r_status = 0;
  CharReqHdr.r_length = sizeof(request);
  execrh(&CharReqHdr, dev);
  if (CharReqHdr.r_status & S_ERROR)
  {
    for (;;) {
      switch (char_error(&CharReqHdr, dev))
      {
      case ABORT:
      case FAIL:
        return DE_INVLDACC;
      case CONTINUE:
        CharReqHdr.r_count = 0;
        return 0;
      case RETRY:
        return 1;
      }
    }
  }
  return SUCCESS;
}

long BinaryCharIO(struct dhdr FAR **pdev, size_t n, void FAR * bp,
                  unsigned command)
{
  int err;
  do
  {
    CharReqHdr.r_count = n;
    CharReqHdr.r_trans = bp;
    err = CharRequest(pdev, command);
  } while (err == 1);
  return err == SUCCESS ? (long)CharReqHdr.r_count : err;
}

/* common - call the clock driver */
void ExecuteClockDriverRequest(BYTE command)
{
  BinaryCharIO(&LoL->clock, sizeof(struct ClockRecord), &ClkRecord, command);
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

  if (ClkReqHdr.r_status & S_ERROR)
    return 0;

  for (Year = 1980, c = ClkRecord.clkDays;;)
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

  return (ClkRecord.clkDays + 2) % 7;
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

  ClkRecord.clkDays = DaysFromYearMonthDay(Year, Month, DayOfMonth);

  ExecuteClockDriverRequest(C_OUTPUT);

  if (ClkReqHdr.r_status & S_ERROR)
    return char_error(&ClkReqHdr, LoL->clock);
  return SUCCESS;
}

void DosGetTime(CPU* cpu)
{
  ExecuteClockDriverRequest(C_INPUT);

  if (ClkReqHdr.r_status & S_ERROR)
    return;

  CPU_CH = ClkRecord.clkHours;
  CPU_CL = ClkRecord.clkMinutes;
  CPU_DH = ClkRecord.clkSeconds;
  CPU_DL = ClkRecord.clkHundredths;
}

int DosSetTime(CPU* cpu)
{
  if (CPU_CH > 23 || CPU_CL > 59 || CPU_DH > 59 || CPU_DL > 99)
     return DE_INVLDDATA;
 
  /* for ClkRecord.clkDays */
  ExecuteClockDriverRequest(C_INPUT);

  ClkRecord.clkHours = CPU_CH;
  ClkRecord.clkMinutes = CPU_CL;
  ClkRecord.clkSeconds = CPU_DH;
  ClkRecord.clkHundredths = CPU_DL;

  ExecuteClockDriverRequest(C_OUTPUT);

  if (ClkReqHdr.r_status & S_ERROR)
    return char_error(&ClkReqHdr, LoL->clock);
  return SUCCESS;
}

/*
DOS 1+ - main DOS handler
*/
bool fdos_21h(CPU* _cpu) {
    cpu = _cpu;
    Int21AX = CPU_AX;
    switch (CPU_AH) {
      case 0x1A: // set DTA
        dta = FP_DS_DX;
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
        CPU_BX = FP_OFF(dta);
        SET_ES(FP_SEG(dta));
        break;
        /* Set PSP                                                      */
      case 0x50:
        cu_psp = CPU_BX;
        break;
      //  TODO: provide DOS copartible replica of real data
      // case 0x51: DOS 2+ internal - SYSVARS - GET LIST OF LISTS -> ES:BX -> DOS list of lists (see #01627)

        /* Get PSP                                                      */
      case 0x51:
        /* UNDOCUMENTED: return current psp                             */
      case 0x62:
        CPU_BX = cu_psp;
        break;
      default:
        no_handler(_cpu);
    }
    return true;
}
