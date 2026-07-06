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
STATIC long muxGo(int subfct, UWORD bp, UWORD cp, UWORD cntry, UWORD bufsize,
		  void FAR *buf)
{
  long ret;
  log(("NLS: muxGo(): subfct=%x, cntry=%u, cp=%u, ES:DI=%p\n",
       subfct, cntry, cp, buf));
  ret = call_nls(bp, buf, subfct, cp, cntry, bufsize);
  log(("NLS: muxGo(): return value = %lx\n", ret));
  return ret;
}

STATIC int muxBufGo(int subfct, int bp, UWORD cp, UWORD cntry,
                    UWORD bufsize, VOID FAR * buf)
{
  log(("NLS: muxBufGo(): subfct=%x, BP=%u, cp=%u, cntry=%u, len=%u, buf=%p\n",
       subfct, bp, cp, cntry, bufsize, buf));

  return (int)muxGo(subfct, bp, cp, cntry, bufsize, buf);
}

/*
 *	Search for the NLS package within the chain
 *	Also resolves the default values (-1) into the currently
 *	active codepage/country code.
 */
STATIC struct nlsPackage* searchPackage(UWORD cp, UWORD cntry)
{
  struct nlsPackage FAR *nls;
  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  log(("searchPackage(%u, %u) nlsInfo: %p\n", cp, cntry, nlsInfo));

  if (nlsInfo->actPkg == NULL || nlsInfo->chain == NULL) {
    log(("ERR: nlsInfo not proper initiased\n"));
    return NULL;
  }

  if (cp == NLS_DEFAULT) {
    cp = nlsInfo->actPkg->cp;
    log(("cp: %u\n", cp));
  }
  if (cntry == NLS_DEFAULT) {
    cntry = nlsInfo->actPkg->cntry;
    log(("cntry: %u\n", cntry));
  }

  for (nls = nlsInfo->chain; nls != NULL; nls = nls->nxt) {
    if (nls->cp == cp && nls->cntry == cntry)
        break;
  }
  log(("nls: %p\n", nls));
  return nls;
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
  VOID FAR *poi;

  log(("NLS: nlsGetData(): subfct=%x, bufsize=%u, cp=%u, cntry=%u\n",
       subfct, bufsize, nls->cp, nls->cntry));

  /* Theoretically tables 1 and, if NLS_REORDER_POINTERS is enabled,
     2 and 4 could be hard-coded, because their
     data is located at predictable (calculatable) locations.
     However, 1 and subfct NLS_DOS_38 are to handle the same
     data and the "locateSubfct()" call has to be implemented anyway,
     in order to handle all subfunctions.
     Also, NLS is often NOT used in any case, so this code is more
     size than speed optimized. */
  if ((poi = locateSubfct(nls, subfct)) != NULL)
  {
    log(("NLS: nlsGetData(): subfunction found\n"));
    switch (subfct)
    {
      case 1:                  /* Extended Country Information */
        return cpyBuf(buf, bufsize, poi,
                      ((struct nlsExtCntryInfo FAR *)poi)->size + 3);
      case NLS_DOS_38:         /* Normal Country Information */
        return cpyBuf(buf, bufsize, &(((struct nlsExtCntryInfo FAR *)poi)->dateFmt), 24);       /* standard cinfo has no more 34 _used_ bytes */
        /* don't copy 34, copy only 0x18 instead, 
           see comment at DosGetCountryInformation                      TE */
      default:
        /* All other subfunctions just return the found nlsPoinerInf
           structure */
        return cpyBuf(buf, bufsize, poi, sizeof(struct nlsPointer));
    }
  }

  /* The requested subfunction could not been located within the
     NLS pkg --> error. Because the data corresponds to the subfunction
     number passed to the API, the failure is the same as that a wrong
     API function has been called. */
  log(("NLS: nlsGetData(): Subfunction not found\n"));
  return DE_INVLDFUNC;
}

/*
 *	Called for all subfunctions other than 0x20-0x23,& 0xA0-0xA2
 *	of DOS-65
 *
 *	If the requested NLS pkg specified via cntry and cp is _not_
 *	loaded, MUX-14 is invoked; otherwise the pkg's NLS_Fct_buf
 *	function is invoked.
 */
COUNT DosGetData(int subfct, UWORD cp, UWORD cntry, UWORD bufsize,
                 VOID FAR * buf)
{
  struct nlsPackage FAR *nls;   /* NLS package to use to return the info from */

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
  if ((nls = searchPackage(cp, cntry)) != NULL)
  {
    /* matching NLS package found */
    if (nls->flags & NLS_FLAG_DIRECT_GETDATA)
      /* Direct access to the data */
      return nlsGetData(nls, subfct, buf, bufsize);
    cp = nls->cp;
    cntry = nls->cntry;
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

STATIC COUNT nlsLoadPackage(struct nlsPackage FAR * nls)
{

  struct nlsInfoBlock *nlsInfo = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
  nlsInfo->actPkg = nls;
  return SUCCESS;
}
STATIC COUNT DosLoadPackage(UWORD cp, UWORD cntry)
{
  struct nlsPackage FAR *nls;   /* NLS package to use to return the info from */

  /* nls := NLS package of cntry/codepage */
  if ((nls = searchPackage(cp, cntry)) != NULL)
    /* OK the NLS pkg is loaded --> activate it */
    return nlsLoadPackage(nls);

  /* not loaded --> invoke NLSFUNC to load it */
  return muxLoadPkg(NLSFUNC_LOAD_PKG, cp, cntry);
}

COUNT DosSetCountry(UWORD cntry)
{
  return DosLoadPackage(NLS_DEFAULT, cntry);
}
