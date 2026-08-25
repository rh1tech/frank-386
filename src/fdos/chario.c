/****************************************************************/
/*                                                              */
/*                          chario.c                            */
/*                           DOS-C                              */
/*                                                              */
/*    Character device functions and device driver interface    */
/*                                                              */
/*                      Copyright (c) 1994                      */
/*                      Pasquale J. Villani                     */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/*                                                              */
/****************************************************************/

/*
 * Port notes:
 *
 * CharRequest() and BinaryCharIO() are defined in fdos_21h.c (they are
 * shared with the clock driver path there). They are declared here via
 * proto.h.
 *
 * Guest-memory rule:
 *   device/SFT/console state is kept as dos_far_ptr or linear guest offsets;
 *   buffered console I/O uses pload/pstore/guest_move_block so no page-cache
 *   pointer survives a DOS/device call.
 *
 * fast_put_char():
 *   Original: inline asm "int 29h"
 *   Port: calls fdos_29h(cpu) - same handler, no BIOS stack games needed.
 *   Requires extern CPU *cpu from fdos_21h.c (see note there).
 *
 * Control-Break handling is kept in this file, as in upstream break.c;
 * the only architectural adaptation is using bios_intcall()/
 * request_terminate() instead of the real-mode spawn_int23() trampoline.
 *
 */

#include "hdrs.h"
#include "request_guest.h"
#include "bios/bios.h"

extern const dos_far_ptr x86_szLine; /// TODO: ensure reusable
extern CPU *cpu;

STATIC int CharIO(dos_far_ptr *pdev, unsigned char ch, unsigned command)
{
  --CPU_SP;
  dos_far_ptr x86_c = MK_FP(CPU_SS, CPU_SP);
  write86(EFFECTIVE(x86_c), ch);
  int err = (int)BinaryCharIO(pdev, 1, x86_c, command);
  ch = read86(EFFECTIVE(x86_c));
  ++CPU_SP;
  if (err == 0)
    return 256;
  if (err < 0)
    return err;
  return ch;
}


#define CHARIO_SDA_LINEAR (((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF)
#define CHARIO_LOL_LINEAR (((uint32_t)DOS_PSP << 4) + 0x08f0u)
#define CHARIO_LOCAL_LINEAR (CHARIO_SDA_LINEAR + offsetof(struct dos_data, local_buffer))
#define CHARIO_KB_LINEAR (CHARIO_SDA_LINEAR + offsetof(struct dos_data, kb_buf))

static uint32_t chario_far_linear(dos_far_ptr p)
{
  return ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
}

static inline uint32_t __attribute__((always_inline)) chario_sft_linear(dos_far_ptr p)
{
  return ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
}

static inline UWORD __attribute__((always_inline)) chario_sft_flags(dos_far_ptr p)
{
  return pload16(chario_sft_linear(p) + offsetof(sft, sft_flags));
}

static inline dos_far_ptr __attribute__((always_inline)) chario_sft_dev(dos_far_ptr p)
{
  const uint32_t a = chario_sft_linear(p) + offsetof(sft, sft_dev);
  return MK_FP(pload16(a + 2u), pload16(a));
}

static inline ULONG __attribute__((always_inline)) chario_sft_position(dos_far_ptr p)
{
  return pload32(chario_sft_linear(p) + offsetof(sft, sft_posit));
}

static inline ULONG __attribute__((always_inline)) chario_sft_size(dos_far_ptr p)
{
  return pload32(chario_sft_linear(p) + offsetof(sft, sft_size));
}

static inline UWORD __attribute__((always_inline)) chario_dhdr_attr(dos_far_ptr p)
{
  return pload16(chario_far_linear(p) + offsetof(struct dhdr, dh_attr));
}

static dos_far_ptr chario_syscon(void)
{
  const uint32_t a = CHARIO_LOL_LINEAR + offsetof(struct lol, syscon);
  return MK_FP(pload16(a + 2u), pload16(a));
}

static UBYTE chario_scr_pos(void)
{
  return pload8(CHARIO_SDA_LINEAR + offsetof(struct dos_data, scr_pos));
}

static void chario_set_scr_pos(UBYTE v)
{
  pstore8(CHARIO_SDA_LINEAR + offsetof(struct dos_data, scr_pos), v);
}

static UBYTE chario_printer_echo(void)
{
  return pload8(CHARIO_SDA_LINEAR + offsetof(struct dos_data, PrinterEcho));
}

static dos_short_ptr chario_inputptr(void)
{
  return pload16(CHARIO_LOL_LINEAR + offsetof(struct lol, inputptr));
}

static void chario_set_inputptr(dos_short_ptr v)
{
  pstore16(CHARIO_LOL_LINEAR + offsetof(struct lol, inputptr), v);
}

static dos_far_ptr chario_internal_kb(void)
{
  return MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, kb_buf)));
}

STATIC int CharRequest(/*struct dhdr*/dos_far_ptr *pdev, unsigned command)
{
  const dos_far_ptr rq_far = fdos_sda_request_far(offsetof(struct dos_data, ClkReqHdr));
  const fdos_request_guest_ref rq = fdos_request_guest(rq_far);
  UWORD status;

  FDOS_REQUEST_SET8(rq, r_command, command);
  FDOS_REQUEST_SET8(rq, r_unit, 0);
  FDOS_REQUEST_SET16(rq, r_status, 0);
  FDOS_REQUEST_SET8(rq, r_length, sizeof(request));
  execrh(rq_far, *pdev);
  status = FDOS_REQUEST_GET16(rq, r_status);
  if (status & S_ERROR)
  {
    for (;;) {
      switch (char_error_status(status, *pdev))
      {
      case ABORT:
      case FAIL:
        return DE_INVLDACC;
      case CONTINUE:
        FDOS_REQUEST_SET16(rq, r_count, 0);
        return 0;
      case RETRY:
        return 1;
      }
    }
  }
  return SUCCESS;
}

long BinaryCharIO(/*struct dhdr*/dos_far_ptr *pdev, size_t n, dos_far_ptr bp, unsigned command)
{
  int err;
  do
  {
    {
      const fdos_request_guest_ref rq = fdos_request_guest(
          fdos_sda_request_far(offsetof(struct dos_data, ClkReqHdr)));
      FDOS_REQUEST_SET16(rq, r_count, n);
      FDOS_REQUEST_SET_FAR(rq, r_trans, bp);
    }
    err = CharRequest(pdev, command);
  } while (err == 1);
  return err == SUCCESS ? (long)FDOS_REQUEST_GET16(
      fdos_request_guest(fdos_sda_request_far(offsetof(struct dos_data, ClkReqHdr))), r_count) : err;
}

/* STATE FUNCTIONS */

STATIC void CharCmd(dos_far_ptr *pdev, unsigned command)
{
  while (CharRequest(pdev, command) == 1);
}

STATIC int Busy(dos_far_ptr *pdev)
{
  CharCmd(pdev, C_NDREAD);
  {
    const fdos_request_guest_ref rq = fdos_request_guest(
        fdos_sda_request_far(offsetof(struct dos_data, ClkReqHdr)));
    if (FDOS_REQUEST_GET16(rq, r_status) & S_ERROR)
      CharCmd(pdev, C_ISTAT);
    return FDOS_REQUEST_GET16(rq, r_status) & S_BUSY;
  }
}

void con_flush(dos_far_ptr *pdev)
{
  CharCmd(pdev, C_IFLUSH);
}

/* if the sft is invalid, then we just monitor syscon */
dos_far_ptr sft_to_dev(dos_far_ptr sft_ptr)
{
  if (far_is_null(sft_ptr) || far_is_end(sft_ptr))
    return chario_syscon();
  if (chario_sft_flags(sft_ptr) & SFT_FDEVICE)
    return chario_sft_dev(sft_ptr);
  return MK_FP(0, 0);
}

int StdinBusy(void)
{
  dos_far_ptr sft_ptr = get_sft(STDIN);
  dos_far_ptr dev = sft_to_dev(sft_ptr);

  if (!far_is_null(dev))
    return Busy(&dev);

  return chario_sft_position(sft_ptr) >= chario_sft_size(sft_ptr);
}

/* get character from the console - this is how DOS gets
   CTL_C/CTL_S/CTL_P when outputting */
int ndread(dos_far_ptr *pdev)
{
  CharCmd(pdev, C_NDREAD);
  {
    const fdos_request_guest_ref rq = fdos_request_guest(
        fdos_sda_request_far(offsetof(struct dos_data, ClkReqHdr)));
    if (FDOS_REQUEST_GET16(rq, r_status) & S_BUSY)
      return -1;
    return FDOS_REQUEST_GET8(rq, r_ndbyte);
  }
}

/* OUTPUT FUNCTIONS */

/*
 * Original: inline asm "int 29h" (WATCOMC/TURBOC/GCC variants).
 * Port: delegate to the existing INT 29h handler directly.
 * cpu is the current INT 21h context (extern from fdos_21h.c).
 */
STATIC void fast_put_char(unsigned char chr)
{
//  CPU_regs saved;
//  cpu_save_regs(cpu, &saved);
//  CPU_AH = 0x0e;
//  CPU_AL = chr;
//  CPU_BX = 0x0007;
//  bios_intcall(cpu, 0x10, "PUT CHAR 10h");
//  cpu_restore_regs(cpu, &saved);
  bios_teletype(cpu, chr, 0);
}

void update_scr_pos(unsigned char c, unsigned char count)
{
  unsigned char scrpos = chario_scr_pos();

  if (c == CR)
    scrpos = 0;
  else if (c == BS) {
    if (scrpos > 0)
      scrpos--;
  } else if (c != LF && c != BELL) {
    scrpos += count;
  }
  chario_set_scr_pos(scrpos);
}

STATIC int raw_get_char(dos_far_ptr *pdev, BOOL check_break);

long cooked_write(dos_far_ptr *pdev, size_t n, dos_far_ptr bp)
{
  size_t xfer;
  uint32_t src = chario_far_linear(bp);

  /* bit 7 means fastcon; low 5 bits count number of characters */
  unsigned char fast_counter = (chario_dhdr_attr(*pdev) & ATTR_FASTCON) << 3;

  for (xfer = 0; xfer < n; xfer++)
  {
    int err;
    unsigned char count = 1, c = pload8(src++);

    if (c == CTL_Z)
      break;
    if (c == HT) {
      count = 8 - (chario_scr_pos() & 7);
      c = ' ';
    }
    update_scr_pos(c, count);
    do {
      if (fast_counter <= 0x80 && check_handle_break(pdev) == CTL_S)
        raw_get_char(pdev, TRUE);
      if (terminate_requested())
        return xfer;
      fast_counter++;
      fast_counter &= 0x9f;
      if (chario_printer_echo())
      {
        pstore8(chario_far_linear(x86_szLine), (UBYTE)c);
        DosWrite(STDPRN, 1, x86_szLine);
      }
      if (fast_counter & 0x80)
        fast_put_char(c);
      else
      {
        err = CharIO(pdev, c, C_OUTPUT);
        if (err < 0)
          return err;
      }
    } while (--count != 0);
  }
  return xfer;
}

/* writes character for disk file or device */
void write_char(int c, int sft_idx)
{
  pstore8(chario_far_linear(x86_DATA), (UBYTE)c);
  dos_far_ptr x86_ch = x86_DATA;
  DosRWSft(sft_idx, 1, x86_ch, XFR_FORCE_WRITE);
}

void write_char_stdout(int c)
{
  unsigned char count = 1;
  unsigned flags = chario_sft_flags(get_sft(STDOUT));

  /* ah=2, ah=9 should expand tabs even for raw devices and disk files */
  if ((flags & (SFT_FDEVICE|SFT_FBINARY)) != SFT_FDEVICE)
  {
    if (c == HT) {
      count = 8 - (chario_scr_pos() & 7);
      c = ' ';
    }
    /* for raw CONOUT devices already updated in dosfns.c */
    if ((flags & (SFT_FDEVICE|SFT_FCONOUT)) != (SFT_FDEVICE|SFT_FCONOUT))
      update_scr_pos(c, count);
  }

  do {
    write_char(c, get_sft_idx(STDOUT));
  } while (--count != 0);
}

#define iscntrl(c) ((unsigned char)(c) < ' ')

/* this is for handling things like ^C, mostly used in echoed input */
STATIC int echo_char(int c, int sft_idx)
{
  int out = c;
  if (iscntrl(c) && c != HT && c != LF && c != CR)
  {
    write_char('^', sft_idx);
    out += '@';
  }
  write_char(out, sft_idx);
  return c;
}

STATIC void destr_bs(int sft_idx)
{
  write_char(BS, sft_idx);
  write_char(' ', sft_idx);
  write_char(BS, sft_idx);
}

/* READ FUNCTIONS */

long cooked_read(dos_far_ptr *pdev, size_t n, dos_far_ptr bp)
{
  unsigned xfer = 0;
  uint32_t dst = chario_far_linear(bp);
  int c;
  while(n--)
  {
    c = raw_get_char(pdev, TRUE);
    if (c < 0)
      return c;
    if (c == 256)
      break;
    pstore8(dst++, (UBYTE)c);
    xfer++;
    if ((unsigned char)c == CTL_Z)
      break;
  }
  return xfer;
}

#define CTRL_BREAK_FLAG_ADDR 0x0471u
#define CTRL_BREAK_FLAG_MASK 0x80u

/* Check the BIOS data-area Ctrl-Break latch (0040:0071 bit 7), exactly
   like upstream FreeDOS' CB_FLG.  Keyboard IRQ handling is responsible
   for setting this bit for the dedicated Break key sequence. */
unsigned char ctrl_break_pressed(void)
{
  return pload8(CTRL_BREAK_FLAG_ADDR) & CTRL_BREAK_FLAG_MASK;
}

unsigned char check_handle_break(dos_far_ptr *pdev)
{
  unsigned char c = CTL_C;

  dos_far_ptr syscon = chario_syscon();

  if (!ctrl_break_pressed())
    c = (unsigned char)ndread(&syscon);

  if (c != CTL_C && EFFECTIVE(*pdev) != EFFECTIVE(syscon))
    c = (unsigned char)ndread(pdev);

  if (c == CTL_C)
    handle_break(pdev, -1);

  return c;
}

void handle_break(dos_far_ptr *pdev, int sft_out)
{
  static const char ctrl_c_text[] = "^C\r\n";

  /* Reset the BIOS Ctrl-Break latch before invoking user code. */
  pstore8(CTRL_BREAK_FLAG_ADDR,
          pload8(CTRL_BREAK_FLAG_ADDR) & ~CTRL_BREAK_FLAG_MASK);

  /* Match upstream: discard pending console input and echo ^C either
     to the supplied output SFT or to the current input device. */
  con_flush(pdev);
  {
    dos_far_ptr x86_cc = guest_stack_alloc(cpu, sizeof(ctrl_c_text) - 1);
    guest_write(x86_cc, ctrl_c_text, sizeof(ctrl_c_text) - 1);
    if (sft_out == -1)
      cooked_write(pdev, sizeof(ctrl_c_text) - 1, x86_cc);
    else
      DosRWSft(sft_out, sizeof(ctrl_c_text) - 1, x86_cc, XFR_FORCE_WRITE);
    CPU_SP = (uint16_t)(CPU_SP + (sizeof(ctrl_c_text) - 1));
  }

  /* Upstream spawn_int23() (procsupt.asm) switches to the user stack,
     does CLC (default action: resume), invokes the process' INT 23h
     handler and inspects HOW the handler returned:

       - IRET or RETF 2 (SP back to the pre-INT value): Carry is IGNORED
         (??int23_ign_carry) - the interrupted DOS call is respawned.
         FreeCOM's cbreak_handler is exactly this: "inc counter; clc;
         retf 2".
       - RETF 0 (the FLAGS image is left on the stack, SP short by one
         word): SP is fixed up and Carry is honoured - CF=1 means
         "terminate program" (break_flg++, term_type=1, AH=0), CF=0
         respawns.

     bios_intcall() performs the guest INT/IRET transition; the live
     guest SS:SP here is the user stack of the interrupted INT 21h, and
     bios_intcall() restores CS:IP but deliberately not SP - which is
     precisely what lets us read the handler's return style out of it.

     Respawn itself (re-dispatching the saved INT 21h request) has no
     native equivalent in this port yet: the C call chain of the current
     INT 21h implementation cannot be unwound and restarted.  The resume
     path therefore RETURNS to the caller, which continues the
     interrupted operation in place; the console-input loops re-issue
     their read, which is observably equivalent for the AH=01/07/08/0Ah
     paths where ^C arrives.  The terminate path goes through
     request_terminate(0, 1) - the port's combined equivalent of
     break_flg/term_type=1/AH=0 - and callers must stop I/O promptly via
     terminate_requested(). */
  {
    UWORD sp_before;
    UBYTE cf_after;

    cf = 0;                     /* spawn_int23: CLC - default is resume */
    sp_before = CPU_SP;
    bios_intcall(cpu, 0x23, "DOS Ctrl-Break INT23");
    cf_after = (UBYTE)cf;

    if (CPU_SP != sp_before)
    {
      /* RETF 0: discard the FLAGS image the handler left behind */
      CPU_SP = sp_before;
      if (cf_after)
        request_terminate(0, 1);
    }
    /* equal SP: IRET or RETF 2 - ignore Carry, resume */
  }
}

void DosIdle_int(void)
{
  CPU_regs saved;
  /* INT 28h is advisory. Like upstream DosIdle_int(), preserve the
     DOS caller's complete register context around the hook. */
  cpu_save_regs(cpu, &saved);
  bios_intcall(cpu, 0x28, "DOS IDLE");
  cpu_restore_regs(cpu, &saved);
}

/*
 * read_char_sft_dev - internal: read one char from device or file sft.
 *
 * sft_in/sft_out: sft indices (-1 = use pdev directly).
 * pdev:           device header far ptr (used if sft_in == -1 or sft is a device).
 * check_break:    whether to monitor for ^C/^S.
 */
STATIC unsigned read_char_sft_dev(int sft_in, int sft_out,
                                       dos_far_ptr *pdev,
                                       BOOL check_break)
{
  unsigned c;
  if (FP_SEG(*pdev) || FP_OFF(*pdev)) /* *pdev != NULL */
  {
    FOREVER
    {
      if (ctrl_break_pressed())
      {
        c = CTL_C;
        break;
      }
      if (!Busy(pdev))
      {
        c = CharIO(pdev, 0, C_INPUT);
        break;
      }
      if (check_break && EFFECTIVE(*pdev) != EFFECTIVE(chario_syscon()))
      {
        dos_far_ptr syscon = chario_syscon();
        check_handle_break(&syscon);
      }
      /* the idle int is only safe if we're using the character stack */
      /* Original: user_r->AH < 0xd
         Port: CPU_AH gives the current INT 21h function number */
      if (CPU_AH < 0xd)
        DosIdle_int();
    }
  }
  else
  {
    dos_far_ptr x86_ch = x86_DATA;
    pstore8(chario_far_linear(x86_ch), 0);
    DosRWSft(sft_in, 1, x86_ch, XFR_READ);
    c = pload8(chario_far_linear(x86_ch));
  }

  /* check for break or stop on sft_in, echo to sft_out */
  if (check_break && (c == CTL_C || c == CTL_S))
  {
    if (c == CTL_S)
      c = read_char_sft_dev(sft_in, sft_out, pdev, FALSE);
    if (c == CTL_C)
      handle_break(pdev, sft_out);
    if (terminate_requested())
      return c;   /* upstream: spawn_int23() never returns on terminate */
    /* DOS oddity: if you press ^S somekey ^C then ^C does not break */
    c = read_char(sft_in, sft_out, FALSE);
  }
  return c;
}

STATIC int raw_get_char(dos_far_ptr *pdev, BOOL check_break)
{
  return read_char_sft_dev(-1, -1, pdev, check_break);
}

unsigned char read_char(int sft_in, int sft_out, BOOL check_break)
{
  dos_far_ptr dev = sft_to_dev(idx_to_sft(sft_in));
  return read_char_sft_dev(sft_in, sft_out, &dev, check_break);
}

STATIC unsigned char read_char_check_break(int sft_in, int sft_out)
{
  return read_char(sft_in, sft_out, TRUE);
}

unsigned char read_char_stdin(BOOL check_break)
{
  return read_char(get_sft_idx(STDIN), get_sft_idx(STDOUT), check_break);
}

/* reads a line (buffered, called by int21/ah=0ah, 3fh) */
void read_line(int sft_in, int sft_out, dos_far_ptr x86_kp)
{
  const uint32_t kbase = chario_far_linear(x86_kp);
  const uint32_t kbuf = kbase + offsetof(keyboard, kb_buf);
  unsigned c;
  unsigned cu_pos = chario_scr_pos();
  unsigned count = 0, stored_pos = 0;
  unsigned size = pload8(kbase + offsetof(keyboard, kb_size));
  unsigned stored_size = pload8(kbase + offsetof(keyboard, kb_count));
  BOOL insert = FALSE, first = TRUE;

  if (size == 0)
    return;

  if (pload8(kbuf + stored_size) != CR)
    stored_size = 0;

  do
  {
    unsigned new_pos = stored_size;

    c = read_char_check_break(sft_in, sft_out);
    if (c == 0)
      c = (unsigned)read_char_check_break(sft_in, sft_out) << 8;
    switch (c)
    {
      case LF:
        if (!first)
        {
          write_char(CR, sft_out);
          write_char(LF, sft_out);
        }
        break;

      case CTL_F:
        break;

      case RIGHT:
      case F1:
        if (stored_pos < stored_size && count < size - 1)
          pstore8(CHARIO_LOCAL_LINEAR + count++,
                  (UBYTE)echo_char(pload8(kbuf + stored_pos++), sft_out));
        break;

      case F2:
      case F4:
        {
          unsigned char c2 = read_char_check_break(sft_in, sft_out);
          new_pos = stored_pos;
          if (c2 == 0)
          {
            read_char_check_break(sft_in, sft_out);
          }
          else
          {
            unsigned pos;
            for (pos = stored_pos; pos < stored_size; ++pos)
              if (pload8(kbuf + pos) == c2)
              {
                new_pos = pos + 1;
                break;
              }
          }
        }
        /* fall through */
      case F3:
        if (c != F4)
        {
          while (stored_pos < new_pos && count < size - 1)
            pstore8(CHARIO_LOCAL_LINEAR + count++,
                    (UBYTE)echo_char(pload8(kbuf + stored_pos++), sft_out));
        }
        stored_pos = new_pos;
        break;

      case F5:
        guest_move_block(kbuf, CHARIO_LOCAL_LINEAR, count);
        stored_size = count;
        write_char('@', sft_out);
        goto start_new_line;

      case INS:
        insert = !insert;
        break;

      case DEL:
        stored_pos++;
        break;

      case LEFT:
      case CTL_BS:
      case BS:
        if (count > 0)
        {
          unsigned new_pos2;
          char c2 = (char)pload8(CHARIO_LOCAL_LINEAR + --count);
          if (c2 == HT)
          {
            unsigned i;
            new_pos2 = cu_pos;
            for (i = 0; i < count; i++)
            {
              UBYTE lc = pload8(CHARIO_LOCAL_LINEAR + i);
              if (lc == HT)
                new_pos2 = (new_pos2 + 8) & ~7;
              else if (iscntrl(lc))
                new_pos2 += 2;
              else
                new_pos2++;
            }
            do
              destr_bs(sft_out);
            while (chario_scr_pos() > new_pos2);
          }
          else
          {
            if (iscntrl(c2))
              destr_bs(sft_out);
            destr_bs(sft_out);
          }
        }
        if (stored_pos > 0)
          stored_pos--;
        break;

      case ESC:
        write_char('\\', sft_out);
    start_new_line:
        write_char(CR, sft_out);
        write_char(LF, sft_out);
        for (count = 0; count < cu_pos; count++)
          write_char(' ', sft_out);
        count = 0;
        stored_pos = 0;
        insert = FALSE;
        break;

      case F6:
        c = CTL_Z;
        /* fall through */

      default:
        if (c >= 256)
          break;
        if (count < size - 1 || c == CR)
          pstore8(CHARIO_LOCAL_LINEAR + count++, (UBYTE)echo_char(c, sft_out));
        else
          write_char(BELL, sft_out);
        if (stored_pos < stored_size && !insert)
          stored_pos++;
        break;
    }
    first = FALSE;
  } while (c != CR);

  guest_move_block(kbuf, CHARIO_LOCAL_LINEAR, count);
  if (count > LINEBUFSIZECON)
    pstore8(CHARIO_KB_LINEAR + offsetof(keyboard, kb_size), 0);
  pstore8(kbase + offsetof(keyboard, kb_count), (UBYTE)(count - 1));
}

/* called by handle func READ (int21/ah=3f) */
size_t read_line_handle(int sft_idx, size_t n, dos_far_ptr bp)
{
  const dos_far_ptr kbp = chario_internal_kb();
  const uint32_t kbase = chario_far_linear(kbp);
  const uint32_t kbuf = kbase + offsetof(keyboard, kb_buf);
  size_t chars_left;
  uint32_t input_linear;
  dos_short_ptr input_off = chario_inputptr();

  if (input_off == 0)
  {
    if (pload8(kbase + offsetof(keyboard, kb_size)) != LINEBUFSIZECON)
    {
      pstore8(kbase + offsetof(keyboard, kb_count), 0);
      pstore8(kbase + offsetof(keyboard, kb_size), LINEBUFSIZECON);
    }
    read_line(sft_idx, sft_idx, kbp);
    {
      UBYTE count = pload8(kbase + offsetof(keyboard, kb_count));
      pstore8(kbuf + count + 1, (UBYTE)echo_char(LF, sft_idx));
    }
    input_off = (dos_short_ptr)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, kb_buf) +
                                offsetof(keyboard, kb_buf));
    chario_set_inputptr(input_off);
    input_linear = ((uint32_t)DOS_PSP << 4) + input_off;
    if (pload8(input_linear) == CTL_Z)
    {
      chario_set_inputptr(0);
      return 0;
    }
  }
  else
  {
    input_linear = ((uint32_t)DOS_PSP << 4) + input_off;
  }

  chars_left = (size_t)((kbuf + pload8(kbase + offsetof(keyboard, kb_count)) + 2u) -
                        input_linear);
  if (n > chars_left)
    n = chars_left;

  guest_move_block(chario_far_linear(bp), input_linear, n);
  input_linear += (uint32_t)n;
  if (n == chars_left)
    chario_set_inputptr(0);
  else
    chario_set_inputptr((dos_short_ptr)(input_linear - ((uint32_t)DOS_PSP << 4)));
  return n;
}
