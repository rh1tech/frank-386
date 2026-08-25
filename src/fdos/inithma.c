/****************************************************************/
/*                                                              */
/*                          initHMA.c                           */
/*                            DOS-C                             */
/*                                                              */
/*          move kernel to HMA area                             */
/*                                                              */
/*                      Copyright (c) 2001                      */
/*                      tom ehlert                              */
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
/****************************************************************/

/*
    current status:
    
    load FreeDOS high, if DOS=HIGH detected
    
    suppress High Loading, if any SHIFT status detected (for debugging)
    
    if no XMS driver (HIMEM,FDXMS,...) loaded, should work
    
    cooperation with XMS drivers as follows:
    
    copy HMA_TEXT segment up.

    after each loaded DEVICE=SOMETHING.SYS, try to request the HMA
    (XMS function 0x01). 
    if no XMS driver detected, during ONFIG.SYS processing,
    create a dummy VDISK entry in high memory
    
    this works with
    
     FD FDXMS - no problems detected
    
    
     MS HIMEM.SYS (from DOS 6.2, 9-30-93)
     
        works if and only if
        
            /TESTMEM:OFF 
            
        is given
            
        otherwise HIMEM will TEST AND ZERO THE HIGH MEMORY+HMA.
        so, in CONFIG.C, if "HIMEM.SYS" is detected, a "/TESTMEM:OFF"
        parameter is forced.
*/
#include "hdrs.h"
#include "kernel_guest_proxy.h"
#include "bios/bios.h"
#include "fdos/fdos.h"

BYTE DosLoadedInHMA BSS_INIT(FALSE);  /* set to TRUE if loaded HIGH          */
BYTE HMAclaimed BSS_INIT(0);          /* set to TRUE if claimed from HIMEM   */
UWORD HMAFree BSS_INIT(0);            /* first byte in HMA not yet used      */
dos_far_ptr XMSDriverAddress = MK_FP(0, 0);

STATIC void InstallVDISK(void);

#define KeyboardShiftState() (*(BYTE FAR *)(MK_FP(0x40,0x17)))

/*
    this tests, if the HMA area can be enabled.
    if so, it simply leaves it on
*/

STATIC int EnabledA20(void)
{
  return cpu_get_a20(cpu);
}

int EnableHMA(VOID)
{

  cpu_set_a20(cpu, 1);

  if (!EnabledA20())
  {
    printf("HMA can't be enabled\n");
    return FALSE;
  }

  cpu_set_a20(cpu, 0);

#ifdef DEBUG
  if (EnabledA20())
  {
    printf("HMA can't be disabled - no problem for us\n");
  }
#endif

  cpu_set_a20(cpu, 1);
  if (!EnabledA20())
  {
    printf("HMA can't be enabled second time\n");
    return FALSE;
  }

  HMAInitPrintf(("HMA success - leaving enabled\n"));

  return TRUE;

}

/*
    move the kernel up to high memory
    this is very unportable
    
    if we thin we succeeded, we return TRUE, else FALSE
*/

#define HMAOFFSET  0x20
#define HMASEGMENT 0xffff

dos_far_ptr DetectXMSDriver(void)
{
    u16 save_ax = CPU_AX;
    CPU_AX = 0x4300;
    // bios_intcall(cpu, 0x2F, "XMS 2F");
    fdos_2fh(cpu);
    if (CPU_AL != 0x80) {
        CPU_AX = save_ax;
        return MK_FP(0, 0);
    }
    UWORD save_es = CPU_ES;
    UWORD save_bx = CPU_BX;
    CPU_AX = 0x4310;
//    bios_intcall(cpu, 0x2F, "XMS 2F");
    fdos_2fh(cpu);
    dos_far_ptr entry = MK_FP(CPU_ES, CPU_BX);
    CPU_AX = save_ax;
    CPU_BX = save_bx;
    SET_ES ( save_es );
    return entry;
}

int init_call_XMScall(dos_far_ptr driverAddress, UWORD ax, UWORD dx)
{
    CPU_AX = ax;
    CPU_DX = dx;
    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));
    return CPU_AX;
}

int MoveKernelToHMA(void)
{
  if (DosLoadedInHMA)
  {
    return TRUE;
  }

  dos_far_ptr xms_addr = DetectXMSDriver();
  if (EFFECTIVE(xms_addr) == 0)
    return FALSE;

  XMSDriverAddress = xms_addr;
///  XMS_Enable_Patch = 0x90;	/* must be set after XMSDriverAddress */

#ifdef DEBUG
  /* A) for debugging purpose, suppress this, 
     if any shift key is pressed 
   */
  if (KeyboardShiftState() & 0x0f)
  {
    printf("Keyboard state is %0x, NOT moving to HMA\n",
           KeyboardShiftState());
    return FALSE;
  }
#endif

  /* B) check out, if we can have HMA */

  if (!EnableHMA())
  {
    printf("Can't enable HMA area (the famous A20), NOT moving to HMA\n");

    return FALSE;
  }

  /*  allocate HMA through XMS driver */

  if (HMAclaimed == 0 &&
      (HMAclaimed =
       init_call_XMScall(xms_addr, 0x0100, 0xffff)) == 0)
  {
    printf("Can't reserve HMA area ??\n");

    return FALSE;
  }

///  MoveKernel(0xffff);

  {
    /* E) up to now, nothing really bad was done. 
       but now, we reuse the HMA area. bad things will happen

       to find bugs early,   
       cause INT 3 on all accesses to this area 
     */

    DosLoadedInHMA = TRUE;
  }

  /*
    on finalize, will install a VDISK
  */

  InstallVDISK();

  /* report the fact we are running high through int 21, ax=3306 */
  fdos_lol_or_version_flags(0x10);

  return TRUE;

}

/*   
    now protect against HIMEM/FDXMS/... by simulating a VDISK 
    FDXMS should detect us and not give HMA access to ohers
    unfortunately this also disables HIMEM completely

    so: we install this after all drivers have been loaded
*/
STATIC void InstallVDISK(void)
{
  static struct {               /* Boot-Sektor of a RAM-Disk */
    UBYTE dummy1[3];            /* HIMEM.SYS uses 3, but FDXMS uses 2 */
    char Name[5];
    BYTE dummy2[3];
    WORD BpS;
    BYTE dummy3[6];
    WORD Sektoren;
    BYTE dummy4;
  } VDISK_BOOT_SEKTOR = {
    {
    0xcf, ' ', ' '},
    {
    'V', 'D', 'I', 'S', 'K'},
    {
    ' ', ' ', ' '}, 512,
    {
    'F', 'D', 'O', 'S', ' ', ' '}, 128, /* 128*512 = 64K */
  ' '};

  if (!DosLoadedInHMA)
    return;

  guest_write_block(EFFECTIVE(MK_FP(0xffff, 0x0010)),
                    &VDISK_BOOT_SEKTOR, sizeof(VDISK_BOOT_SEKTOR));

  pstore16(EFFECTIVE(MK_FP(0xffff, 0x002e)), 1024 + 64);
}

/*
    this allocates some bytes from the HMA area
    only available if DOS=HIGH was successful
*/
dos_far_ptr HMAalloc(COUNT bytesToAllocate)
{
  if (!DosLoadedInHMA)
    return MK_FP(0, 0);

  if (HMAFree > 0xfff0 - bytesToAllocate)
    return MK_FP(0, 0);

  dos_far_ptr HMAptr = MK_FP(0xffff, HMAFree);

  /* align on 16 byte boundary */
  HMAFree = (HMAFree + bytesToAllocate + 0xf) & 0xfff0;

  /*printf("HMA allocated %d byte at %x\n", bytesToAllocate, HMAptr); */

  fmemset(HMAptr, 0, bytesToAllocate);

  return HMAptr;
}
