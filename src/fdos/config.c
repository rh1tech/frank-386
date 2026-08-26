#include <ctype.h>

#define new fdos_new
#define strchr fdos_strchr_compat
#define _Static_assert static_assert
extern "C" {
#include "bios/bios.h"
#include "hdrs.h"
}
#undef _Static_assert
#undef strchr
#undef new
#ifdef load
#undef load
#endif
#include "guest_ref.hpp"

using fdos_guest::lol_ref;
using fdos_guest::dos_data_ref;
static constexpr uint32_t config_fixed_data_linear = ((uint32_t)DOS_PSP << 4) + 0x08F0u;
static constexpr uint32_t config_internal_data_linear = ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF;
static const lol_ref config_lol(config_fixed_data_linear);
static const dos_data_ref config_idata(config_internal_data_linear);

#define HMA_NONE 0              /* do nothing */
#define HMA_REQ 1               /* DOS = HIGH detected */
#define HMA_DONE 2              /* Moved kernel to HMA */
#define HMA_LOW 3               /* Definitely LOW */

#define EOF 0x1a

#define szBuf ((BYTE *)ARM_PTR(x86_SZ_BUF))
STATIC unsigned nCfgLine BSS_INIT(0);
static UBYTE ErrorAlreadyPrinted[128] BSS_INIT({0});
BYTE *pLineStart BSS_INIT(0);
STATIC seg base_seg BSS_INIT(0);
static COUNT UmbState BSS_INIT(0);
STATIC seg umb_base_seg BSS_INIT(0);
UWORD umb_start BSS_INIT(0), UMB_top BSS_INIT(0);
static BYTE HMAState BSS_INIT(0);
static COUNT nFileDesc BSS_INIT(0);
#define MAX_CHAINS 5
struct CfgFile {
  COUNT nFileDesc;
  COUNT nCfgLine;
} cfgFile[MAX_CHAINS] BSS_INIT({0});

static COUNT nCurChain BSS_INIT(0);
STATIC BOOL askThisSingleCommand BSS_INIT(0);
STATIC BOOL DontAskThisSingleCommand BSS_INIT(0);
STATIC unsigned MenuLine BSS_INIT(0);
STATIC unsigned Menus BSS_INIT(0);
STATIC dos_far_ptr x86_stackBase BSS_INIT({0});
STATIC COUNT nStacks BSS_INIT(0);
STATIC COUNT stackSize BSS_INIT(0);
STATIC int MenuColor = -1;
STATIC COUNT MenuTimeout = -1;
STATIC BYTE  MenuSelected BSS_INIT(0);
BYTE singleStep BSS_INIT(FALSE);        /* F8 processing */
BYTE SkipAllConfig BSS_INIT(FALSE);     /* F5 processing */
BYTE ASM ReturnAnyDosVersionExpected = 0; // originated from?
/**
  Menu selection bar struct:
  x pos, ypos, string
*/
#define MENULINEMAX 80
#define MENULINESMAX 10
struct MenuSelector
{
  int x;
  int y;
  BYTE bSelected;
  BYTE Text[MENULINEMAX];
};

/** Structure below holds the menu-strings */
/* CONFIG.SYS-only menu table. It is the sole live user of
   CORE0_STACK_EXT after InstallCommands is kept in normal SRAM. main() copies
   .stack_ext_area from its FLASH image at startup; after init_kernel() returns
   this table is dead and the whole 4 KiB region may be reclaimed. */
STATIC struct MenuSelector MenuStruct[MENULINESMAX]
    __attribute__((section(".core0_stack_ext"))) BSS_INIT({0});
STATIC int nMenuLine BSS_INIT(0);

/*struct buffer*/dos_far_ptr x86_firstAvailableBuf;

extern const dos_far_ptr x86_szLine;
const size_t szLine_len = 256;

struct config Config = { 
  0,
  NUMBUFF,
  NFILES,
  0,
  NFCBS,
  0,
  "command.com",
  " /P /E:256\r\n",
  NLAST,
  0,
  NSTACKS,
  0,
  STACKSIZE
  /* COUNTRY= is initialized within DoConfig() */
  , 0                       /* strategy for command.com is low by default */
  , 0                       /* default value for switches=/E:nnnn */
};

nlsCountryInfoHardcoded_t nlsCountryInfoHardcoded = {
  1,
  0x001c,
  {
    1,                  /* CountryID */
    437,                /* CodePage */
    0,                  /* DateFormat */
    "$",                /* CurrencyString */
    ",",                /* ThousandSeparator */
    ".",                /* DecimalPoint */
    "-",                /* DateSeparator */
    ":",                /* TimeSeparator */
    0,                  /* CurrencyFormat */
    2,                  /* CurrencyPrecision */
    0,                  /* TimeFormat */
    CharMapSrvc,
    ","                 /* DataSeparator */
  }
};

#ifdef DEBUG
#define InstallPrintf(x) printf x
#else
#define InstallPrintf(x)
#endif

#define MAX_INSTALL_CMDS 10
struct instCmds {
  char buffer[128];
  int mode;
};
static int numInstallCmds;
/*
 * INSTALL= state has runtime lifetime: INT 2F/AX=AE01 can call DoInstall()
 * after normal boot.  Keep it in ordinary SRAM; CORE0_STACK_EXT is reserved
 * only for data whose lifetime really ends with CONFIG.SYS processing.
 */
static struct instCmds InstallCommands[MAX_INSTALL_CMDS];

STATIC void config_init_buffers_ex(int wantedbuffers, int allow_hma)
{
  unsigned buffers = 0;

  /* fill HMA with buffers if BUFFERS count >=0 and DOS in HMA        */
  if (wantedbuffers < 0)
    wantedbuffers = -wantedbuffers;
  else if (allow_hma && HMAState == HMA_DONE)
    buffers = (0xfff0 - HMAFree) / sizeof(struct buffer);

  if (wantedbuffers < 6)         /* min 6 buffers                     */
    wantedbuffers = 6;
  if (wantedbuffers > 99)        /* max 99 buffers                    */
  {
    printf("BUFFERS=%u not supported, reducing to 99\n", wantedbuffers);
    wantedbuffers = 99;
  }
  if (wantedbuffers > buffers)   /* more specified than available -> get em */
    buffers = wantedbuffers;

  config_lol.nbuffers() = buffers;
  config_lol.inforecptr(config_lol.firstbuf());
  struct buffer *pbuffer;
  dos_far_ptr x86_buffer;
  {
    size_t bytes = sizeof(struct buffer) * buffers;
    x86_buffer = allow_hma ? HMAalloc(bytes) : MK_FP(0, 0);

    if (!FP_SEG(x86_buffer) && !FP_OFF(x86_buffer))
    {
      x86_buffer = KernelAlloc(bytes, 'B', 0);
      if (HMAState == HMA_DONE)
        x86_firstAvailableBuf = MK_FP(0xffff, HMAFree);
    }
    else
    {
      config_lol.bufloc() = LOC_HMA;
      /* space in HMA beyond requested buffers available as user space */
      x86_firstAvailableBuf = MK_FP(FP_SEG(x86_buffer), FP_OFF(x86_buffer) + wantedbuffers * sizeof(struct buffer));
    }
    pbuffer = (struct buffer*)ARM_PTR(x86_buffer);
  }
  config_lol.deblock_buf(DiskTransferBuffer);
  config_lol.firstbuf(x86_buffer);

  CfgDbgPrintf(("init_buffers (size %u) at", sizeof(struct buffer)));
  CfgDbgPrintf((" (%p)", config_lol.firstbuf()));

  buffers--;
  UWORD base_off = FP_OFF(x86_buffer);
  {
    unsigned last = buffers;
    unsigned idx;
    for (idx = 0; idx <= last; idx++)
    {
      pbuffer->b_prev = base_off + (idx ? idx - 1 : last) * sizeof(struct buffer);
      pbuffer->b_next = base_off + (idx == last ? 0 : idx + 1) * sizeof(struct buffer);
      pbuffer++;
    }
  }

    /* now, we can have quite some buffers in HMA
       -- up to 50 for KE38616.
       so we fill the HMA with buffers
       but not if the BUFFERS count is negative ;-)
     */

  CfgDbgPrintf((" done\n"));

  if (FP_SEG(x86_buffer) == 0xffff)
  {
    buffers++;
    if (InitKernelConfig.Verbose >= 0) 
    {
      printf("Kernel: allocated %d Diskbuffers = %u Bytes in HMA\n",
           buffers, buffers * sizeof(struct buffer));
    }
  }
}

STATIC void config_init_buffers(int wantedbuffers)
{
  config_init_buffers_ex(wantedbuffers, 1);
}

/* Do first time initialization.  Store last so that we can reset it    */
/* later.                                                               */
void PreConfig(void)
{
  /* Initialize the base memory pointers                          */

  CfgDbgPrintf(("SDA located at 0x%p\n", internal_data));
  /* Begin by initializing our system buffers                     */
  /* DebugPrintf(("Preliminary %d buffers allocated at 0x%p\n", Config.cfgBuffers, buffers));*/
  config_lol.sfthead(MK_FP(FP_SEG(x86_FIXED_DATA), FP_OFF(x86_FIXED_DATA) + 0xcc)); /* &(LoL->firstsftt) */
  /* config_lol.fcb() = (sfttbl FAR *)&FcbSft; */
  /* config_lol.fcb() = (sfttbl FAR *)
     KernelAlloc(sizeof(sftheader)
     + Config.cfgFiles * sizeof(sft)); */

  config_init_buffers(Config.cfgBuffers);

  config_lol.cds(KernelAlloc(sizeof(struct cds) * config_lol.lastdrive(), 'L', 0));

/*  CfgDbgPrintf((" FCB table 0x%p\n",config_lol.fcb()));*/
  CfgDbgPrintf((" sft table 0x%p\n", config_lol.sfthead()));
  CfgDbgPrintf((" CDS table 0x%p\n", config_lol.cds()));
  CfgDbgPrintf((" DPB table 0x%p\n", config_lol.dpb()));

  /* Done.  Now initialize the MCB structure                      */
  /* This next line is 8086 and 80x86 real mode specific          */
  CfgDbgPrintf(("Preliminary  allocation completed: top at %04x:%04x\n", lpTop.segment,  lpTop.offset));
}

/// TODO:
STATIC VOID DoMenu(void) {}

/*
    CfgFailure(pLine) - report a CONFIG.SYS syntax error at pLine,
    pointing at the offending character with a caret, and suppressing
    repeat reports for the same line number.

    Migrated from config.c verbatim.
*/
STATIC VOID CfgFailure(BYTE * pLine)
{
  BYTE *pTmp = pLineStart;

  /* suppress multiple printing of same unrecognized lines */

  if (nCfgLine < sizeof(ErrorAlreadyPrinted)*8)
  {
    if (ErrorAlreadyPrinted[nCfgLine/8] & (1 << (nCfgLine%8)))
      return;

    ErrorAlreadyPrinted[nCfgLine/8] |= (1 << (nCfgLine%8));
  }
  printf("CONFIG.SYS error in line %d\n", nCfgLine);
  printf(">>>%s\n   ", pTmp);
  while (++pTmp != pLine)
    printf(" ");
  printf("^\n");
}

/* true if c is a CONFIG.SYS line whitespace character.
   Migrated from config.c verbatim. */
STATIC int iswh(unsigned char c)
{
  return (c == '\r' || c == '\n' || c == '\t' || c == ' ');
}

/* skip whitespace, return pointer to the first non-whitespace char.
   Migrated from config.c verbatim. */
STATIC BYTE * skipwh(BYTE * s)
{
  while (iswh(*s))
    ++s;
  return s;
}

/* Migrated from config.c verbatim. */
STATIC BOOL isnum(char ch)
{
  return (ch >= '0' && ch <= '9');
}

/*
    scan(s, d, fMenuSelect) - extract the next CONFIG.SYS "verb"
    (directive name, up to whitespace or '='), upcasing the
    askThisSingleCommand ('?')/DontAskThisSingleCommand ('!')/numbered-
    menu-line ("N?") prefixes along the way, into d. Returns a
    pointer to whatever follows the verb in s (the directive's
    arguments).

    Migrated from config.c verbatim.
*/
STATIC BYTE * scan(BYTE * s, BYTE * d, int fMenuSelect)
{
  askThisSingleCommand = FALSE;
  DontAskThisSingleCommand = FALSE;
  s = skipwh(s);
  MenuLine = 0;
  /* only check at beginning of line, ie when looking for
     menu selection line applies to.  Fixes issue where
	 value after = starts with number, eg shell=4dos */
  /* does the line start with "123?" */
  if (fMenuSelect && isnum(*s))
  {
    unsigned numbers = 0;
    for ( ; isnum(*s); s++)
        numbers |= 1 << (*s -'0');
    if (*s == '?')
    {
      MenuLine = numbers;
      Menus |= numbers;
      s = skipwh(s+1);
    }
  }
  /* !dos=high,umb    ?? */
  if (*s == '!')
  {
    DontAskThisSingleCommand = TRUE;
    s = skipwh(s+1);
  }
  if (*s == ';')
  {
    /* semicolon is a synonym for rem */
    *d++ = *s++;
  }
  else
    while (*s && !iswh(*s) && *s != '=')
    {
      if (*s == '?')
        askThisSingleCommand = TRUE;
      else
        *d++ = *s;
      s++;
    }
  *d = '\0';
  return s;
}

// from kernel/config.c
typedef void config_sys_func_t(BYTE * pLine);

struct table {
  BYTE *entry;
  signed char pass;
  config_sys_func_t *func;
};

/* case-insensitive string equality. Migrated from config.c verbatim. */
STATIC char strcaseequal(const char * d, const char * s)
{
  char ch;
  while ((ch = toupper(*s++)) == toupper(*d++))
    if (ch == '\0')
      return 1;
  return 0;
}

/*
    LookUp(p, token) - find the command table entry whose name
    case-insensitively matches token, or the table's terminating
    (empty-name) entry if none match.

    Migrated from config.c verbatim.
*/
STATIC struct table * LookUp(struct table *p, BYTE * token)
{
  while (p->entry[0] != '\0' && !strcaseequal((const char *)p->entry, (const char *)token))
    ++p;
  return p;
}


/*
    CfgIgnore(pLine) - the handler for REM/';' (a comment line): does
    nothing.

    Migrated from config.c verbatim.
*/
STATIC VOID CfgIgnore(BYTE * pLine)
{
  UNREFERENCED_PARAMETER(pLine);
}

/*
    GetNumArg(p, num)/GetStringArg(pLine, pszString) - parse a
    directive's numeric (decimal, or hex with a trailing 'x'/'X')
    argument, or just copy its string argument verbatim.

    Migrated from config.c verbatim.
*/
STATIC char *GetNumArg(char *p, int *num)
{
  static char digits[] = "0123456789ABCDEF";
  unsigned char base = 10;
  int sign = 1;
  int n = 0;
  /* look for NUMBER                               */
  p = (char *)skipwh((BYTE *)p);
  if (*p == '-')
  {
    p++;
    sign = -1;
  }
  else if (!isnum(*p))
  {
    CfgFailure((BYTE *)p);
    return NULL;
  }
  for( ; *p; p++)
  {
    char ch = toupper(*p);
    if (ch == 'X')
      base = 16;
    else
    {
      char *q = strchr(digits, ch);
      if (q == NULL)
        break;
      n = n * base + (q - digits);
    }
  }
  *num = n * sign;
  return p;
}

BYTE *GetStringArg(BYTE * pLine, BYTE * pszString)
{
  /* just return whatever string is there, including null         */
  return scan(pLine, pszString, 0);
}

/*
    Config_Buffers(pLine)/CfgBuffersHigh(pLine) - BUFFERS=/BUFFERSHIGH=:
    set the number of FAT buffers to allocate (see config_init_buffers()
    above, which actually allocates them later in PreConfig()).

    Migrated from config.c verbatim.
*/
STATIC void Config_Buffers(BYTE * pLine)
{
  COUNT nBuffers;

  /* Get the argument                                             */
  if (GetNumArg(pLine, &nBuffers))
    Config.cfgBuffers = nBuffers;
}

STATIC void CfgBuffersHigh(BYTE * pLine)
{
  Config_Buffers(pLine);
  if (InitKernelConfig.Verbose >= 0) printf("Note: BUFFERS will be in HMA or low RAM, not in UMB\n");
}


STATIC VOID Stacks(BYTE * pLine)
{
  COUNT stacks;

  /* Format: STACKS = stacks [, stackSize] */
  pLine = GetNumArg((char *)pLine, &stacks);
  if (pLine == NULL)
    return;

  Config.cfgStacks = stacks;
  pLine = skipwh(pLine);

  if (*pLine == ',')
  {
    pLine = GetNumArg((char *)++pLine, &stacks);
    if (pLine == NULL)
      return;
    Config.cfgStackSize = stacks;
  }

  if (Config.cfgStacks)
  {
    if (Config.cfgStackSize < 32)
      Config.cfgStackSize = 32;
    if (Config.cfgStackSize > 512)
      Config.cfgStackSize = 512;
    if (Config.cfgStacks > 64)
      Config.cfgStacks = 64;
  }
  Config.cfgStacksHigh = 0;
}

STATIC VOID StacksHigh(BYTE * pLine)
{
  Stacks(pLine);
  Config.cfgStacksHigh = 1;
}

STATIC VOID init_stacks(dos_far_ptr x86_base, COUNT stacks, COUNT size)
{
  x86_stackBase = x86_base;
  nStacks = stacks;
  stackSize = size;

  memset(ARM_PTR(x86_stackBase), 0,
         (size_t)nStacks * (size_t)stackSize);
}

static void _CmdInstall(BYTE *pLine, int mode)
{
  struct instCmds *cmd;

  if (numInstallCmds >= MAX_INSTALL_CMDS)
  {
    printf("Too many Install commands given (%d max)\n", MAX_INSTALL_CMDS);
    CfgFailure(pLine);
    return;
  }

  cmd = &InstallCommands[numInstallCmds++];
  memcpy(cmd->buffer, pLine, sizeof(cmd->buffer) - 1);
  cmd->buffer[sizeof(cmd->buffer) - 1] = 0;
  cmd->mode = mode;
}

STATIC VOID CmdInstall(BYTE *pLine)
{
  _CmdInstall(pLine, FIRST_FIT);
}

STATIC VOID CmdInstallHigh(BYTE *pLine)
{
  _CmdInstall(pLine, FIRST_FIT_U);
}

STATIC VOID Fcbs(BYTE * pLine)
{
  /* Format: FCBS = totalFcbs [,protectedFcbs]
   *
   * Ported from FreeDOS config.c. This only records CONFIG.SYS values
   * in Config; actual FCB table allocation/use is handled elsewhere
   * and is not implemented in this iteration.
   */
  COUNT fcbs;

  if ((pLine = GetNumArg((char *)pLine, &fcbs)) == NULL)
    return;
  Config.cfgFcbs = fcbs;

  pLine = skipwh(pLine);

  if (*pLine == ',')
  {
    if (GetNumArg((char *)++pLine, &fcbs) != NULL)
      Config.cfgProtFcbs = fcbs;
  }

  if (Config.cfgProtFcbs > Config.cfgFcbs)
    Config.cfgProtFcbs = Config.cfgFcbs;
}

/*
    UmbState of confidence, 1 is sure, 2 maybe, 4 unknown and 0 no way.
*/

STATIC VOID Dosmem(BYTE * pLine)
{
  BYTE *pTmp;
  BYTE UMBwanted = FALSE;

  GetStringArg(pLine, szBuf);
  strcpy(szBuf, pLine);
  strupr(szBuf);

  /* printf("DOS called with %s\n", szBuf); */

  for (pTmp = szBuf;;)
  {
    while (*pTmp == ' ' || *pTmp == '\t')
      pTmp++;

    if (memcmp(pTmp, "UMB", 3) == 0)
    {
      UMBwanted = TRUE;
      pTmp += 3;
    }
    if (memcmp(pTmp, "HIGH", 4) == 0)
    {
      HMAState = HMA_REQ;
      pTmp += 4;
    }
    if (memcmp(pTmp, "LOW", 3) == 0)
    {
      HMAState = HMA_LOW;
      pTmp += 3;
    }
    if (memcmp(pTmp, "NOUMB", 5) == 0)
    {
      UMBwanted = FALSE;
      pTmp += 5;
    }
/*        if (memcmp(pTmp, "CLAIMINIT",9) == 0) { INITDataSegmentClaimed = 0; pTmp += 9; }*/
    pTmp = skipwh(pTmp);

    if (*pTmp == '\0')
      break;
    if (*pTmp != ',')
    {
      CfgFailure(pLine + (pTmp - szBuf));
      break;
    }
    pTmp++;
  }

  if (UmbState == 0)
  {
    config_lol.uppermem_link() = 0;
    config_lol.uppermem_root() = 0xffff;
    UmbState = UMBwanted ? 2 : 0;
  }
  /* Check if HMA is available straight away */
  if (HMAState == HMA_REQ && MoveKernelToHMA())
  {
    HMAState = HMA_DONE;
  }
}

static BYTE cfg_mcb_type(seg s) { return fdos_mcb_type(s); }
static UWORD cfg_mcb_size(seg s) { return fdos_mcb_size(s); }
static void cfg_mcb_set_type(seg s, BYTE type) { fdos_mcb_set_type(s, type); }
static void cfg_mcb_set_size(seg s, UWORD size) { fdos_mcb_set_size(s, size); }
static void cfg_mcb_add_size(seg s, UWORD add) { fdos_mcb_add_size(s, add); }

STATIC seg prev_mcb(seg cur_mcb, seg start)
{
  /* determine prev mcb */
  seg mcb_prev, mcb_next;
  mcb_prev = mcb_next = start;
  while (mcb_next < cur_mcb && cfg_mcb_type(mcb_next) == MCB_NORMAL)
  {
    mcb_prev = mcb_next;
    mcb_next += cfg_mcb_size(mcb_prev) + 1;
  }
  return mcb_prev;
}

STATIC VOID mumcb_init(UCOUNT seg, UWORD size);
STATIC VOID mcb_init(UCOUNT seg, UWORD size, BYTE type);

STATIC void umb_init(void)
{
  UCOUNT umb_seg, umb_size;
  seg umb_max;
  dos_far_ptr xms_addr = DetectXMSDriver();

  if (EFFECTIVE(xms_addr) == 0)
    return;

  if (UMB_get_largest(xms_addr, &umb_seg, &umb_size))
  {
    UmbState = 1;

    /* reset root */
    /* Note: since device drivers can change what is considered top of memory (e.g. move XBDA) we must requery */
    /* Conventional memory size, in KB. Read straight from the BIOS data
       area rather than via INT 12h (the former init_oem() helper, now
       removed): 0040:0013 is the same value INT 12h returns, and this
       avoids a guest interrupt during early init. */
    ram_top = pload16(0x413);
    config_lol.uppermem_root() = ram_top * 64 - 1;

#ifdef INT21_DIAG
    printf("UMBINIT ram_top=%u root=%04x base_seg=%04x zsize=%04x zend=%04x umb=%04x+%04x\n",
           ram_top, config_lol.uppermem_root(), base_seg,
           cfg_mcb_size(base_seg),
           (UWORD)(base_seg + 1 + cfg_mcb_size(base_seg)),
           umb_seg, umb_size);
#endif
    /* create link mcb (below) */
    fdos_mcb_set_type(base_seg, MCB_NORMAL);
    fdos_mcb_set_size(base_seg, (UWORD)(fdos_mcb_size(base_seg) - 1u));
    mumcb_init(config_lol.uppermem_root(), umb_seg - config_lol.uppermem_root() - 1);

    /* setup the real mcb for the devicehigh block */
    mcb_init(umb_seg, umb_size - 2, MCB_NORMAL);

    umb_base_seg = umb_max = umb_start = umb_seg;
    UMB_top = umb_size;

    /* there can be more UMBs !
       this happens, if memory mapped devces are in between
       like UMB memory c800..c8ff, d8ff..efff with device at d000..d7ff
       However some of the xxxHIGH commands still only work with
       the first UMB.
    */

    while (UMB_get_largest(xms_addr, &umb_seg, &umb_size))
    {
      seg umb_prev, umb_next;

      /* setup the real mcb for the devicehigh block */
      mcb_init(umb_seg, umb_size - 2, MCB_NORMAL);

      /* determine prev and next umbs */
      umb_prev = prev_mcb(umb_seg, config_lol.uppermem_root());
      umb_next = umb_prev + cfg_mcb_size(umb_prev) + 1;

      if (umb_seg < umb_max)
      {
        if (umb_next - umb_seg - umb_size == 0)
        {
          /* should the UMB driver return
             adjacent memory in several pieces */
          umb_size += cfg_mcb_size(umb_next) + 1;
          cfg_mcb_set_size(umb_seg, umb_size);
        }
        else
        {
          /* create link mcb (above) */
          mumcb_init(umb_seg + umb_size - 1, umb_next - umb_seg - umb_size);
        }
      }
      else /* umb_seg >= umb_max */
      {
        umb_prev = umb_next;
      }

      if (umb_seg - umb_prev - 1 == 0)
        /* should the UMB driver return
           adjacent memory in several pieces */
        cfg_mcb_add_size(prev_mcb(umb_prev, config_lol.uppermem_root()), umb_size);
      else
      {
        /* create link mcb (below) */
        mumcb_init(umb_prev, umb_seg - umb_prev - 1);
      }

      if (umb_seg > umb_max)
        umb_max = umb_seg;
    }
    fdos_mcb_set_size(umb_max, (UWORD)(fdos_mcb_size(umb_max) + 1u));
    fdos_mcb_set_type(umb_max, MCB_LAST);
    CfgDbgPrintf(("UMB Allocation completed: start at 0x%x\n", umb_base_seg));
  }
}

STATIC VOID InitPgm(BYTE * pLine)
{
  static char init[NAMEMAX];
  static char inittail[NAMEMAX];

  /*
   * Ported from FreeDOS config.c.
   *
   * SHELL=/COMMAND= only selects the command interpreter and its tail.
   * Actual execution is still done later by process-0 / EXEC startup.
   */
  Config.cfgInit = init;
  Config.cfgInitTail = inittail;

  pLine = GetStringArg(pLine, (BYTE *)Config.cfgInit);

  strcpy(Config.cfgInitTail, (char *)pLine);
  strcat(Config.cfgInitTail, "\r\n");

  Config.cfgP_0_startmode = 0;
}

STATIC VOID InitPgmHigh(BYTE * pLine)
{
  InitPgm(pLine);
  /*
   * Original FreeDOS marks process-0 start mode high here.
   * This flag is preserved even though EXEC/high loading is not yet
   * implemented in the current port.
   */
  Config.cfgP_0_startmode = 0x80;
}

/* RE function for menu. */
int  findend(BYTE * s)
{
  int nLen = 0;
  /* 'marks' end if at least ten spaces, 0, or newline is found. */
  while (*s && (*s != 0x0d || *s != 0x0a) )
  {
    BYTE *p= skipwh(s);
    /* ah, more than 9 whitespaces ? We're done here (hrmph!) */
    if(p - s >= 10)
      break;
    nLen++;
    ++s;
  }
  return nLen;
}

STATIC VOID CfgMenu(BYTE * pLine)
{
  int nLen;
  BYTE *pNumber = pLine;

  printf("%s\n",pLine);
  if (MenuColor == -1)
    return;

  pLine = skipwh(pLine);

  /* skip drawing characters in cp437, which is what we'll have
     just after booting! */
  while ((unsigned char)*pLine >= 0xb0 && (unsigned char)*pLine < 0xe0)
    pLine++;

  pLine = skipwh(pLine);  /* skip more whitespaces... */

  /* now I'm expecting a number here if this is a menu-choice line. */
  if (isnum(pLine[0]))
  {
    struct MenuSelector *menu = &MenuStruct[pLine[0]-'0'];

    menu->x = (pLine-pNumber);  /* xpos is at start of number */
    menu->y = nMenuLine;
    /* copy menu text: */
    nLen = findend(pLine); /* length is until cr/lf, null or three spaces */

    /* max 40 chars including nullterminator
       (change struct at top of file if you want more...) */
    if (nLen > MENULINEMAX-1)
      nLen = MENULINEMAX-1;
    memcpy(menu->Text, pLine, nLen);
    menu->Text[nLen] = 0;  /* nullTerminate */
  }
  nMenuLine++;
}

STATIC VOID CfgMenuEsc(BYTE * pLine) {
  BYTE * check;
  for (check = pLine; check[0]; check++)
    if (check[0] == '$') check[0] = 27;	/* translate $ to ESC */
  printf("%s\n",pLine);
}
STATIC VOID Files(BYTE * pLine)
{
  COUNT nFiles;

  /* Get the argument                                             */
  if (GetNumArg(pLine, &nFiles) == (BYTE *) 0)
    return;

  /* Got the value, assign either default or new value            */
  Config.cfgFiles = max(Config.cfgFiles, nFiles);
  Config.cfgFilesHigh = 0;
}

STATIC VOID FilesHigh(BYTE * pLine)
{
  Files(pLine);
  Config.cfgFilesHigh = 1;
}

STATIC VOID CfgLastdrive(BYTE * pLine)
{
  /* Format:   LASTDRIVE = letter         */
  BYTE drv;

  pLine = skipwh(pLine);
  drv = toupper(*pLine);

  if (drv < 'A' || drv > 'Z')
  {
    CfgFailure(pLine);
    return;
  }
  drv -= 'A' - 1;               /* Make real number */
  if (drv > Config.cfgLastdrive)
    Config.cfgLastdrive = drv;
  Config.cfgLastdriveHigh = 0;
}

STATIC VOID CfgLastdriveHigh(BYTE * pLine)
{
  /* Format:   LASTDRIVEHIGH = letter         */
  CfgLastdrive(pLine);
  Config.cfgLastdriveHigh = 1;
}

STATIC VOID CfgBreak(BYTE *pLine)
{
    pLine = skipwh(pLine);
    if (toupper(pLine[0]) == 'O' && toupper(pLine[1]) == 'N') {
        config_idata.break_ena() = TRUE;
        return;
    }
    if (toupper(pLine[0]) == 'O' &&
        toupper(pLine[1]) == 'F' &&
        toupper(pLine[2]) == 'F') {
        config_idata.break_ena() = FALSE;
        return;
    }
    CfgFailure(pLine);
}

STATIC VOID Numlock(BYTE * pLine)
{
  /* Format:      NUMLOCK = (ON | OFF)      */
  GetStringArg(pLine, szBuf);
  BYTE flags = pload8(0x417);
  flags &= ~0x20;
  if (!strcaseequal((const char *)szBuf, "OFF"))
    flags |= 0x20;
  pstore8(0x417, flags);
  keycheck();
}

STATIC struct table commands[];

STATIC VOID CfgSwitches(BYTE * pLine)
{
  pLine = skipwh(pLine);
  if (*pLine == '=')
  {
    pLine = skipwh(pLine + 1);
  }
  while (*pLine)
  {
    if (*pLine == '/') {
      pLine++;
      switch(toupper(*pLine)) {
      case 'K':
        if (commands[0].pass == 1)
          kbdType = 0; /* force conv keyb */
        break;
      case 'N':
        InitKernelConfig.SkipConfigSeconds = -1;
        break;
      case 'F':
        InitKernelConfig.SkipConfigSeconds = 0;
        break;
      case 'E': /* /E[[:]nnnn]  Set the desired EBDA amount to move in bytes */
        {       /* Note that if there is no EBDA, this will have no effect */
          int n = 0;
          if (*++pLine == ':')
            pLine++;                    /* skip optional separator */
          if (!(isnum(*pLine) || (*pLine == '-')))
          {
            pLine--;
            break;
          }
          pLine = GetNumArg(pLine, &n) - 1;
          /* allowed values: [48..1024] bytes, multiples of 16
           * e.g. AwardBIOS: 48, AMIBIOS: 1024
           * (Phoenix, MRBIOS, Unicore = ????)
           */
          if (n == -1)
          {
            Config.ebda2move = 0xffff;
            break;
          }
          else if (n >= 48 && n <= 1024)
          {
            Config.ebda2move = (n + 15) & 0xfff0;
            break;
          }
          /* else fall through (failure) */
        }
        __attribute__((fallthrough));
      default:
        CfgFailure(pLine);
      }
    } else {
      CfgFailure(pLine);
    }
    pLine = skipwh(pLine+1);
  }
  commands[0].pass = 1;
}

STATIC void ClearScreen(unsigned char attr)
{
  /* scroll down (newlines): */
  unsigned char rows;

  /* clear */
  CPU_AX = 0x0600;
  CPU_BH = attr;
  CPU_CX = 0;
  CPU_DL = peekb(0x40, 0x4a) - 1; /* columns */
  rows = peekb(0x40, 0x84);
  if (rows == 0) rows = 24;
  CPU_DH = rows;
  bios_intcall(cpu, 0x10, "CLS");

  /* move cursor to pos 0,0: */
  CPU_AH = 0x02; /* set cursorpos */
  CPU_BH = 0;    /* displaypage: */
  CPU_DX = 0;  /* pos 0,0 */
  bios_intcall(cpu, 0x10, "CLS");
  MenuColor = attr;
}

/**
  MENUCOLOR[=] fg[, bg]
*/
STATIC void CfgMenuColor(BYTE * pLine)
{
  int num = 0;
  unsigned char fg, bg = 0;

  pLine = skipwh(pLine);

  if ('=' == *pLine)
    pLine = skipwh(pLine + 1);

  pLine = GetNumArg(pLine, &num);
  if (pLine == 0)
    return;
  fg = (unsigned char)num;

  pLine = skipwh(pLine);

  if (*pLine == ',')
  {
    pLine = GetNumArg(skipwh(pLine+1), &num);
    if (pLine == 0)
      return;
    bg = (unsigned char)num;
  }
  ClearScreen((bg << 4) | fg);
}

STATIC VOID CfgMenuDefault(BYTE * pLine)
{
  COUNT num = 0;

  pLine = skipwh(pLine);

  if ('=' != *pLine)
  {
    CfgFailure(pLine);
    return;
  }
  pLine = skipwh(pLine + 1);

  /* Format:  STACKS = stacks [, stackSize]       */
  pLine = GetNumArg(pLine, &num);
  MenuSelected = num;
  pLine = skipwh(pLine);

  if (*pLine == ',')
  {
    GetNumArg(++pLine, &MenuTimeout);
  }
}

STATIC VOID DosData(BYTE * pLine)
{
  pLine = GetStringArg(pLine, szBuf);
  strupr(szBuf);

  if (memcmp(szBuf, "UMB", 3) == 0)
    Config.cfgDosDataUmb = TRUE;
}

/*
   Keyboard buffer relocation: KEYBUF=start[,end]
   Select a new location for the  keyboard buffer  at 0x40:xx,
   for example 0x40:0xac-0xff, but 0x50:5-0xff ("basica" only?)
   feels safer? 0x60:0-0xff is scratch, we use it as SHELL PSP.
   (sys / boot sector load_segment / LOADSEG, exeflat call in
   makefile, DOS_PSP in mcb.h, main.c P_0, task.c, kernel.asm)
   (50:e0..ff used as early kernel boot drive / config buffer)
*/
STATIC VOID CfgKeyBuf(BYTE * pLine)
{
  /*  Format:     KEYBUF = startoffset [,endoffset]    */
  UWORD FAR *keyfill = (UWORD FAR *)ARM_PTR( MK_FP(0x40, 0x1a) );
  UWORD FAR *keyrange = (UWORD FAR *)ARM_PTR( MK_FP(0x40, 0x80) );
  COUNT startbuf, endbuf;

  if ((pLine = GetNumArg(pLine, &startbuf)) == 0)
    return;
  pLine = skipwh(pLine);
  endbuf = (startbuf | 0xff)+1;	/* default end: end of the same "page" */
  if (*pLine == ',')
  {
    if ((pLine = GetNumArg(++pLine, &endbuf)) == 0)
      return;
  }
  startbuf &= 0xfffe;
  endbuf &= 0xfffe;
  if (endbuf<startbuf || (endbuf-startbuf)<=0x20 ||
    ((startbuf & 0xff00) != ((endbuf-1) & 0xff00)) )
    startbuf = 0;		/* flag as bad: too small or page wrap */
  if (startbuf<0xac || (startbuf>=0x100 && startbuf<0x105) || startbuf>0x1de)
  {				/* 50:0 / 50:4 are for prtscr / A:/B: DJ */
    printf("Must start at 0xac..0x1de, not 0x100..0x104\n");
    return;
  }
  keyfill[0] = startbuf;
  keyfill[1] = startbuf;
  keyrange[0] = startbuf;
  keyrange[1] = endbuf;
  keycheck();
}

STATIC VOID CfgSwitchar(BYTE * pLine)
{
  /* Format: SWITCHAR = character         */
  GetStringArg(pLine, szBuf);
  init_switchar(*szBuf);
}

/**
  Set screen mode - rewritten to use init_call_intr() by RE / ICD
*/
STATIC VOID sysScreenMode(BYTE * pLine)
{
  COUNT nMode;
  COUNT nFunc = 0x11;

  /* Get the argument                                             */
  if (GetNumArg(pLine, &nMode) == (BYTE *) 0)
    return;

  if(nMode<0x10)
    nFunc = 0; /* set lower screenmode */
  else if ((nMode != 0x11) && (nMode != 0x12) && (nMode != 0x14))
    return; /* do nothing; invalid screenmode */

/* Modes
   0x11 (17)   28 lines
   0x12 (18)   43/50 lines
   0x14 (20)   25 lines
 */
  /* move cursor to pos 0,0: */
  CPU_AH = nFunc; /* set videomode */
  CPU_AL = nMode;
  CPU_BL = 0;
  bios_intcall(cpu, 0x10, "MODE");
}

STATIC VOID sysVersion(BYTE * pLine)
{
  COUNT major, minor;
  char *p = strchr(pLine, '.');

  if (p == NULL)
    return;

  p++;

  /* Get major number */
  if (GetNumArg(pLine, &major) == (BYTE *) 0)
    return;

  /* Get minor number */
  if (GetNumArg(p, &minor) == (BYTE *) 0)
    return;

  if (InitKernelConfig.Verbose >= 0) printf("Changing reported version to %d.%d\n", major, minor);

  config_lol.os_setver_major() = major; /* not the internal os_major */
  config_lol.os_setver_minor() = minor; /* not the internal os_minor */
  ((psp far *) ARM_PTR(x86_PSP))->ps_retdosver = (minor << 8) + major;
}

/*
    Undocumented feature:  ANYDOS
        will report to MSDOS programs just the version number
        they expect. be careful with it!
*/
STATIC VOID SetAnyDos(BYTE * pLine)
{
  UNREFERENCED_PARAMETER(pLine);
  ReturnAnyDosVersionExpected = TRUE;
}

/*
   Kernel built-in energy saving: IDLEHALT=haltlevel
   -1 max savings, 0 never HLT, 1 safe kernel only HLT,
   2 (3) also hooks int2f.1680 (and sets al=0)
*/
STATIC VOID SetIdleHalt(BYTE * pLine)
{
  COUNT haltlevel;
  if (GetNumArg(pLine, &haltlevel))
    HaltCpuWhileIdle = haltlevel; /* 0 for no HLT, 1..n more, -1 max */
}

STATIC BYTE far * searchvar(const BYTE * name, int length)
{
  BYTE* pp = ((BYTE*)ARM_PTR(x86_master_env));
  do {
    if (!fmemcmp(name, pp, length + 1)) {
      return pp;
    }
    pp += strlen(pp) + 1;
  } while (*pp);
  return NULL;
}

static char* envp = 0;
STATIC void deletevar(BYTE far * pp) {
  if (!envp) {
    envp = ((char*)ARM_PTR(x86_master_env));
  }
  int variablelength;
  if (NULL == pp)
    return;
  variablelength = fstrlen(pp) + 1;
  memcpy(pp, pp + variablelength, (unsigned)(envp + 3 - (pp + variablelength)));
  /* our fmemcpy always copies forwards */
  envp -= variablelength;
  return;
}

STATIC VOID CmdSet(BYTE *pLine)
{
  if (!envp) {
    envp = ((char*)ARM_PTR(x86_master_env));
  }
  pLine = GetStringArg(pLine, szBuf);
  pLine = skipwh(pLine);  /* scan() stops at the equal sign or space */
  if (*pLine == '=')      /* equal sign is required */
  {
    int size, oldsize, namesize;
    BYTE far * pp;
    strupr(szBuf);        /* all environment variables must be uppercase */
    namesize = strlen(szBuf);
    strcat(szBuf, "=");
    pp = searchvar(szBuf, namesize);
    pLine = skipwh(++pLine);
    strcat(szBuf, pLine); /* append the variable value (may include spaces) */
    size = strlen(szBuf);
    if (size == namesize + 1) {
      /* empty variable ?  then just delete. (cannot fail) */
      deletevar(pp);
      return;
    }
    if (pp) {
      oldsize = fstrlen(pp) + 1;
    } else {
      oldsize = 0;
    }
    BYTE* master_env = ((BYTE*)ARM_PTR(x86_master_env));
    dpb_watch_check_chain("prep_shell-after-env");
    if (size < master_env + 128 - (envp - oldsize) - 1 - 2)
    {                     /* must end with two consequtive zeros */
      deletevar(pp);      /* now that there's enough space, actually delete */
      fstrcpy(envp, szBuf);
      envp += size + 1;   /* add next variables starting at the second zero */
      *envp = 0;
      envp[1] = 0;
      envp[2] = 0;
      /* The word marker after last variable should not equal 1,
          to indicate that there is no executable pathname following.  */
    }
    else
      printf("Master environment is full - can't add \"%s\"\n", szBuf);
  }
  else
    printf("Invalid SET command: \"%s\"\n", szBuf);
}

STATIC VOID CmdChain(BYTE * pLine)
{
  struct CfgFile *cfg;
  int fd;

  InstallPrintf(("CHAIN: %s\n", pLine));
  if (nCurChain >= MAX_CHAINS) {
    CfgFailure(pLine);
    return;
  }
  /* upstream: open(pLine) из собственного буфера, не из SDA */
  strcpy((char *)szBuf, (const char *)pLine);
  if ((fd = open(x86_SZ_BUF, 0)) < 0) {
    CfgFailure(pLine);
    return;
  }
  cfg = &cfgFile[nCurChain++];
  cfg->nFileDesc = nFileDesc;
  cfg->nCfgLine = nCfgLine;
  nFileDesc = fd;
  nCfgLine = 0;
}

/*********************************************************************************
    National specific things.
    this handles only Date/Time/Currency, and NOT codepage things.
    Some may consider this a hack, but I like to see 24 Hour support. tom.
*********************************************************************************/

#define _DATE_MDY 0 /* mm/dd/yy */
#define _DATE_DMY 1  /* dd.mm.yy */
#define _DATE_YMD 2  /* yy/mm/dd */

#define _TIME_12 0
#define _TIME_24 1

struct CountrySpecificInfoSmall {
  short CountryID;    /*  = W1 W437   # Country ID */
  char  DateFormat;           /*    Date format: 0/1/2: U.S.A./Europe/Japan */
  char  CurrencyString[3];    /* '$' ,'EUR'   */
  char  ThousandSeparator;    /* ','          # Thousand's separator */
  char  DecimalPoint;         /* '.'        # Decimal point        */
  char  DateSeparator;        /* '-'  */
  char  TimeSeparator;        /* ':'  */
  char  CurrencyFormat;       /* = 0  # Currency format (bit array)  */
  char  CurrencyPrecision;    /* = 2  # Currency precision           */
  char  TimeFormat;           /* = 0  # time format: 0/1: 12/24 houres */
};

/* Read-only table: keep it in FLASH (.rodata) rather than SRAM (.data). */
const struct CountrySpecificInfoSmall specificCountriesSupported[] = {
#include "country/kernel.tb1"
};

STATIC int LoadCountryInfoHardCoded(COUNT ctryCode)
{
  const struct CountrySpecificInfoSmall *country;

  /* printf("cntry: %u, CP%u, file=\"%s\"\n", ctryCode, codePage, filename);  */

  for (country = specificCountriesSupported;
       country < specificCountriesSupported + LENGTH(specificCountriesSupported);
       country++)
  {
    if (country->CountryID == ctryCode)
    {
      nlsCountryInfoHardcoded.C.CountryID = country->CountryID;
      nlsCountryInfoHardcoded.C.DateFormat = country->DateFormat;
      nlsCountryInfoHardcoded.C.CurrencyString[0] = country->CurrencyString[0];
      nlsCountryInfoHardcoded.C.CurrencyString[1] = country->CurrencyString[1];
      nlsCountryInfoHardcoded.C.CurrencyString[2] = country->CurrencyString[2];
      nlsCountryInfoHardcoded.C.ThousandSeparator[0] = country->ThousandSeparator;
      nlsCountryInfoHardcoded.C.DecimalPoint[0] = country->DecimalPoint;
      nlsCountryInfoHardcoded.C.DateSeparator[0] = country->DateSeparator;
      nlsCountryInfoHardcoded.C.TimeSeparator[0] = country->TimeSeparator;
      nlsCountryInfoHardcoded.C.CurrencyFormat = country->CurrencyFormat;
      nlsCountryInfoHardcoded.C.CurrencyPrecision = country->CurrencyPrecision;
      nlsCountryInfoHardcoded.C.TimeFormat = country->TimeFormat;
      return 0;
    }
  }

  printf("could not find country info for country ID %u\n", ctryCode);
  printf("current supported countries are ");

  for (country = specificCountriesSupported;
       country < specificCountriesSupported + LENGTH(specificCountriesSupported);
       country++)
  {
    printf("%u ", country->CountryID);
  }
  printf("\n");

  return 1;
}

#pragma pack(push, 1)
/*      LoadCountryInfo():
 *      Searches a file in the COUNTRY.SYS format for an entry
 *      matching the specified code page and country code, and loads
 *      the corresponding information into memory. If code page is 0,
 *      the default code page for the country will be used.
 *
 *      Returns TRUE if successful, FALSE if not.
 */
STATIC BOOL LoadCountryInfo(char *filenam, UWORD ctryCode, UWORD codePage)
{
  /* .max is the biggest payload we may copy out of COUNTRY.SYS for this
     subfunction; it is the capacity of the fixed hardcoded slot the data
     lands in (see NLS_HC_TBL*_SIZE / init_nls_hardcoded()).
     CTYINFO is the exception: MS-DOS ships up to 38 bytes there and the
     code below clamps it to sizeof(struct CountrySpecificInfo) instead of
     rejecting the file, so accept up to 38. */
  static struct subf_tbl table[9] = {
    {"\377       ", -1, 0},                     /* 0, unused */
    {"\377CTYINFO", 5,  38},                    /* 1 */
    {"\377UCASE  ", 0,  NLS_HC_TBL2_SIZE - 2},  /* 2 */
    {"\377LCASE  ", -1, 0},                     /* 3, not supported [yet] */
    {"\377FUCASE ", 1,  NLS_HC_TBL4_SIZE - 2},  /* 4 */
    {"\377FCHAR  ", 2,  NLS_HC_TBL5_SIZE - 2},  /* 5 */
    {"\377COLLATE", 3,  NLS_HC_TBL6_SIZE - 2},  /* 6 */
    {"\377DBCS   ", 4,  NLS_HC_TBL7_SIZE - 2},  /* 7, not supported [yet] */
    {"\377YESNO  ", -1, 4}                      /* 35 */
  };
  int fd, i, subf_tbl_ndx;
  const char* filename = filenam == NULL ? "\\COUNTRY.SYS" : filenam;
  BOOL rc = FALSE;
  BYTE FAR *ptable;
  dos_far_ptr CharMapFn;

  /* upstream: open(filename) из собственного буфера, не из SDA */
  if (szBuf != filename) // for case "\\COUNTRY.SYS", in other case, filename == filenam == szBuf
    strcpy((char *)szBuf, filename);
  if ((fd = open(x86_SZ_BUF, 0)) < 0)
  {
    if (filenam == NULL)
      return !LoadCountryInfoHardCoded(ctryCode);
    printf("%s not found\n", filename);
    return rc;
  }

  /* COUNTRY.SYS file data structures - see RBIL tables 2619-2622 */
  struct header {      /* file header */
    char name[8];       /* "\377COUNTRY.SYS" */
    char reserved[11];
    ULONG offset;       /* offset of first entry in file */
  };
  u16 sp = CPU_SP;
  dos_far_ptr x86_header = guest_stack_alloc(cpu, sizeof(struct header));
  struct header* header = (struct header*)ARM_PTR(x86_header);
  if (read(fd, x86_header, sizeof(struct header)) != sizeof(struct header))
  {
    printf("Error reading %s\n", filename);
    goto ret;
  }

  if (memcmp(header->name, "\377COUNTRY", sizeof(header->name)))
  {
err:printf("%s has invalid format\n", filename);
    goto ret;
  }

  dos_far_ptr x86_entries = x86_nlsEntries;
  UWORD* p_entries = (UWORD*)ARM_PTR(x86_entries);
  if (lseek(fd, header->offset) == 0xffffffffL || read(fd, x86_entries, sizeof(UWORD)) != sizeof(*p_entries))
    goto err;

  struct entry {      /* entry */
    UWORD length;       /* length of entry, not counting this word, = 12 */
    UWORD country;      /* country ID */
    UWORD codepage;     /* codepage ID */
    UWORD reserved[2];
    ULONG offset;       /* offset of country-subfunction-header in file */
  };
  dos_far_ptr x86_entry = guest_stack_alloc(cpu, sizeof(struct entry));
  struct entry* entry = (struct entry*)ARM_PTR(x86_entry);

  dos_far_ptr x86_count = x86_nlsCount;
  UWORD* p_count = (UWORD*)ARM_PTR(x86_count);

  dos_far_ptr x86_hdr = x86_subf_hdr;
  struct subf_hdr* hdr = (struct subf_hdr*)ARM_PTR(x86_hdr);

  dos_far_ptr x86_subf_data_ = x86_subf_data;
  struct subf_data* subf_data = (struct subf_data*)ARM_PTR(x86_subf_data_);

  struct nlsPackage* nlsPackageHardcoded = (struct nlsPackage*)ARM_PTR(_nlsPackageHardcoded);

  for (i = 0; i < *p_entries; i++)
  {
    if (read(fd, x86_entry, sizeof(struct entry)) != sizeof(struct entry) || entry->length != 12)
      goto err;
    if (entry->country != ctryCode || (entry->codepage != codePage && codePage))
      continue;
    if (lseek(fd, entry->offset) == 0xffffffffL
      || read(fd, x86_count, sizeof(UWORD)) != sizeof(UWORD)
      || *p_count > 9
      || read(fd, x86_hdr, sizeof(struct subf_hdr) * *p_count) != sizeof(struct subf_hdr) * *p_count)
      goto err;

    /* Note: we reuse i here as we only process 1 entry, goto after inner for ends outer for */
    for (i = 0; i < *p_count; i++)
    {
      if (hdr[i].length != 6)
        goto err;
      subf_tbl_ndx = hdr[i].id;
      if (subf_tbl_ndx == 3 || ((subf_tbl_ndx < 1 || subf_tbl_ndx > 7) && subf_tbl_ndx != 35))
        continue;
      if (subf_tbl_ndx == 35)
        subf_tbl_ndx = 8;  /* 0 through 7 match, but subfunction 35 is 9th entry in table[] */
      if (lseek(fd, hdr[i].offset) == 0xffffffffL
       || read(fd, x86_subf_data_, 10) != 10
       || (memcmp(subf_data->signature, table[subf_tbl_ndx].sig, 8) && (hdr[i].id !=4
       || memcmp(subf_data->signature, table[2].sig, 8)))  /* UCASE for FUCASE ^*/
       || subf_data->length > sizeof(subf_data->buffer)
       || subf_data->length > table[subf_tbl_ndx].max
       || read(fd, x86_subf_data_buffer, subf_data->length) != subf_data->length)
        goto err;
      if (hdr[i].id == 1)
      {
        if (((struct CountrySpecificInfo *)subf_data->buffer)->CountryID != entry->country
         || (((struct CountrySpecificInfo *)subf_data->buffer)->CodePage != entry->codepage && codePage)
        ) {
          continue;
        }
        nlsPackageHardcoded->cntry = entry->country;
        nlsPackageHardcoded->cp = entry->codepage;
        subf_data->length =      /* MS-DOS "CTYINFO" is up to 38 bytes */
                min(subf_data->length, sizeof(struct CountrySpecificInfo));
        CharMapFn = nlsCountryInfoHardcoded.C.CharMapFn;
      }
      if (hdr[i].id == 35)
      {
        if (subf_data->length < 4)
          goto err;
        memcpy(&nlsPackageHardcoded->yeschar, subf_data->buffer, 2);
        memcpy(&nlsPackageHardcoded->nochar, subf_data->buffer + 2, 2);
        continue;
      }
      if (hdr[i].id == 1)
        ptable = (BYTE FAR *)&nlsPackageHardcoded->nlsExt.size;
      else
        ptable = (BYTE FAR *)ARM_PTR(nlsPackageHardcoded->nlsPointers[table[subf_tbl_ndx].idx].pointer);
      if (hdr[i].id == 7)
      {
        if (subf_data->length == 0)
        {
          /* if DBCS table (in country.sys) is empty, clear internal table */
          *(DWORD *)(subf_data->buffer) = 0L;
          memcpy(ptable, subf_data->buffer, 4);
        }
        else
        {
          memcpy(ptable + 2, subf_data->buffer, subf_data->length);
          /* write length */
          *(UWORD *)(subf_data->buffer) = subf_data->length;
          memcpy(ptable, subf_data->buffer, 2);
        }
        continue;
      }

      /* for 0-7 we store COUNTRY.SYS data directly in the NLS table. */
      memcpy(ptable + 2, subf_data->buffer,
              /* skip length ^*/  subf_data->length);
      if (hdr[i].id == 1) {
          /* fixup user callable address in case we overwrote it */
          ((struct CountrySpecificInfo *)ptable)->CharMapFn = CharMapFn;
       }
    }
    rc = TRUE;
    goto ret;
  }
  printf("could not find country info for country ID %u\n", ctryCode);
ret:
  CPU_SP = sp;
  close(fd);
  return rc;
}
#pragma pack(pop)

STATIC VOID Country(BYTE * pLine)
{
  /* Format: COUNTRY = countryCode, [codePage], filename   */
  COUNT ctryCode;
  COUNT codePage = 0;
  char  *filename = NULL;

  if ((pLine = GetNumArg(pLine, &ctryCode)) == 0)
    goto error;

  pLine = skipwh(pLine);
  if (*pLine == ',')
  {
    pLine = skipwh(pLine + 1);

    if (*pLine != ',')
      if ((pLine = GetNumArg(pLine, &codePage)) == 0)
        goto error;

    pLine = skipwh(pLine);
    if (*pLine == ',')
    {
      GetStringArg(++pLine, szBuf);
      filename = szBuf;
    }
  }

  if (LoadCountryInfo(filename, ctryCode, codePage))
    return;

error:
  CfgFailure(pLine);
}

/* Resident placeholder for a memory manager implemented by the host.
 *
 * Some DOS diagnostics enumerate the device chain instead of probing the
 * XMS/EMS APIs.  Keep a real DOS allocation and a valid character-device
 * header in that chain even though the guest manager itself is skipped.
 * The tiny x86 stubs accept any request and complete it successfully. */
#pragma pack(push, 1)
struct fake_memmgr_driver
{
  struct dhdr hdr;
  UBYTE strategy[11];
  UBYTE interrupt[26];
  UWORD request_off;
  UWORD request_seg;
};
#pragma pack(pop)

STATIC BOOL InstallFakeMemMgr(const char devname[8], const char mcbname[8], COUNT mode)
{
  const size_t bytes = sizeof(struct fake_memmgr_driver);
  const size_t paras = (bytes + 15) / 16;
  dos_far_ptr x86_dhp = KernelAllocPara(paras, 'D', (char *)mcbname, mode);
  struct fake_memmgr_driver *drv =
      (struct fake_memmgr_driver *)ARM_PTR(x86_dhp);
  UWORD req_off = (UWORD)offsetof(struct fake_memmgr_driver, request_off);
  UWORD req_seg = (UWORD)offsetof(struct fake_memmgr_driver, request_seg);

  memset(drv, 0, paras * 16);
  drv->hdr.dh_next = config_lol.nul_dev_next();
  drv->hdr.dh_attr = ATTR_CHAR;
  drv->hdr.x86.dh_strategy =
      (UWORD)offsetof(struct fake_memmgr_driver, strategy);
  drv->hdr.x86.dh_interrupt =
      (UWORD)offsetof(struct fake_memmgr_driver, interrupt);
  memcpy(drv->hdr.dh_name, devname, sizeof(drv->hdr.dh_name));

  /* strategy: save ES:BX request-header pointer in resident storage; RETF */
  {
    const UBYTE code[] = {
      0x2e, 0x89, 0x1e, (UBYTE)req_off, (UBYTE)(req_off >> 8),
      0x2e, 0x8c, 0x06, (UBYTE)req_seg, (UBYTE)(req_seg >> 8),
      0xcb
    };
    memcpy(drv->strategy, code, sizeof(code));
  }

  /* interrupt: request->r_status = S_DONE; RETF */
  {
    const UBYTE code[] = {
      0x50,                         /* push ax */
      0x53,                         /* push bx */
      0x1e,                         /* push ds */
      0x2e, 0x8b, 0x1e, (UBYTE)req_off, (UBYTE)(req_off >> 8),
      0x2e, 0xa1, (UBYTE)req_seg, (UBYTE)(req_seg >> 8),
      0x8e, 0xd8,                   /* mov ds,ax */
      0xc7, 0x47, 0x03,
      (UBYTE)S_DONE, (UBYTE)(S_DONE >> 8),
      0x1f,                         /* pop ds */
      0x5b,                         /* pop bx */
      0x58,                         /* pop ax */
      0xcb                          /* retf */
    };
    memcpy(drv->interrupt, code, sizeof(code));
  }

  config_lol.nul_dev_next(x86_dhp);
  CfgDbgPrintf(("Installed fake memory-manager device %.8s at %04x:0000 (%u paragraphs)\n",
                devname, FP_SEG(x86_dhp), (unsigned)paras));
  return TRUE;
}

STATIC BOOL LoadDevice(BYTE * pLine, dos_far_ptr top, COUNT mode)
{
  exec_blk eb;
  dos_far_ptr dhp;
  BOOL result;
  seg base, start;

  if (mode)
  {
    base = umb_base_seg;
    start = umb_start;
  }
  else
  {
    base = base_seg;
    start = config_lol.first_mcb();
  }

  if (base == start)
    base++;
  base++;

  /* Get the device driver name                                   */
  GetStringArg(pLine, szBuf);
  
  char* driver_name = szBuf + strlen((const char *)szBuf);
  while (driver_name > szBuf &&
         driver_name[-1] != '\\' &&
         driver_name[-1] != '/' &&
         driver_name[-1] != ':')
    --driver_name;

  if (strncasecmp(driver_name, "HIMEM.SYS", 10) == 0 ||
      strncasecmp(driver_name, "HIMEMX.EXE", 11) == 0 ||
      strncasecmp(driver_name, "XMGR.SYS", 9) == 0 ||
      strncasecmp(driver_name, "XMGR.EXE", 9) == 0 ||
      strncasecmp(driver_name, "QRAM.SYS", 9) == 0 ||
      strncasecmp(driver_name, "QEMM.SYS", 9) == 0 ||
      strncasecmp(driver_name, "386MAX.SYS", 11) == 0 ||
      strncasecmp(driver_name, "QEMM386.SYS", 12) == 0 ||
      strncasecmp(driver_name, "BLUEMAX.SYS", 12) == 0 ||
      strncasecmp(driver_name, "NETROOM.SYS", 12) == 0) {
    printf("Using host XMS manager; install guest device-chain placeholder instead of: %s\n", szBuf);
    BOOL installed = InstallFakeMemMgr("XMSXXXX0", "HIMEM   ", mode);
    if (installed)
      fdos_disk_enable_guest_int13();
    return installed ? SUCCESS : DE_NOMEM;
  }
#ifndef I386_MODE
  if (strncasecmp(driver_name, "EMM386.EXE", 11) == 0) {
    printf("Using host EMM manager; install guest device-chain placeholder instead of: %s\n", szBuf);
    BOOL installed = InstallFakeMemMgr("EMMXXXX0", "EMM386  ", mode);
    if (installed)
      fdos_disk_enable_guest_int13();
    return installed ? SUCCESS : DE_NOMEM;
  }
#endif

  /* The driver is loaded at the top of allocated memory.         */
  /* The device driver is paragraph aligned.                      */
  eb.load.reloc = eb.load.load_seg = base;

  CfgDbgPrintf(("Loading device driver %s at segment %04x\n", szBuf, base));

  /* upstream: init_DosExec(3, &eb, szBuf) - szBuf теперь гостевой
     (x86_SZ_BUF), поэтому передаётся напрямую. SDA PriPathName здесь
     запрещён: это dest-буфер truename(), и DosOpenSft(имя ИЗ него)
     давал src==dest - имя разрушалось посреди разбора (регрессия
     UMB+конфиг 11ca8c0 -> bef7e2f). DosExec()'s "lp" - нативный
     указатель в гостевую память (linear_to_far в task.c), сегмент
     значения не имеет. */
  if ((result = DosExec(EXEC_OVERLAY, &eb, (BYTE FAR *) szBuf)) != SUCCESS)
  {
    dpb_watch_check_chain("LoadDevice err");
    CfgFailure(pLine);
    return result;
  }
  dpb_watch_check_chain("LoadDevice");

  /*
   * Guest driver code now exists and may install or depend on interrupt hooks.
   * From here on DOS block I/O must enter through the current IVT[13h],
   * including disk traffic issued while this driver's INIT request runs.
   */
  fdos_disk_enable_guest_int13();

  strcpy(szBuf, pLine);
  /* uppercase the device driver command */
  strupr(szBuf);

  /* TE this fixes the loading of devices drivers with
     multiple devices in it. NUMEGA's SoftIce is such a beast
   */

  /* add \r\n to the command line */
  strcat(szBuf, " \r\n");

  dhp = MK_FP(base, 0);

  /* NOTE - Modification for multisegmented device drivers:          */
  /*   In order to emulate the functionallity experienced with other */
  /*   DOS operating systems, the original 'top' end address is      */
  /*   updated with the end address returned from the INIT request.  */
  /*   The updated end address is then used when issuing the next    */
  /*   INIT request for the following device driver within the file  */
  dos_far_ptr next_dhp = MK_FP(0, 0);
  while (FP_OFF(next_dhp) != 0xffff)
  {
    struct dhdr *p = (struct dhdr *) ARM_PTR(dhp);

#ifdef DEBUGCFG
    UBYTE *img = (UBYTE *)ARM_PTR(dhp);
#endif

    /* One-shot check of the loaded x86 device header before init_device()
       and before x86_execrh() can execute dh_strategy.  Keep it to one
       line: header signature, header fields, and first bytes at strategy. */
    CfgDbgPrintf(("DEVHDR %04x:%04x sig=%02x%02x%02x%02x attr=%04x strat=%04x intr=%04x op=%02x%02x%02x%02x%02x\n",
                  FP_SEG(dhp), FP_OFF(dhp),
                  img[0], img[1], img[2], img[3],
                  p->dh_attr, p->x86.dh_strategy, p->x86.dh_interrupt,
                  img[p->x86.dh_strategy + 0], img[p->x86.dh_strategy + 1],
                  img[p->x86.dh_strategy + 2], img[p->x86.dh_strategy + 3],
                  img[p->x86.dh_strategy + 4]));    
    /* /// TODO:
     native external drivers need a separate load path, e.g. DEVICENATIVE.
     ATTR_NATIVE cannot be trusted in disk-loaded DOS driver headers. */
    p->dh_attr &= ~ATTR_NATIVE;
    if ((result = init_device(dhp, szBuf, mode, &top)) != SUCCESS) {
      break;
    }

    /* dh_next chains multiple device headers within the *same*
       loaded driver segment: only its offset is meaningful, the
       segment is always this driver's own load segment (see
       DosExec()'s relocation pass, which treats every loaded driver
       image as living entirely in one segment). */
    next_dhp = MK_FP(FP_SEG(dhp), FP_OFF(p->dh_next));

    /* Link in device driver and save LoL->nul_dev pointer to next */
    p->dh_next = config_lol.nul_dev_next();
    config_lol.nul_dev_next(dhp);

    dhp = next_dhp;
  }

  return result;
}

STATIC VOID DeviceHigh(BYTE * pLine)
{
  /* might have been the UMB driver or DOS=UMB */
  if (UmbState == 2)
    umb_init();
  if (UmbState == 1)
  {
    if (LoadDevice(pLine, MK_FP(umb_start + UMB_top, 0), TRUE) == DE_NOMEM)
    {
      printf("Not enough free memory in UMBs: loading low\n");
      LoadDevice(pLine, lpTop, FALSE);
    }
  }
  else
  {
    printf("UMBs unavailable!\n");
    LoadDevice(pLine, lpTop, FALSE);
  }
}

STATIC void Device(BYTE * pLine)
{
  LoadDevice(pLine, lpTop, FALSE);
}

STATIC struct table commands[] = {
  /* first = switches! this one is special; some options will
     always be ran, others depends on F5/F8 and ? processing */
  {"SWITCHES", 0, CfgSwitches},

  /* rem is never executed by locking out pass                    */
  {"REM", 0, CfgIgnore},
  {";", 0,   CfgIgnore},

  {"MENUCOLOR", 0, CfgMenuColor},
  {"MENUDEFAULT", 0, CfgMenuDefault},
  {"MENU", 0, CfgMenu},      /* lines to print in pass 0 */
  {"ECHO", 2, CfgMenu},      /* lines to print in pass 2 - install(high) */
  {"EECHO", 2, CfgMenuEsc},            /* modified ECHO (ea) */

  {"BREAK", 1, CfgBreak},
 
  {"BUFFERS", 1, Config_Buffers},
  {"BUFFERSHIGH", 1, CfgBuffersHigh}, /* as BUFFERS - we use HMA anyway */

  {"COMMAND", 1, InitPgm},
  {"COUNTRY", 1, Country},
  {"DOS", 1, Dosmem},
  {"DOSDATA", 1, DosData},
  {"FCBS", 1, Fcbs},
  {"KEYBUF", 1, CfgKeyBuf},	/* ea */
  {"FILES", 1, Files},
  {"FILESHIGH", 1, FilesHigh},
  {"LASTDRIVE", 1, CfgLastdrive},
  {"LASTDRIVEHIGH", 1, CfgLastdriveHigh}, 
  {"NUMLOCK", 1, Numlock},
  {"SHELL", 1, InitPgm},
  {"SHELLHIGH", 1, InitPgmHigh},
  {"STACKS", 1, Stacks},
  {"STACKSHIGH", 1, StacksHigh},
  {"SWITCHAR", 1, CfgSwitchar},
  {"SCREEN", 1, sysScreenMode},   /* JPP */
  {"VERSION", 1, sysVersion},     /* JPP */
  {"ANYDOS", 1, SetAnyDos},       /* tom */
  {"IDLEHALT", 1, SetIdleHalt},   /* ea  */

  {"DEVICE", 2, Device},
  {"DEVICEHIGH", 2, DeviceHigh},
  {"INSTALL", 2, CmdInstall},
  {"INSTALLHIGH", 2, CmdInstallHigh},
  {"CHAIN", 2, CmdChain},
  {"SET", 2, CmdSet},
  /* default action                                               */
  {"", -1, CfgFailure}
};

STATIC BOOL SkipLine(char *pLine)
{
  short key;
  COUNT i;
  signed char originalskipconfigseconds = InitKernelConfig.SkipConfigSeconds;

  if (originalskipconfigseconds >= 0)
  {
    if (originalskipconfigseconds > 0)
      printf("Press F8 to trace or F5 to skip CONFIG.SYS/AUTOEXEC.BAT");

    key = GetBiosKey(originalskipconfigseconds);       /* wait 2 seconds */

    InitKernelConfig.SkipConfigSeconds = -1;

    if (key == 0x3f00)          /* F5 */
    {
      SkipAllConfig = TRUE;
    }
    else if (key == 0x4200)     /* F8 */
    {
      singleStep = TRUE;
    }

    if (originalskipconfigseconds > 0)
      printf("\r%79s\r", "");     /* clear line */

    if (SkipAllConfig)
      printf("Skipping CONFIG.SYS/AUTOEXEC.BAT\n");
  }

  if (SkipAllConfig)
    return TRUE;

  /* 1?device=CDROM.SYS */
  /* 12?device=OAKROM.SYS */
  /* 123?device=EMM386.EXE NOEMS */
  if ( MenuLine != 0 &&
      (MenuLine & (1 << MenuSelected)) == 0)
    return TRUE;

  if (DontAskThisSingleCommand)     /* !files=30 */
    return FALSE;

  if (!askThisSingleCommand && !singleStep)
    return FALSE;

  for (i = 0; i < nCurChain; i++)
    printf(" ");
  printf("%s[Y,N]?", pLine);

  for (;;)
  {
    key = GetBiosKey(-1);

    switch (toupper(key & 0x00ff))
    {
      case 'N':
        printf("N\n");
        return TRUE;

      case 0x1b:               /* don't know where documented
                                   ESCAPE answers all following questions
                                   with YES
                                 */
        singleStep = FALSE;
        __attribute__((fallthrough));

      case '\r':
      case '\n':
      case 'Y':
        printf("Y\n");
        return FALSE;

    }

    if (key == 0x3f00)          /* YES, you may hit F5 here, too */
    {
      printf("N\n");
      SkipAllConfig = TRUE;
      return TRUE;
    }
  }

}

/// TODO:
#if 0
char kernel_command_line[1] = "";
size_t kernel_command_line_length = 0;
#endif

VOID DoConfig(int nPass)
{
  BOOL bEof = FALSE;
#ifdef MEMDISK_ARGS
  /* check if MEMDISK used for config_lol.boot_drive(), if so check for special appended arguments */
  struct memdiskinfo FAR *mdsk = NULL;
  BYTE FAR *cLine;
  /* memdisk check & usage requires 386+, DO NOT invoke if less than 386 */
  if (config_lol.cpu_family() >= 3)
  {
    UBYTE drv = (config_lol.boot_drive() < 3)?0x0:0x80; /* 1=A,2=B,3=C */
    mdsk = query_memdisk(drv);
    if (mdsk != NULL)
    {
      cLine = ProcessMemdiskLine(mdsk->cmdline);
    }
  }
#endif

  if (nPass==0)
  {
    HaltCpuWhileIdle = 0; /* init to "no HLT while idle" */
#ifdef MEMDISK_ARGS
    if (mdsk != NULL)
    {
      printf("MEMDISK version %u.%02u  (%lu sectors)\n", mdsk->version, mdsk->version_minor, mdsk->size);
      CfgDbgPrintf(("MEMDISK args:{%S}\n", mdsk->cmdline));
    }
    else
    {
      CfgDbgPrintf(("MEMDISK not detected!\n"));
    }
#endif
  }
  {
  /// TODO:
#if 0
    char * pp = kernel_command_line;
    char * cc;
    unsigned ii;
    static char commandbuffer[256];
    char * end = &kernel_command_line[kernel_command_line_length];
    static char * configfile = "";
    static char * altconfigfile = "fdconfig.sys";
    static char * oldconfigfile = "config.sys";
    static struct { char ** pointer; char const * command; }
      configcommands[] = {
        { &configfile, "CONFIG" },
        { &altconfigfile, "ALTCONFIG" },
        { &oldconfigfile, "OLDCONFIG" },
        { NULL, NULL }
        };
    for (; pp < end; pp += strlen(pp) + 1) {
      for (cc = pp; *cc == '\t' || *cc == ' '; ++cc);
      strcpy(commandbuffer, cc);
      strupr(commandbuffer);
      for (ii = 0; configcommands[ii].pointer != NULL; ++ii)
        if (check_config_commandline(configcommands[ii].pointer,
          cc, commandbuffer, configcommands[ii].command))
          break;
    }

    /* Check to see if we have a config.sys file.  If not, just     */
    /* exit since we don't force the user to have one (but 1st      */
    /* also process MEMDISK passed config options if present).      */
    for (ii = 0; configcommands[ii].pointer != NULL; ++ii) {
      if (**configcommands[ii].pointer != '\0') {
        if ((nFileDesc = open(*configcommands[ii].pointer, 0)) >= 0) {
          CfgDbgPrintf(("Reading \"%s\"...\n", *configcommands[ii].pointer));
          break;
        } else {
          CfgDbgPrintf(("\"%s\" not found\n", *configcommands[ii].pointer));
        }
      }
    }
    if (configcommands[ii].pointer == NULL) {
#else
    static const char * const configcommands[] = {
      "fdconfig.sys", "config.sys", NULL
    };
    int ii;
    /* upstream: open("fdconfig.sys") - имя из СОБСТВЕННОГО буфера (в
       оригинале это литерал init-сегмента). SDA PriPathName здесь
       запрещён: DosOpenSft() -> truename(имя, PriPathName) при
       src==dest разрушал имя посреди разбора, и CONFIG.SYS молча не
       открывался - корень регрессии UMB+конфиг 11ca8c0 -> bef7e2f. */

    for (ii = 0; configcommands[ii] != NULL; ++ii) {
      strcpy((char *)szBuf, configcommands[ii]);
      if ((nFileDesc = open(x86_SZ_BUF, 0)) >= 0) {
        CfgDbgPrintf(("Reading \"%s\"...\n", configcommands[ii]));
        break;
      } else {
        CfgDbgPrintf(("\"%s\" not found\n", configcommands[ii]));
      }
    }
    if (configcommands[ii] == NULL) {
#endif
      /* at this point no config file was found, may return early */
#ifdef MEMDISK_ARGS
      /* if memdisk in use then only assume end of file reached and proceed, else return early */
      if (mdsk != NULL)
        bEof = TRUE;
      else
#endif
        return;
    }
  }

  nCfgLine = 0;  /* keep track of which line in file for errors   */

  /* Read each line into the buffer and then parse the line,      */
  /* do the table lookup and execute the handler for that         */
  /* function.                                                    */

  BYTE* szLine = (BYTE*)ARM_PTR(x86_szLine);
#ifdef MEMDISK_ARGS
  for (; !bEof || (mdsk != NULL); nCfgLine++)
#else
  for (; !bEof; nCfgLine++)
#endif
  {
    struct table *pEntry;
    pLineStart = szLine;

#ifdef MEMDISK_ARGS
    if (!bEof)
    {
#endif

    /* read in a single line, \n or ^Z terminated */
    BYTE *pLine;
    for (pLine = szLine;;)
    {
      /* pLine walks the szLine buffer; keep the original guest pointer and
         derive the current byte by offset instead of converting host->guest. */
      if (read(nFileDesc,
               ADD_OFF(x86_szLine, (UWORD)(pLine - szLine)) /* -> char[] */, 1) == 0)
      {
        bEof = TRUE;
        break;
      }

      if (pLine >= szLine + szLine_len - 3)
      {
        CfgFailure(pLine);
        printf("error - line overflow line %d \n", nCfgLine);
        break;
      }

      if (*pLine == '\n' || *pLine == EOF)      /* end of line */
        break;

      if (*pLine != '\r')       /* ignore CR */
        pLine++;
    }

    *pLine = 0;
#ifdef MEMDISK_ARGS
    }
    else if (mdsk != NULL)
    {
      cLine = GetNextMemdiskLine(cLine, szLine);
      /* if end of memdisk command line reached, flag done */
      if (!*cLine)
        mdsk = NULL;
    }
#endif

    if (bEof && nCurChain) {
      struct CfgFile *cfg = &cfgFile[--nCurChain];
      close(nFileDesc);
      bEof = FALSE;
      nFileDesc = cfg->nFileDesc;
      nCfgLine = cfg->nCfgLine;
      continue;
    }

    CfgDbgPrintf(("CONFIG=[%s]\n", szLine));

    /* Skip leading white space and get verb.               */
    pLine = scan(szLine, szBuf, 1);

    /* If the line was blank, skip it.  Otherwise, look up  */
    /* the verb and execute the appropriate function.       */
    if (*szBuf == '\0')
      continue;

    pEntry = LookUp(commands, szBuf);

	/* should config command be executed on this pass? */
    if (pEntry->pass >= 0 && pEntry->pass != nPass)
      continue;

	/* pass 0 always executed (rem Menu prompt switches) */
    if (nPass == 0)
    {
      pEntry->func(pLine);
      continue;
    }
    else
    {
      if (SkipLine(pLineStart))   /* F5/F8/?/! processing */
        continue;
    }

    if ((pEntry->func != CfgMenu) && (pEntry->func != CfgMenuEsc))
    {
      /* compatibility "device foo.sys" */
      if (' ' != *pLine && '\t' != *pLine && '=' != *pLine)
      {
        CfgFailure(pLine);
        continue;
      }
      pLine = skipwh(pLine);
    }
    if ('=' == *pLine || pEntry->func == CfgMenu || pEntry->func == CfgMenuEsc)
      pLine = skipwh(pLine+1);

    /* YES. DO IT */
    pEntry->func(pLine);
  }
  close(nFileDesc);

  if (nPass == 0)
  {
    DoMenu();
  }
}

/*
    GetBiosKey(timeout) - poll for a keystroke (INT 16h AH=01h/00h),
    waiting up to "timeout" seconds (or forever if timeout < 0, or
    just once if timeout == 0) for one to appear.

    timeout < 0: no timeout
    timeout = 0: poll only once
    timeout > 0: timeout in seconds

    return
            0xffff : no key hit
            0xHHLL : scancode in upper half, ASCII in lower half
*/
#define GetBiosTime() pload32(0x46c)
UWORD GetBiosKey(int timeout)
{
  ULONG res;
  CPU_regs saved;
  ULONG startTime = GetBiosTime();
  cpu_save_regs(cpu, &saved);
  if (timeout >= 0)
  {
    do
    {
      /* optionally HLT here - timer will IRQ even if no keypress */
      CPU_AX = 0x0100;             /* are there keys available ? */
      bios_intcall(cpu, 0x16, "GetBiosKey");
      if (!zf) {
        CPU_AX = 0x0000;
        bios_intcall(cpu, 0x16, "GetBiosKey");
        goto ok;
      }
    } while ((unsigned)(GetBiosTime() - startTime) < timeout * 18u);
    res = 0xffff;
    goto ret;
  }
  CPU_AX = 0x0000;
  bios_intcall(cpu, 0x16, "GetBiosKey");
ok:
  res = CPU_AX;
ret:
  cpu_restore_regs(cpu, &saved);
  return res;
}

STATIC dos_far_ptr AlignParagraph(dos_far_ptr lpPtr)
{
  UWORD uSegVal;

  /* First, convert the segmented pointer to linear address       */
  uSegVal = FP_SEG(lpPtr);
  uSegVal += (FP_OFF(lpPtr) + 0xf) >> 4;
  if (FP_OFF(lpPtr) > 0xfff0)
    uSegVal += 0x1000;          /* handle overflow */

  /* and return an adddress adjusted to the nearest paragraph     */
  /* boundary.                                                    */
  return MK_FP(uSegVal, 0);
}

STATIC VOID mcb_init(UCOUNT seg, UWORD size, BYTE type)
{
  unsigned i;
  fdos_mcb_set_type((seg)seg, type);
  fdos_mcb_set_owner((seg)seg, 0);
  fdos_mcb_set_size((seg)seg, size);
  for (i = 0; i < 8; ++i)
    fdos_mcb_set_name_byte((seg)seg, i, 0);
}

STATIC VOID mumcb_init(UCOUNT seg, UWORD size)
{
  unsigned i;
  fdos_mcb_set_type((seg)seg, MCB_NORMAL);
  fdos_mcb_set_owner((seg)seg, 8);
  fdos_mcb_set_size((seg)seg, size);
  for (i = 0; i < 8; ++i)
    fdos_mcb_set_name_byte((seg)seg, i, 0);
  fdos_mcb_set_name_byte((seg)seg, 0, 'S');
  fdos_mcb_set_name_byte((seg)seg, 1, 'C');
}

/*
 * PreConfig2() from FreeDOS config.c, ported to the current native/guest
 * pointer split.
 *
 * Original effects kept here:
 *   - initialize config_lol.first_mcb()/base_seg;
 *   - create the low-memory MCB chain;
 *   - append the second 3-entry SFT block after the built-in 5-entry block.
 * 
 * /// TODO: Not ported here:
 *   - EBDA move;
 *   - UMB init;
 *   - HMA finalization.
 */
VOID PreConfig2(VOID)
{
  dos_far_ptr x86_dyn_last = DynLast();
  dos_far_ptr x86_first_mcb = AlignParagraph(ADD_OFF(x86_dyn_last, 0x0F));
  base_seg = config_lol.first_mcb() = FP_SEG(x86_first_mcb);

  /*
    * ram_top is in Kbytes; MCB size is in paragraphs.
    * DynAlloc() data is below first_mcb, as in the original kernel.
    * The MCB itself occupies first_mcb:0000, so usable size is -1.
   */
  mcb_init(base_seg, ram_top * 64 - config_lol.first_mcb() - 1, MCB_LAST);

  /*
   * Built-in firstsftt has 5 SFT entries. Original PreConfig2 appends
   * a second 3-entry SFT block, giving the initial 8 entries expected
   * before PostConfig() allocates the final FILES= block.
   */
  sfttbl *sp = (sfttbl *)ARM_PTR(config_lol.sfthead());
  dos_far_ptr x86_sft2 = KernelAlloc(sizeof(sftheader) + 3 * sizeof(sft), 'F', 0);
  sp->sftt_next = x86_sft2;

  sp = (sfttbl *)ARM_PTR(x86_sft2);
  sp->sftt_next = MK_FP(-1, -1);
  sp->sftt_count = 3;
}

#pragma pack(push, 1)
struct submcb
{
  char type;
  unsigned short start;
  unsigned short size;
  char unused[3];
  char name[8];
};
#pragma pack(pop)

dos_far_ptr KernelAllocPara(size_t nPara, char type, char *name, int mode)
{
  seg base, start;

  /* if no umb available force low allocation */
  if (UmbState != 1)
    mode = 0;

  if (mode)
  {
    base = umb_base_seg;
    start = umb_start;
  }
  else
  {
    base = base_seg;
    start = config_lol.first_mcb();
  }

  /* create the special DOS data MCB if it doesn't exist yet */
  CfgDbgPrintf(("kernelallocpara: %x %x %x %c %d\n", start, base, nPara, type, mode));

  if (base == start)
  {
    UWORD first_size = fdos_mcb_size(base);
    BYTE first_type = fdos_mcb_type(base);
    base++;
    mcb_init(base, first_size - 1, first_type);
    mumcb_init(start, 0);
    fdos_mcb_set_name_byte(start, 1, 'D');
  }

  nPara++;
  {
    UWORD cur_size = fdos_mcb_size(base);
    BYTE cur_type = fdos_mcb_type(base);
    mcb_init(base + nPara, cur_size - nPara, cur_type);
  }
  cfg_mcb_add_size(start, (UWORD)nPara);

  struct submcb* p = (struct submcb*)ARM_PTR(MK_FP(base, 0));
  seg alloc_seg = base + 1;
  p->type = type;
  p->start = alloc_seg;
  p->size = nPara-1;
  if (name)
    memcpy(p->name, name, 8);
  base += nPara;
  if (mode)
    umb_base_seg = base;
  else
    base_seg = base;

  return MK_FP(alloc_seg, 0);
}

dos_far_ptr KernelAlloc(size_t nBytes, char type, int mode)
{
  dos_far_ptr p;
  size_t nPara = (nBytes + 15)/16;

  if (config_lol.first_mcb() == 0)
  {
    /* prealloc */
    lpTop = MK_FP(FP_SEG(lpTop) - nPara, FP_OFF(lpTop));
    p = AlignParagraph(lpTop);
  }
  else
  {
    p = KernelAllocPara(nPara, type, NULL, mode);
  }
  fmemset(p, 0, nBytes);
  return p;
}

/*
 * Do third pass initialization.
 * Also prepares final CONFIG.SYS pass for DEVICE/INSTALL handlers.
 */
VOID PostConfig(VOID)
{
  dos_far_ptr x86_sp;
  sfttbl *sp;
  COUNT extra_files;

  /* DOS=UMB requests UMB use during pass 1, but historically umb_init()
     was only triggered lazily by DEVICEHIGH.  Finalize that pending request
     here as well so DOS=HIGH,UMB works even when CONFIG.SYS has no
     DEVICEHIGH directive. */
  if (UmbState == 2)
    umb_init();

  if (Config.cfgDosDataUmb)
  {
    Config.cfgFilesHigh = TRUE;
    Config.cfgLastdriveHigh = TRUE;
    Config.cfgStacksHigh = TRUE;
  }

  config_lol.lastdrive() = Config.cfgLastdrive;
  if (config_lol.lastdrive() < config_lol.nblkdev())
    config_lol.lastdrive() = config_lol.nblkdev();

  CfgDbgPrintf(("starting FAR allocations at %x\n", base_seg));
  /* Original FreeDOS reinitializes buffers here after first_mcb exists.
   * The preliminary PreConfig() buffers are only temporary.  In this port,
   * keep the final buffers in low MCB memory for now: HMA buffer placement
   * is a separate path and conflicts with the current HMA model. */
  config_init_buffers_ex(Config.cfgBuffers, 0);

  /*
   * PreConfig2() appended the second 3-entry SFT block after the
   * built-in 5-entry block. Now append the final FILES= extension.
   */
  x86_sp = config_lol.sfthead();
  sp = (sfttbl *)ARM_PTR(x86_sp);
  x86_sp = sp->sftt_next;
  sp = (sfttbl *)ARM_PTR(x86_sp);

  extra_files = Config.cfgFiles - 8;
  if (extra_files > 0)
  {
    dos_far_ptr x86_sft3 =
      KernelAlloc(sizeof(sftheader) + extra_files * sizeof(sft),
                  'F', Config.cfgFilesHigh);

    sp->sftt_next = x86_sft3;
    sp = (sfttbl *)ARM_PTR(x86_sft3);
    sp->sftt_next = MK_FP(-1, -1);
    sp->sftt_count = extra_files;
  }

  config_lol.cds(KernelAlloc(sizeof(struct cds) * config_lol.lastdrive(),
                          'L', Config.cfgLastdriveHigh));

  CfgDbgPrintf((" sft table %04x:%04x\n",
                FP_SEG(((sfttbl *)ARM_PTR(config_lol.sfthead()))->sftt_next),
                FP_OFF(((sfttbl *)ARM_PTR(config_lol.sfthead()))->sftt_next)));
  CfgDbgPrintf((" CDS table %04x:%04x\n", FP_SEG(config_lol.cds()), FP_OFF(config_lol.cds())));
  CfgDbgPrintf((" DPB table %04x:%04x\n", FP_SEG(config_lol.dpb()), FP_OFF(config_lol.dpb())));

  if (Config.cfgStacks)
  {
    dos_far_ptr stackBase =
      KernelAlloc((size_t)Config.cfgStacks * (size_t)Config.cfgStackSize,
                  'S', Config.cfgStacksHigh);

    init_stacks(stackBase, Config.cfgStacks, Config.cfgStackSize);

    CfgDbgPrintf(("Stacks allocated at %04x:%04x count=%u size=%u\n",
                  FP_SEG(stackBase), FP_OFF(stackBase),
                  Config.cfgStacks, Config.cfgStackSize));
  }

  CfgDbgPrintf(("Allocation completed: top at 0x%x\n", base_seg));
}

/* This code must be executed after device drivers has been loaded */
VOID configDone(VOID)
{
  if (UmbState == 1)
    cfg_mcb_set_type(base_seg, MCB_LAST);

/* ///TODO: ??
  if (HMAState != HMA_DONE)
  {
    mcb FAR *p;
    unsigned short kernel_seg;
    unsigned short hma_paras = (HMAFree+0xf)/16;

    kernel_seg = allocmem(hma_paras);
    p = para2far(kernel_seg - 1);

    p->m_name[0] = 'S';
    p->m_name[1] = 'C';
    p->m_psp = 8;

    CfgDbgPrintf(("HMA not available, moving text to %x\n", kernel_seg));
    MoveKernel(kernel_seg);

    kernel_seg += hma_paras + 1;

    CfgDbgPrintf(("kernel is low, start alloc at %x\n", kernel_seg));
  }
*/

  /* The standard handles should be reopened here, because
     we may have loaded new console or printer drivers in CONFIG.SYS */
}

STATIC VOID InstallExec(struct instCmds *icmd)
{
  BYTE *cmd = (BYTE *)icmd->buffer;
  BYTE *s;
  UWORD namelen, taillen;
  dos_far_ptr x86_filename, x86_tail;
  BYTE *filename, *tail;
  exec_blk exb;

  cmd = skipwh(cmd);

  for (s = cmd; *s > 0x20 && *s != '/'; s++)
    ;
  namelen = (UWORD) (s - cmd);

  taillen = 0;
  while (s[taillen] && taillen < CTBUFFERSIZE - 1)
    taillen++;

  /* exec_blk.exec.cmd_line (like DosExec()'s "lp" filename argument -
     see LoadDevice()'s identical requirement for szBuf, above) is a
     dos_far_ptr: it has to point at guest RAM, since the CommandTail
     it addresses ends up copied into the new process's PSP by
     patchPSP() using an ordinary far-pointer dereference. icmd->buffer
     is native (ARM) memory, so borrow some guest stack space for a
     guest-RAM copy of the filename and the CommandTail - same
     technique init_device() (kernel.c) uses for its request packet,
     and MakeFATChain()/etc. already use elsewhere in this file. */
  /* Carve one block for both, then locate the tail inside it with a wrapped
     offset. The previous "MK_FP(CPU_SS, CPU_SP + namelen + 1)" added without
     masking to 16 bits, so a stack pointer near the top of its segment would
     produce a truncated offset pointing away from the bytes just reserved. */
  x86_filename = guest_stack_alloc(cpu, (uint16_t)((namelen + 1) + (taillen + 3)));
  x86_tail = MK_FP(FP_SEG(x86_filename),
                   (uint16_t)(FP_OFF(x86_filename) + namelen + 1));
  filename = (BYTE *) ARM_PTR(x86_filename);
  tail = (BYTE *) ARM_PTR(x86_tail);

  memcpy(filename, cmd, namelen);
  filename[namelen] = 0;

  tail[0] = (BYTE) taillen;
  memcpy(tail + 1, s, taillen);
  tail[taillen + 1] = '\r';
  tail[taillen + 2] = 0;

  exb.exec.env_seg = 0;
  exb.exec.cmd_line = x86_tail;
  exb.exec.fcb_1 = exb.exec.fcb_2 = MK_FP(0xffff, 0xffff);  /* "no FCBs
                                                                to copy" -
                                                                see
                                                                far_is_end()/
                                                                patchPSP()
                                                                in
                                                                task.c */

  InstallPrintf(("INSTALL exec file='%s' tail_len=%u tail='%s'\n",
                 filename, tail[0], tail + 1));

  {
    /* icmd->mode is an allocation-strategy value (FIRST_FIT or
       FIRST_FIT_U for INSTALL=/INSTALLHIGH= respectively - see
       CmdInstall()/CmdInstallHigh() above), not an exec mode: DOS
       always actually runs an INSTALL= program with EXEC_LOADNGO,
       just possibly preferring UMB for its allocations. This mirrors
       upstream's own set_strategy(cmd->mode) before InstallExec(). */
    UBYTE saved_mem_access_mode = config_idata.mem_access_mode();

    config_idata.mem_access_mode() = icmd->mode;
    if (DosExec(EXEC_LOADNGO, &exb, filename) != SUCCESS)
      CfgFailure(cmd);
    config_idata.mem_access_mode() = saved_mem_access_mode;
  }

  CPU_SP += (namelen + 1) + (taillen + 3);
}

VOID DoInstall(void)
{
  int i;

  if (numInstallCmds == 0)
    return;

  InstallPrintf(("Installing commands now\n"));

  /* grab memory for this install code
     we KNOW, that we are executing somewhere at top of memory
     we need to protect the INIT_CODE from other programs
     that will be executing soon
  */

///TODO:  set_strategy(LAST_FIT);
///  installMemory = ((unsigned)_init_end + ebda_size + 15) / 16;
///  installMemory = allocmem(installMemory);
///  InstallPrintf(("allocated memory at %x\n",installMemory));

  for (i = 0; i < numInstallCmds; i++)
  {
    InstallPrintf(("INSTALL[%d] mode=%02X: %s\n", i, InstallCommands[i].mode, InstallCommands[i].buffer));
 ///   set_strategy(cmd->mode);
    InstallExec(&InstallCommands[i]);
  }
///  set_strategy(FIRST_FIT);
///  free(installMemory);

  InstallPrintf(("Done with installing commands\n"));
  return;
}
