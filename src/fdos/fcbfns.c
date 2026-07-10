/****************************************************************/
/*                                                              */
/*                          fcbfns.c                            */
/*                                                              */
/*         Port of the original kernel/fcbfns.c (FCB layer).    */
/*                                                              */
/*  Porting status:                                             */
/*    Block B: FatGetDrvData() only (backend for INT 21h        */
/*             AH=1Bh/1Ch - allocation table information).      */
/*    Block G (planned): the full FCB function set              */
/*             (AH=0Fh..17h, 21h..24h, 27h/28h) lands here.     */
/*                                                              */
/*  Port conventions (see dosfns.c/DosGetExtFree for the        */
/*  reference idiom): guest pointers travel as dos_far_ptr and  */
/*  are dereferenced through ARM_PTR(); functions that hand a   */
/*  pointer back to the guest must return a dos_far_ptr into    */
/*  guest memory, never an ARM address.                         */
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include "fdos.h"
#include "globals.h"
#include "proto.h"

/*
    FatGetDrvData() - INT 21h AH=1Bh/1Ch backend.
    Ported from the original kernel/fcbfns.c with one deliberate
    deviation: the original keeps a local `static BYTE mdb` to hand out
    a media-byte copy for NETWORK drives (where no local DPB exists).
    An ARM-side static is invisible to the guest, and the network
    redirector is permanently stubbed on this platform, so that branch
    is unreachable here: DosGetFree() reports network drives as invalid
    (0xffff) and we return a null far pointer (caller answers AL=0FFh).
    For local drives the returned pointer aims at dpb_mdb inside the
    guest-resident DPB, exactly like the original.
*/
dos_far_ptr FatGetDrvData(UBYTE drive, UBYTE * pspc, UWORD * bps, UWORD * nc)
{
  UWORD spc;

  /* get the data available from dpb                       */
  spc = DosGetFree(drive, NULL, bps, nc);
  if (spc != 0xffff)
  {
    dos_far_ptr dpbp_x86 =
        get_dpb(drive == 0 ? internal_data->default_drive : drive - 1);
    /* Point to the media descriptor for this drive                */
    *pspc = (UBYTE) spc;
    if (far_is_null(dpbp_x86))
    {
      /* original: network drive - media byte packed in spc>>8 and
         served from a private static. Unreachable with the stubbed
         redirector (DosGetFree already returned 0xffff above), and a
         native static cannot be exposed to the guest anyway. */
      return MK_FP(0, 0);
    }
    return MK_FP(FP_SEG(dpbp_x86),
                 (UWORD)(FP_OFF(dpbp_x86) + offsetof(struct dpb, dpb_mdb)));
  }
  return MK_FP(0, 0);
}
