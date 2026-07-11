#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"

struct int2526_packet {
  ULONG blkno;
  UWORD nblks;
  dos_far_ptr buf;
} __attribute__((packed));

static void fdos_set_cf(CPU *cpu, bool set)
{
  uint32_t frame = ((uint32_t)CPU_SS << 4) + CPU_SP;
  UWORD flags = readw86(frame + 4);

  if (set)
    flags |= 0x0001;
  else
    flags &= (UWORD)~0x0001;

  writew86(frame + 4, flags);
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
  if (drive >= LoL->lastdrive || far_is_null(dpb_fp))
  {
    CPU_AX = 0x0201;
    fdos_set_cf(cpu, true);
    return true;
  }

#ifdef WITHFAT32
  {
    struct dpb *dpb = (struct dpb *)ARM_PTR(dpb_fp);

    /* Legacy INT 25h/26h may access the FAT32 boot sector only. */
    if (block != 0 && ISFAT32(dpb) && dpb->dpb_xfatsize != 0)
    {
      CPU_AX = 0x0207;
      fdos_set_cf(cpu, true);
      return true;
    }
  }
#endif

  if (count == 0xffff)
  {
    struct int2526_packet *packet =
        (struct int2526_packet *)ARM_PTR(buffer);

    block = packet->blkno;
    count = packet->nblks;
    buffer = packet->buf;
  }

  internal_data->InDOS++;

  if (mode == DSKWRITEINT26)
    DeleteBlockInBufferCache(block, block, drive, XFR_WRITE);

  CPU_AX = dskxfer(drive, block, buffer, count, mode);

  if (CPU_AX != 0)
  {
    if (mode == DSKWRITEINT26)
      setinvld(drive);
    fdos_set_cf(cpu, true);
  }
  else
    fdos_set_cf(cpu, false);

  internal_data->InDOS--;
  return true;
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

  DosMemChange(internal_data->cu_psp, paragraphs, NULL);
  request_terminate(0, 3);
  return true;
}

bool fdos_28h(CPU *cpu)
{
  (void)cpu;
  return true;
}
