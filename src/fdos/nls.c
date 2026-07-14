#include <ctype.h>
#include "hdrs.h"
#include "bios/bios.h"

#define mux65(s,cp,cc,bs,b)	muxBufGo(2, (s), (cp), (cc), (bs), (b))
#define mux38(cp,cc,bs,b)	muxBufGo(4, 0, (cp), (cc), (bs), (b))
#define muxYesNo(ch)		muxBufGo(NLSFUNC_YESNO,0, NLS_DEFAULT, NLS_DEFAULT, (ch), 0)
#define muxUpMem(s,b,bs)	muxBufGo((s),0, NLS_DEFAULT,NLS_DEFAULT, (bs), (b))

#define log(x)
/// printf x

ULONG call_nls(UWORD bp,
               UWORD FAR *buf,
               UWORD subfct,
               UWORD cp,
               UWORD cntry,
               UWORD bufsize)
{
    dos_far_ptr x86_ptr = x86_nlsInfo;
    CPU_regs regs;
    cpu_save_regs(cpu, &regs);
    SET_DS ( FP_SEG (x86_ptr) );
    CPU_SI = FP_OFF (x86_ptr);
    if (buf) {
        /*
           This is the one deliberate native->guest recovery left in the tree,
           and it is legitimate rather than a shortcut. Every path that reaches
           call_nls():

             - is taken ONLY when an external NLSFUNC has cleared a package's
               NLS_FLAG_DIRECT_* bits. The built-in package is created with
               NLS_FLAG_HARDCODED (all DIRECT bits set, see kernel.c), so every
               upcase/yesno/getdata request is served natively and never comes
               here at all;
             - passes a buf that ALWAYS originates as ARM_PTR() of a guest
               register: ARM_PTR(R_FP_DS_DX) (DosUpMem/DosUpFMem/
               DosGetCountryInformation via INT 21h) or ARM_PTR(FP_ES_DI)
               (fdos_nls_2fh). The genuinely-native callers (kernel.c boot-time
               upcase, an SFT name) all carry a DIRECT flag and take the native
               branch, so they cannot reach this line.

           So buf is always a guest-window address here and linear_to_far()
           recovers its unique linear location - exactly what the external
           driver needs in ES:DI. Threading a dos_far_ptr through the whole
           muxBufGo()/muxGo()/call_nls() spine was considered and rejected:
           several intermediate callers hold only native pointers, so it would
           just move this same recovery to each of them (more sites, more risk)
           without removing it. Guarded with is_guest_ptr() so a future
           out-of-window caller fails loudly instead of silently. */
        if (!is_guest_ptr(buf)) {
            cpu_restore_regs(cpu, &regs);   /* unreachable per analysis; refuse
                                               rather than pass a bogus ES:DI */
            return 0;
        }
        x86_ptr = linear_to_far(buf);
        SET_ES ( FP_SEG (x86_ptr) );
        CPU_DI = FP_OFF (x86_ptr);
    } else {
        SET_ES ( 0 );
        CPU_DI = 0;
    }
    CPU_BP = bp;
    CPU_CX = bufsize;
    CPU_DX = cntry;
    CPU_BX = cp;
    CPU_AX = (0x14u << 8) | (subfct & 0x00ff);
    bios_intcall(cpu, 0x2F, "NLS 2F");
    ULONG res =((ULONG)CPU_BX << 16) | CPU_AX;
    cpu_restore_regs(cpu, &regs);
    return res;
}
/*== DS:SI _always_ points to global NLS info structure <-> no
 * subfct can use these registers for anything different. ==ska*/
STATIC long muxGo(int subfct, UWORD bp, UWORD cp, UWORD cntry, UWORD bufsize,
		  void FAR *buf)
{
  long ret;
  log(("NLS: muxGo(): subfct=%x, cntry=%u, cp=%u, ES:DI=%p\n",
       subfct, cntry, cp, buf));
  ret = (long)call_nls(bp, buf, subfct, cp, cntry, bufsize);
  log(("NLS: muxGo(): return value = %lx\n", ret));
  return ret;
}

STATIC int muxBufGo(int subfct, int bp, UWORD cp, UWORD cntry,
                    UWORD bufsize, VOID FAR * buf)
{
  log(("NLS: muxBufGo(): subfct=%x, BP=%u, cp=%u, cntry=%u, len=%u, buf=%p\n",
       subfct, bp, cp, cntry, bufsize, buf));

  /* The original is a 16-bit kernel, where "(int)" of the long returned by
     call_nls() keeps AX only - the high word carries BX and is used by the
     install check in muxLoadPkg() alone.  On ARM "int" is 32 bit, so the
     truncation has to be explicit; without it every MUX result came back as
     (BX << 16) | AX, i.e. non-zero == error even on success. */
  return (int)(int16_t)muxGo(subfct, bp, cp, cntry, bufsize, buf);
}

/*
 *	Search for the NLS package within the chain
 *	Also resolves the default values (-1) into the currently
 *	active codepage/country code.
 */

static inline dos_far_ptr nls_null_ptr(void) {
  return MK_FP(0, 0);
}
static inline struct nlsPackage *nls_pkg_ptr(dos_far_ptr p) {
  return far_is_null(p) ? NULL : (struct nlsPackage *)ARM_PTR(p);
}

        /* getTableX return the pointer to the X'th table; X==subfct */
        /* subfct 2: normal upcase table; 4: filename upcase table */
#ifdef NLS_REORDER_POINTERS
#define getTable2(nls)	(((struct nlsPackage *)ARM_PTR(nls))->nlsPointers[0].pointer)
#define getTable4(nls)	(((struct nlsPackage *)ARM_PTR(nls))->nlsPointers[1].pointer)
#define getTable7(nls)	(((struct nlsPackage *)ARM_PTR(nls))->nlsPointers[4].pointer)
#else
#define getTable2(nls)	getTable(2, (nls))
#define getTable4(nls)	getTable(4, (nls))
#define getTable7(nls)	getTable(7, (nls))
#define NEED_GET_TABLE
#endif
        /*== both chartables must be 128 bytes long and lower range is
		identical to 7bit-US-ASCII ==ska*/
#define getCharTbl2(nls)			\
	 (((struct nlsCharTbl *)ARM_PTR(getTable2(nls)))->tbl - 0x80)
#define getCharTbl4(nls)			\
	 (((struct nlsCharTbl *)ARM_PTR(getTable4(nls)))->tbl - 0x80)

STATIC dos_far_ptr/*struct nlsPackage*/ searchPackage(UWORD cp, UWORD cntry)
{
  dos_far_ptr p;
  struct nlsPackage FAR *nls;
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  log(("searchPackage(%u, %u) nlsInfo: %p\n", cp, cntry, nlsInfo));

  if (far_is_null(nlsInfo->actPkg) || far_is_null(nlsInfo->chain)) {
    log(("ERR: nlsInfo not properly initialised\n"));
    return MK_FP(0, 0);
  }

  nls = nls_pkg_ptr(nlsInfo->actPkg);
  if (cp == NLS_DEFAULT) {
    cp = nls->cp;
    log(("cp: %u\n", cp));
  }
  if (cntry == NLS_DEFAULT) {
    cntry = nls->cntry;
    log(("cntry: %u\n", cntry));
  }

  for (p = nlsInfo->chain; !far_is_null(p); )
  {
    struct nlsPackage FAR *nls = (struct nlsPackage FAR *)ARM_PTR(p);
    if (nls->cp == cp && nls->cntry == cntry)
      return p;
    p = nls->nxt;
  }  
  return MK_FP(0, 0);
}

/* For various robustnesses reasons and to simplify the implementation
	at other places, locateSubfct() returns NULL (== "not found"),
	if nls == NULL on entry. */
STATIC VOID FAR *locateSubfct(struct nlsPackage FAR * nls, int subfct)
{
  int cnt;
  struct nlsPointer FAR *p;

  if (nls)
    for (cnt = nls->numSubfct, p = &nls->nlsPointers[0]; cnt--; ++p)
      if (p->subfct == (UBYTE) subfct)
        return p;

  log(("NLS: locateSubfct(): not found subfct=%x nls=%p\n", subfct, nls));
  if (nls) {
    int i;
    log(("NLS: locateSubfct(): pkg cp=%u cntry=%u numSubfct=%u\n",
         nls->cp, nls->cntry, nls->numSubfct));
    for (i = 0, p = &nls->nlsPointers[0]; i < nls->numSubfct; i++, p++)
      log(("NLS: locateSubfct(): ptr[%d] subfct=%u ptr=%04x:%04x\n",
           i, p->subfct, FP_SEG(p->pointer), FP_OFF(p->pointer)));
  }

  return NULL;
}

/*
 *	Copy a buffer and test the size of the buffer
 *	Returns SUCCESS on success; DE_INVLDFUNC on failure
 *
 *	Efficiency note: This function is used as:
 *		return cpyBuf(buf, bufsize, ...)
 *	three times. If the code optimizer is some good, it can re-use
 *	the code to push bufsize, buf, call cpyBuf() and return its result.
 *	The parameter were ordered to allow this code optimization.
 */
STATIC COUNT cpyBuf(VOID FAR * dst, UWORD dstlen, VOID FAR * src,
                    UWORD srclen)
{
  if (srclen <= dstlen)
  {
    memcpy(dst, src, srclen);
    return SUCCESS;
  }
  return DE_INVLDFUNC;          /* buffer too small */
}

/*
 *	This function assumes that 'map' is adjusted such that
 *	map[0x80] is the uppercase of character 0x80.
 *== 128 byte chartables, lower range conform to 7bit-US-ASCII ==ska*/
STATIC VOID upMMem(UBYTE FAR * map, UBYTE FAR * str, unsigned len)
{
  REG unsigned c;

  if (len)
    do
    {
      if ((c = *str) >= 'a' && c <= 'z')
        *str += 'A' - 'a';
      else if (c > 0x7f)
        *str = map[c];
      ++str;
    }
    while (--len);
}


/* GetData function used by both the MUX-callback function and
	the direct-access interface.
	subfct == NLS_DOS_38 is a value > 0xff in order to not clash
	with subfunctions valid to be passed as DOS-65-XX. */
STATIC int nlsGetData(struct nlsPackage FAR * nls, int subfct,
                      UBYTE FAR * buf, unsigned bufsize)
{
  struct nlsPointer FAR *poi;
  VOID FAR *data;

  log(("NLS: nlsGetData(): subfct=%x, bufsize=%u, cp=%u, cntry=%u\n",
       subfct, bufsize, nls->cp, nls->cntry));

  if ((poi = locateSubfct(nls, subfct)) != NULL)
  {
    data = ARM_PTR(poi->pointer);
    log(("NLS: nlsGetData(): subfunction found\n"));
    switch (subfct)
    {
      case 1:                  /* Extended Country Information */
        return cpyBuf(buf, bufsize, data,
                      ((struct nlsExtCntryInfo FAR *)data)->size + 3);
      case NLS_DOS_38:         /* Normal Country Information */
        return cpyBuf(buf, bufsize, &(((struct nlsExtCntryInfo FAR *)data)->dateFmt), 24);       /* standard cinfo has no more 34 _used_ bytes */
        /* don't copy 34, copy only 0x18 instead, 
           see comment at DosGetCountryInformation                      TE */
      default:
        /* All other subfunctions return the guest-visible nlsPointer. */
        return cpyBuf(buf, bufsize, poi, sizeof(struct nlsPointer));
    }
  }

  log(("NLS: nlsGetData(): Subfunction not found\n"));
  return DE_INVLDFUNC;
}

STATIC void nlsUpMem(dos_far_ptr nls, VOID FAR * str, int len)
{
  log(("NLS: nlsUpMem()\n"));
  upMMem(getCharTbl2(nls), (UBYTE FAR *) str, len);
}
STATIC void nlsFUpMem(dos_far_ptr nls, VOID FAR * str, int len)
{
  log(("NLS: nlsFUpMem()\n"));
  upMMem(getCharTbl4(nls), (UBYTE FAR *) str, len);
}

STATIC VOID xUpMem(dos_far_ptr nlsp, VOID FAR * str, unsigned len)
/* upcase a memory area */
{
  struct nlsPackage *nls = nls_pkg_ptr(nlsp);

  if (nls == NULL)
    return;

  log(("NLS: xUpMem(): cp=%u, cntry=%u\n", nls->cp, nls->cntry));

  if (nls->flags & NLS_FLAG_DIRECT_UPCASE)
    nlsUpMem(nlsp, str, len);
  else
    muxBufGo(NLSFUNC_UPMEM, 0, nls->cp, nls->cntry, len, str);
}

BOOL nlsIsDBCS(UBYTE ch)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);

  if (ch < 128)
    return FALSE;              /* No leadbyte is smaller than that */

  if (far_is_null(nlsInfo->actPkg))
    return FALSE;

  {
    /* Same class of problem as upMMem(): the original walks dbcsTbl[] until a
       zero word, trusting the table to be zero-terminated. dbcsTbl is a fixed
       4-word slot here (struct nlsDBCS), and a COUNTRY.SYS DBCS table that
       fills all four words carries no terminator - the loop would then walk
       straight into the NLS scratch area behind the table. Bound it by
       numEntries (the table length in bytes) as well. */
    struct nlsDBCS FAR *dbcs =
        (struct nlsDBCS FAR *)ARM_PTR(getTable7(nlsInfo->actPkg));
    UWORD FAR *t = dbcs->dbcsTbl;
    unsigned n = dbcs->numEntries / sizeof(UWORD);

    if (n > LENGTH(dbcs->dbcsTbl))
      n = LENGTH(dbcs->dbcsTbl);

    for (; n != 0 && *t != 0; --n, ++t)
      if (ch >= (*t & 0xFF) && ch <= (*t >> 8))
        return TRUE;
  }

  return FALSE;
}

STATIC int nlsYesNo(dos_far_ptr nlsp, UWORD ch)
{
  struct nlsPackage *nls = nls_pkg_ptr(nlsp);

  if (nls == NULL)
    return 2;

  /* Check if it is a dual byte character */
  if (!nlsIsDBCS(ch & 0xFF)) {
    UBYTE *guest_ch = (UBYTE *)ARM_PTR(x86_nlsUpChar);

    ch &= 0xFF;
    /* Upcase character. Cannot use DosUpChar(), because
       maybe: nls != current NLS pkg
       However: Upcase character within lowlevel
       function to allow a yesNo() function
       catched by external MUX-14 handler, which
       does NOT upcase character. */
    *guest_ch = (UBYTE)ch;
    xUpMem(nlsp, guest_ch, 1);
    ch = *guest_ch;
  }

  if (ch == nls->yeschar)
    return 1;
  if (ch == nls->nochar)
    return 0;
  return 2;
}

/********************************************************************
 ***** DOS API ******************************************************
 ********************************************************************/

BYTE DosYesNo(UWORD ch)
/* returns: 0: ch == "No", 1: ch == "Yes", 2: ch crap */
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  struct nlsPackage *act = nls_pkg_ptr(nlsInfo->actPkg);

  if (act != NULL && (act->flags & NLS_FLAG_DIRECT_YESNO))
    return (BYTE)nlsYesNo(nlsInfo->actPkg, ch);
  else
    return (BYTE)muxYesNo(ch);
}

#ifndef DosUpMem
VOID DosUpMem(VOID FAR * str, unsigned len)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  xUpMem(nlsInfo->actPkg, str, len);
}
#endif

/*
 * This function is also called by the backdoor entry specified by
 * the "upCaseFct" member of the Country Information structure. Therefore
 * the HiByte of the first argument must remain unchanged.
 *	See NLSSUPT.ASM -- 2000/03/30 ska
 */
unsigned char ASMCFUNC DosUpChar(unsigned char ch)
 /* upcase a single character */
{
  UBYTE *guest_ch = (UBYTE *)ARM_PTR(x86_nlsUpChar);

  *guest_ch = ch;
  DosUpMem(guest_ch, 1);
  return *guest_ch;
}

VOID DosUpString(char FAR * str)
/* upcase a string */
{
  DosUpMem(str, fstrlen(str));
}

VOID DosUpFMem(VOID FAR * str, unsigned len)
/* upcase a memory area for file names */
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  struct nlsPackage *act = nls_pkg_ptr(nlsInfo->actPkg);

  if (act != NULL && (act->flags & NLS_FLAG_DIRECT_FUPCASE))
    nlsFUpMem(nlsInfo->actPkg, str, len);
  else
    muxUpMem(NLSFUNC_FILE_UPMEM, str, len);
}

unsigned char DosUpFChar(unsigned char ch)
 /* upcase a single character for file names */
{
  UBYTE *guest_ch = (UBYTE *)ARM_PTR(x86_nlsUpChar);

  *guest_ch = ch;
  DosUpFMem(guest_ch, 1);
  return *guest_ch;
}

VOID DosUpFString(char FAR * str)
/* upcase a string for file names */
{
  DosUpFMem(str, fstrlen(str));
}
/*
 *	Called for all subfunctions other than 0x20-0x23,& 0xA0-0xA2
 *	of DOS-65
 *
 *	If the requested NLS pkg specified via cntry and cp is _not_
 *	loaded, MUX-14 is invoked; otherwise the pkg's NLS_Fct_buf
 *	function is invoked.
 */
COUNT DosGetData(int subfct, UWORD cp, UWORD cntry, UWORD bufsize, VOID FAR * buf)
{
  dos_far_ptr nls;              /* NLS package to use to return the info from */

  log(("NLS: GetData(): subfct=%x, cp=%u, cntry=%u, bufsize=%u\n", subfct, cp, cntry, bufsize));

  if (!buf || !bufsize) {
    log(("!buf || !bufsize\n"));
    return DE_INVLDDATA;
  }
  if (subfct == 0) {              /* Currently not supported */
    log(("subfct == 0\n"));
    return DE_INVLDFUNC;
  }

  /* nls := NLS package of cntry/codepage */
  if (!far_is_null(nls = searchPackage(cp, cntry)))
  {
    struct nlsPackage FAR *pkg = (struct nlsPackage FAR *)ARM_PTR(nls);
    /* matching NLS package found */
    if (pkg->flags & NLS_FLAG_DIRECT_GETDATA)
      /* Direct access to the data */
      return nlsGetData(pkg, subfct, buf, bufsize);
    cp = pkg->cp;
    cntry = pkg->cntry;
  }

  /* If the NLS pkg is not loaded into memory or the direct-access
     flag is disabled, the request must be passed through MUX */
  return (subfct == NLS_DOS_38)
        ? mux38(cp, cntry, bufsize, buf)
        : mux65(subfct, cp, cntry, bufsize, buf);
}

COUNT DosGetCountryInformation(UWORD cntry, VOID FAR * buf)
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
  ret = muxGo(0, 0, NLS_FREEDOS_NLSFUNC_VERSION, 0, NLS_FREEDOS_NLSFUNC_ID, 0);
  if ((int16_t)ret != 0x14ff)   /* AX; the kernel's built-in MUX-14 root */
    return DE_FILENOTFND;       /* No NLSFUNC --> no load */
  if ((UWORD)(ret >> 16) != NLS_FREEDOS_NLSFUNC_ID) /* FreeDOS NLSFUNC will return */
    return DE_INVLDACC;         /* This magic number */

  /* OK, the correct NLSFUNC is available --> load pkg */
  /* If cp == -1 on entry, NLSFUNC updates cp to the codepage loaded
     into memory. The system must then change to this one later */
  return (int)muxGo(subfct, 0, cp, cntry, 0, 0);
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
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  struct nlsPackage *nls = nls_pkg_ptr(nlsp);
  struct nlsPackage *act = nls_pkg_ptr(nlsInfo->actPkg);

  if (nls == NULL)
    return DE_INVLDDATA;

  if (act != NULL && nls->cp != act->cp)
    /* Codepage gets changed --> inform all character drivers thereabout.
       If this fails, it would be possible that the old NLS pkg had been
       removed from memory by NLSFUNC. */
    nlsCPchange(nls->cp);

  nlsInfo->actPkg = nlsp;

  return SUCCESS;
}

STATIC COUNT nlsLoadPackage(dos_far_ptr nls)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  nlsInfo->actPkg = nls;
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
COUNT DosGetCodepage(UWORD * actCP, UWORD * sysCP)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  *sysCP = nlsInfo->sysCodePage;
  *actCP = nls_pkg_ptr(nlsInfo->actPkg)->cp;
  return SUCCESS;
}

/*
 *	Called for DOS-66-02 set CP
 *	Note: One cannot change the system CP. Why it is necessary
 *	to specify it, is lost to me. (2000/02/13 ska)
 */
COUNT DosSetCodepage(UWORD actCP, UWORD sysCP)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  if (sysCP == NLS_DEFAULT || sysCP == nlsInfo->sysCodePage)
    return DosSetPackage(actCP, NLS_DEFAULT);
  return DE_INVLDDATA;
}

COUNT DosSetCountry(UWORD cntry)
{
  return DosLoadPackage(NLS_DEFAULT, cntry);
}

dos_far_ptr DosGetDBCS(void)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
	return getTable7(nlsInfo->actPkg);
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
  struct nlsPackage *nls;
  VOID FAR *buf = far_is_null(FP_ES_DI) ? NULL : ARM_PTR(FP_ES_DI);
  COUNT rc;

  log(("NLS: MUX14(): subfct=%x, cp=%u, cntry=%u\n", CPU_AL, CPU_BX, CPU_DX));

  nlsp = searchPackage(CPU_BX, CPU_DX);
  if (far_is_null(nlsp))
  {
    CPU_AX = (UWORD)DE_INVLDFUNC;   /* no such package */
    return true;
  }
  nls = nls_pkg_ptr(nlsp);

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
      rc = nlsGetData(nls, NLS_DOS_38, buf, 34);
      break;

    case NLSFUNC_GETDATA:
      rc = nlsGetData(nls, CPU_BP, buf, CPU_CX);
      break;

    case NLSFUNC_DRDOS_GETDATA:
      /* Does not pass buffer length */
      rc = nlsGetData(nls, CPU_CL, buf, 512);
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
      nlsUpMem(nlsp, buf, CPU_CX);
      rc = SUCCESS;
      break;

    case NLSFUNC_FILE_UPMEM:
      nlsFUpMem(nlsp, buf, CPU_CX);
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
