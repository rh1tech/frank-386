/* FreeDOS headers describe the legacy C ABI.  Keep their declarations with
 * C linkage now that this translation unit is compiled as C++. */
#define new fdos_new
extern "C" {
#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"
}
#undef new
#ifdef load
#undef load
#endif
#include <cstring>
#include "guest_ref.hpp"

using fdos_guest::cpu_regs_ref;
using fdos_guest::dos_data_ref;
using fdos_guest::psp_ref;
using fdos_guest::lol_ref;
using fdos_guest::cds_ref;
using fdos_guest::sft_ref;
using fdos_guest::sfttbl_ref;
using fdos_guest::dpb_ref;
using fdos_guest::buffer_ref;
using fdos_guest::guest_bytes_ref;
using fdos_guest::request_ref;

static const lol_ref fdos_lol(((uint32_t)DOS_PSP << 4) + 0x08F0u);
static const dos_data_ref fdos_idata(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF);

static inline request_ref fdos_char_request()
{
  return request_ref(MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, ClkReqHdr))));
}

#if PDB_DEBUG
extern "C" int snprintf(char *s, size_t n, const char *fmt, ...);
static void dpb_watch_int21_checkpoint(CPU* cpu, const char *where)
{
    static char tags[16][40];
    static unsigned tag_idx;

    char *tag = tags[tag_idx++ & 15];
    snprintf(tag, 40, "INT21-%s AH=%02x AL=%02x", where, CPU_AH, CPU_AL);
    dpb_watch_check_chain(tag);
}
#else
#define dpb_watch_int21_checkpoint(...)
#endif
/*
 * Guest-visible INT 21h register frame.
 *
 * The field order is the original FreeDOS iregs/PUSH$ALL ABI:
 * AX, BX, CX, DX, SI, DI, BP, DS, ES, followed by the hardware
 * interrupt frame IP, CS, FLAGS.
 */
static void int21_store_guest_frame(dos_far_ptr frame,
                                    const cpu_regs_ref &regs,
                                    UWORD ip, UWORD cs, UWORD flags)
{
  const uint32_t base = ((uint32_t)FP_SEG(frame) << 4) + FP_OFF(frame);
  pstore16(base + offsetof(struct int21_guest_iregs, ax), regs.r16(regax));
  pstore16(base + offsetof(struct int21_guest_iregs, bx), regs.r16(regbx));
  pstore16(base + offsetof(struct int21_guest_iregs, cx), regs.r16(regcx));
  pstore16(base + offsetof(struct int21_guest_iregs, dx), regs.r16(regdx));
  pstore16(base + offsetof(struct int21_guest_iregs, si), regs.r16(regsi));
  pstore16(base + offsetof(struct int21_guest_iregs, di), regs.r16(regdi));
  pstore16(base + offsetof(struct int21_guest_iregs, bp), regs.r16(regbp));
  pstore16(base + offsetof(struct int21_guest_iregs, ds), regs.ds());
  pstore16(base + offsetof(struct int21_guest_iregs, es), regs.es());
  pstore16(base + offsetof(struct int21_guest_iregs, ip), ip);
  pstore16(base + offsetof(struct int21_guest_iregs, cs), cs);
  pstore16(base + offsetof(struct int21_guest_iregs, flags), flags);
}

#ifdef NO_HANDLER_DETECTOR
static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 21H - ERROR: no handler defined ");
while(1); // remove it
    return true;
}
#endif

/*
 * Invoke the current process' INT 24h critical-error handler.
 *
 * The guest-visible INT 21h frame published at PSP:2Eh is used as the
 * user stack, matching entry.asm.  INT 24h receives:
 *
 *   AH    critical-error flags
 *   AL    drive number
 *   DI    device error code
 *   BP:SI device-header pointer
 *
 * The live CPU context and the published INT 21h frame are restored
 * after the handler returns.  A nested critical error cannot recurse:
 * it is converted directly to FAIL, as in the original kernel.
 */
struct critical_error_workspace
{
  CPU_regs saved_regs;
  struct int21_guest_iregs saved_frame;
};

static_assert(sizeof(struct critical_error_workspace) <=
               offsetof(struct dos_data, disk_stack) -
               offsetof(struct dos_data, error_stack),
               "critical-error workspace must fit the resident DOS error stack");

COUNT ASMCFUNC CriticalError(COUNT nFlag, COUNT nDrive, COUNT nError,
                             dos_far_ptr /* -> struct dhdr */ x86_lpDevice)
{
  const UWORD error_tos =
      (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, disk_stack));
  const UWORD work_sp =
      (UWORD)((error_tos - sizeof(struct critical_error_workspace)) &
              (UWORD)~3u);
  const uint32_t work_linear = ((uint32_t)DOS_PSP << 4) + work_sp;
  const cpu_regs_ref saved_regs(
      work_linear + offsetof(struct critical_error_workspace, saved_regs));
  const uint32_t saved_frame_linear =
      work_linear + offsetof(struct critical_error_workspace, saved_frame);
  dos_far_ptr user_stack;
  dos_far_ptr /* -> struct dhdr */ device = x86_lpDevice;
  UWORD saved_ss;
  UBYTE saved_error_mode;
  UBYTE saved_indos;
  COUNT action;
  BOOL have_frame;

  if ((UBYTE)fdos_idata.error_mode() != 0)
    return FAIL;

  user_stack = psp_ref((seg)(UWORD)fdos_idata.cu_psp()).stack();
  have_frame = !far_is_null(user_stack) && !far_is_end(user_stack);

  /*
   * INT 24h is normally reached from an active INT 21h and therefore
   * has a PSP:2Eh frame.  Keep a safe fallback for internal callers
   * such as INT 2Fh helpers invoked outside INT 21h.
   */
  if (have_frame) {
    const uint32_t user_stack_linear =
        ((uint32_t)FP_SEG(user_stack) << 4) + FP_OFF(user_stack);
    guest_move_block(saved_frame_linear, user_stack_linear,
                     sizeof(struct int21_guest_iregs));
  }
  else
    user_stack = MK_FP(CPU_SS, CPU_SP);

  /* device is the caller's genuine guest dhdr pointer (or 0000:0000 for
     "no device"); it is published verbatim as CritErrDev, which INT 21h/
     AH=59h hands back to the guest as ES:DI. No native<->guest juggling. */

  /*
   * Publish the failing drive and device in the SDA before calling the
   * user handler.  INT 21h/AH=59h returns CritErrDev as ES:DI, and DOS
   * internal users inspect CritErrDrive directly.
   *
   * Do not synthesize class/action/locus here: their values are selected
   * by the higher-level DOS error path, not derivable from the device
   * request status alone.
   */
  fdos_idata.crit_err_drive() = (UBYTE)nDrive;
  fdos_idata.crit_err_dev(device);

  /*
   * The original kernel runs INT 24h processing on _error_tos, a dedicated
   * resident guest stack in the SDA.  Keep the two objects that must survive
   * the user handler there as well: the complete CPU register snapshot and
   * the published INT 21h frame.  ErrorMode rejects recursion, so one LIFO
   * reservation below _error_tos is sufficient and cannot collide with a
   * second CriticalError() instance.
   *
   * Deliberately left on the native stack: scalar flags, offsets and pointers.
   * They are small and do not remain live across another DOS process level.
   */
  saved_regs.save_cpu(cpu);
  saved_ss = CPU_SS;
  saved_error_mode = fdos_idata.error_mode();
  saved_indos = fdos_idata.indos();

  fdos_idata.error_mode() = (UBYTE)(fdos_idata.error_mode() + 1u);
  if ((UBYTE)fdos_idata.indos() != 0)
    fdos_idata.indos() = (UBYTE)(fdos_idata.indos() - 1u);

  SET_SS(FP_SEG(user_stack));
  CPU_SP = FP_OFF(user_stack);
  CPU_AH = (UBYTE)nFlag;
  CPU_AL = (UBYTE)nDrive;
  CPU_DI = (UWORD)nError;
  CPU_BP = FP_SEG(device);
  CPU_SI = FP_OFF(device);

  bios_intcall(cpu, 0x24, "DOS Critical Error INT24");
  action = CPU_AL;

  /*
   * The INT instruction and a user handler are allowed to use the user
   * stack.  Restore the published DOS frame before returning to the
   * interrupted INT 21h dispatcher.
   */
  if (have_frame) {
    const uint32_t user_stack_linear =
        ((uint32_t)FP_SEG(user_stack) << 4) + FP_OFF(user_stack);
    guest_move_block(user_stack_linear, saved_frame_linear,
                     sizeof(struct int21_guest_iregs));
  }

  saved_regs.restore_cpu(cpu);
  SET_SS(saved_ss);
  fdos_idata.error_mode() = saved_error_mode;
  fdos_idata.indos() = saved_indos;

  /* Force disallowed responses through the same sequence as entry.asm. */
  if (action == CONTINUE && !(nFlag & EFLG_IGNORE))
    action = FAIL;

  if (action == RETRY && !(nFlag & EFLG_RETRY))
    action = FAIL;

  /*
   * Bit 3 is named EFLG_ABORT in the C headers, but entry.asm uses the
   * same bit as OK_FAIL when validating an INT 24h FAIL response.
   */
  if (action == FAIL && !(nFlag & EFLG_ABORT))
    action = ABORT;

  if (action == ABORT)
  {
    if ((UBYTE)fdos_idata.abort_progress())
      return FAIL;
    /*
     * Original entry.asm leaves ErrorMode set while the process-abort
     * path runs.  This prevents a second device error during handle/FCB/
     * memory cleanup from entering INT 24h recursively.
     */
    fdos_idata.error_mode() = 1;
    request_terminate(0, 2);   /* critical-error abort */
    return FAIL;
  }

  return action;
}

/* Abort, retry or fail for character devices                   */
COUNT char_error_status(UWORD status, dos_far_ptr /* -> struct dhdr */ x86_lpDevice)
{
  fdos_idata.crit_err_code() = (status & S_MASK) + 0x13;
  return CriticalError(EFLG_CHAR | EFLG_ABORT | EFLG_RETRY | EFLG_IGNORE,
                       0, status & S_MASK, x86_lpDevice);
}

/* Abort, retry or fail for block devices                       */
COUNT block_error_status(UWORD status, COUNT nDrive,
                         dos_far_ptr /* -> struct dhdr */ x86_lpDevice, int mode)
{
  fdos_idata.crit_err_code() = (status & S_MASK) + 0x13;
  return CriticalError(EFLG_ABORT | EFLG_RETRY | EFLG_IGNORE |
                       (mode == DSKWRITE ? EFLG_WRITE : 0),
                       nDrive, status & S_MASK, x86_lpDevice);
}


/* common - call the clock driver */
void ExecuteClockDriverRequest(BYTE command)
{
  dos_far_ptr clock = fdos_lol.clock();
  BinaryCharIO(&clock, sizeof(struct ClockRecord),
               MK_FP(DOS_PSP, X86_INTERNAL_DATA_OFF + offsetof(dos_data, ClkRecord)),
               command);
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

  if (fdos_char_request().status() & S_ERROR)
    return 0;

  for (Year = 1980, c = fdos_idata.clock_days();;)
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

  return (fdos_idata.clock_days() + 2) % 7;
}

unsigned char DosGetDate(CPU *cpu)
{
  /* Seed the locals from the register frame BEFORE calling down.

     DosGetDateRegs() returns early, storing nothing, when the clock
     driver reports S_ERROR. Upstream is immune to that because it hands
     the register frame itself to the worker (inthndlr.c does
     DosGetDate((struct dosdate *)&lr.CX)), so an error simply leaves the
     guest's CX/DH/DL holding whatever it passed in. This port uses stack
     locals instead, so leaving them uninitialised meant the error path
     copied indeterminate stack bytes into the guest as the current date.
     Pre-seeding reproduces upstream's "frame unchanged on error"
     semantics exactly. */
  UWORD year  = CPU_CX;
  UBYTE month = CPU_DH;
  UBYTE day   = CPU_DL;
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

  fdos_idata.clock_days() = DaysFromYearMonthDay(Year, Month, DayOfMonth);

  ExecuteClockDriverRequest(C_OUTPUT);

  if (fdos_char_request().status() & S_ERROR)
    return char_error_status(fdos_char_request().status(), fdos_lol.clock());
  return SUCCESS;
}

int DosSetDate(CPU *cpu)
{
  return DosSetDateRegs(CPU_CX, CPU_DH, CPU_DL);
}

static void DosGetTimeRegs(UBYTE *out_hour, UBYTE *out_minute, UBYTE *out_second, UBYTE *out_hundredth)
{
  ExecuteClockDriverRequest(C_INPUT);

  if (fdos_char_request().status() & S_ERROR)
    return;

  *out_hour = fdos_idata.clock_hours();
  *out_minute = fdos_idata.clock_minutes();
  *out_second = fdos_idata.clock_seconds();
  *out_hundredth = fdos_idata.clock_hundredths();
}

void DosGetTime(CPU *cpu)
{
  /* Same reasoning as DosGetDate() above: DosGetTimeRegs() stores
     nothing when the clock driver reports S_ERROR, so the locals must
     already hold the guest's own register values or the error path
     reports indeterminate stack bytes as the current time. */
  UBYTE hour      = CPU_CH;
  UBYTE minute    = CPU_CL;
  UBYTE second    = CPU_DH;
  UBYTE hundredth = CPU_DL;

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

  fdos_idata.clock_hours() = hour;
  fdos_idata.clock_minutes() = minute;
  fdos_idata.clock_seconds() = second;
  fdos_idata.clock_hundredths() = hundredth;

  ExecuteClockDriverRequest(C_OUTPUT);

  if (fdos_char_request().status() & S_ERROR)
    return char_error_status(fdos_char_request().status(), fdos_lol.clock());
  return SUCCESS;
}

int DosSetTime(CPU *cpu)
{
  return DosSetTimeRegs(CPU_CH, CPU_CL, CPU_DH, CPU_DL);
}

/* get current directory structure for drive
   return NULL if the CDS is not valid or the
   drive is not within range */
dos_far_ptr/*struct cds*/ get_cds(unsigned drive)
{
  const UBYTE lastdrive = fdos_lol.lastdrive();
  const dos_far_ptr cds_base = fdos_lol.cds();

  if (drive >= lastdrive || far_is_null(cds_base))
    return MK_FP(0, 0);

  const dos_far_ptr cds_ptr =
      MK_FP(FP_SEG(cds_base),
            (UWORD)(FP_OFF(cds_base) + drive * sizeof(struct cds)));
  const cds_ref entry(cds_ptr);
  const unsigned flags = entry.flags();

  /* Entry is disabled or JOINed drives are accessable by the path only.
     Do not materialize a host pointer here: truename() keeps this guest
     address across media/path operations which may fault another page in. */
  if (!(flags & CDSVALID) || (flags & CDSJOINED) != 0)
    return MK_FP(0, 0);
  if (!(flags & CDSNETWDRV) && EFFECTIVE(entry.dpb()) == 0)
    return MK_FP(0, 0);
  return cds_ptr;
}

UBYTE DosSelectDrv(UBYTE drv)
{
  /* dosfns.c:
   *     current_ldt = get_cds(drv);
   *     if (current_ldt != NULL)
   *       default_drive = drv;
   *     return lastdrive;
   *
   * current_ldt IS updated unconditionally - a failed select leaves "no
   * current CDS" behind, it must not leave a stale CDS of another drive.
   * Only the default drive is protected.
   *
   * The old port checked FP_OFF() != 0xFFFF here, which is the *other*
   * sentinel (FFFF:FFFF, set by truename() for device/network paths);
   * get_cds() signals "drive unavailable" with 0000:0000, so the test never
   * fired and an invalid drive became the default one.
   */
  const dos_far_ptr current = get_cds(drv);
  fdos_idata.current_ldt(current);

  if (!far_is_null(current))
    fdos_idata.default_drive() = drv;

  return fdos_lol.lastdrive();
}

static int fcb_parse_common_sep(int c)
{
  return c != 0 && strchr(":;,=+ \t", c) != NULL;
}
 
static int fcb_parse_field_sep(int c)
{
  return (unsigned char)c <= ' ' || strchr("/\\\"[]<>|.:;,=+\t", c) != NULL;
}
 
static size_t fcb_parse_skip_wh(const guest_bytes_ref &src, size_t off)
{
  while (src.byte(off) == ' ' || src.byte(off) == '\t')
    ++off;
  return off;
}
 
static size_t fcb_parse_name_field(const guest_bytes_ref &src, size_t off,
                                   const guest_bytes_ref &dst, size_t dst_off,
                                   COUNT field_size, BOOL *wild)
{
  COUNT i = 0;
  BYTE fill = ' ';

  while (src.byte(off) != 0 && !fcb_parse_field_sep(src.byte(off)) &&
         i < field_size)
  {
    const BYTE ch = src.byte(off);
    if (ch == '*')
    {
      *wild = TRUE;
      fill = '?';
      ++off;
      break;
    }
    if (ch == '?')
      *wild = TRUE;

    dst.byte(dst_off + i, DosUpFChar(ch));
    ++off;
    ++i;
  }

  if (i < field_size)
    guest_fill_block(dst.linear(dst_off + i), fill, field_size - i);
  return off;
}

static UBYTE DosParseFilenameIntoFcbRegs(UBYTE mode, dos_far_ptr srcp,
                                         dos_far_ptr fcbp, UWORD *next_si)
{
  const guest_bytes_ref src(srcp);
  const guest_bytes_ref dst(fcbp);
  size_t off = 0;
  BOOL bad_drive = FALSE;
  BOOL wild_name = FALSE;
  BOOL wild_ext = FALSE;

  if (mode & 0x01)
    while (fcb_parse_common_sep(src.byte(off)))
      ++off;

  /* MS-DOS skips spaces and tabs even without PARSE_SKIP_LEAD_SEP. */
  off = fcb_parse_skip_wh(src, off);

  if (!fcb_parse_field_sep(src.byte(off)) && src.byte(off + 1) == ':')
  {
    UBYTE drive = DosUpFChar(src.byte(off)) - 'A';

    /* Keep parsing even for an invalid drive, as DOS does. */
    dos_far_ptr cds_ = get_cds(drive);
    if (far_is_null(cds_))
      bad_drive = TRUE;

    dst.byte(offsetof(fcb, fcb_drive), drive + 1);
    off += 2;
  }
  else if (!(mode & 0x02))
  {
    dst.byte(offsetof(fcb, fcb_drive), 0);
  }

  /* Undocumented DOS behaviour: these two fields are always reset. */
  dst.word(offsetof(fcb, fcb_cublock), 0);
  dst.word(offsetof(fcb, fcb_recsiz), 0);

  if (!(mode & 0x04))
    guest_fill_block(dst.linear(offsetof(fcb, fcb_fname)), ' ', FNAME_SIZE);
  if (!(mode & 0x08))
    guest_fill_block(dst.linear(offsetof(fcb, fcb_fext)), ' ', FEXT_SIZE);

  /* Special names '.' and '..' return immediately, without extension. */
  if (src.byte(off) == '.')
  {
    dst.byte(offsetof(fcb, fcb_fname), '.');
    ++off;
    if (src.byte(off) == '.')
    {
      dst.byte(offsetof(fcb, fcb_fname) + 1, '.');
      ++off;
    }
    *next_si = FP_OFF(srcp) + (UWORD)off;
    return 0;
  }

  off = fcb_parse_name_field(src, off, dst, offsetof(fcb, fcb_fname),
                             FNAME_SIZE, &wild_name);

  if (src.byte(off) == '.')
    off = fcb_parse_name_field(src, off + 1, dst, offsetof(fcb, fcb_fext),
                               FEXT_SIZE, &wild_ext);

  *next_si = FP_OFF(srcp) + (UWORD)off;

  if (bad_drive)
    return 0xff;
  return (wild_name || wild_ext) ? 1 : 0;
}

/* INT 21h is dispatched against a local register frame, as in upstream
 * FreeDOS int21_service().  Guest BIOS/device calls may use the live CPU as
 * scratch state, but cannot leak register changes into this frame. */
#define R_AX    regs_ref.r16(regax)
#define R_BX    regs_ref.r16(regbx)
#define R_CX    regs_ref.r16(regcx)
#define R_DX    regs_ref.r16(regdx)
#define R_SI    regs_ref.r16(regsi)
#define R_DI    regs_ref.r16(regdi)
#define R_BP    regs_ref.r16(regbp)
#define R_AL    regs_ref.r8l(regax)
#define R_AH    regs_ref.r8h(regax)
#define R_BL    regs_ref.r8l(regbx)
#define R_BH    regs_ref.r8h(regbx)
#define R_CL    regs_ref.r8l(regcx)
#define R_CH    regs_ref.r8h(regcx)
#define R_DL    regs_ref.r8l(regdx)
#define R_DH    regs_ref.r8h(regdx)
#define R_DS    regs_ref.ds()
#define R_ES    regs_ref.es()
#define R_FS    regs_ref.fs()
#define R_GS    regs_ref.gs()
#define R_CF    regs_ref.carry()
#define R_ZF    regs_ref.zero()
#define R_FP_DS_DX MK_FP(R_DS, R_DX)
#define R_FP_DS_SI MK_FP(R_DS, R_SI)
#define R_FP_ES_DI MK_FP(R_ES, R_DI)

#ifdef WITHFAT32
static COUNT int21_fat32_regs(cpu_regs_ref regs_ref)
{
  COUNT rc;

  switch (R_AL)
  {
    /* Get extended drive parameter block */
    case 0x02:
    {
      if (R_CX < sizeof(struct xdpbdata))
        return DE_INVLDBUF;

      dos_far_ptr _dpb = GetDriveDPB(R_DL, &rc);
      if (rc != SUCCESS)
        return rc;

      const dpb_ref src_dpb(_dpb);
      flush_buffers(src_dpb.dpb_unit());
      src_dpb.flags(M_CHANGED);

      if (media_check_tagged(_dpb, "INT21/7302/GetDriveDPB") < 0)
        return DE_INVLDDRV;

      {
        const uint32_t src_linear =
          (static_cast<uint32_t>(FP_SEG(_dpb)) << 4) + FP_OFF(_dpb);
        const uint32_t out_linear =
          (static_cast<uint32_t>(FP_SEG(R_FP_ES_DI)) << 4) + FP_OFF(R_FP_ES_DI);
        const uint32_t out_dpb_linear =
          out_linear + offsetof(struct xdpbdata, xdd_dpb);
        const dpb_ref out_dpb(out_dpb_linear);

        pstore16(out_linear + offsetof(struct xdpbdata, xdd_dpbsize),
                 sizeof(struct dpb));
        pstore_block(out_dpb_linear, src_linear, sizeof(struct dpb));

        if (src_dpb.dpb_fatsize() != 0 &&
            src_dpb.dpb_xsize() != src_dpb.dpb_size())
        {
          out_dpb.nfree_hi(src_dpb.nfree() == 0xFFFF ? 0xFFFF : 0);
          out_dpb.dpb_xflags(0);
          out_dpb.dpb_xfsinfosec(0xffff);
          out_dpb.dpb_xbackupsec(0xffff);
          out_dpb.dpb_xrootclst(0);
          out_dpb.dpb_xdata(src_dpb.dpb_data());
          out_dpb.dpb_xsize(src_dpb.dpb_size());
          out_dpb.dpb_xfatsize(src_dpb.dpb_fatsize());
          out_dpb.dpb_xcluster(
            src_dpb.cluster() == 0xFFFF ? 0xFFFFFFFFuL : src_dpb.cluster());
        }
      }
      break;
    }

    /* Get extended free drive space */
    case 0x03:
    {
      if (R_CX < sizeof(struct xfreespace))
        return DE_INVLDBUF;

      rc = DosGetExtFree(R_FP_DS_DX, R_FP_ES_DI);
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

      const guest_bytes_ref xdff(R_FP_ES_DI);
      constexpr size_t xdff_union = offsetof(struct xdpbforformat, xdff_f);
      xdff.word(offsetof(struct xdpbforformat, xdff_datasize),
                sizeof(struct xdpbforformat));
      xdff.word(offsetof(struct xdpbforformat, xdff_version), 0);

      const dpb_ref dpb(_dpb);
      const UWORD xdff_function =
          (UWORD)xdff.dword(offsetof(struct xdpbforformat, xdff_function));
      switch (xdff_function)
      {
        case 0x00:
        {
          ULONG nfreeclst = xdff.dword(xdff_union + 0);
          ULONG cluster = xdff.dword(xdff_union + 4);
          if (dpb.dpb_fatsize() == 0)
          {
            const ULONG xsize = dpb.dpb_xsize();
            if ((dpb.dpb_xfsinfosec() == 0xffff && (nfreeclst != 0 || cluster != 0))
                || nfreeclst == 1 || nfreeclst > xsize
                || cluster == 1 || cluster > xsize)
              return DE_INVLDPARM;
            dpb.xnfree(nfreeclst);
            dpb.dpb_xcluster(cluster);
            write_fsinfo(_dpb);
          }
          else
          {
            const UWORD size = dpb.dpb_size();
            if ((unsigned)nfreeclst == 1 || (unsigned)nfreeclst > size ||
                (unsigned)cluster == 1 || (unsigned)cluster > size)
              return DE_INVLDPARM;
            dpb.nfree((UWORD)nfreeclst);
            dpb.cluster((UWORD)cluster);
          }
          break;
        }

        case 0x01:
        {
          fdos_guest::ddt_ref ddt(getddt_far(R_DL));
          /* Both endpoints are guest addresses; do not expose either cache slot. */
          const dos_far_ptr src =
              MK_FP(xdff.word(xdff_union + 6), xdff.word(xdff_union + 4));
          const uint32_t src_linear = ((uint32_t)FP_SEG(src) << 4) + FP_OFF(src);
          guest_move_block(ddt.current_bpb_linear(), src_linear, sizeof(bpb));
        }
        /* fall through */
        case 0x02:
rebuild_dpb:
          flush_buffers(dpb.dpb_unit());
          dpb.flags(M_CHANGED);
          if (media_check_tagged(_dpb, "INT21/7304/GetDriveDPB") < 0)
            return DE_INVLDDRV;
          break;

        case 0x03:
        case 0x04:
        {
          ULONG value;
          if (dpb.dpb_fatsize() != 0)
            return DE_INVLDPARM;

          value = xdff.dword(xdff_union + 0);
          if (xdff_function == 0x03)
          {
            if (value != 0xFFFFFFFFUL && (value & ~(0xf | 0x80)))
              return DE_INVLDPARM;
            xdff.dword(xdff_union + 4, dpb.dpb_xflags());
          }
          else
          {
            if (value != 0xFFFFFFFFUL && (value < 2 || value > dpb.dpb_xsize()))
              return DE_INVLDPARM;
            xdff.dword(xdff_union + 4, dpb.dpb_xrootclst());
          }

          if (value != 0xFFFFFFFFUL)
          {
            const dos_far_ptr x86_bp = getblock(1, dpb.dpb_unit());
            if (far_is_null(x86_bp))
              return DE_ACCESS;
            const buffer_ref b(x86_bp);
            BYTE flag = static_cast<BYTE>(b.flag() & ~(BFR_DATA | BFR_DIR | BFR_FAT));
            b.flag(static_cast<BYTE>(flag | BFR_VALID | BFR_DIRTY));
            if (xdff_function == 0x03)
              b.data16(BT_BPB + offsetof(bpb, bpb_xflags), (UWORD)value);
            else
              b.data32(BT_BPB + offsetof(bpb, bpb_xrootclst), value);
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
      const guest_bytes_ref SectorBlock(MK_FP(R_DS, R_BX));
      ULONG blkno;
      UWORD nblks;
      dos_far_ptr bufp;
      UBYTE mode;

      if (R_CX != 0xffff || (R_SI & ~0x6001))
        return DE_INVLDPARM;

      if (R_DL > (UBYTE)fdos_lol.lastdrive() || R_DL == 0)
        return -0x207;

      blkno = SectorBlock.dword(0);
      nblks = SectorBlock.word(4);
      bufp = MK_FP(SectorBlock.word(8), SectorBlock.word(6));
        
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

/* NOTE: there used to be an int21_fat32(void) wrapper here that did
   cpu_save_regs() / int21_fat32_regs() / cpu_restore_regs() against the LIVE
   CPU registers. It was left behind when case 0x73 moved to the INT 21h
   register frame and had no callers (-Wunused-function). Removed rather than
   kept: cpu_restore_regs() restores the whole gprx set AND flags, so that
   wrapper would have discarded every result int21_fat32_regs() had just
   written - it was a trap waiting for the next person to reuse it. The live
   path is "case 0x73: rc = int21_fat32_regs(regs)", which writes into the
   guest's frame directly. */
#endif

/*
DOS 1+ - main DOS handler
*/
bool fdos_21h(CPU* _cpu) {
    COUNT rc;
    UWORD entry_ss, entry_sp;
    UWORD entry_ip, entry_cs;
    UWORD frame_sp;
    UWORD regs_sp;
    UWORD guard_sp;
    dos_far_ptr old_ps_stack;
    dos_far_ptr old_user_r;
    dos_far_ptr old_prev_user_r;

    cpu = _cpu;
    entry_ss = CPU_SS;
    entry_sp = CPU_SP;
    entry_ip = readw86(stk_lin(entry_ss, entry_sp, 0));
    entry_cs = readw86(stk_lin(entry_ss, entry_sp, 2));
    uint16_t flags_on_stack = readw86(stk_lin(entry_ss, entry_sp, 4));

    /*
     * Keep both INT 21h register images on the current DOS process stack.
     *
     * The upper frame is the original FreeDOS PUSH$ALL/iRegs ABI published
     * through PSP:2Eh for critical-error handlers and DOS extenders.  The
     * lower CPU_regs object is the native dispatcher's private working copy:
     * BIOS/device calls may freely modify the live CPU, while R_AX..R_GS keep
     * referring to this saved process state.
     *
     * CPU_regs used to be a long-lived local C object, so every nested DOS
     * call retained another copy on the scarce ARM stack.  A real DOS keeps
     * this state on its user/internal guest stack; reserve it below the
     * public iRegs frame here for the complete duration of this invocation.
     * Align the native view to 4 bytes because CPU_regs contains 32-bit union
     * members.  Restoring entry_sp releases both frames at once.
     *
     * Nested INT 21h calls reserve their own pair below the outer pair. Save
     * and restore ps_stack so the outer public frame becomes current again
     * when the nested call returns.
     */
    frame_sp = (UWORD)(entry_sp - sizeof(struct int21_guest_iregs));

    /*
     * Layout, high -> low addresses:
     *
     *   entry_sp
     *   public int21_guest_iregs
     *   high canary (>=16 bytes)
     *   CPU_regs
     *   low canary (16 bytes)
     *   CPU_SP -> nested guest pushes continue below here
     *
     * Therefore nested BIOS/device calls cannot legitimately touch either
     * canary. A hit means somebody wrote into the active INT21 frame area,
     * even if the 4-KB DOS stack itself never approached its lower boundary.
     */
    regs_sp = (UWORD)((frame_sp - sizeof(CPU_regs)) & (UWORD)~3u);
    guard_sp = (UWORD)(regs_sp);

    CPU_SP = guard_sp;
    cpu_regs_ref regs_ref(((uint32_t)entry_ss << 4) + regs_sp);
    dos_data_ref idata(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF);
    regs_ref.save_cpu(_cpu);
    regs_ref.flags() = (uint32_t(regs_ref.flags()) & ~0x0041u) | (flags_on_stack & 0x0041u);

    psp_ref current_psp((seg)(UWORD)idata.cu_psp());
    old_ps_stack = current_psp.stack();
    old_user_r = idata.user_r();
    old_prev_user_r = idata.prev_user_r();

    const dos_far_ptr active_ps_stack = MK_FP(entry_ss, frame_sp);
    current_psp.stack(active_ps_stack);
    idata.prev_user_r(old_user_r);
    idata.user_r(active_ps_stack);

    int21_store_guest_frame(active_ps_stack, regs_ref,
                            entry_ip, entry_cs, flags_on_stack);

    idata.int21ax() = (UWORD)R_AX;
    ++idata.indos();

    /*
     * A user INT 24h handler is allowed to abandon the DOS critical-error
     * frame and return directly to its application instead of IRETing back
     * to CriticalError().  In that case ErrorMode remains set.
     *
     * Match upstream int21_service(): clear such a stale ErrorMode on the
     * next ordinary DOS call.  AH=30h and AH=59h are queries which must not
     * disturb the pending version/extended-error state; old character I/O
     * functions 00h..0Ch are excluded by the original condition as well.
     */
    if (R_AH > 0x0c && R_AH != 0x30 && R_AH != 0x59)
      idata.error_mode() = 0;

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
    /*
     * Match FreeDOS int21_service(): file/path/network functions clear
     * carry before dispatch and start a fresh extended-error record.
     * Without resetting CritErrCode here, AH=59h can return an error from
     * an older DOS call instead of the failure that immediately preceded
     * it.  Volkov Commander relies on AH=59h after AX=4300h when probing a
     * not-yet-existing copy destination.
     */
    if ((R_AH >= 0x38 && R_AH <= 0x4f) ||
        (R_AH >= 0x56 && R_AH <= 0x5c) ||
        (R_AH >= 0x5e && R_AH <= 0x60) ||
        (R_AH >= 0x65 && R_AH <= 0x6a) ||
        R_AH == 0x6c)
    {
      R_CF = 0;
      if (R_AH != 0x59)
        idata.crit_err_code() = SUCCESS;
    }

    switch (R_AH) {
      /* Read Keyboard With Echo                                      */
      case 0x01:
      DOS_01:
        R_AL = read_char_stdin(TRUE);
        write_char_stdout(R_AL);
        break;

      case 0x02:
        write_char_stdout(R_AL);
        R_AL = (R_DL == HT) ? (UBYTE)' ' : (UBYTE)R_DL;
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
        read_line(get_sft_idx(STDIN), get_sft_idx(STDOUT), R_FP_DS_DX);
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
        dos_far_ptr dev = sft_to_dev(get_sft(STDIN));
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
          uint32_t bp = ((uint32_t)R_DS << 4) + R_DX;

          while ((c = pload8(bp++)) != '$') // TODO: use massive block find
            write_char_stdout(c);

          R_AL = c;
        }
        break;

      case 0x0E: // set drive
        R_AL = DosSelectDrv(R_DL);
        break;

        /* Get default drive                                           */
      case 0x19:
        R_AL = fdos_idata.default_drive();
        break;

      case 0x1A: // set DTA
        fdos_idata.dta(R_FP_DS_DX);
        break;

      case 0x29: /* DOS 1+ - PARSE FILENAME INTO FCB */
        {
          UWORD next_si;
          R_AL = DosParseFilenameIntoFcbRegs((UBYTE)R_AL, MK_FP(R_DS, R_SI),
                                             MK_FP(R_ES, R_DI), &next_si);
          R_SI = next_si;
        }
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
        {
          UWORD year = 0;
          UBYTE month = 0, day = 0;
          R_AL = DosGetDateRegs(&year, &month, &day);
          R_CX = year;
          R_DH = month;
          R_DL = day;
        }
        break;

        /* Set Date                                                     */
      case 0x2b:
        R_AL = DosSetDateRegs(R_CX, R_DH, R_DL) == SUCCESS ? 0 : 0xFF;
        break;

        /* Get Time                                                     */
      case 0x2c:
        {
          UBYTE hour = 0, minute = 0, second = 0, hundredth = 0;
          DosGetTimeRegs(&hour, &minute, &second, &hundredth);
          R_CH = hour;
          R_CL = minute;
          R_DH = second;
          R_DL = hundredth;
        }
        break;

        /* Set Time                                                     */
      case 0x2d:
        R_AL = DosSetTimeRegs(R_CH, R_CL, R_DH, R_DL) == SUCCESS ? 0 : 0xFF;
        break;
        // get DTA
      case 0x2f:
        {
          const dos_far_ptr dta = fdos_idata.dta();
          R_BX = FP_OFF(dta);
          R_ES = FP_SEG(dta);
        }
        break;

      /* Get (editable) DOS Version                                   */
      case 0x30:
      {
        if (R_AL == 1) /* from RBIL, if AL=1 then return version_flags */
            R_BH = fdos_lol.version_flags();
        else
            R_BH = OEM_ID;
        const psp_ref current_psp((seg)(UWORD)fdos_idata.cu_psp());
        UWORD ver = current_psp.return_dos_version();

        /* PSP мог быть создан не через child_psp() (нативный загрузчик FreeCOM)
           — тогда поле нулевое. Фолбэк на реальную версию ядра. */
        if ((ver & 0x00FF) == 0)
            ver = ((UWORD)fdos_lol.os_setver_minor() << 8) | fdos_lol.os_setver_major();

        R_BH = (R_AL == 1) ? (int)(UBYTE)fdos_lol.version_flags() : OEM_ID;
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
          const uint32_t retp = ((uint32_t)CPU_CS << 4) + CPU_IP;
          const UBYTE b0 = pload8(retp);

          if (b0 == 0x3d &&  /* cmp ax, xxyy */
              (pload8(retp + 3u) == 0x75 ||
               pload8(retp + 3u) == 0x74))       /* je/jne error    */
          {
            R_AL = pload8(retp + 1u);
            R_AH = pload8(retp + 2u);
          }
          else if (b0 == 0x86 &&     /* xchg al,ah   */
                  pload8(retp + 1u) == 0xc4 &&
                  pload8(retp + 2u) == 0x3d &&  /* cmp ax, xxyy */
                  (pload8(retp + 5u) == 0x75 ||
                   pload8(retp + 5u) == 0x74))  /* je/jne error    */
          {
            R_AL = pload8(retp + 4u);
            R_AH = pload8(retp + 3u);
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
          R_DL = fdos_idata.switchar();
          R_AL = 0x00;
          break;
        case 0x01:              /* set switch character */
          fdos_idata.switchar() = R_DL;
          R_AL = 0x00;
          break;
        default:
          /* Upstream (inthndlr.c case 0x37) sends an unsupported SWITCHAR
             subfunction to error_invalid: AX=0001h with CF set. Returning
             AL=FFh with carry untouched is a different API contract - a guest
             testing CF would think the call succeeded. */
          goto error_invalid;
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
            rc = DosGetCountryInformation(cntry, R_FP_DS_DX);
            if (rc >= SUCCESS)
            {
              if (cntry == (UWORD) - 1)
                cntry = DosGetCountry();
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
          rc = (int)result;
          goto error_exit;
        }
        R_CF = 0;
        R_AX = (UWORD)result;
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
          rc = (int)result;
          goto error_exit;
        }
        R_CF = 0;
        R_AX = (UWORD)result;
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
          rc = result;
          goto error_exit;
        }
        R_CF = 0;
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
          rc = (int)result;
          goto error_exit;
        }
        R_CF = 0;
        R_AX = (UWORD)result;
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
          rc = (int)result;
          goto error_exit;
        }
        R_CF = 0;
        R_AX = (UWORD)result;
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
        regs_ref.restore_cpu(_cpu);
        rc = DosDevIOctl();      /* can set critical error code! */
        regs_ref.save_cpu(_cpu);
        cpu_restore_regs(_cpu, &live_saved);

        if (rc < SUCCESS)
        {
          R_AX = -rc;
          if (rc != DE_DEVICE && rc != DE_ACCESS)
            fdos_idata.crit_err_code() = R_AX;
          goto error_carry;
        }
        R_CF = 0;
      }
        break;

      case 0x45: /* DOS 2+ - DUP - DUPLICATE FILE HANDLE */
      {
        const unsigned old_hndl = R_BX;
        const psp_ref process(fdos_idata.cu_psp());
        const UWORD max_files = process.max_files();
        const dos_far_ptr filetab = process.file_table();
        dos_far_ptr old_sft;
        UBYTE old_idx;
        unsigned new_hndl;

        if (far_is_null(filetab) || far_is_end(filetab) || old_hndl >= max_files)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        old_idx = process.file_handle((UWORD)old_hndl);
        if (old_idx == 0xff)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        old_sft = idx_to_sft(old_idx);
        if (far_is_end(old_sft) || (sft_ref(old_sft).mode() & O_NOINHERIT))
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        for (new_hndl = 0; new_hndl < max_files; ++new_hndl)
          if (process.file_handle((UWORD)new_hndl) == 0xff)
            break;

        if (new_hndl >= max_files)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_TOOMANY);
          break;
        }

        process.file_handle((UWORD)new_hndl, old_idx);
        {
          const sft_ref entry(old_sft);
          entry.count((UWORD)(entry.count() + 1u));
        }

        R_AX = (UWORD)new_hndl;
        R_CF = 0;
      }
        break;

      case 0x46: // DOS 2+ - DUP2, FORCEDUP - FORCE DUPLICATE FILE HANDLE
      // BX = existing handle (old), CX = handle to redirect (new)
      {
        const unsigned old_hndl = R_BX;
        const unsigned new_hndl = R_CX;
        const psp_ref process(fdos_idata.cu_psp());
        const UWORD max_files = process.max_files();
        const dos_far_ptr filetab = process.file_table();
        UBYTE old_idx;

        if (far_is_null(filetab) || far_is_end(filetab) ||
            old_hndl >= max_files || new_hndl >= max_files)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        old_idx = process.file_handle((UWORD)old_hndl);
        if (old_idx == 0xff)
        {
          R_CF = 1;
          R_AX = (UWORD)(-DE_INVLDHNDL);
          break;
        }

        if (new_hndl != old_hndl)
        {
          /* DosClose() may remap guest RAM; psp_ref keeps only the PSP
             guest address and re-reads ps_filetab for every access. */
          if (process.file_handle((UWORD)new_hndl) != 0xff)
          {
            COUNT close_rc = DosClose(new_hndl);
            if (close_rc < SUCCESS)
            {
              R_CF = 1;
              R_AX = (UWORD)(-close_rc);
              break;
            }
          }

          const dos_far_ptr old_sft = idx_to_sft(old_idx);
          if (far_is_end(old_sft))
          {
            R_CF = 1;
            R_AX = (UWORD)(-DE_INVLDHNDL);
            break;
          }

          process.file_handle((UWORD)new_hndl, old_idx);
          const sft_ref entry(old_sft);
          entry.count((UWORD)(entry.count() + 1u));
        }
        R_CF = 0;
      }
        break;

      case 0x47: /* DOS 2+ - CWD - GET CURRENT DIRECTORY */
        rc = DosGetCuDir(R_DL, MK_FP(R_DS, R_SI));
        goto short_check;

        /* Set PSP                                                      */
      case 0x50:
        fdos_idata.cu_psp() = R_BX;
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
        R_BX = fdos_idata.cu_psp();
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
            DosUpMemGuest(R_FP_DS_DX, R_CX);
            R_CF = 0;
            break;
          case 0x22:             /* upcase ASCIZ */
            DosUpStringGuest(R_FP_DS_DX);
            R_CF = 0;
            break;
          case 0xA0:             /* upcase single filename character */
            R_DL = DosUpFChar(R_DL);
            R_CF = 0;
            break;
          case 0xA1:             /* upcase filename memory area */
            DosUpFMemGuest(R_FP_DS_DX, R_CX);
            R_CF = 0;
            break;
          case 0xA2:             /* upcase filename ASCIZ */
            DosUpFStringGuest(R_FP_DS_DX);
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
            rc = DosGetData(R_AL, R_BX, R_DX, R_CX, R_FP_ES_DI);
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
          const dos_far_ptr x86_ep = MK_FP(R_ES, R_BX);
          const dos_far_ptr x86_lp = R_FP_DS_DX;
          exec_blk ep;

          /* ES:BX and DS:DX are DOS far pointers, not native pointers.
             Keep the EXEC parameter block in a native snapshot while the
             loader is allowed to page guest memory; only EXEC_LOAD returns
             modified stack/start fields to the caller. */
          guest_read(&ep, x86_ep, sizeof(ep));
          rc = DosExecGuest(R_AL, &ep, x86_lp);
          if (rc == SUCCESS && (R_AL & 0x7f) == EXEC_LOAD)
            guest_write(x86_ep, &ep, sizeof(ep));

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

          rc = DosMemAlloc((UWORD)R_BX, (UBYTE)idata.mem_access_mode(), &para, &asize);
#ifdef INT21_DIAG
          printf("MEM 48 by %04x:%04x bx=%04x -> rc=%d seg=%04x max=%04x\n",
                 readw86(stk_lin(CPU_SS, CPU_SP, 2)),
                 readw86(stk_lin(CPU_SS, CPU_SP, 0)),
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
               readw86(stk_lin(CPU_SS, CPU_SP, 2)),
               readw86(stk_lin(CPU_SS, CPU_SP, 0)), R_ES, rc);
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
                 readw86(stk_lin(CPU_SS, CPU_SP, 2)),
                 readw86(stk_lin(CPU_SS, CPU_SP, 0)),
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
            {
              UWORD date_word, time_word;
              rc = DosGetFtime((COUNT)R_BX, (ddate *)&date_word, (dtime *)&time_word);
              R_DX = date_word;
              R_CX = time_word;
            }
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
            R_AX = fdos_idata.mem_access_mode();
            R_CF = 0;
            break;
          case 0x01:            /* set allocation strategy */
#ifdef INT21_DIAG
            printf("STRAT 5801 bl=%02x by %04x:%04x\n", R_BL,
                   readw86(stk_lin(CPU_SS, CPU_SP, 2)),
                   readw86(stk_lin(CPU_SS, CPU_SP, 0)));
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
              fdos_idata.mem_access_mode() = R_BL;
              R_CF = 0;
            }
            break;
          case 0x02:            /* get UMB link state */
            R_AL = fdos_lol.uppermem_link();
            R_CF = 0;
            break;
          case 0x03:            /* set UMB link state */
#ifdef INT21_DIAG
            printf("LINK 5803 bx=%04x (was %u, root=%04x) by %04x:%04x\n",
                   R_BX, fdos_lol.uppermem_link() & 1, fdos_lol.uppermem_root(),
                   readw86(stk_lin(CPU_SS, CPU_SP, 2)),
                   readw86(stk_lin(CPU_SS, CPU_SP, 0)));
#endif
            /*
             * FreeDOS accepts only BX=0 (unlink) and BX=1 (link).
             * Do not silently normalize every non-zero value to 1.
             */
            if (R_BX > 1 || fdos_lol.uppermem_root() == 0xffff)
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
            printf("LINK done: link=%u\n", fdos_lol.uppermem_link() & 1);
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
        fdos_idata.crit_err_code() = SUCCESS;
        rc = int21_fat32_regs(regs_ref);
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
            rc = lfn_create_entries(R_BX, R_FP_DS_DX);
            break;
          case 0x05:
            rc = lfn_dir_read(R_BX, R_FP_DS_DX);
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
        DosMemChange(fdos_idata.cu_psp(),
                     (UWORD)R_DX < 6u ? (UWORD)6u : (UWORD)R_DX, NULL);
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
            R_DS = DOS_PSP;
            R_SI = (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, ErrorMode));
            R_CX = (UWORD)(sizeof(dos_data) - offsetof(dos_data, ErrorMode));
            R_DX = (UWORD)(offsetof(dos_data, Int21AX) - offsetof(dos_data, ErrorMode));
            R_CF = 0;
            break;
          }

          case 0x07:
          case 0x08:
          case 0x09:
            rc = (int)network_redirector_mx(REM_PRINTREDIR,
                     NULL, (void *)(intptr_t)fdos_idata.int21ax());
            R_CF = 0;
            if (rc != SUCCESS)
              goto error_exit;
            break;

            /* Set Extended Error: DS:DX -> lregs frame
               (AX=0 BX=2 CX=4 DX=6 SI=8 DI=10 DS=12 ES=14)           */
          case 0x0a:
          {
            uint32_t er = (R_DS << 4) + R_DX;
            fdos_idata.crit_err_code()   = readw86(er + 0);
            fdos_idata.crit_err_dev(MK_FP(readw86(er + 14), readw86(er + 10)));
            fdos_idata.crit_err_locus() = read86(er + 5);   /* CH */
            fdos_idata.crit_err_class() = read86(er + 3);   /* BH */
            fdos_idata.crit_err_action() = read86(er + 2);  /* BL */
            R_CF = 0;
            break;
          }

          default:
            fdos_idata.crit_err_code() = SUCCESS;
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
                     (void *)(intptr_t)fdos_idata.int21ax());
            goto short_check;
        }
        break;

      case 0x5f:
        if (R_AL == 7 || R_AL == 8)
        {
          if (R_DL < fdos_lol.lastdrive())
          {
            const dos_far_ptr cds_base = fdos_lol.cds();
            if (!far_is_null(cds_base) && !far_is_end(cds_base))
            {
              const dos_far_ptr cds_ptr =
                  MK_FP(FP_SEG(cds_base),
                        (UWORD)(FP_OFF(cds_base) + (UWORD)R_DL * sizeof(cds)));
              const cds_ref cdsp(cds_ptr);
              /* Upstream tests the whole far pointer ("if (cdsp->cdsDpb)");
                 an offset-only test misreads a DPB that sits at offset 0 of
                 its segment as "absent". */
              if (!far_is_null(cdsp.dpb()))  /* letter of physical drive? */
              {
                UWORD flags = (UWORD)(cdsp.flags() & ~CDSPHYSDRV);
                if (R_AL == 7)
                  flags |= CDSPHYSDRV;
                cdsp.flags(flags);
                break;
              }
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
                   (void *)(intptr_t)fdos_idata.int21ax());
          if (rc != SUCCESS)
          {
            fdos_idata.crit_err_code() = -rc;   /* Maybe set */
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
                    ? (int)(UBYTE)fdos_idata.default_drive() : R_DL - 1;
        const dos_far_ptr dpbp_x86 = get_dpb(drv);

        if (far_is_null(dpbp_x86))
        {
          fdos_idata.crit_err_code() = (UWORD)(-DE_INVLDDRV);
          R_AL = 0xFF;
          break;
        }

        const dpb_ref dpbp(dpbp_x86);
        /* hazard: no error checking! */
        flush_buffers(dpbp.dpb_unit());
        dpbp.flags(M_CHANGED);  /* force flush and reread of drive BPB/DPB */

#ifdef WITHFAT32
        if (media_check(dpbp_x86) < 0 || dpbp.is_fat32())
#else
        if (media_check(dpbp_x86) < 0)
#endif
        {
          R_AL = 0xff;
          fdos_idata.crit_err_code() = (UWORD)(-DE_INVLDDRV);
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
        bpb_to_dpb(MK_FP(R_DS, R_SI), MK_FP(R_ES, R_BP),
#ifdef WITHFAT32
                              (R_CX == 0x4558 && R_DX == 0x4152)
#else
                              FALSE
#endif
        );
        break;

      /* Get/Set disk serial number: original wraps generic IOCTL
         44h/0Dh with CX=0866h (get) / 0846h (set); the drive travels
         in BL, which DosDevIOctl's 0Dh path reads directly.          */
      case 0x69:
      {
        int drv = (R_BL == 0 ? (int)(UBYTE)fdos_idata.default_drive() : R_BL - 1);
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
        /* Caller's return CS, off the INT 21h frame the CPU pushed. */
        new_psp(R_DX, readw86(stk_lin(CPU_SS, CPU_SP, 2)));
        break;

      /* Set Verify Flag                                              */
      case 0x2e:
        fdos_idata.verify_ena() = R_AL & 1;
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
            fdos_idata.break_ena() = R_DL & 1;
            /* fall through so DL only low bit (as in MS-DOS) */
            __attribute__((fallthrough));
          /* Get Ctrl-C flag                                          */
          case 0x00:
            R_DL = fdos_idata.break_ena();
            break;
          case 0x02:            /* get/set extended control break     */
          {
            UBYTE tmp = fdos_idata.break_ena();
            fdos_idata.break_ena() = R_DL & 1;
            R_DL = tmp;
            break;
          }
          /* Get Boot Drive                                           */
          case 0x05:
            R_DL = fdos_lol.boot_drive();
            break;
          /* Get (real) DOS-C version                                 */
          case 0x06:
            R_BL = fdos_lol.os_major();
            R_BH = fdos_lol.os_minor();
            R_DL = 0;                    /* revision, remaining 0   */
            R_DH = fdos_lol.version_flags();   /* bit3: ROM, bit4: HMA    */
            break;
          /* FreeDOS extension: CPU family. Both emulator cores are at
             least 286-class; keep the conservative answer.           */
          case 0xfa:
            R_AL = 2;
            break;
          /* FreeDOS extension: set version returned by INT 21h/30h   */
          case 0xfc:
            fdos_lol.os_setver_major() = R_BL;
            fdos_lol.os_setver_minor() = R_BH;
            break;
          /* FreeDOS extension: get release string pointer in DX:AX   */
          case 0xff:
            R_DX = DOS_PSP;
            R_AX = (UWORD)(0x08F0u + offsetof(lol, os_release_str));
            break;
          default:              /* set AL=0xFF as error, NOT carry    */
            R_AL = 0xff;
            break;
        }
        break;

      /* Get InDOS flag address                                       */
      case 0x34:
        R_ES = ( DOS_PSP );
        R_BX = (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, InDOS));
        break;

      /* Get Verify Flag                                              */
      case 0x54:
        R_AL = fdos_idata.verify_ena();
        break;

      /* UNDOCUMENTED: create child PSP at DX, memory top in SI       */
      case 0x55:
        child_psp(R_DX, fdos_idata.cu_psp(), R_SI);
        /* copy command line from the parent (required for some device
           loaders) */
        fmemcpy(MK_FP(R_DX, 0x80), MK_FP(fdos_idata.cu_psp(), 0x80), 128);

        fdos_idata.cu_psp() = R_DX;
        break;

      /* Get Extended Error information                               */
      case 0x59:
        R_AX = fdos_idata.crit_err_code();
        R_CH = fdos_idata.crit_err_locus();
        R_BH = fdos_idata.crit_err_class();
        R_BL = fdos_idata.crit_err_action();
        {
          const dos_far_ptr dev = fdos_idata.crit_err_dev();
          R_DI = FP_OFF(dev);
          R_ES = FP_SEG(dev);
        }
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
        /* Upstream's dispatcher default deliberately falls through into the
           CP/M compatibility group just above (inthndlr.c: its default label
           is marked "Fall through" and lands on case 0x18/0x1d/0x1e/0x20/
           0x61/0x6b, which do AL = 0 and break). So an unknown AH returns
           AL=0 with carry UNCHANGED. Leaving the registers untouched, as this
           port did, is a third behaviour matching neither. NO_HANDLER_DETECTOR
           stays available for debugging without altering release behaviour. */
        R_AL = 0;
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
    if ((UWORD)idata.crit_err_code() == SUCCESS)
        idata.crit_err_code() = (UWORD)R_AX;      /* Maybe set */
    R_CF = 1;
    goto exit_dispatch;

error_carry:
    R_CF = 1;

exit_dispatch:
    flags_on_stack = (flags_on_stack & ~0x0041u)
                   | (uint32_t(regs_ref.flags()) & 0x0041u);
    /*
     * Keep the published frame coherent through the end of dispatch.
     * Critical-error handling and DOS extenders may inspect PSP:2Eh
     * while this invocation is active.
     */
    int21_store_guest_frame(current_psp.stack(), regs_ref,
                            entry_ip, entry_cs, flags_on_stack);

    /* Upstream copies its local lregs frame back only after dispatch.
     * Do the same here, then patch CF/ZF in the caller's IRET frame. */
    regs_ref.restore_cpu(_cpu);
    dpb_watch_int21_checkpoint(cpu, "exit");
    writew86(stk_lin(entry_ss, entry_sp, 4), flags_on_stack);
    dpb_watch_int21_checkpoint(cpu, "after-flags-write");
    current_psp.stack(old_ps_stack);
    idata.user_r(old_user_r);
    idata.prev_user_r(old_prev_user_r);
    CPU_SP = entry_sp;
    --idata.indos();
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

/*
 * CP/M-compatible DOS CALL 5 entry.
 *
 * PSP:0005h contains  9Ah C0h 00h 00h 00h  = CALL FAR 0000:00C0, and
 * 0000:00C0 is a JMP FAR to the native fake-BIOS page FFE0:0030, which the
 * dispatcher routes here. PSPInit() (kernel.c) writes both. This is a
 * FAR-CALL entry, not a software interrupt: on entry the guest stack holds
 *
 *   SS:SP+0  000Ah              the offset just past CALL FAR in the PSP
 *   SS:SP+2  caller PSP segment (== the CALL FAR return CS)
 *   SS:SP+4  return offset from the program's near CALL PSP:0005h
 *
 * Upstream entry.asm discards the 000Ah, rebuilds the rest into a normal
 * INT 21h frame, runs function CL (only 00h..24h are valid here), then IRETs
 * straight back to the near-call return address.
 *
 * All stack addressing goes through stk_lin() so the 16-bit SP wrap is
 * honoured (see init-mod.h) - the hand-written "(SS<<4)+sp+n" form in the
 * original proposal would mis-address a caller whose SP sits near 0xFFFF.
 *
 * Returns false: this entry consumes its own CALL/RETF-style frame and must
 * not fall through to the dispatcher's common IRET path.
 */
bool fdos_30h(CPU* _cpu)
{
    const UWORD entry_sp = CPU_SP;
    const UWORD caller_cs = readw86(stk_lin(CPU_SS, entry_sp, 2));
    const UWORD caller_ip = readw86(stk_lin(CPU_SS, entry_sp, 4));
    const UWORD return_sp = (UWORD)(entry_sp + 6);
    const UWORD int_sp    = (UWORD)(return_sp - 6);   /* == entry_sp */
    UWORD return_flags = cpu_getflags(_cpu);

    cpu = _cpu;

    if (CPU_CL > 0x24)
    {
        CPU_AL = 0;
    }
    else
    {
        /* Build the hardware half of a normal INT 21h frame (IP, CS, FLAGS);
           fdos_21h() adds its guest-visible iregs below it. */
        writew86(stk_lin(CPU_SS, int_sp, 0), caller_ip);
        writew86(stk_lin(CPU_SS, int_sp, 2), caller_cs);
        writew86(stk_lin(CPU_SS, int_sp, 4), return_flags);
        CPU_SP = int_sp;
        CPU_AH = CPU_CL;

        fdos_21h(_cpu);

        return_flags = readw86(stk_lin(CPU_SS, int_sp, 4));
    }

    /* Complete the synthetic IRET straight back to the near caller.
       cpu_setflags() is (set-mask, clear-mask). */
    cpu_setflags(_cpu, return_flags, (UWORD)~return_flags);
    CPU_SP = return_sp;
    SET_CS(caller_cs);
    SET_IP(caller_ip);
    return false;
}
