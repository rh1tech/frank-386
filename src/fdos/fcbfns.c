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
/*      addressed directly from DOS_PSP + X86_INTERNAL_DATA_OFF.*/
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include "fdos.h"
#include "globals.h"
#include "proto.h"
#include "kernel_guest_proxy.h"

#define FCB_SUCCESS     0
#define FCB_ERR_NODATA  1
#define FCB_ERR_SEGMENT_WRAP 2
#define FCB_ERR_EOF     3
#define FCB_ERROR       0xff

STATIC dos_far_ptr ExtFcbToFcb(dos_far_ptr lpExtFcb);
STATIC dos_far_ptr CommonFcbInit(dos_far_ptr lpExtFcb, dos_far_ptr x86_buffer,
                                 COUNT * pCurDrive);
STATIC void FcbNameInit(fcb * lpFcb, BYTE * pszBuffer, COUNT * pCurDrive);
STATIC void FcbNextRecord(fcb * lpFcb);
STATIC void FcbCalcRec(dos_far_ptr lpXfcb);

#define TestCmnSeps(lpFileName) (*lpFileName && strchr(":;,=+ \t", *lpFileName) != NULL)
#define TestFieldSeps(lpFileName) ((unsigned char)*lpFileName <= ' ' || strchr("/\\\"[]<>|.:;,=+\t", *lpFileName) != NULL)

/* Guest-resident SDA scratch used as temporary DTA. */
#define FCB_SDA_LINEAR (((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF)
#define DMATCH_LINEAR (FCB_SDA_LINEAR + offsetof(struct dos_data, fcb_dmatch))
#define DMATCH_TMP_LINEAR (FCB_SDA_LINEAR + offsetof(struct dos_data, fcb_dmatch_tmp))
#define FCB_REN_LINEAR (FCB_SDA_LINEAR + offsetof(struct dos_data, fcb_ren_name))
#define FCB_PRI_PATH_FAR MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, PriPathBuffer)))
#define FCB_SEC_PATH_FAR MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, SecPathBuffer)))
#define FCB_PRI_PATH_LINEAR (FCB_SDA_LINEAR + offsetof(struct dos_data, PriPathBuffer))
#define FCB_SEC_PATH_LINEAR (FCB_SDA_LINEAR + offsetof(struct dos_data, SecPathBuffer))
#define FCB_SEARCHDIR_LINEAR (FCB_SDA_LINEAR + offsetof(struct dos_data, SearchDir))
#define Dmatch_x86 MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, fcb_dmatch)))
#define DmatchTmp_x86 MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, fcb_dmatch_tmp)))

static uint32_t fcb_far_linear(dos_far_ptr p)
{
  return ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
}

static dos_far_ptr fcb_dta(void)
{
  const uint32_t a = FCB_SDA_LINEAR + offsetof(struct dos_data, dta);
  return MK_FP(pload16(a + 2u), pload16(a));
}

static void fcb_set_dta(dos_far_ptr p)
{
  const uint32_t a = FCB_SDA_LINEAR + offsetof(struct dos_data, dta);
  pstore16(a, FP_OFF(p));
  pstore16(a + 2u, FP_SEG(p));
}

static void fcb_set_crit(UWORD v)
{
  pstore16(FCB_SDA_LINEAR + offsetof(struct dos_data, CritErrCode), v);
}

static void fcb_set_lp_fcb(dos_far_ptr p)
{
  const uint32_t a = FCB_SDA_LINEAR + offsetof(struct dos_data, sda_lpFcb);
  pstore16(a, FP_OFF(p));
  pstore16(a + 2u, FP_SEG(p));
}

static void fcb_load(dos_far_ptr p, fcb *out)
{
  guest_read_block(fcb_far_linear(p), out, sizeof(*out));
}

static void fcb_store(dos_far_ptr p, const fcb *in)
{
  guest_write_block(fcb_far_linear(p), in, sizeof(*in));
}

static void dmatch_load(uint32_t addr, dmatch *out)
{
  guest_read_block(addr, out, sizeof(*out));
}

static void dmatch_store(uint32_t addr, const dmatch *in)
{
  guest_write_block(addr, in, sizeof(*in));
}

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
        get_dpb(drive == 0 ? fdos_dos_default_drive() : drive - 1);
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
  nf_memset(lpDestField, cFill, nFieldSize - nIndex);
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

  nf_memset(lpFcb->fcb_fname, ' ', FNAME_SIZE);
  nf_memset(lpFcb->fcb_fext, ' ', FEXT_SIZE);
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
  const dos_far_ptr fcbp = ExtFcbToFcb(lpXfcb);
  fcb local;
  ULONG lPosit;
  long nTransfer;
  unsigned size;
  unsigned long bigsize;
  unsigned recsiz;
  dos_far_ptr dta;

  fcb_load(fcbp, &local);
  recsiz = local.fcb_recsiz;
  bigsize = (ULONG)recsiz * recno;
  if (bigsize > 0xffff)
    return FCB_ERR_SEGMENT_WRAP;
  size = (unsigned)bigsize;
  dta = fcb_dta();
  if ((UWORD)(FP_OFF(dta) + size) < FP_OFF(dta))
    return FCB_ERR_SEGMENT_WRAP;

  lPosit = FcbRec(&local) * recsiz;
  {
    long rc = SftSeek(local.fcb_sftno, lPosit, 0);
    fcb_set_crit((UWORD)-rc);
    if (rc != SUCCESS)
      return FCB_ERR_NODATA;
  }

  nTransfer = DosRWSft(local.fcb_sftno, size, dta, mode & ~XFR_FCB_RANDOM);
  if (nTransfer < 0)
    fcb_set_crit((UWORD)(-(int)nTransfer));
  if (mode & XFR_WRITE)
    local.fcb_fsize = SftGetFsize(local.fcb_sftno);
  if (mode & XFR_FCB_RANDOM && recsiz > 0)
    local.fcb_rndm += ((unsigned)nTransfer + recsiz - 1) / recsiz;

  size -= (unsigned)nTransfer;
  if (size == 0)
  {
    FcbNextRecord(&local);
    fcb_store(fcbp, &local);
    return FCB_SUCCESS;
  }
  size %= local.fcb_recsiz;
  if (mode & XFR_READ && size > 0)
  {
    fmemset(MK_FP(FP_SEG(dta), FP_OFF(dta) + (unsigned)nTransfer), 0, size);
    FcbNextRecord(&local);
    fcb_store(fcbp, &local);
    return FCB_ERR_EOF;
  }
  fcb_store(fcbp, &local);
  return FCB_ERR_NODATA;
}

UBYTE FcbGetFileSize(dos_far_ptr lpXfcb)
{
  int FcbDrive, sft_idx;
  const dos_far_ptr fcbp = CommonFcbInit(lpXfcb, FCB_SEC_PATH_FAR, &FcbDrive);
  fcb local;
  unsigned recsiz;
  fcb_load(fcbp, &local);
  recsiz = local.fcb_recsiz;
  if (!far_is_null(IsDeviceGuest(FCB_SEC_PATH_FAR)) || recsiz == 0)
    return FCB_ERROR;
  sft_idx = (short)DosOpenSft(FCB_SEC_PATH_FAR,
                              O_LEGACY | O_RDONLY | O_OPEN, 0);
  if (sft_idx >= 0)
  {
    long rc;
    local.fcb_rndm = (SftGetFsize(sft_idx) + (recsiz - 1)) / recsiz;
    fcb_store(fcbp, &local);
    rc = DosCloseSft(sft_idx, FALSE);
    fcb_set_crit((UWORD)-rc);
    if (rc == SUCCESS)
      return FCB_SUCCESS;
  }
  else
    fcb_set_crit((UWORD)-sft_idx);
  return FCB_ERROR;
}

void FcbSetRandom(dos_far_ptr lpXfcb)
{
  const dos_far_ptr fcbp = ExtFcbToFcb(lpXfcb);
  fcb local;
  fcb_load(fcbp, &local);
  local.fcb_rndm = FcbRec(&local);
  fcb_store(fcbp, &local);
}

STATIC void FcbCalcRec(dos_far_ptr lpXfcb)
{
  const dos_far_ptr fcbp = ExtFcbToFcb(lpXfcb);
  fcb local;
  fcb_load(fcbp, &local);
  local.fcb_cublock = (UWORD)(local.fcb_rndm / 128);
  local.fcb_curec = (UBYTE)local.fcb_rndm & 127;
  fcb_store(fcbp, &local);
}

UBYTE FcbRandomBlockIO(dos_far_ptr lpXfcb, UWORD *nRecords, int mode)
{
  UBYTE rc;
  const dos_far_ptr fcbp = ExtFcbToFcb(lpXfcb);
  fcb local;
  ULONG old;
  FcbCalcRec(lpXfcb);
  fcb_load(fcbp, &local);
  old = local.fcb_rndm;
  rc = FcbReadWrite(lpXfcb, *nRecords, mode);
  fcb_load(fcbp, &local);
  *nRecords = (UWORD)(local.fcb_rndm - old);
  FcbCalcRec(lpXfcb);
  return rc;
}

UBYTE FcbRandomIO(dos_far_ptr lpXfcb, int mode)
{
  UWORD block;
  UBYTE record;
  UBYTE rc;
  const dos_far_ptr fcbp = ExtFcbToFcb(lpXfcb);
  fcb local;
  FcbCalcRec(lpXfcb);
  fcb_load(fcbp, &local);
  block = local.fcb_cublock;
  record = local.fcb_curec;
  rc = FcbReadWrite(lpXfcb, 1, mode);
  fcb_load(fcbp, &local);
  local.fcb_cublock = block;
  local.fcb_curec = record;
  fcb_store(fcbp, &local);
  return rc;
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
  const uint32_t xbase = fcb_far_linear(lpXfcb);
  COUNT sft_idx, FcbDrive;
  unsigned attr = 0;
  dos_far_ptr fcbp = CommonFcbInit(lpXfcb, FCB_SEC_PATH_FAR, &FcbDrive);
  fcb local;

  fcb_load(fcbp, &local);
  if ((flags & O_CREAT) && pload8(xbase + offsetof(xfcb, xfcb_flag)) == 0xff)
    attr = pload8(xbase + offsetof(xfcb, xfcb_attrib));

  sft_idx = (short)DosOpenSft(FCB_SEC_PATH_FAR,
                              flags, attr);
  if (sft_idx < 0)
  {
    fcb_set_crit((UWORD)-sft_idx);
    return FCB_ERROR;
  }

  {
    dos_far_ptr sftp = idx_to_sft(sft_idx);
    UWORD entry_flags = fdos_sft_flags_raw(sftp);
    fdos_sft_or_mode(sftp, O_FCB);
    local.fcb_sftno = (BYTE)sft_idx;
    local.fcb_cublock = 0;
    local.fcb_recsiz = 0;
    if (!(entry_flags & SFT_FDEVICE))
    {
      local.fcb_drive = FcbDrive;
      local.fcb_recsiz = 128;
    }
    local.fcb_fsize = fdos_sft_size(sftp);
    local.fcb_date = fdos_sft_date(sftp);
    local.fcb_time = fdos_sft_time(sftp);
    fcb_store(fcbp, &local);
  }
  return FCB_SUCCESS;
}

STATIC dos_far_ptr ExtFcbToFcb(dos_far_ptr lpExtFcb)
{
  dos_far_ptr res = lpExtFcb;
  if (pload8(fcb_far_linear(lpExtFcb)) == 0xff)
    res = MK_FP(FP_SEG(lpExtFcb),
                (UWORD)(FP_OFF(lpExtFcb) + offsetof(xfcb, xfcb_fcb)));
  fcb_set_lp_fcb(res);
  return res;
}

STATIC dos_far_ptr CommonFcbInit(dos_far_ptr lpExtFcb, dos_far_ptr x86_buffer,
                                 COUNT * pCurDrive)
{
  dos_far_ptr lpFcb = ExtFcbToFcb(lpExtFcb);
  fcb local;
  BYTE path[2 + FNAME_SIZE + 1 + FEXT_SIZE + 1];
  fcb_load(lpFcb, &local);
  FcbNameInit(&local, path, pCurDrive);
  guest_write(x86_buffer, path, strlen((const char *)path) + 1u);
  return lpFcb;
}

STATIC void FcbNameInit(fcb * lpFcb, BYTE * szBuffer, COUNT * pCurDrive)
{
  BYTE *pszBuffer = szBuffer;

  /* Build a traditional DOS file name                            */
  *pCurDrive = fdos_dos_default_drive() + 1;
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
  dos_far_ptr lpOldDta = fcb_dta();
  const uint32_t xbase = fcb_far_linear(lpXfcb);

  CommonFcbInit(lpXfcb, FCB_SEC_PATH_FAR, &FcbDrive);
  if (!far_is_null(IsDeviceGuest(FCB_SEC_PATH_FAR)))
    result = FCB_ERROR;
  else
  {
    const int attr = (pload8(xbase + offsetof(xfcb, xfcb_flag)) == 0xff)
                     ? pload8(xbase + offsetof(xfcb, xfcb_attrib)) : D_ALL;
    long rc;
    fcb_set_dta(DmatchTmp_x86);
    rc = DosFindFirst(attr, FCB_SEC_PATH_FAR);
    fcb_set_crit((UWORD)-rc);
    if (rc != SUCCESS)
      result = FCB_ERROR;
    else do
    {
      dmatch dm;
      dmatch_load(DMATCH_TMP_LINEAR, &dm);
      pstore8(FCB_SEC_PATH_LINEAR, (UBYTE)('A' + FcbDrive - 1));
      pstore8(FCB_SEC_PATH_LINEAR + 1u, ':');
      guest_write_block(FCB_SEC_PATH_LINEAR + 2u, dm.dm_name,
                        strlen(dm.dm_name) + 1u);
      if (DosDelete(FCB_SEC_PATH_FAR, attr) != SUCCESS)
      {
        result = FCB_ERROR;
        break;
      }
      fcb_set_dta(DmatchTmp_x86);
      rc = DosFindNext();
      fcb_set_crit((UWORD)-rc);
    } while (rc == SUCCESS);
  }
  fcb_set_dta(lpOldDta);
  return result;
}

UBYTE FcbRename(dos_far_ptr lpXfcb)
{
  BYTE buf[FNAME_SIZE + FEXT_SIZE];
  BOOL bWildCard;
  COUNT FcbDrive;
  UBYTE result = FCB_SUCCESS;
  dos_far_ptr lpOldDta = fcb_dta();
  const uint32_t xbase = fcb_far_linear(lpXfcb);
  const dos_far_ptr fcbp = CommonFcbInit(lpXfcb, FCB_SEC_PATH_FAR, &FcbDrive);
  rfcb rename_fcb;

  guest_read_block(fcb_far_linear(fcbp), &rename_fcb, sizeof(rename_fcb));
  GetNameField(rename_fcb.renNewName, buf, FNAME_SIZE, &bWildCard);
  GetNameField(rename_fcb.renNewExtent, buf + FNAME_SIZE, FEXT_SIZE, &bWildCard);

  if (!far_is_null(IsDeviceGuest(FCB_SEC_PATH_FAR)))
    result = FCB_ERROR;
  else
  {
    COUNT rc;
    wAttr = (pload8(xbase + offsetof(xfcb, xfcb_flag)) == 0xff)
            ? pload8(xbase + offsetof(xfcb, xfcb_attrib)) : D_ALL;
    fcb_set_dta(DmatchTmp_x86);
    rc = DosFindFirst(wAttr, FCB_SEC_PATH_FAR);
    fcb_set_crit((UWORD)-rc);
    if (rc != SUCCESS)
      result = FCB_ERROR;
    else do
    {
      dmatch dm;
      fcb LocalFcb;
      BYTE *pToName;
      const char *pToPattern = (const char *)buf;
      BYTE ren_name[sizeof(((struct dos_data *)0)->fcb_ren_name)];
      int i;

      dmatch_load(DMATCH_TMP_LINEAR, &dm);
      FcbNameFromSZ(&LocalFcb, (const BYTE *)dm.dm_name);
      pToName = LocalFcb.fcb_fname;
      for (i = 0; i < FNAME_SIZE + FEXT_SIZE; i++)
      {
        if (*pToPattern != '?')
          *pToName = *pToPattern;
        pToName++;
        pToPattern++;
      }

      pstore8(FCB_SEC_PATH_LINEAR, (UBYTE)('A' + FcbDrive - 1));
      pstore8(FCB_SEC_PATH_LINEAR + 1u, ':');
      guest_write_block(FCB_SEC_PATH_LINEAR + 2u, dm.dm_name,
                        strlen(dm.dm_name) + 1u);
      rc = truename_guest(FCB_SEC_PATH_FAR, FCB_PRI_PATH_FAR, 0);
      if (rc < SUCCESS || (rc & IS_DEVICE))
      {
        result = FCB_ERROR;
        break;
      }

      LocalFcb.fcb_drive = FcbDrive;
      nf_memset(ren_name, 0, sizeof(ren_name));
      FcbNameInit(&LocalFcb, ren_name, &FcbDrive);
      guest_write_block(FCB_REN_LINEAR, ren_name, sizeof(ren_name));
      rc = truename_guest(MK_FP(DOS_PSP,
                          (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, fcb_ren_name))),
                    FCB_SEC_PATH_FAR, 0);
      if (rc < SUCCESS || (rc & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE ||
          DosRenameTrueGuest(FCB_PRI_PATH_FAR, FCB_SEC_PATH_FAR, wAttr) != SUCCESS)
      {
        result = FCB_ERROR;
        break;
      }
      fcb_set_dta(DmatchTmp_x86);
      rc = DosFindNext();
      fcb_set_crit((UWORD)-rc);
    } while (rc == SUCCESS);
  }
  fcb_set_dta(lpOldDta);
  return result;
}

/* TE:the MoveDirInfo() is now done by simply copying the dirEntry into the FCB
   this prevents problems with ".", ".." and saves code
   BO:use global SearchDir, as produced by FindFirst/Next
*/

UBYTE FcbClose(dos_far_ptr lpXfcb)
{
  const dos_far_ptr fcbp = ExtFcbToFcb(lpXfcb);
  fcb local;
  dos_far_ptr sftp;
  long rc;

  fcb_load(fcbp, &local);
  if (local.fcb_sftno == (BYTE)0xff)
    return FCB_SUCCESS;

  sftp = idx_to_sft(local.fcb_sftno);
  if (far_is_end(sftp))
    return FCB_ERROR;

  fdos_sft_set_size(sftp, local.fcb_fsize);
  if (!(fdos_sft_flags_raw(sftp) & SFT_FSHARED))
    dos_merge_file_changes(local.fcb_sftno);
  DosSetFtimeSft(local.fcb_sftno, local.fcb_date, local.fcb_time);
  rc = DosCloseSft(local.fcb_sftno, FALSE);
  fcb_set_crit((UWORD)-rc);
  if (rc == SUCCESS)
  {
    local.fcb_sftno = (BYTE)0xff;
    fcb_store(fcbp, &local);
    return FCB_SUCCESS;
  }
  return FCB_ERROR;
}

VOID FcbCloseAll(void)
{
#if DIAG
  extern volatile uint32_t dos_diag_kernel_code;
#endif
  dos_far_ptr block = fdos_lol_sfthead();
  COUNT idx = 0;
  unsigned blocks = 0;
  const UWORD owner = fdos_dos_cu_psp();

  while (!far_is_end(block))
  {
    const UWORD count = fdos_sfttbl_count(block);
    dos_far_ptr next;
    UWORD j;

    if (++blocks > 64u)
    {
#if DIAG
      dos_diag_kernel_code = 0x63fe0000u | (blocks & 0xffffu);
#endif
      return;
    }

    if (count > SFTMAX)
    {
#if DIAG
      dos_diag_kernel_code = 0x63fd0000u | (count & 0xffffu);
#endif
      return;
    }

#if DIAG
    dos_diag_kernel_code =
        0x63000000u | ((blocks & 0xffu) << 16) | ((unsigned)idx & 0xffffu);
#endif
    for (j = 0; j < count; ++j, ++idx)
    {
      const dos_far_ptr sftp = fdos_sfttbl_entry(block, j);
      if ((fdos_sft_mode_raw(sftp) & O_FCB) && fdos_sft_psp(sftp) == owner)
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

    next = fdos_sfttbl_next(block);
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
  dos_far_ptr orig_dta = fcb_dta();
  uint32_t out = fcb_far_linear(orig_dta);
  COUNT FcbDrive;
  const dos_far_ptr fcbp = CommonFcbInit(lpXfcb, FCB_SEC_PATH_FAR, &FcbDrive);
  fcb local;
  dmatch dm;
  long rc;

  fcb_set_dta(Dmatch_x86);
  fcb_load(fcbp, &local);
  if (First)
  {
    nf_memset(&dm, 0, sizeof(dm));
    dm.dm_drive = local.fcb_sftno;
    dos_api_memcpy(dm.dm_name_pat, local.fcb_fname, FNAME_SIZE + FEXT_SIZE);
    DosUpFMem((BYTE FAR *)dm.dm_name_pat, FNAME_SIZE + FEXT_SIZE);
    dm.dm_attr_srch = wAttr;
    dm.dm_entry = local.fcb_strtclst;
    dm.dm_dircluster = local.fcb_dirclst;
    dmatch_store(DMATCH_LINEAR, &dm);
    wAttr = D_ALL;
  }

  if (pload8(fcb_far_linear(lpXfcb) + offsetof(xfcb, xfcb_flag)) == 0xff)
  {
    wAttr = pload8(fcb_far_linear(lpXfcb) + offsetof(xfcb, xfcb_attrib));
    guest_move_block(out, fcb_far_linear(lpXfcb), 7);
    out += 7;
  }

  rc = First ? DosFindFirst(wAttr, FCB_SEC_PATH_FAR)
             : DosFindNext();
  fcb_set_crit((UWORD)-rc);
  if (rc != SUCCESS)
  {
    fcb_set_dta(orig_dta);
    return FCB_ERROR;
  }

  dmatch_load(DMATCH_LINEAR, &dm);
  pstore8(out++, (UBYTE)FcbDrive);
  guest_move_block(out, FCB_SEARCHDIR_LINEAR, sizeof(struct dirent));

  local.fcb_dirclst = (UWORD)dm.dm_dircluster;
  local.fcb_strtclst = dm.dm_entry;
  local.fcb_sftno = dm.dm_drive;
  fcb_store(fcbp, &local);
  fcb_set_dta(orig_dta);
  return FCB_SUCCESS;
}
