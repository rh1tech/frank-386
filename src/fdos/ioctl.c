/****************************************************************/
/*                                                              */
/*                          ioctl.c                             */
/*                                                              */
/*                    DOS-C ioctl system call                   */
/*                                                              */
/*                    Copyright (c) 1995,1998                   */
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
/****************************************************************/

#include "hdrs.h"

/*
 * WARNING:  this code is non-portable (8086 specific).
 */

/*  TE 10/29/01

	although device drivers have only 20 pushes available for them,
	MS NET plays by its own rules

	at least TE's network card driver DM9PCI (some 10$ NE2000 clone) does:
	with SP=8DC before calling down to execrh, and SP=8CC when 
	callf [interrupt], 	DM9PCI touches DOSDS:792, 
	14 bytes into error stack :-(((
	
	so some optimizations were made.		
	this uses the fact, that only CharReq device buffer is ever used.
	fortunately, this saves some code as well :-)

*/

/* this is a file scope static because with Turbo C 2.01 "static const" does
 * not work correctly inside the function */
STATIC const UBYTE cmd [] = {
  0, 0,
  /* 0x02 */ C_IOCTLIN,
  /* 0x03 */ C_IOCTLOUT,
  /* 0x04 */ C_IOCTLIN,
  /* 0x05 */ C_IOCTLOUT,
  /* 0x06 */ C_ISTAT,
  /* 0x07 */ C_OSTAT,
  /* 0x08 */ C_REMMEDIA,
  0, 0, 0,
  /* 0x0c */ C_GENIOCTL,
  /* 0x0d */ C_GENIOCTL,
  /* 0x0e */ C_GETLDEV,
  /* 0x0f */ C_SETLDEV,
  /* 0x10 */ C_IOCTLQRY,
  /* 0x11 */ C_IOCTLQRY,
};

int DosDevIOctl(lregs * r)
{
  struct dhdr* dev;
  dos_far_ptr x86_dev;

  if (CPU_AL > 0x11)
    return DE_INVLDFUNC;

  switch (CPU_AL)
  {
    case 0x0b:
      /* skip, it's a special case.                           */
      LoL->NetDelay = CPU_CX;
      if (CPU_DX)
        LoL->NetRetry = CPU_DX;
      return SUCCESS;

    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x06:
    case 0x07:
    case 0x0a:
    case 0x0c:
    case 0x10:
    {
      dos_far_ptr _s = get_sft(CPU_BX);
      /* Test that the handle is valid and                    */
      /* get the SFT block that contains the SFT              */
      if ( far_is_end (_s) )
        return DE_INVLDHNDL;

      sft* s = (sft*) ARM_PTR (_s);
      unsigned flags = s->sft_flags;

      switch (CPU_AL)
      {
        case 0x00:
          /* Get the flags from the SFT                           */
          CPU_AX = flags & 0xff;
          if (flags & SFT_FDEVICE) {
            x86_dev = s->sft_dev;
            dev = (struct dhdr*)ARM_PTR(x86_dev);
            CPU_AX |= (dev->dh_attr & 0xff00);
          }
          /* else: files/networks return 0 in AH/DH */
          /* Undocumented result, Ax = Dx seen using Pcwatch */
          CPU_DX = CPU_AX;
          return SUCCESS;

        case 0x01:
          /* sft_flags is a file, return an error because you     */
          /* can't set the status of a file.                      */
          if (!(flags & SFT_FDEVICE))
            return DE_INVLDFUNC;
          /* RBIL says this is only for DOS < 6, but MSDOS 7.10   */
          /* returns this as well... and some buggy program relies*/
          /* on it :(                                             */
          if (CPU_DH != 0)
            return DE_INVLDDATA;

          /* Undocumented: AL should get the old value            */
          CPU_AL = s->sft_flags_lo;
          /* Set it to what we got in the DL register from the    */
          /* user.                                                */
          s->sft_flags_lo = SFT_FDEVICE | CPU_DL;
          return SUCCESS;

        case 0x0a:
          CPU_DX = flags;
          CPU_AX = 0;
          return SUCCESS;
      }
      if (!(flags & SFT_FDEVICE))
      {
        if (CPU_AL == 0x06)
          CPU_AL = s->sft_posit >= s->sft_size ? 0 : 0xFF;
        else if (CPU_AL == 0x07)
          CPU_AL = 0;
        else
          return DE_INVLDFUNC;
        return SUCCESS;
      }
      x86_dev = s->sft_dev;
      dev = (struct dhdr*)ARM_PTR(x86_dev);
      CharReqHdr.r_unit = 0;
      break;
    }

    default: /* block IOCTL: 4, 5, 8, 9, d, e, f, 11 */
    {
      unsigned attr;
/*
   This line previously returned the deviceheader at CPU_bl. But,
   DOS numbers its drives starting at 1, not 0. A=1, B=2, and so
   on. Changed this line so it is now zero-based.

   -SRM
 */
/* JPP - changed to use default drive if drive=0 */
/* JT Fixed it */

      /* NDN feeds the actual ASCII drive letter to this function */
      dos_far_ptr _dpbp = get_dpb((CPU_BL & 0x1f) == 0 ? internal_data->default_drive : (CPU_BL & 0x1f) - 1);
      if (! far_is_null(_dpbp))
      {
        struct dpb* dpbp = (struct dpb*)ARM_PTR(_dpbp);
        CharReqHdr.r_unit = dpbp->dpb_subunit;
        x86_dev = dpbp->dpb_device;
        dev = (struct dhdr*)ARM_PTR(x86_dev);
        attr = dev->dh_attr;
      }
      else
      {
        if (CPU_AL != 8 && CPU_AL != 9)
          return DE_INVLDDRV;
        dev = NULL;
        x86_dev = MK_FP(0, 0);
        attr = ATTR_REMOTE;
      }

      switch (CPU_AL)
      {
        case 0x08:
        {
          struct cds FAR *cdsp = get_cds1(CPU_BL & 0x1f);
          if (cdsp == NULL)
            return DE_INVLDDRV;
          if (cdsp->cdsFlags & CDSNETWDRV)
            return DE_INVLDFUNC;
          struct dpb* dpbp = (struct dpb*)ARM_PTR(_dpbp);
          CPU_AX = (dpbp->dpb_flags == M_DONT_KNOW);
          return SUCCESS;
        }
        case 0x09:
        {
          /* note from get_dpb()                            */
          /* that if cdsp == NULL then dev must be NULL too */
          struct cds FAR *cdsp = get_cds1(CPU_BL & 0x1f);
          if (cdsp == NULL)
            return DE_INVLDDRV;
          if (cdsp->cdsFlags & CDSSUBST)
            attr |= ATTR_SUBST;
          CPU_AX = S_DONE | S_BUSY;
          CPU_DX = attr;
          return SUCCESS;
        }
        case 0x0d:
          if ((CPU_CX & ~(0x486B-0x084A)) == 0x084A)
          {             /* 084A/484A, 084B/484B, 086A/486A, 086B/486B */
            CPU_AX = 0;  /* (lock/unlock logical/physical volume) */
            /* simulate success for MS-DOS 7+ SCANDISK etc. --LG */
            return SUCCESS;
          }
          /* fall through */
        default: /* 0x04, 0x05, 0x0e, 0x0f, 0x11 */
          break;
      }
      break;
    }
  }

  {
    unsigned testattr = ATTR_QRYIOCTL;
    if (CPU_AL<=0x0f)
      testattr = ATTR_GENIOCTL;
    if (CPU_AL<=0x08)
      testattr = ATTR_EXCALLS;
    if (CPU_AL<=0x07)
      testattr = 0xffff;
    if (CPU_AL<=0x05)
      testattr = ATTR_IOCTL;

    if (!(dev->dh_attr & testattr))
      return DE_INVLDFUNC;
  }

  CharReqHdr.r_command = cmd[CPU_AL];
  if (CPU_AL == 0x0C || CPU_AL == 0x0D || CPU_AL >= 0x10) /* generic or query */
  {
    CharReqHdr.r_cat = CPU_CH;            /* category (major) code */
    CharReqHdr.r_fun = CPU_CL;            /* function (minor) code */
    CharReqHdr.r_si = CPU_SI;             /* contents of SI and DI */
    CharReqHdr.r_di = CPU_DI;
    CharReqHdr.r_io = MK_FP(CPU_DS, CPU_DX);    /* parameter block */
  }
  else
  {
    CharReqHdr.r_count = CPU_CX;
    CharReqHdr.r_trans = MK_FP(CPU_DS, CPU_DX);
  }
  CharReqHdr.r_length = sizeof(request);
  CharReqHdr.r_status = 0;

  execrh(x86_FAR_PTR(DOS_PSP, &CharReqHdr) /* -> request */, x86_dev);

  if (CharReqHdr.r_status & S_ERROR)
  {
    internal_data->CritErrCode = (CharReqHdr.r_status & S_MASK) + 0x13;
    return DE_ACCESS;
  }

  if (CPU_AL <= 0x05)                       /* 0x02, 0x03, 0x04, 0x05 */
    CPU_AX = CharReqHdr.r_count;
  else if (CPU_AL <= 0x07)                  /* 0x06, 0x07 */
    CPU_AX = (CharReqHdr.r_status & S_BUSY) ? 0000 : 0x00ff;
  else if (CPU_AL == 0x08)                  /* 0x08 */
    CPU_AX = (CharReqHdr.r_status & S_BUSY) ? 1 : 0;
  else if (CPU_AL == 0x0e || CPU_AL == 0x0f) /* 0x0e, 0x0f */
    CPU_AL = CharReqHdr.r_unit;
  else                                     /* 0x0c, 0x0d, 0x10, 0x11 */
    CPU_AX = CharReqHdr.r_status;
  return SUCCESS;
}
