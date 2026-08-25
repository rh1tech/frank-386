#include <ctype.h>
#include "hdrs.h"
#include "bios/bios.h"

#define log(x)
/// printf x

typedef enum nls_buf_kind {
  NLS_BUF_NONE = 0,
  NLS_BUF_NATIVE,
  NLS_BUF_GUEST
} nls_buf_kind;

typedef struct nls_buf_ref {
  nls_buf_kind kind;
  union {
    VOID *native;
    dos_far_ptr guest;
  } u;
} nls_buf_ref;

static inline uint32_t nls_linear(dos_far_ptr p)
{
  return EFFECTIVE(p);
}

static inline dos_far_ptr nls_far_load(uint32_t addr)
{
  uint32_t v = pload32(addr);
  return MK_FP((UWORD)(v >> 16), (UWORD)v);
}

static inline void nls_far_store(uint32_t addr, dos_far_ptr p)
{
  pstore32(addr, ((uint32_t)FP_SEG(p) << 16) | FP_OFF(p));
}

static inline UBYTE nls_pkg_u8(dos_far_ptr p, size_t off)
{
  return pload8(nls_linear(p) + off);
}

static inline UWORD nls_pkg_u16(dos_far_ptr p, size_t off)
{
  return pload16(nls_linear(p) + off);
}

static inline dos_far_ptr nls_pkg_far(dos_far_ptr p, size_t off)
{
  return nls_far_load(nls_linear(p) + off);
}

static inline dos_far_ptr nls_info_actpkg(void)
{
  return nls_far_load(nls_linear(x86_nlsInfo) + offsetof(struct nlsInfoBlock, actPkg));
}

static inline dos_far_ptr nls_info_chain(void)
{
  return nls_far_load(nls_linear(x86_nlsInfo) + offsetof(struct nlsInfoBlock, chain));
}

static inline UWORD nls_info_syscp(void)
{
  return pload16(nls_linear(x86_nlsInfo) + offsetof(struct nlsInfoBlock, sysCodePage));
}

static inline void nls_info_set_actpkg(dos_far_ptr p)
{
  nls_far_store(nls_linear(x86_nlsInfo) + offsetof(struct nlsInfoBlock, actPkg), p);
}

static inline nls_buf_ref nls_native_buf(VOID *p)
{
  nls_buf_ref r;
  r.kind = p ? NLS_BUF_NATIVE : NLS_BUF_NONE;
  r.u.native = p;
  return r;
}

static inline nls_buf_ref nls_guest_buf(dos_far_ptr p)
{
  nls_buf_ref r;
  r.kind = far_is_null(p) ? NLS_BUF_NONE : NLS_BUF_GUEST;
  r.u.guest = p;
  return r;
}

static inline UBYTE nls_buf_load8(nls_buf_ref b, unsigned off)
{
  if (b.kind == NLS_BUF_GUEST)
    return pload8(nls_linear(b.u.guest) + off);
  return ((const UBYTE *)b.u.native)[off];
}

static inline void nls_buf_store8(nls_buf_ref b, unsigned off, UBYTE v)
{
  if (b.kind == NLS_BUF_GUEST)
    pstore8(nls_linear(b.u.guest) + off, v);
  else
    ((UBYTE *)b.u.native)[off] = v;
}

static inline dos_far_ptr nls_pointer_entry(dos_far_ptr pkg, unsigned index)
{
  return add_far_x86(pkg, offsetof(struct nlsPackage, nlsPointers) +
                           index * sizeof(struct nlsPointer));
}

static inline UBYTE nls_pointer_subfct(dos_far_ptr p)
{
  return pload8(nls_linear(p) + offsetof(struct nlsPointer, subfct));
}

static inline dos_far_ptr nls_pointer_data(dos_far_ptr p)
{
  return nls_far_load(nls_linear(p) + offsetof(struct nlsPointer, pointer));
}

ULONG call_nls(UWORD bp,
               dos_far_ptr buf,
               UWORD subfct,
               UWORD cp,
               UWORD cntry,
               UWORD bufsize)
{
    dos_far_ptr x86_ptr = x86_nlsInfo;
    CPU_regs regs;
    cpu_save_regs(cpu, &regs);
    SET_DS(FP_SEG(x86_ptr));
    CPU_SI = FP_OFF(x86_ptr);
    if (!far_is_null(buf)) {
        SET_ES(FP_SEG(buf));
        CPU_DI = FP_OFF(buf);
    } else {
        SET_ES(0);
        CPU_DI = 0;
    }
    CPU_BP = bp;
    CPU_CX = bufsize;
    CPU_DX = cntry;
    CPU_BX = cp;
    CPU_AX = (0x14u << 8) | (subfct & 0x00ff);
    bios_intcall(cpu, 0x2F, "NLS 2F");
    {
      ULONG res = ((ULONG)CPU_BX << 16) | CPU_AX;
      cpu_restore_regs(cpu, &regs);
      return res;
    }
}

STATIC long muxGo(int subfct, UWORD bp, UWORD cp, UWORD cntry, UWORD bufsize,
                  dos_far_ptr buf)
{
  long ret;
  ret = (long)call_nls(bp, buf, subfct, cp, cntry, bufsize);
  return ret;
}

STATIC int muxBufGo(int subfct, int bp, UWORD cp, UWORD cntry,
                    UWORD bufsize, dos_far_ptr buf)
{
  return (int)(int16_t)muxGo(subfct, bp, cp, cntry, bufsize, buf);
}

STATIC dos_far_ptr searchPackage(UWORD cp, UWORD cntry)
{
  dos_far_ptr p = nls_info_actpkg();

  if (far_is_null(p) || far_is_null(nls_info_chain()))
    return MK_FP(0, 0);

  if (cp == NLS_DEFAULT)
    cp = nls_pkg_u16(p, offsetof(struct nlsPackage, cp));
  if (cntry == NLS_DEFAULT)
    cntry = nls_pkg_u16(p, offsetof(struct nlsPackage, cntry));

  for (p = nls_info_chain(); !far_is_null(p);
       p = nls_pkg_far(p, offsetof(struct nlsPackage, nxt)))
    if (nls_pkg_u16(p, offsetof(struct nlsPackage, cp)) == cp &&
        nls_pkg_u16(p, offsetof(struct nlsPackage, cntry)) == cntry)
      return p;

  return MK_FP(0, 0);
}

STATIC dos_far_ptr locateSubfct(dos_far_ptr nls, int subfct)
{
  unsigned i, count;

  if (far_is_null(nls))
    return MK_FP(0, 0);

  count = nls_pkg_u16(nls, offsetof(struct nlsPackage, numSubfct));
  for (i = 0; i < count; ++i) {
    dos_far_ptr p = nls_pointer_entry(nls, i);
    if (nls_pointer_subfct(p) == (UBYTE)subfct)
      return p;
  }
  return MK_FP(0, 0);
}

static dos_far_ptr getTable(unsigned subfct, dos_far_ptr nls)
{
  dos_far_ptr p = locateSubfct(nls, (int)subfct);
  return far_is_null(p) ? MK_FP(0, 0) : nls_pointer_data(p);
}

#define getTable2(nls) getTable(2, (nls))
#define getTable4(nls) getTable(4, (nls))
#define getTable7(nls) getTable(7, (nls))

STATIC COUNT cpyGuestBuf(dos_far_ptr dst, UWORD dstlen,
                         uint32_t src, UWORD srclen)
{
  if (srclen > dstlen)
    return DE_INVLDFUNC;
  guest_move_block(nls_linear(dst), src, srclen);
  return SUCCESS;
}

STATIC VOID upMMem(dos_far_ptr map, nls_buf_ref str, unsigned len)
{
  unsigned i;
  uint32_t map_linear = nls_linear(map) + offsetof(struct nlsCharTbl, tbl);

  for (i = 0; i < len; ++i) {
    unsigned c = nls_buf_load8(str, i);
    if (c >= 'a' && c <= 'z')
      nls_buf_store8(str, i, (UBYTE)(c + 'A' - 'a'));
    else if (c > 0x7f)
      nls_buf_store8(str, i, pload8(map_linear + (c - 0x80)));
  }
}

STATIC int nlsGetData(dos_far_ptr nls, int subfct,
                      dos_far_ptr buf, unsigned bufsize)
{
  dos_far_ptr poi;

  if (far_is_null(buf) || !bufsize)
    return DE_INVLDDATA;

  poi = locateSubfct(nls, subfct);
  dos_far_ptr data;
  uint32_t src;

  if (far_is_null(poi))
    return DE_INVLDFUNC;

  data = nls_pointer_data(poi);
  src = nls_linear(data);

  switch (subfct)
  {
    case 1:
      return cpyGuestBuf(buf, (UWORD)bufsize, src,
                         (UWORD)(pload16(src + offsetof(struct nlsExtCntryInfo, size)) + 3));
    case NLS_DOS_38:
      return cpyGuestBuf(buf, (UWORD)bufsize,
                         src + offsetof(struct nlsExtCntryInfo, dateFmt), 24);
    default:
      return cpyGuestBuf(buf, (UWORD)bufsize, nls_linear(poi),
                         sizeof(struct nlsPointer));
  }
}

STATIC void nlsUpMem(dos_far_ptr nls, nls_buf_ref str, int len)
{
  upMMem(getTable2(nls), str, (unsigned)len);
}

STATIC void nlsFUpMem(dos_far_ptr nls, nls_buf_ref str, int len)
{
  upMMem(getTable4(nls), str, (unsigned)len);
}

static void muxUpNative(int subfct, UWORD cp, UWORD cntry,
                        VOID *str, unsigned len)
{
  unsigned i;
  for (i = 0; i < len; ++i) {
    pstore8(nls_linear(x86_nlsUpChar), ((UBYTE *)str)[i]);
    muxBufGo(subfct, 0, cp, cntry, 1, x86_nlsUpChar);
    ((UBYTE *)str)[i] = pload8(nls_linear(x86_nlsUpChar));
  }
}

STATIC VOID xUpMem(dos_far_ptr nlsp, nls_buf_ref str, unsigned len, BOOL filename)
{
  UWORD flags, cp, cntry;
  UWORD direct = filename ? NLS_FLAG_DIRECT_FUPCASE : NLS_FLAG_DIRECT_UPCASE;
  int muxf = filename ? NLSFUNC_FILE_UPMEM : NLSFUNC_UPMEM;

  if (far_is_null(nlsp) || str.kind == NLS_BUF_NONE)
    return;

  flags = nls_pkg_u16(nlsp, offsetof(struct nlsPackage, flags));
  cp = nls_pkg_u16(nlsp, offsetof(struct nlsPackage, cp));
  cntry = nls_pkg_u16(nlsp, offsetof(struct nlsPackage, cntry));

  if (flags & direct) {
    if (filename)
      nlsFUpMem(nlsp, str, len);
    else
      nlsUpMem(nlsp, str, len);
  } else if (str.kind == NLS_BUF_GUEST) {
    muxBufGo(muxf, 0, cp, cntry, (UWORD)len, str.u.guest);
  } else {
    muxUpNative(muxf, cp, cntry, str.u.native, len);
  }
}

BOOL nlsIsDBCS(UBYTE ch)
{
  dos_far_ptr act = nls_info_actpkg();
  dos_far_ptr table;
  uint32_t base;
  unsigned i, n;

  if (ch < 128 || far_is_null(act))
    return FALSE;

  table = getTable7(act);
  if (far_is_null(table))
    return FALSE;
  base = nls_linear(table);
  n = pload16(base + offsetof(struct nlsDBCS, numEntries)) / sizeof(UWORD);
  if (n > LENGTH(((struct nlsDBCS *)0)->dbcsTbl))
    n = LENGTH(((struct nlsDBCS *)0)->dbcsTbl);

  for (i = 0; i < n; ++i) {
    UWORD range = pload16(base + offsetof(struct nlsDBCS, dbcsTbl) + i * sizeof(UWORD));
    if (range == 0)
      break;
    if (ch >= (range & 0xff) && ch <= (range >> 8))
      return TRUE;
  }
  return FALSE;
}

STATIC int nlsYesNo(dos_far_ptr nlsp, UWORD ch)
{
  if (far_is_null(nlsp))
    return 2;

  if (!nlsIsDBCS(ch & 0xff)) {
    ch &= 0xff;
    pstore8(nls_linear(x86_nlsUpChar), (UBYTE)ch);
    xUpMem(nlsp, nls_guest_buf(x86_nlsUpChar), 1, FALSE);
    ch = pload8(nls_linear(x86_nlsUpChar));
  }

  if (ch == nls_pkg_u16(nlsp, offsetof(struct nlsPackage, yeschar)))
    return 1;
  if (ch == nls_pkg_u16(nlsp, offsetof(struct nlsPackage, nochar)))
    return 0;
  return 2;
}

BYTE DosYesNo(UWORD ch)
{
  dos_far_ptr act = nls_info_actpkg();
  if (!far_is_null(act) &&
      (nls_pkg_u16(act, offsetof(struct nlsPackage, flags)) & NLS_FLAG_DIRECT_YESNO))
    return (BYTE)nlsYesNo(act, ch);
  return (BYTE)muxBufGo(NLSFUNC_YESNO, 0, NLS_DEFAULT, NLS_DEFAULT,
                        ch, MK_FP(0, 0));
}

#ifndef DosUpMem
VOID DosUpMem(VOID FAR *str, unsigned len)
{
  xUpMem(nls_info_actpkg(), nls_native_buf(str), len, FALSE);
}
#endif

VOID DosUpMemGuest(dos_far_ptr str, unsigned len)
{
  xUpMem(nls_info_actpkg(), nls_guest_buf(str), len, FALSE);
}

unsigned char ASMCFUNC DosUpChar(unsigned char ch)
{
  pstore8(nls_linear(x86_nlsUpChar), ch);
  DosUpMemGuest(x86_nlsUpChar, 1);
  return pload8(nls_linear(x86_nlsUpChar));
}

VOID DosUpString(char FAR *str)
{
  DosUpMem(str, fstrlen(str));
}

VOID DosUpStringGuest(dos_far_ptr str)
{
  size_t len = guest_strnlen_block(nls_linear(str), 0x10000u);
  DosUpMemGuest(str, (unsigned)len);
}

VOID DosUpFMem(VOID FAR *str, unsigned len)
{
  xUpMem(nls_info_actpkg(), nls_native_buf(str), len, TRUE);
}

VOID DosUpFMemGuest(dos_far_ptr str, unsigned len)
{
  xUpMem(nls_info_actpkg(), nls_guest_buf(str), len, TRUE);
}

unsigned char DosUpFChar(unsigned char ch)
{
  pstore8(nls_linear(x86_nlsUpChar), ch);
  DosUpFMemGuest(x86_nlsUpChar, 1);
  return pload8(nls_linear(x86_nlsUpChar));
}

VOID DosUpFString(char FAR *str)
{
  DosUpFMem(str, fstrlen(str));
}

VOID DosUpFStringGuest(dos_far_ptr str)
{
  size_t len = guest_strnlen_block(nls_linear(str), 0x10000u);
  DosUpFMemGuest(str, (unsigned)len);
}

COUNT DosGetData(int subfct, UWORD cp, UWORD cntry, UWORD bufsize, dos_far_ptr buf)
{
  dos_far_ptr nls;

  if (far_is_null(buf) || !bufsize)
    return DE_INVLDDATA;
  if (subfct == 0)
    return DE_INVLDFUNC;

  nls = searchPackage(cp, cntry);
  if (!far_is_null(nls)) {
    UWORD flags = nls_pkg_u16(nls, offsetof(struct nlsPackage, flags));
    if (flags & NLS_FLAG_DIRECT_GETDATA)
      return nlsGetData(nls, subfct, buf, bufsize);
    cp = nls_pkg_u16(nls, offsetof(struct nlsPackage, cp));
    cntry = nls_pkg_u16(nls, offsetof(struct nlsPackage, cntry));
  }

  return (subfct == NLS_DOS_38)
       ? muxBufGo(4, 0, cp, cntry, bufsize, buf)
       : muxBufGo(2, subfct, cp, cntry, bufsize, buf);
}

COUNT DosGetCountryInformation(UWORD cntry, dos_far_ptr buf)
{
  return DosGetData(NLS_DOS_38, NLS_DEFAULT, cntry, 0x18, buf);
}

/*
 *	Call NLSFUNC to load the NLS package
 */
STATIC COUNT muxLoadPkg(int subfct, UWORD cp, UWORD cntry)
{
  long ret;

  /*          0x1400 == not installed, ok to install              */
  /*          0x1401 == not installed, not ok to install          */
  /*          0x14FF == installed                                 */

#if NLS_FREEDOS_NLSFUNC_VERSION == NLS_FREEDOS_NLSFUNC_ID
  /* make sure the NLSFUNC ID is updated */
#error "NLS_FREEDOS_NLSFUNC_VERSION == NLS_FREEDOS_NLSFUNC_ID"
#endif
  /* Install check must pass the FreeDOS NLSFUNC version as codepage (cp) and
     the FreeDOS NLSFUNC ID as buffer size (bufsize).  If they match the
     version in NLSFUNC, on return it will set BX (cp on entry) to FreeDOS
     NLSFUNC ID.  call_nls will set the high word = BX on return.
  */
  ret = muxGo(0, 0, NLS_FREEDOS_NLSFUNC_VERSION, 0, NLS_FREEDOS_NLSFUNC_ID, MK_FP(0, 0));
  if ((int16_t)ret != 0x14ff)   /* AX; the kernel's built-in MUX-14 root */
    return DE_FILENOTFND;       /* No NLSFUNC --> no load */
  if ((UWORD)(ret >> 16) != NLS_FREEDOS_NLSFUNC_ID) /* FreeDOS NLSFUNC will return */
    return DE_INVLDACC;         /* This magic number */

  /* OK, the correct NLSFUNC is available --> load pkg */
  /* If cp == -1 on entry, NLSFUNC updates cp to the codepage loaded
     into memory. The system must then change to this one later */
  return (int)muxGo(subfct, 0, cp, cntry, 0, MK_FP(0, 0));
}

VOID nlsCPchange(UWORD cp)
{
  UNREFERENCED_PARAMETER(cp);
  put_string("\7\nchange codepage not yet done ska\n");
}

/*
 *	Changes the current active codepage or cntry
 *
 *	Note: Usually any call sees a value of -1 (0xFFFF) as "the current
 *	country/CP". When a new NLS pkg is loaded, there is however a little
 *	difference, because one could mean that when switching to country XY
 *	the system may change to any codepage required.
 *	Therefore, setPackage() will substitute the current country ID, if
 *	cntry==-1, but leaves cp==-1 in order to let NLSFUNC choose the most
 *	appropriate codepage on its own.
 */
STATIC COUNT nlsSetPackage(dos_far_ptr nlsp)
{
  dos_far_ptr act = nls_info_actpkg();
  if (far_is_null(nlsp))
    return DE_INVLDDATA;
  if (!far_is_null(act) &&
      nls_pkg_u16(nlsp, offsetof(struct nlsPackage, cp)) !=
      nls_pkg_u16(act, offsetof(struct nlsPackage, cp)))
    nlsCPchange(nls_pkg_u16(nlsp, offsetof(struct nlsPackage, cp)));
  nls_info_set_actpkg(nlsp);
  return SUCCESS;
}

STATIC COUNT nlsLoadPackage(dos_far_ptr nls)
{
  nls_info_set_actpkg(nls);
  return SUCCESS;
}
STATIC COUNT DosLoadPackage(UWORD cp, UWORD cntry)
{
  dos_far_ptr nls;              /* NLS package to use to return the info from */

  /* nls := NLS package of cntry/codepage */
  if (!far_is_null(nls = searchPackage(cp, cntry)))
    /* OK the NLS pkg is loaded --> activate it */
    return nlsLoadPackage(nls);

  /* not loaded --> invoke NLSFUNC to load it */
  return muxLoadPkg(NLSFUNC_LOAD_PKG, cp, cntry);
}

/*
 *	Ported from the original kernel/nls.c for INT 21h AH=66h.
 */
STATIC COUNT DosSetPackage(UWORD cp, UWORD cntry)
{
  /* Right now, we do not have codepage change support in kernel, so push
     it through the mux in any case. (original comment; the port's
     muxLoadPkg()/mux dispatcher already handles NLSFUNC_LOAD_PKG2) */
  return muxLoadPkg(NLSFUNC_LOAD_PKG2, cp, cntry);
}

/*
 *	Called for DOS-66-01 get CP
 */
COUNT DosGetCodepage(UWORD *actCP, UWORD *sysCP)
{
  dos_far_ptr act = nls_info_actpkg();
  *sysCP = nls_info_syscp();
  *actCP = nls_pkg_u16(act, offsetof(struct nlsPackage, cp));
  return SUCCESS;
}

/*
 *	Called for DOS-66-02 set CP
 *	Note: One cannot change the system CP. Why it is necessary
 *	to specify it, is lost to me. (2000/02/13 ska)
 */
COUNT DosSetCodepage(UWORD actCP, UWORD sysCP)
{
  if (sysCP == NLS_DEFAULT || sysCP == nls_info_syscp())
    return DosSetPackage(actCP, NLS_DEFAULT);
  return DE_INVLDDATA;
}

COUNT DosSetCountry(UWORD cntry)
{
  return DosLoadPackage(NLS_DEFAULT, cntry);
}

UWORD DosGetCountry(void)
{
  dos_far_ptr act = nls_info_actpkg();
  return far_is_null(act) ? 0 : nls_pkg_u16(act, offsetof(struct nlsPackage, cntry));
}

dos_far_ptr DosGetDBCS(void)
{
  return getTable7(nls_info_actpkg());
}

/* This is the kernel's default NLSFUNC multiplex int 2F/14 (MUX-14)
   handling.  If made it here then no other program has hooked MUX-14
   and handled request -- either none loaded or choose to pass request
   on.

   Registers:
	AH == 14
	AL == subfunction
	BP == DOS-65 subfunction (GETDATA)
	BX == codepage
	CX == buffer size / character
	DX == country code
	DS:SI == internal global nlsInfo
	ES:DI == user block

	Return value: AX (SUCCESS == 0, otherwise a negative DOS error).
	Ported 1:1 from syscall_MUX14() in the original kernel/nls.c.
*/
bool fdos_nls_2fh(CPU *cpu)
{
  dos_far_ptr nlsp;             /* addressed NLS package */
  dos_far_ptr buf = FP_ES_DI;
  COUNT rc;

  log(("NLS: MUX14(): subfct=%x, cp=%u, cntry=%u\n", CPU_AL, CPU_BX, CPU_DX));

  nlsp = searchPackage(CPU_BX, CPU_DX);
  if (far_is_null(nlsp))
  {
    CPU_AX = (UWORD)DE_INVLDFUNC;   /* no such package */
    return true;
  }
  log(("NLS: MUX14(): NLS pkg found\n"));

  switch (CPU_AL)
  {
    case NLSFUNC_INSTALL_CHECK:
      /* The kernel just simulates the default functions: it is NOT an
         installed NLSFUNC, so AX stays != 14FFh and muxLoadPkg() correctly
         reports "no NLSFUNC --> no load". */
      CPU_BX = NLS_FREEDOS_NLSFUNC_ID;
      rc = SUCCESS;
      break;

    case NLSFUNC_DOS38:
      rc = nlsGetData(nlsp, NLS_DOS_38, buf, 34);
      break;

    case NLSFUNC_GETDATA:
      rc = nlsGetData(nlsp, CPU_BP, buf, CPU_CX);
      break;

    case NLSFUNC_DRDOS_GETDATA:
      /* Does not pass buffer length */
      rc = nlsGetData(nlsp, CPU_CL, buf, 512);
      break;

    case NLSFUNC_LOAD_PKG:
      rc = nlsLoadPackage(nlsp);
      break;

    case NLSFUNC_LOAD_PKG2:
      rc = nlsSetPackage(nlsp);
      break;

    case NLSFUNC_YESNO:
      rc = nlsYesNo(nlsp, CPU_CX);
      break;

    case NLSFUNC_UPMEM:
      nlsUpMem(nlsp, nls_guest_buf(buf), CPU_CX);
      rc = SUCCESS;
      break;

    case NLSFUNC_FILE_UPMEM:
      nlsFUpMem(nlsp, nls_guest_buf(buf), CPU_CX);
      rc = SUCCESS;
      break;

    default:
      log(("NLS: MUX14(): Invalid function %x\n", CPU_AL));
      rc = DE_INVLDFUNC;        /* no such function */
      break;
  }

  CPU_AX = (UWORD)rc;
  return true;
}
