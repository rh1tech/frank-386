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
 * FAR pointers to struct dhdr:
 *   Original: struct dhdr FAR ** pdev
 *   Port:     dos_far_ptr *pdev  (same pattern as fdos_21h.c)
 *   Callers always dereference via ARM_PTR(*pdev) or pass directly.
 *
 * SDA / LoL field access:
 *   scr_pos      -> internal_data->scr_pos
 *   PrinterEcho  -> internal_data->PrinterEcho
 *   local_buffer -> internal_data->local_buffer
 *   kb_buf       -> internal_data->kb_buf
 *   user_r->AH   -> CPU_AH  (cpu is the current INT 21h context,
 *                             extern'd from fdos_21h.c)
 *   inputptr     -> LoL->inputptr  (dos_short_ptr, offset within
 *                   DOS_PSP segment; NULL == 0; dereference via
 *                   ARM_PTR(MK_FP(DOS_PSP, LoL->inputptr)))
 *   syscon       -> LoL->syscon  (dos_far_ptr)
 *
 * fast_put_char():
 *   Original: inline asm "int 29h"
 *   Port: calls fdos_29h(cpu) - same handler, no BIOS stack games needed.
 *   Requires extern CPU *cpu from fdos_21h.c (see note there).
 *
 * ctrl_break_pressed(), check_handle_break(), handle_break(),
 * DosIdle_int(): stubs for this iteration (same approach as the
 * cooked_read/write stubs that were in kernel.c).
 *
 * DosWrite(STDPRN, 1, &c) in PrinterEcho path:
 *   DosWrite is a macro -> DosRWSft(..., XFR_WRITE).
 *   'c' is a local unsigned char; we pass its address as a native
 *   pointer cast to dos_far_ptr via the NATIVE_TO_FAR() pattern used
 *   elsewhere (treat the native address as a linear address, which is
 *   valid because dosfns.c/DosRWSft uses ARM_PTR(bp) to dereference it
 *   for device writes).
 */

#include "hdrs.h"
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

STATIC int CharRequest(/*struct dhdr*/dos_far_ptr *pdev, unsigned command)
{
  struct dhdr* dev = (struct dhdr*)ARM_PTR(*pdev);
  CharReqHdr.r_command = command;
  CharReqHdr.r_unit = 0;
  CharReqHdr.r_status = 0;
  CharReqHdr.r_length = sizeof(request);
  execrh(linear_to_far( &CharReqHdr ), *pdev);
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

long BinaryCharIO(/*struct dhdr*/dos_far_ptr *pdev, size_t n, dos_far_ptr bp, unsigned command)
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

/* STATE FUNCTIONS */

STATIC void CharCmd(dos_far_ptr *pdev, unsigned command)
{
  while (CharRequest(pdev, command) == 1);
}

STATIC int Busy(dos_far_ptr *pdev)
{
  CharCmd(pdev, C_NDREAD);
  if (CharReqHdr.r_status & S_ERROR)
    CharCmd(pdev, C_ISTAT);
  return CharReqHdr.r_status & S_BUSY;
}

void con_flush(dos_far_ptr *pdev)
{
  CharCmd(pdev, C_IFLUSH);
}

/* if the sft is invalid, then we just monitor syscon */
dos_far_ptr sft_to_dev(sft *s)
{
  if (s == NULL)
    return LoL->syscon;
  if (s->sft_flags & SFT_FDEVICE)
    return s->sft_dev;
  return MK_FP(0, 0); /* NULL far ptr */
}

int StdinBusy(void)
{
  dos_far_ptr _s = get_sft(STDIN);
  sft* s = (sft*) ARM_PTR ( _s );
  dos_far_ptr dev = sft_to_dev(s);

  if (FP_SEG(dev) || FP_OFF(dev))
    return Busy(&dev);

  return s->sft_posit >= s->sft_size;
}

/* get character from the console - this is how DOS gets
   CTL_C/CTL_S/CTL_P when outputting */
int ndread(dos_far_ptr *pdev)
{
  CharCmd(pdev, C_NDREAD);
  if (CharReqHdr.r_status & S_BUSY)
    return -1;
  return CharReqHdr.r_ndbyte;
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
  unsigned char scrpos = internal_data->scr_pos;

  if (c == CR)
    scrpos = 0;
  else if (c == BS) {
    if (scrpos > 0)
      scrpos--;
  } else if (c != LF && c != BELL) {
    scrpos += count;
  }
  internal_data->scr_pos = scrpos;
}

STATIC int raw_get_char(dos_far_ptr *pdev, BOOL check_break);

long cooked_write(dos_far_ptr *pdev, size_t n, char *bp)
{
  size_t xfer;

  /* bit 7 means fastcon; low 5 bits count number of characters */
  struct dhdr *dev = (struct dhdr *)ARM_PTR(*pdev);
  unsigned char fast_counter = (dev->dh_attr & ATTR_FASTCON) << 3;

  for (xfer = 0; xfer < n; xfer++)
  {
    int err;
    unsigned char count = 1, c = *bp++;

    if (c == CTL_Z)
      break;

    /* write a character in cooked mode; maybe with printer echo;
       handles TAB expansion */
    if (c == HT) {
      count = 8 - (internal_data->scr_pos & 7);
      c = ' ';
    }
    update_scr_pos(c, count);
    do {
      /* if not fast then < 0x80; always check
         otherwise check every 32 characters */
      if (fast_counter <= 0x80 && check_handle_break(pdev) == CTL_S)
        raw_get_char(pdev, TRUE); /* Test for hold char and ctl_c */
      fast_counter++;
      fast_counter &= 0x9f;
      if (internal_data->PrinterEcho)
      {
        char* ch = ARM_PTR(x86_szLine);
        *ch = (char)c;
        dos_far_ptr x86_ch = x86_szLine;
        DosWrite(STDPRN, 1, x86_ch);
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
  char* ch = ARM_PTR(x86_DATA);
  *ch = (char)c;
  dos_far_ptr x86_ch = x86_DATA;
  DosRWSft(sft_idx, 1, x86_ch, XFR_FORCE_WRITE);
}

void write_char_stdout(int c)
{
  unsigned char count = 1;
  unsigned flags = ((sft*)ARM_PTR(get_sft(STDOUT)))->sft_flags;

  /* ah=2, ah=9 should expand tabs even for raw devices and disk files */
  if ((flags & (SFT_FDEVICE|SFT_FBINARY)) != SFT_FDEVICE)
  {
    if (c == HT) {
      count = 8 - (internal_data->scr_pos & 7);
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

long cooked_read(dos_far_ptr *pdev, size_t n, char *bp)
{
  unsigned xfer = 0;
  int c;
  while(n--)
  {
    c = raw_get_char(pdev, TRUE);
    if (c < 0)
      return c;
    if (c == 256)
      break;
    *bp++ = c;
    xfer++;
    if ((unsigned char)c == CTL_Z)
      break;
  }
  return xfer;
}

/*
 * ctrl_break_pressed / check_handle_break / handle_break / DosIdle_int
 *
 * These are not yet implemented (same //TODO category as the former
 * cooked_read/write stubs in kernel.c). Stubs below panic loudly so
 * that any path that reaches them is immediately visible.
 *
 * ctrl_break_pressed(): should sample i8042/keyboard buffer for ^C/^Break.
 * check_handle_break(): should call ndread(syscon) and handle ^C/^S.
 * handle_break():       should invoke INT 23h (^C handler) and terminate.
 * DosIdle_int():        should issue INT 28h (DOS idle).
 */
unsigned char ctrl_break_pressed(void)
{
  /// TODO: sample keyboard for ^C/^Break
  return 0; /* never reports break for now */
}

unsigned char check_handle_break(dos_far_ptr *pdev)
{
  /// TODO: check for ^C/^S on the device; handle break if found
  (void)pdev;
  return 0;
}

void handle_break(dos_far_ptr *pdev, int sft_out)
{
  (void)pdev;
  (void)sft_out;
  /* Ctrl-Break delivery is not ported yet. Do not deadlock DOS if this
     path is reached; the caller continues as though no custom INT 23h
     action was installed. */
  return;
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
  dos_far_ptr null_far = MK_FP(0, 0);

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
      if (check_break && ARM_PTR(*pdev) != ARM_PTR(LoL->syscon))
        check_handle_break(&LoL->syscon);
      /* the idle int is only safe if we're using the character stack */
      /* Original: user_r->AH < 0xd
         Port: CPU_AH gives the current INT 21h function number */
      if (CPU_AH < 0xd)
        DosIdle_int();
    }
  }
  else
  {
    char* ch = ARM_PTR(x86_DATA);
    *ch = 0;
    dos_far_ptr x86_ch = x86_DATA;
    DosRWSft(sft_in, 1, x86_ch, XFR_READ);
    c = *ch;
  }

  /* check for break or stop on sft_in, echo to sft_out */
  if (check_break && (c == CTL_C || c == CTL_S))
  {
    if (c == CTL_S)
      c = read_char_sft_dev(sft_in, sft_out, pdev, FALSE);
    if (c == CTL_C)
      handle_break(pdev, sft_out);
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
  sft* s = (sft*) ARM_PTR( idx_to_sft(sft_in) );
  dos_far_ptr dev = sft_to_dev(s);
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
void read_line(int sft_in, int sft_out, keyboard *kp)
{
  unsigned c;
  unsigned cu_pos = internal_data->scr_pos;
  unsigned count = 0, stored_pos = 0;
  unsigned size = kp->kb_size, stored_size = kp->kb_count;
  BOOL insert = FALSE, first = TRUE;

  if (size == 0)
    return;

  /* the stored line is invalid unless it ends with a CR */
  if (kp->kb_buf[stored_size] != CR)
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
        /* show LF if it's not the first character. Never store it */
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
          internal_data->local_buffer[count++] =
            echo_char(kp->kb_buf[stored_pos++], sft_out);
        break;

      case F2:
      case F4:
        /* insert/delete up to character c */
        {
          unsigned char c2 = read_char_check_break(sft_in, sft_out);
          new_pos = stored_pos;
          if (c2 == 0)
          {
            read_char_check_break(sft_in, sft_out);
          }
          else
          {
            char *sp = (char *)memchr(&kp->kb_buf[stored_pos], c2, stored_size - stored_pos);
            if (sp != NULL)
              new_pos = (sp - &kp->kb_buf[stored_pos]) + 1;
          }
        }
        /* fall through */
      case F3:
        if (c != F4) /* not delete */
        {
          while (stored_pos < new_pos && count < size - 1)
            internal_data->local_buffer[count++] =
              echo_char(kp->kb_buf[stored_pos++], sft_out);
        }
        stored_pos = new_pos;
        break;

      case F5:
        memcpy(kp->kb_buf, internal_data->local_buffer, count);
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
          char c2 = internal_data->local_buffer[--count];
          if (c2 == HT)
          {
            unsigned i;
            new_pos2 = cu_pos;
            for (i = 0; i < count; i++)
            {
              if (internal_data->local_buffer[i] == HT)
                new_pos2 = (new_pos2 + 8) & ~7;
              else if (iscntrl(internal_data->local_buffer[i]))
                new_pos2 += 2;
              else
                new_pos2++;
            }
            do
              destr_bs(sft_out);
            while (internal_data->scr_pos > new_pos2);
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
          internal_data->local_buffer[count++] = echo_char(c, sft_out);
        else
          write_char(BELL, sft_out);
        if (stored_pos < stored_size && !insert)
          stored_pos++;
        break;
    }
    first = FALSE;
  } while (c != CR);

  memcpy(kp->kb_buf, internal_data->local_buffer, count);
  /* if local_buffer overflows into the CON default buffer we
     must invalidate it */
  if (count > LINEBUFSIZECON)
    internal_data->kb_buf.kb_size = 0;
  /* kb_count does not include the final CR */
  kp->kb_count = count - 1;
}

/* called by handle func READ (int21/ah=3f) */
size_t read_line_handle(int sft_idx, size_t n, char *bp)
{
  size_t chars_left;
  keyboard *kbp = &internal_data->kb_buf;
  char *inputptr;

  if (LoL->inputptr == 0)
  {
    /* can we reuse kb_buf or was it overwritten? */
    if (kbp->kb_size != LINEBUFSIZECON)
    {
      kbp->kb_count = 0;
      kbp->kb_size = LINEBUFSIZECON;
    }
    read_line(sft_idx, sft_idx, kbp);
    kbp->kb_buf[kbp->kb_count + 1] = echo_char(LF, sft_idx);
    /* inputptr = kb_buf.kb_buf (offset within DOS_PSP segment) */
    LoL->inputptr = (dos_short_ptr)(
      (char *)kbp->kb_buf - (char *)ARM_PTR(MK_FP(DOS_PSP, 0)));
    inputptr = kbp->kb_buf;
    if (*inputptr == CTL_Z)
    {
      LoL->inputptr = 0;
      return 0;
    }
  }
  else
  {
    inputptr = (char *)ARM_PTR(MK_FP(DOS_PSP, LoL->inputptr));
  }

  chars_left = &kbp->kb_buf[kbp->kb_count + 2] - inputptr;
  if (n > chars_left)
    n = chars_left;

  memcpy(bp, inputptr, n);
  inputptr += n;
  if (n == chars_left)
    LoL->inputptr = 0;
  else
    LoL->inputptr = (dos_short_ptr)(
      inputptr - (char *)ARM_PTR(MK_FP(DOS_PSP, 0)));
  return n;
}
