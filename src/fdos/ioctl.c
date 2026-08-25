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
#include "ioctl_guest_proxy.h"
#include "request_guest.h"

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
  const dos_far_ptr rq_far = fdos_sda_request_far(offsetof(struct dos_data, ClkReqHdr));
  const fdos_request_guest_ref rq = fdos_request_guest(rq_far);
  dos_far_ptr x86_dev;
  unsigned attr = 0;

  if (CPU_AL > 0x11)
    return DE_INVLDFUNC;

  switch (CPU_AL)
  {
    case 0x0b:
      /* skip, it's a special case.                           */
      fdos_ioctl_set_network_retry(CPU_CX, CPU_DX, CPU_DX != 0);
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

      unsigned flags = fdos_ioctl_sft_flags(_s);

      switch (CPU_AL)
      {
        case 0x00:
          /* Get the flags from the SFT                           */
          CPU_AX = flags & 0xff;
          if (flags & SFT_FDEVICE) {
            x86_dev = fdos_ioctl_sft_dev(_s);
            CPU_AX |= (fdos_ioctl_dhdr_attr(x86_dev) & 0xff00);
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
          CPU_AL = (UBYTE)flags;
          /* Set it to what we got in the DL register from the    */
          /* user.                                                */
          fdos_ioctl_sft_set_flags_lo(_s, (UBYTE)(SFT_FDEVICE | CPU_DL));
          return SUCCESS;

        case 0x0a:
          CPU_DX = flags;
          CPU_AX = 0;
          return SUCCESS;
      }
      if (!(flags & SFT_FDEVICE))
      {
        if (CPU_AL == 0x06)
          CPU_AL = fdos_ioctl_sft_position(_s) >= fdos_ioctl_sft_size(_s) ? 0 : 0xFF;
        else if (CPU_AL == 0x07)
          CPU_AL = 0;
        else
          return DE_INVLDFUNC;
        return SUCCESS;
      }
      x86_dev = fdos_ioctl_sft_dev(_s);
      attr = fdos_ioctl_dhdr_attr(x86_dev);
      FDOS_REQUEST_SET8(rq, r_unit, 0);
      break;
    }

    default: /* block IOCTL: 4, 5, 8, 9, d, e, f, 11 */
    {
/*
   This line previously returned the deviceheader at CPU_bl. But,
   DOS numbers its drives starting at 1, not 0. A=1, B=2, and so
   on. Changed this line so it is now zero-based.

   -SRM
 */
/* JPP - changed to use default drive if drive=0 */
/* JT Fixed it */

      /* NDN feeds the actual ASCII drive letter to this function */
      dos_far_ptr _dpbp = get_dpb((CPU_BL & 0x1f) == 0 ? fdos_ioctl_default_drive() : (CPU_BL & 0x1f) - 1);
      if (! far_is_null(_dpbp))
      {
        FDOS_REQUEST_SET8(rq, r_unit, fdos_ioctl_dpb_subunit(_dpbp));
        x86_dev = fdos_ioctl_dpb_device(_dpbp);
        attr = fdos_ioctl_dhdr_attr(x86_dev);
      }
      else
      {
        if (CPU_AL != 8 && CPU_AL != 9)
          return DE_INVLDDRV;
        x86_dev = MK_FP(0, 0);
        attr = ATTR_REMOTE;
      }

      switch (CPU_AL)
      {
        case 0x08:
        {
          UWORD cds_flags;
          if (!fdos_ioctl_cds_flags(CPU_BL & 0x1f, &cds_flags))
            return DE_INVLDDRV;
          if (cds_flags & CDSNETWDRV)
            return DE_INVLDFUNC;
          CPU_AX = (fdos_ioctl_dpb_flags(_dpbp) == M_DONT_KNOW);
          return SUCCESS;
        }
        case 0x09:
        {
          /* note from get_dpb()                            */
          /* that if cdsp == NULL then dev must be NULL too */
          UWORD cds_flags;
          if (!fdos_ioctl_cds_flags(CPU_BL & 0x1f, &cds_flags))
            return DE_INVLDDRV;
          if (cds_flags & CDSSUBST)
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

    if (!(attr & testattr))
      return DE_INVLDFUNC;
  }

  FDOS_REQUEST_SET8(rq, r_command, cmd[CPU_AL]);
  if (CPU_AL == 0x0C || CPU_AL == 0x0D || CPU_AL >= 0x10) /* generic or query */
  {
    FDOS_REQUEST_SET8(rq, r_cat, CPU_CH);            /* category (major) code */
    FDOS_REQUEST_SET8(rq, r_fun, CPU_CL);            /* function (minor) code */
    fdos_request_set16(rq, offsetof(request, r_si), CPU_SI);             /* contents of SI and DI */
    fdos_request_set16(rq, offsetof(request, r_di), CPU_DI);
    FDOS_REQUEST_SET_FAR(rq, r_io, MK_FP(CPU_DS, CPU_DX));    /* parameter block */
  }
  else
  {
    FDOS_REQUEST_SET16(rq, r_count, CPU_CX);
    FDOS_REQUEST_SET_FAR(rq, r_trans, MK_FP(CPU_DS, CPU_DX));
  }
  FDOS_REQUEST_SET8(rq, r_length, sizeof(request));
  FDOS_REQUEST_SET16(rq, r_status, 0);

  execrh(rq_far, x86_dev);

  if (FDOS_REQUEST_GET16(rq, r_status) & S_ERROR)
  {
    fdos_ioctl_set_crit_err_code((UWORD)((FDOS_REQUEST_GET16(rq, r_status) & S_MASK) + 0x13));
    return DE_ACCESS;
  }

  if (CPU_AL <= 0x05)                       /* 0x02, 0x03, 0x04, 0x05 */
    CPU_AX = FDOS_REQUEST_GET16(rq, r_count);
  else if (CPU_AL <= 0x07)                  /* 0x06, 0x07 */
    CPU_AX = (FDOS_REQUEST_GET16(rq, r_status) & S_BUSY) ? 0000 : 0x00ff;
  else if (CPU_AL == 0x08)                  /* 0x08 */
    CPU_AX = (FDOS_REQUEST_GET16(rq, r_status) & S_BUSY) ? 1 : 0;
  else if (CPU_AL == 0x0e || CPU_AL == 0x0f) /* 0x0e, 0x0f */
    CPU_AL = FDOS_REQUEST_GET8(rq, r_unit);
  else                                     /* 0x0c, 0x0d, 0x10, 0x11 */
    CPU_AX = FDOS_REQUEST_GET16(rq, r_status);
  return SUCCESS;
}
