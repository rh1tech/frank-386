/****************************************************************/
/*                                                              */
/*                          fcbfns.c                            */
/*                                                              */
/*         Port of the original kernel/fcbfns.c (FCB layer).    */
/*                                                              */
/*  Porting status:                                             */
/*    Block B: FatGetDrvData() (backend for INT 21h AH=1Bh/1Ch).*/
/*    Block G: the full FCB function set                        */
/*             (AH=0Fh..17h, 21h..24h, 27h/28h).                */
/*    NOT ported: FcbParseFname()/ParseSkipWh() - INT 21h AH=29h*/
/*             uses the port's own DosParseFilenameIntoFcb()    */
/*             (fdos_21h.c).                                    */
/*                                                              */
/*  Port conventions:                                           */
/*    - Guest structures (the caller's FCB, the caller's DTA)   */
/*      travel as dos_far_ptr and are dereferenced through      */
/*      ARM_PTR() at use sites; sda_lpFcb keeps the GUEST       */
/*      pointer, as real DOS does.                              */
/*    - The original redirects the DTA onto kernel-data dmatch  */
/*      instances (DGROUP static + stack locals). DGROUP is     */
/*      guest-visible on real DOS; here the equivalents are the */
/*      guest-resident scratch members appended to struct       */
/*      dos_data (fcb_dmatch / fcb_dmatch_tmp / fcb_ren_name),  */
/*      addressed via x86_FAR_PTR(DOS_PSP, ...).                */
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include "fdos.h"
#include "globals.h"
#include "proto.h"

#define FCB_SUCCESS     0
#define FCB_ERR_NODATA  1
#define FCB_ERR_SEGMENT_WRAP 2
#define FCB_ERR_EOF     3
#define FCB_ERROR       0xff

STATIC dos_far_ptr ExtFcbToFcb(dos_far_ptr lpExtFcb);
STATIC dos_far_ptr CommonFcbInit(dos_far_ptr lpExtFcb, BYTE * pszBuffer,
                                 COUNT * pCurDrive);
STATIC void FcbNameInit(fcb * lpFcb, BYTE * pszBuffer, COUNT * pCurDrive);
STATIC void FcbNextRecord(fcb * lpFcb);
STATIC void FcbCalcRec(dos_far_ptr lpXfcb);

#define TestCmnSeps(lpFileName) (*lpFileName && strchr(":;,=+ \t", *lpFileName) != NULL)
#define TestFieldSeps(lpFileName) ((unsigned char)*lpFileName <= ' ' || strchr("/\\\"[]<>|.:;,=+\t", *lpFileName) != NULL)

/* original: static dmatch Dmatch (DGROUP); port: guest-resident SDA
   appendix, so that internal_data->dta can point at it */
#define Dmatch (internal_data->fcb_dmatch)
#define Dmatch_x86 (x86_FAR_PTR(DOS_PSP, (void *)&internal_data->fcb_dmatch))
#define DmatchTmp (internal_data->fcb_dmatch_tmp)
#define DmatchTmp_x86 (x86_FAR_PTR(DOS_PSP, (void *)&internal_data->fcb_dmatch_tmp))

/* original: extern UWORD wAttr (DGROUP global, used only by this
   module - kernel-private, never peeked by guest software) */
static UWORD wAttr;

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

const BYTE * GetNameField(const BYTE * lpFileName, BYTE * lpDestField,
                          COUNT nFieldSize, BOOL * pbWildCard)
{
  COUNT nIndex = 0;
  BYTE cFill = ' ';

  while (*lpFileName != '\0' && !TestFieldSeps(lpFileName)
         && nIndex < nFieldSize)
  {
    /* convert * into multiple ? for remaining length of field    */
    if (*lpFileName == '*')
    {
      *pbWildCard = TRUE;
      cFill = '?';
      ++lpFileName;
      break;
    }
    /* include ? as-is but flag for return purposes wildcard used */
    if (*lpFileName == '?')
      *pbWildCard = TRUE;

    /* store uppercased character, and advance to next char       */
    *lpDestField++ = DosUpFChar(*lpFileName++);
    ++nIndex;
  }

  /* Blank out remainder of field on exit                         */
  memset(lpDestField, cFill, nFieldSize - nIndex);
  return lpFileName;
}

/* Fill LocalFcb's name fields from an ASCIIZ 'NAME.EXT' produced by
   the find machinery (dm_name: no drive, no path, no whitespace).
   The original calls the full FcbParseFname() here; the port's AH=29h
   parser lives in fdos_21h.c and takes guest pointers, so this small
   native-side equivalent covers the only internal use. */
STATIC void FcbNameFromSZ(fcb * lpFcb, const BYTE * pszName)
{
  BOOL bDummy = FALSE;

  memset(lpFcb->fcb_fname, ' ', FNAME_SIZE);
  memset(lpFcb->fcb_fext, ' ', FEXT_SIZE);
  pszName = GetNameField(pszName, (BYTE *) lpFcb->fcb_fname,
                         FNAME_SIZE, &bDummy);
  if (*pszName == '.')
    GetNameField(++pszName, (BYTE *) lpFcb->fcb_fext, FEXT_SIZE, &bDummy);
}

STATIC void FcbNextRecord(fcb * lpFcb)
{
  if (++lpFcb->fcb_curec >= 128)
  {
    lpFcb->fcb_curec = 0;
    ++lpFcb->fcb_cublock;
  }
}

STATIC ULONG FcbRec(fcb * lpFcb)
{
  return ((ULONG) lpFcb->fcb_cublock * 128) + lpFcb->fcb_curec;
}

UBYTE FcbReadWrite(dos_far_ptr lpXfcb, UCOUNT recno, int mode)
{
  ULONG lPosit;
  long nTransfer;
  fcb *lpFcb;
  unsigned size;
  unsigned long bigsize;
  unsigned recsiz;

  /* Convert to fcb if necessary                                  */
  lpFcb = (fcb *) ARM_PTR (ExtFcbToFcb(lpXfcb));

  recsiz = lpFcb->fcb_recsiz;
  bigsize = (ULONG) recsiz * recno;
  if (bigsize > 0xffff)
    return FCB_ERR_SEGMENT_WRAP;
  size = (unsigned) bigsize;

  if ((UWORD)(FP_OFF(internal_data->dta) + size) < FP_OFF(internal_data->dta))
    return FCB_ERR_SEGMENT_WRAP;

  /* Now update the fcb and compute where we need to position     */
  /* to.                                                          */
  lPosit = FcbRec(lpFcb) * recsiz;
  if ((internal_data->CritErrCode =
       -SftSeek(lpFcb->fcb_sftno, lPosit, 0)) != SUCCESS)
    return FCB_ERR_NODATA;

  /* Do the read                                                  */
  nTransfer = DosRWSft(lpFcb->fcb_sftno, size, internal_data->dta,
                       mode & ~XFR_FCB_RANDOM);
  if (nTransfer < 0)
    internal_data->CritErrCode = -(int)nTransfer;

  /* Now find out how we will return and do it.                   */
  if (mode & XFR_WRITE)
    lpFcb->fcb_fsize = SftGetFsize(lpFcb->fcb_sftno);

  /* if end-of-file, then partial read should count last record */
  if (mode & XFR_FCB_RANDOM && recsiz > 0)
    lpFcb->fcb_rndm += ((unsigned)nTransfer + recsiz - 1) / recsiz;
  size -= (unsigned)nTransfer;
  if (size == 0)
  {
    FcbNextRecord(lpFcb);
    return FCB_SUCCESS;
  }
  size %= lpFcb->fcb_recsiz;
  if (mode & XFR_READ && size > 0)
  {
    fmemset(MK_FP(FP_SEG(internal_data->dta),
                  FP_OFF(internal_data->dta) + (unsigned)nTransfer),
            0, size);
    FcbNextRecord(lpFcb);
    return FCB_ERR_EOF;
  }
  return FCB_ERR_NODATA;
}

UBYTE FcbGetFileSize(dos_far_ptr lpXfcb)
{
  int FcbDrive, sft_idx;
  unsigned recsiz;

  /* Build a traditional DOS file name                            */
  fcb *lpFcb = (fcb *) ARM_PTR (CommonFcbInit(lpXfcb, SecPathName, &FcbDrive));
  recsiz = lpFcb->fcb_recsiz;

  /* check for a device                                           */
  if (!lpFcb || !far_is_null(IsDevice(SecPathName)) || (recsiz == 0))
    return FCB_ERROR;

  sft_idx = (short)DosOpenSft(x86_FAR_PTR(DOS_PSP, (void *)SecPathName),
                              O_LEGACY | O_RDONLY | O_OPEN, 0);
  if (sft_idx >= 0)
  {
    ULONG fsize;

    /* Get the size                                         */
    fsize = SftGetFsize(sft_idx);

    /* compute the size and update the fcb                  */
    lpFcb->fcb_rndm = (fsize + (recsiz - 1)) / recsiz;

    /* close the file and leave                             */
    if ((internal_data->CritErrCode = -DosCloseSft(sft_idx, FALSE)) == SUCCESS)
      return FCB_SUCCESS;
  }
  else
    internal_data->CritErrCode = -sft_idx;
  return FCB_ERROR;
}

void FcbSetRandom(dos_far_ptr lpXfcb)
{
  /* Convert to fcb if necessary                                  */
  fcb *lpFcb = (fcb *) ARM_PTR (ExtFcbToFcb(lpXfcb));

  /* Now update the fcb and compute where we need to position     */
  /* to. */
  lpFcb->fcb_rndm = FcbRec(lpFcb);
}

STATIC void FcbCalcRec(dos_far_ptr lpXfcb)
{

  /* Convert to fcb if necessary                                  */
  fcb *lpFcb = (fcb *) ARM_PTR (ExtFcbToFcb(lpXfcb));

  /* Now update the fcb and compute where we need to position     */
  /* to.                                                          */
  lpFcb->fcb_cublock = (UWORD)(lpFcb->fcb_rndm / 128);
  lpFcb->fcb_curec = (UBYTE)lpFcb->fcb_rndm & 127;
}

UBYTE FcbRandomBlockIO(dos_far_ptr lpXfcb, UWORD *nRecords, int mode)
{
  UBYTE nErrorCode;
  fcb *lpFcb;
  unsigned long old;

  FcbCalcRec(lpXfcb);

  /* Convert to fcb if necessary                                  */
  lpFcb = (fcb *) ARM_PTR (ExtFcbToFcb(lpXfcb));

  old = lpFcb->fcb_rndm;
  nErrorCode = FcbReadWrite(lpXfcb, *nRecords, mode);
  *nRecords = (UWORD)(lpFcb->fcb_rndm - old);

  /* Now update the fcb                                           */
  FcbCalcRec(lpXfcb);

  return nErrorCode;
}

UBYTE FcbRandomIO(dos_far_ptr lpXfcb, int mode)
{
  UWORD uwCurrentBlock;
  UBYTE ucCurrentRecord;
  UBYTE nErrorCode;
  fcb *lpFcb;

  FcbCalcRec(lpXfcb);

  /* Convert to fcb if necessary                                  */
  lpFcb = (fcb *) ARM_PTR (ExtFcbToFcb(lpXfcb));

  uwCurrentBlock = lpFcb->fcb_cublock;
  ucCurrentRecord = lpFcb->fcb_curec;

  nErrorCode = FcbReadWrite(lpXfcb, 1, mode);

  lpFcb->fcb_cublock = uwCurrentBlock;
  lpFcb->fcb_curec = ucCurrentRecord;
  return nErrorCode;
}

/* FcbOpen and FcbCreate
   Expects lpXfcb to point to a valid, unopened FCB, containing file name to open (create)
   Create will attempt to find the file name in the current directory, if found truncates
     setting file size to 0, otherwise if does not exist will create the new file; the
     FCB is filled in same as the open call.
   On any error returns FCB_ERROR
   On success returns FCB_SUCCESS, and sets the following fields (other non-system reserved ones left unchanged)
     drive identifier (fcb_drive) set to actual drive (1=A, 2=B, ...; always >0 if not device)
     current block number (fcb_cublock) to 0
     file size (fcb_fsize) value from directory entry (0 if create)
     record size (fcb_recsiz) to 128; set to 0 for devices
     time & date (fcb_time & fcb_date) values from directory entry
     fcb_sftno, fcb_attrib_hi/_lo, fcb_strtclst, fcb_dirclst/off_unused are for internal use (system reserved)
*/
UBYTE FcbOpen(dos_far_ptr lpXfcb, unsigned flags)
{
  sft *sftp;
  dos_far_ptr _sftp;
  COUNT sft_idx, FcbDrive;
  unsigned attr = 0;
  xfcb *lpExt = (xfcb *) ARM_PTR (lpXfcb);

  /* Build a traditional DOS file name                            */
  fcb *lpFcb = (fcb *) ARM_PTR (CommonFcbInit(lpXfcb, SecPathName, &FcbDrive));
  if ((flags & O_CREAT) && lpExt->xfcb_flag == 0xff)
    /* pass attribute without constraints (dangerous for directories) */
    attr = lpExt->xfcb_attrib;

  sft_idx = (short)DosOpenSft(x86_FAR_PTR(DOS_PSP, (void *)SecPathName),
                              flags, attr);
  if (sft_idx < 0)
  {
    internal_data->CritErrCode = -sft_idx;
    return FCB_ERROR;
  }

  _sftp = idx_to_sft(sft_idx);
  sftp = (sft *) ARM_PTR (_sftp);
  sftp->sft_mode |= O_FCB;

  lpFcb->fcb_sftno = sft_idx;
  lpFcb->fcb_cublock = 0;
  /* should not be cleared, programs e.g. GEM depend on these values remaining unchanged
  lpFcb->fcb_curec = 0;
  lpFcb->fcb_rndm = 0;
  */

  lpFcb->fcb_recsiz = 0;      /* true for devices   */
  if (!(sftp->sft_flags & SFT_FDEVICE)) /* check for a device */
  {
    lpFcb->fcb_drive = FcbDrive;
    lpFcb->fcb_recsiz = 128;
  }
  lpFcb->fcb_fsize = sftp->sft_size;
  lpFcb->fcb_date = sftp->sft_date;
  lpFcb->fcb_time = sftp->sft_time;
  return FCB_SUCCESS;
}

STATIC dos_far_ptr ExtFcbToFcb(dos_far_ptr lpExtFcb)
{
  dos_far_ptr res = lpExtFcb;
  if (*(UBYTE *) ARM_PTR (lpExtFcb) == 0xff)
    res = MK_FP(FP_SEG(lpExtFcb),
                (UWORD)(FP_OFF(lpExtFcb) + offsetof(xfcb, xfcb_fcb)));
  internal_data->sda_lpFcb = res;   /* guest pointer, as real DOS keeps it */
  return res;
}

STATIC dos_far_ptr CommonFcbInit(dos_far_ptr lpExtFcb, BYTE * pszBuffer,
                                 COUNT * pCurDrive)
{
  dos_far_ptr lpFcb;

  /* convert to fcb if needed first (also latches sda_lpFcb)      */
  lpFcb = ExtFcbToFcb(lpExtFcb);

  /* Build a traditional DOS file name                            */
  FcbNameInit((fcb *) ARM_PTR (lpFcb), pszBuffer, pCurDrive);
  /* and return the fcb pointer                                   */
  return lpFcb;
}

STATIC void FcbNameInit(fcb * lpFcb, BYTE * szBuffer, COUNT * pCurDrive)
{
  BYTE *pszBuffer = szBuffer;

  /* Build a traditional DOS file name                            */
  *pCurDrive = internal_data->default_drive + 1;
  if (lpFcb->fcb_drive != 0)
  {
    *pCurDrive = lpFcb->fcb_drive;
    pszBuffer[0] = 'A' + lpFcb->fcb_drive - 1;
    pszBuffer[1] = ':';
    pszBuffer += 2;
  }
  ConvertName83ToNameSZ(pszBuffer, lpFcb->fcb_fname);
}

UBYTE FcbDelete(dos_far_ptr lpXfcb)
{
  COUNT FcbDrive;
  UBYTE result = FCB_SUCCESS;
  dos_far_ptr lpOldDta = internal_data->dta;
  xfcb *lpExt = (xfcb *) ARM_PTR (lpXfcb);

  /* Build a traditional DOS file name                            */
  CommonFcbInit(lpXfcb, SecPathName, &FcbDrive);
  /* check for a device                                           */
  if (!far_is_null(IsDevice(SecPathName)))
  {
    result = FCB_ERROR;
  }
  else
  {
    int attr = (lpExt->xfcb_flag == 0xff ? lpExt->xfcb_attrib : D_ALL);

    /* original: local dmatch on the stack; port: transient guest
       scratch (dta must be a guest pointer) */
    internal_data->dta = DmatchTmp_x86;
    if ((internal_data->CritErrCode =
         -DosFindFirst(attr, x86_FAR_PTR(DOS_PSP, (void *)SecPathName)))
        != SUCCESS)
    {
      result = FCB_ERROR;
    }
    else do
    {
      SecPathName[0] = 'A' + FcbDrive - 1;
      SecPathName[1] = ':';
      strcpy(&SecPathName[2], DmatchTmp.dm_name);
      if (DosDelete(x86_FAR_PTR(DOS_PSP, (void *)SecPathName), attr)
          != SUCCESS)
      {
        result = FCB_ERROR;
        break;
      }
      internal_data->dta = DmatchTmp_x86;
    }
    while ((internal_data->CritErrCode = -DosFindNext()) == SUCCESS);
  }
  internal_data->dta = lpOldDta;
  return result;
}

UBYTE FcbRename(dos_far_ptr lpXfcb)
{
  BYTE buf[FNAME_SIZE + FEXT_SIZE];
  BOOL bWildCard;
  rfcb *lpRenameFcb;
  COUNT FcbDrive;
  UBYTE result = FCB_SUCCESS;
  dos_far_ptr lpOldDta = internal_data->dta;
  xfcb *lpExt = (xfcb *) ARM_PTR (lpXfcb);

  /* Build a traditional DOS file name                            */
  lpRenameFcb = (rfcb *) ARM_PTR (CommonFcbInit(lpXfcb, SecPathName, &FcbDrive));
  /* expand wildcards in dest                                     */
  GetNameField(lpRenameFcb->renNewName, buf, FNAME_SIZE, &bWildCard);
  GetNameField(lpRenameFcb->renNewExtent, buf + FNAME_SIZE, FEXT_SIZE,
               &bWildCard);

  /* check for a device                                           */
  if (!far_is_null(IsDevice(SecPathName)))
  {
    result = FCB_ERROR;
  }
  else
  {
    COUNT rc;

    wAttr = (lpExt->xfcb_flag == 0xff ? lpExt->xfcb_attrib : D_ALL);
    /* original: local dmatch on the stack; port: transient guest
       scratch (dta must be a guest pointer) */
    internal_data->dta = DmatchTmp_x86;
    if ((internal_data->CritErrCode =
         -DosFindFirst(wAttr, x86_FAR_PTR(DOS_PSP, (void *)SecPathName)))
        != SUCCESS)
    {
      result = FCB_ERROR;
    }
    else do
    {
      fcb LocalFcb;
      BYTE *pToName;
      const char *pToPattern = buf;
      int i;

      /* original: FcbParseFname(&mode, Dmatch.dm_name, &LocalFcb);
         see FcbNameFromSZ() above for why the port differs */
      FcbNameFromSZ(&LocalFcb, (const BYTE *)DmatchTmp.dm_name);
      /* Overlay the pattern, skipping '?'            */
      /* I'm cheating because this assumes that the   */
      /* struct alignments are on byte boundaries     */
      pToName = LocalFcb.fcb_fname;
      for (i = 0; i < FNAME_SIZE + FEXT_SIZE; i++)
      {
        if (*pToPattern != '?')
          *pToName = *pToPattern;
        pToName++;
        pToPattern++;
      }

      SecPathName[0] = 'A' + FcbDrive - 1;
      SecPathName[1] = ':';
      strcpy(&SecPathName[2], DmatchTmp.dm_name);
      rc = truename(x86_FAR_PTR(DOS_PSP, (void *)SecPathName),
                    PriPathName, 0);

      if (rc < SUCCESS || (rc & IS_DEVICE))
      {
        result = FCB_ERROR;
        break;
      }
      /* now to build a dos name again                */
      LocalFcb.fcb_drive = FcbDrive;
      FcbNameInit(&LocalFcb, (BYTE *)internal_data->fcb_ren_name, &FcbDrive);
      rc = truename(x86_FAR_PTR(DOS_PSP,
                                (void *)internal_data->fcb_ren_name),
                    SecPathName, 0);
      if (rc < SUCCESS || (rc & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE
          || DosRenameTrue(PriPathName, SecPathName, wAttr) != SUCCESS)
      {
        result = FCB_ERROR;
        break;
      }
      internal_data->dta = DmatchTmp_x86;
    }
    while ((internal_data->CritErrCode = -DosFindNext()) == SUCCESS);
  }
  internal_data->dta = lpOldDta;
  return result;
}

/* TE:the MoveDirInfo() is now done by simply copying the dirEntry into the FCB
   this prevents problems with ".", ".." and saves code
   BO:use global SearchDir, as produced by FindFirst/Next
*/

UBYTE FcbClose(dos_far_ptr lpXfcb)
{
  sft *s;
  dos_far_ptr _s;

  /* Convert to fcb if necessary                                  */
  fcb *lpFcb = (fcb *) ARM_PTR (ExtFcbToFcb(lpXfcb));

  /* An already closed FCB can be closed again without error */
  if (lpFcb->fcb_sftno == (BYTE) 0xff)
    return FCB_SUCCESS;

  /* Get the SFT block that contains the SFT      */
  _s = idx_to_sft(lpFcb->fcb_sftno);
  if (far_is_end(_s))
    return FCB_ERROR;
  s = (sft *) ARM_PTR (_s);

  /* change time and set file size                */
  s->sft_size = lpFcb->fcb_fsize;
  if (!(s->sft_flags & SFT_FSHARED))
    dos_merge_file_changes(lpFcb->fcb_sftno);
  DosSetFtimeSft(lpFcb->fcb_sftno, lpFcb->fcb_date, lpFcb->fcb_time);
  if ((internal_data->CritErrCode =
       -DosCloseSft(lpFcb->fcb_sftno, FALSE)) == SUCCESS)
  {
    lpFcb->fcb_sftno = (BYTE) 0xff;
    return FCB_SUCCESS;
  }
  return FCB_ERROR;
}

/* close all files the current process opened by FCBs */
VOID FcbCloseAll(void)
{
#if DIAG
  extern volatile uint32_t dos_diag_kernel_code;
#endif
  dos_far_ptr block = LoL->sfthead;
  COUNT idx = 0;
  unsigned blocks = 0;

  /*
   * Walk the SFT chain once.  The old implementation called idx_to_sft()
   * for idx=0,1,...; each call restarted at LoL->sfthead, so a corrupt
   * cyclic sftt_next chain could trap process teardown forever.
   */
  while (!far_is_end(block))
  {
    sfttbl *sp;
    dos_far_ptr next;
    COUNT j;

    if (++blocks > 64u)
    {
#if DIAG
      dos_diag_kernel_code = 0x63fe0000u | (blocks & 0xffffu);
#endif
      return;
    }

    sp = (sfttbl *)ARM_PTR(block);
    if (sp->sftt_count < 0 || sp->sftt_count > SFTMAX)
    {
#if DIAG
      dos_diag_kernel_code =
          0x63fd0000u | ((unsigned)sp->sftt_count & 0xffffu);
#endif
      return;
    }

#if DIAG
    dos_diag_kernel_code =
        0x63000000u | ((blocks & 0xffu) << 16) | ((unsigned)idx & 0xffffu);
#endif
    for (j = 0; j < sp->sftt_count; ++j, ++idx)
    {
      sft *sftp = &sp->sftt_table[j];
      if ((sftp->sft_mode & O_FCB) && sftp->sft_psp == internal_data->cu_psp)
      {
#if DIAG
        dos_diag_kernel_code = 0x63100000u | ((unsigned)idx & 0xffffu);
#endif
        DosCloseSft(idx, FALSE);
#if DIAG
        dos_diag_kernel_code = 0x63200000u | ((unsigned)idx & 0xffffu);
#endif
      }
    }

    next = sp->sftt_next;
    if (!far_is_end(next) &&
        FP_SEG(next) == FP_SEG(block) && FP_OFF(next) == FP_OFF(block))
    {
#if DIAG
      dos_diag_kernel_code =
          0x63fc0000u | ((unsigned)FP_OFF(block) & 0xffffu);
#endif
      return;
    }
    block = next;
  }
#if DIAG
  dos_diag_kernel_code = 0x63ff0000u | ((unsigned)idx & 0xffffu);
#endif
}

UBYTE FcbFindFirstNext(dos_far_ptr lpXfcb, BOOL First)
{
  dos_far_ptr orig_dta = internal_data->dta;
  UBYTE *lpDir;
  COUNT FcbDrive;
  fcb *lpFcb;
  dos_far_ptr lpFcb_x86;

  /* First, move the dta to a local and change it around to match */
  /* our functions.                                               */
  /* (port: lpDir walks the CALLER's DTA - a guest buffer)        */
  lpDir = (UBYTE *) ARM_PTR (orig_dta);
  internal_data->dta = Dmatch_x86;

  /* Next initialze local variables by moving them from the fcb   */
  lpFcb_x86 = CommonFcbInit(lpXfcb, SecPathName, &FcbDrive);
  lpFcb = (fcb *) ARM_PTR (lpFcb_x86);
  if (First)
  {
    /* Reconstruct the dirmatch structure from the fcb */
    Dmatch.dm_drive = lpFcb->fcb_sftno;

    memcpy(Dmatch.dm_name_pat, lpFcb->fcb_fname, FNAME_SIZE + FEXT_SIZE);
    DosUpFMem((BYTE FAR *) Dmatch.dm_name_pat, FNAME_SIZE + FEXT_SIZE);

    Dmatch.dm_attr_srch = wAttr;
    Dmatch.dm_entry = lpFcb->fcb_strtclst;
    Dmatch.dm_dircluster = lpFcb->fcb_dirclst;

    wAttr = D_ALL;
  }

  /* original: `if ((xfcb FAR *)lpFcb != lpXfcb)` - i.e. "is this an
     extended FCB" (ExtFcbToFcb() returned a shifted pointer). The
     port's dos_far_ptr is a struct and cannot be compared with !=;
     the equivalent test is the extended-FCB signature byte itself. */
  if (((xfcb *) ARM_PTR (lpXfcb))->xfcb_flag == 0xff)
  {
    wAttr = ((xfcb *) ARM_PTR (lpXfcb))->xfcb_attrib;
    guest_read_block(((uint32_t)FP_SEG(lpXfcb) << 4) + FP_OFF(lpXfcb), lpDir, 7);
    lpDir += 7;
  }

  internal_data->CritErrCode =
      -(First ? DosFindFirst(wAttr, x86_FAR_PTR(DOS_PSP, (void *)SecPathName))
              : DosFindNext());
  if (internal_data->CritErrCode != SUCCESS)
  {
    internal_data->dta = orig_dta;
    return FCB_ERROR;
  }

  *lpDir++ = FcbDrive;
  memcpy(lpDir, &SearchDirD, sizeof(struct dirent));

  lpFcb->fcb_dirclst = (UWORD) Dmatch.dm_dircluster;
  lpFcb->fcb_strtclst = Dmatch.dm_entry;

/*
  This is undocumented and seen using Pcwatch and Ramview.
  The First byte is the current directory count and the second seems
  to be the attribute byte.
 */
  lpFcb->fcb_sftno = Dmatch.dm_drive;   /* MSD seems to save this @ fcb_date. */
#if 0
  lpFcb->fcb_cublock = Dmatch.dm_entry;
  lpFcb->fcb_cublock *= 0x100;
  lpFcb->fcb_cublock += wAttr;
#endif
  internal_data->dta = orig_dta;
  return FCB_SUCCESS;
}
