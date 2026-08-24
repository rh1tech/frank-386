#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"
#include "kernel_guest_proxy.h"

struct int2526_packet {
  ULONG blkno;
  UWORD nblks;
  dos_far_ptr buf;
} __attribute__((packed));

/*
 * Возврат INT 25h/26h по их особому ABI: обработчик завершается RETF,
 * ОСТАВЛЯЯ исходный FLAGS вызывающего на стеке (тот обязан снять его
 * своим POPF), а CF результата несут ЖИВЫЕ флаги. Диспетчер трапов
 * завершает нативные обработчики универсальной IRET-заглушкой (снимает
 * IP/CS/FLAGS), поэтому кадр перед возвратом сдвигается на слово вниз:
 *
 *   вход:   [SP]=IP [SP+2]=CS [SP+4]=FLAGS(исходные)
 *   выход:  [SP-2]=IP [SP]=CS [SP+2]=FLAGS|CF [SP+4]=FLAGS(исходные)
 *           CPU_SP -= 2
 *
 * IRET заглушки снимает IP/CS/результат-флаги, и гость продолжает с
 * SP = entry-2 и исходным FLAGS на вершине - в точности состояние после
 * RETF реального DOS. Без сдвига POPF вызывающего съедал собственное
 * слово стека: случайный TF (INT 1 на честной TF-механике порта),
 * смещённый RET - исполнение мусора. Дисковые утилиты (NDD и родня)
 * зовут INT 25h в прологе - и висли ровно так.
 */
static bool fdos_2526h_return(CPU *cpu, bool set_cf)
{
  uint32_t frame = ((uint32_t)CPU_SS << 4) + CPU_SP;
  UWORD ip = readw86(frame);
  UWORD cs = readw86(frame + 2);
  UWORD flags = readw86(frame + 4);

  if (set_cf)
    flags |= 0x0001;
  else
    flags &= (UWORD)~0x0001;

  CPU_SP = (UWORD)(CPU_SP - 2);
  frame -= 2;
  writew86(frame, ip);
  writew86(frame + 2, cs);
  writew86(frame + 4, flags);
  /* frame + 6: исходный FLAGS вызывающего, остаётся на стеке */

  return true;
}

static bool fdos_2526h(CPU *cpu, COUNT mode)
{
  UBYTE drive = CPU_AL & 0x7f;
  ULONG block = CPU_DX;
  UWORD count = CPU_CX;
  dos_far_ptr buffer = MK_FP(CPU_DS, CPU_BX);
  dos_far_ptr dpb_fp = get_dpb(drive);

  /* Match upstream: mask AH/high AL bit and reject invalid drives with
     DOS absolute-disk error 0201h. */
  if (drive >= fdos_dos_lastdrive() || far_is_null(dpb_fp))
  {
    CPU_AX = 0x0201;
    return fdos_2526h_return(cpu, true);
  }

#ifdef WITHFAT32
  {
    /* Legacy INT 25h/26h may access the FAT32 boot sector only. */
    if (block != 0 && fdos_dpb_is_fat32(dpb_fp) && fdos_dpb_xfatsize(dpb_fp) != 0)
    {
      CPU_AX = 0x0207;
      return fdos_2526h_return(cpu, true);
    }
  }
#endif

  if (count == 0xffff)
  {
    const uint32_t packet = ((uint32_t)FP_SEG(buffer) << 4) + FP_OFF(buffer);
    block = pload32(packet + offsetof(struct int2526_packet, blkno));
    count = pload16(packet + offsetof(struct int2526_packet, nblks));
    buffer = MK_FP(pload16(packet + offsetof(struct int2526_packet, buf) + 2u),
                   pload16(packet + offsetof(struct int2526_packet, buf)));
  }

  pstore8(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, InDOS),
          (UBYTE)(pload8(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, InDOS)) + 1u));

  if (mode == DSKWRITEINT26)
    DeleteBlockInBufferCache(block, block, drive, XFR_WRITE);

  CPU_AX = dskxfer(drive, block, buffer, count, mode);

  pstore8(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, InDOS),
          (UBYTE)(pload8(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, InDOS)) - 1u));

  if (CPU_AX != 0)
  {
    if (mode == DSKWRITEINT26)
      setinvld(drive);
    return fdos_2526h_return(cpu, true);
  }
  return fdos_2526h_return(cpu, false);
}

bool fdos_25h(CPU *cpu)
{
  return fdos_2526h(cpu, DSKREADINT25);
}

bool fdos_26h(CPU *cpu)
{
  return fdos_2526h(cpu, DSKWRITEINT26);
}

bool fdos_27h(CPU *cpu)
{
  UWORD paragraphs = (UWORD)((CPU_DX + 0x0f) >> 4);

  /* Upstream converts DX bytes to paragraphs, then enters AH=31h.
     Keep the same six-paragraph minimum used by the native AH=31h path. */
  if (paragraphs < 6)
    paragraphs = 6;

  DosMemChange(fdos_dos_cu_psp(), paragraphs, NULL);
  request_terminate(0, 3);
  return true;
}

bool fdos_28h(CPU *cpu)
{
  (void)cpu;
  return true;
}
