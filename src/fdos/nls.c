#include <ctype.h>
#include "hdrs.h"
#include "bios/bios.h"

#define mux65(s,cp,cc,bs,b)	muxBufGo(2, (s), (cp), (cc), (bs), (b))
#define mux38(cp,cc,bs,b)	muxBufGo(4, 0, (cp), (cc), (bs), (b))
#define muxYesNo(ch)		muxBufGo(NLSFUNC_YESNO,0, NLS_DEFAULT, NLS_DEFAULT, (ch), 0)
#define muxUpMem(s,b,bs)	muxBufGo((s),0, NLS_DEFAULT,NLS_DEFAULT, (bs), (b))

#define log(x) printf x

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
STATIC int muxGo(int subfct, UWORD bp, UWORD cp, UWORD cntry, UWORD bufsize, void FAR *buf)
{
  log(("NLS: muxGo(): subfct=%x, cntry=%u, cp=%u, ES:DI=%p\n",
       subfct, cntry, cp, buf));
  ULONG ret = call_nls(bp, buf, subfct, cp, cntry, bufsize);
  int16_t r16 = (int16_t)(ret & 0xFFFF);
  if (r16 < 0) {
    log(("NLS: muxGo(): return value = %d\n", r16));
    return r16;
  }
  log(("NLS: muxGo(): return value = %p\n", ret));
  return (int)ret;
}

STATIC int muxBufGo(int subfct, int bp, UWORD cp, UWORD cntry,
                    UWORD bufsize, VOID FAR * buf)
{
  log(("NLS: muxBufGo(): subfct=%x, BP=%u, cp=%u, cntry=%u, len=%u, buf=%p\n",
       subfct, bp, cp, cntry, bufsize, buf));
  return muxGo(subfct, bp, cp, cntry, bufsize, buf);
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

BYTE DosYesNo(UWORD ch) {
    return (BYTE)muxYesNo(ch);
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
  if ((int)ret != 0x14ff)
    return DE_FILENOTFND;       /* No NLSFUNC --> no load */
  if ((int)(ret >> 16) != NLS_FREEDOS_NLSFUNC_ID) /* FreeDOS NLSFUNC will return */
    return DE_INVLDACC;         /* This magic number */

  /* OK, the correct NLSFUNC is available --> load pkg */
  /* If cp == -1 on entry, NLSFUNC updates cp to the codepage loaded
     into memory. The system must then change to this one later */
  return (int)muxGo(subfct, 0, cp, cntry, 0, 0);
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

COUNT DosSetCountry(UWORD cntry)
{
  return DosLoadPackage(NLS_DEFAULT, cntry);
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

dos_far_ptr DosGetDBCS(void)
{
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
	return getTable7(nlsInfo->actPkg);
}

/*
 * INT 2Fh / AH=14h - FreeDOS NLS MUX root handler.
 *
 * call_nls() enters here with:
 *   AL = NLSFUNC_* subfunction
 *   BP = DOS-65 subfunction / package-load selector
 *   BX = codepage
 *   DX = country
 *   CX = buffer size or character
 *   DS:SI = nlsInfoBlock
 *   ES:DI = caller buffer, when applicable
 *
 * Return convention used by nls.c:
 *   AX = DOS error/status, SUCCESS == 0
 *   BX may carry an extra word for install-check.
 */
bool fdos_nls_2fh(CPU *cpu)
{
  COUNT rc = SUCCESS;
  UWORD subfct = CPU_AL;
  UWORD cp = CPU_BX;
  UWORD cntry = CPU_DX;
  UWORD bufsize = CPU_CX;
  VOID FAR *buf = far_is_null(FP_ES_DI) ? NULL : ARM_PTR(FP_ES_DI);
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  dos_far_ptr pkgp = MK_FP(0, 0);

  switch (subfct)
  {
    case NLSFUNC_INSTALL_CHECK:
      /*
       * muxLoadPkg() expects AX=14FFh and BX=NLS_FREEDOS_NLSFUNC_ID.
       * This is the kernel's built-in MUX-14 root, not an external TSR,
       * but it is sufficient for internal NLS routing.
       */
      CPU_AX = 0x14ff;
      CPU_BX = NLS_FREEDOS_NLSFUNC_ID;
      return true;

    case NLSFUNC_LOAD_PKG:
    case NLSFUNC_LOAD_PKG2:
      /*
       * No external COUNTRY.SYS loader lives here.  We can only activate a
       * package that is already present in the in-memory NLS chain.
       */
      pkgp = searchPackage(cp, cntry);
      if (!far_is_null(pkgp))
        rc = nlsLoadPackage(pkgp);
      else
        rc = DE_FILENOTFND;
      CPU_AX = (UWORD)rc;
      return true;

    case NLSFUNC_GETDATA:
      log(("NLS 2F/1402: BP=%04x BX(cp)=%04x DX(cntry)=%04x CX(size)=%04x ES:DI=%04x:%04x\n",
           CPU_BP, cp, cntry, bufsize, CPU_ES, CPU_DI));
      pkgp = searchPackage(cp, cntry);
      log(("NLS 2F/1402: pkgp=%04x:%04x\n", FP_SEG(pkgp), FP_OFF(pkgp)));
      rc = !far_is_null(pkgp)
           ? nlsGetData((struct nlsPackage *)ARM_PTR(pkgp), CPU_BP, buf, bufsize)
           : DE_FILENOTFND;
      log(("NLS 2F/1402: rc=%d AX=%04x\n", rc, (UWORD)rc));
      CPU_AX = (UWORD)rc;
      return true;

    case NLSFUNC_DOS38:
      pkgp = searchPackage(cp, cntry);
      rc = !far_is_null(pkgp)
           ? nlsGetData((struct nlsPackage *)ARM_PTR(pkgp), NLS_DOS_38, buf, bufsize)
           : DE_FILENOTFND;
      CPU_AX = (UWORD)rc;
      return true;

    case NLSFUNC_DRDOS_GETDATA:
      /*
       * DR-DOS compatible alias: same register contract as GETDATA in this
       * port.  Keep it handled so callers do not fall into no_handler().
       */
      pkgp = searchPackage(cp, cntry);
      rc = !far_is_null(pkgp)
           ? nlsGetData((struct nlsPackage *)ARM_PTR(pkgp), CPU_BP, buf, bufsize)
           : DE_FILENOTFND;
      CPU_AX = (UWORD)rc;
      return true;

    case NLSFUNC_YESNO: {
      /*
       * CX carries the character in muxYesNo().  Return 1 for yes,
       * 0 for no, DE_INVLDFUNC for neither.
       */
      struct nlsPackage *pkg = (struct nlsPackage *)ARM_PTR(nlsInfo->actPkg);
      if (toupper((unsigned char)bufsize) == toupper((unsigned char)pkg->yeschar))
        CPU_AX = 1;
      else if (toupper((unsigned char)bufsize) == toupper((unsigned char)pkg->nochar))
        CPU_AX = 0;
      else
        CPU_AX = (UWORD)DE_INVLDFUNC;
      return true;
    }
    case NLSFUNC_UPMEM:
    case NLSFUNC_FILE_UPMEM:
      /*
       * Minimal safe built-in service: CP437 hardcoded tables currently map
       * ASCII correctly, and non-ASCII remains byte-preserving unless the
       * direct table path is used elsewhere.
       */
      if (buf == NULL)
        rc = DE_INVLDDATA;
      else
      {
        UBYTE *p = (UBYTE *)buf;
        while (bufsize--)
        {
          if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
          p++;
        }
      }
      CPU_AX = (UWORD)rc;
      return true;

    default:
      CPU_AX = (UWORD)DE_INVLDFUNC;
      return true;
  }
}
