#include <ctype.h>
#include "bios/bios.h"
#include "hdrs.h"

#define HMA_NONE 0              /* do nothing */
#define HMA_REQ 1               /* DOS = HIGH detected */
#define HMA_DONE 2              /* Moved kernel to HMA */
#define HMA_LOW 3               /* Definitely LOW */

#define EOF 0x1a

STATIC BYTE szBuf[256] BSS_INIT({0});
STATIC unsigned nCfgLine BSS_INIT(0);
static UBYTE ErrorAlreadyPrinted[128] BSS_INIT({0});
BYTE *pLineStart BSS_INIT(0);
STATIC seg base_seg BSS_INIT(0);
static COUNT UmbState BSS_INIT(0);
STATIC seg umb_base_seg BSS_INIT(0);
UWORD umb_start BSS_INIT(0), UMB_top BSS_INIT(0);
static BYTE HMAState BSS_INIT(0);
static COUNT nFileDesc BSS_INIT(0);
/* CHAIN= support (multiple nested CONFIG.SYS-like files) - the
   table exists so DoConfig()'s "if (bEof && nCurChain)" check below
   compiles and behaves correctly, but nCurChain can never become
   nonzero: CmdChain() (the CHAIN= handler) is CfgNotImplemented() in
   this iteration (see the command table below), so nothing ever
   pushes onto cfgFile[]. */
#define MAX_CHAINS 5
struct CfgFile {
  COUNT nFileDesc;
  COUNT nCfgLine;
} cfgFile[MAX_CHAINS] BSS_INIT({0});

static COUNT nCurChain BSS_INIT(0);
/* [MENU]/numbered-block ("1?DEVICE=...") CONFIG.SYS menu support -
   these are real, live state read/written by scan() below (and by
   CfgMenu()/CfgMenuColor()/etc, all CfgNotImplemented() in this
   iteration - see the command table below), so they need to exist
   and behave correctly even though nothing exercises [MENU] yet. */
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
STATIC struct MenuSelector MenuStruct[MENULINESMAX] BSS_INIT({0});
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

void CharMapSrvc(void) {
  /// TODO:
}

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
    (void FAR *)CharMapSrvc,
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
static struct instCmds InstallCommands[MAX_INSTALL_CMDS];

STATIC void config_init_buffers(int wantedbuffers)
{
  unsigned buffers = 0;

  /* fill HMA with buffers if BUFFERS count >=0 and DOS in HMA        */
  if (wantedbuffers < 0)
    wantedbuffers = -wantedbuffers;
  else if (HMAState == HMA_DONE)
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

  LoL->nbuffers = buffers;
  LoL->inforecptr = LoL->firstbuf;
  struct buffer *pbuffer;
  dos_far_ptr x86_buffer;
  {
    size_t bytes = sizeof(struct buffer) * buffers;
    x86_buffer = HMAalloc(bytes);

    if (!FP_SEG(x86_buffer) && !FP_OFF(x86_buffer))
    {
      x86_buffer = KernelAlloc(bytes, 'B', 0);
      if (HMAState == HMA_DONE)
        x86_firstAvailableBuf = MK_FP(0xffff, HMAFree);
    }
    else
    {
      LoL->bufloc = LOC_HMA;
      /* space in HMA beyond requested buffers available as user space */
      x86_firstAvailableBuf = MK_FP(FP_SEG(x86_buffer), FP_OFF(x86_buffer) + wantedbuffers * sizeof(struct buffer));
    }
    pbuffer = (struct buffer*)ARM_PTR(x86_buffer);
  }
  LoL->deblock_buf = DiskTransferBuffer;
  LoL->firstbuf = x86_buffer;

  CfgDbgPrintf(("init_buffers (size %u) at", sizeof(struct buffer)));
  CfgDbgPrintf((" (%p)", LoL->firstbuf));

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

/* Do first time initialization.  Store last so that we can reset it    */
/* later.                                                               */
void PreConfig(void)
{
  /* Initialize the base memory pointers                          */

  CfgDbgPrintf(("SDA located at 0x%p\n", internal_data));
  /* Begin by initializing our system buffers                     */
  /* DebugPrintf(("Preliminary %d buffers allocated at 0x%p\n", Config.cfgBuffers, buffers));*/
  LoL->sfthead = MK_FP(FP_SEG(x86_FIXED_DATA), FP_OFF(x86_FIXED_DATA) + 0xcc); /* &(LoL->firstsftt) */
  /* LoL->FCBp = (sfttbl FAR *)&FcbSft; */
  /* LoL->FCBp = (sfttbl FAR *)
     KernelAlloc(sizeof(sftheader)
     + Config.cfgFiles * sizeof(sft)); */

  config_init_buffers(Config.cfgBuffers);

  LoL->CDSp = KernelAlloc(sizeof(struct cds) * LoL->lastdrive, 'L', 0);

/*  CfgDbgPrintf((" FCB table 0x%p\n",LoL->FCBp));*/
  CfgDbgPrintf((" sft table 0x%p\n", LoL->sfthead));
  CfgDbgPrintf((" CDS table 0x%p\n", LoL->CDSp));
  CfgDbgPrintf((" DPB table 0x%p\n", LoL->DPBp));

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
    CfgNotImplemented(pLine) - the handler for every directive this
    iteration doesn't migrate a real implementation for (SWITCHES,
    MENU*, BREAK, COMMAND/SHELL, COUNTRY, DOS, DOSDATA, FCBS, KEYBUF,
    NUMLOCK, STACKS, SWITCHAR, SCREEN, VERSION, ANYDOS, IDLEHALT,
    DEVICE*, INSTALL*, CHAIN, SET - see the command table below).

    /// TODO: each of these is a real, separate piece of FreeDOS
    /// kernel functionality (device driver loading, country/codepage
    /// switching, FCB tables, stack-overflow protection, multi-config
    /// menus, environment variables, ...), none of which is migrated
    /// yet. Printing the directive name and otherwise doing nothing
    /// lets CONFIG.SYS parsing continue past a line this kernel
    /// can't yet act on, rather than failing the whole file (which
    /// CfgFailure()'s "unrecognized directive" handling is for) or
    /// silently miscompiling/crashing.
*/
STATIC VOID CfgNotImplemented(BYTE * pLine)
{
  UNREFERENCED_PARAMETER(pLine);
  printf("CONFIG.SYS: directive not implemented yet, ignoring line %d\n", nCfgLine);
  printf(">>>%s\n", pLine);
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
    LoL->uppermem_link = 0;
    LoL->uppermem_root = 0xffff;
    UmbState = UMBwanted ? 2 : 0;
  }
  /* Check if HMA is available straight away */
  if (HMAState == HMA_REQ && MoveKernelToHMA())
  {
    HMAState = HMA_DONE;
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
        break_ena = TRUE;
        return;
    }
    if (toupper(pLine[0]) == 'O' &&
        toupper(pLine[1]) == 'F' &&
        toupper(pLine[2]) == 'F') {
        break_ena = FALSE;
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
  iregs r;
  unsigned char rows;

  /* clear */
  CPU_AX = 0x0600;
  CPU_BH = attr;
  CPU_CX = 0;
  CPU_DL = peekb(0x40, 0x4a) - 1; /* columns */
  rows = peekb(0x40, 0x84);
  if (rows == 0) rows = 24;
  CPU_DH = rows;
  bios_intcall(cpu, 0x10);

  /* move cursor to pos 0,0: */
  CPU_AH = 0x02; /* set cursorpos */
  CPU_BH = 0;    /* displaypage: */
  CPU_DX = 0;  /* pos 0,0 */
  bios_intcall(cpu, 0x10);
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
  iregs r;
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
  bios_intcall(cpu, 0x10);
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

  LoL->os_setver_major = major; /* not the internal os_major */
  LoL->os_setver_minor = minor; /* not the internal os_minor */
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
  dos_far_ptr x86_path = x86_FAR_PTR(DOS_PSP, PriPathName);
  strcpy(PriPathName, pLine);
  if ((fd = open(x86_path, 0)) < 0) {
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

struct CountrySpecificInfoSmall specificCountriesSupported[] = {
#include "country/kernel.tb1"
};

STATIC int LoadCountryInfoHardCoded(COUNT ctryCode)
{
  struct CountrySpecificInfoSmall *country;

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
  /* COUNTRY.SYS file data structures - see RBIL tables 2619-2622 */

  struct {      /* file header */
    char name[8];       /* "\377COUNTRY.SYS" */
    char reserved[11];
    ULONG offset;       /* offset of first entry in file */
  } header;
  struct {      /* entry */
    int length;         /* length of entry, not counting this word, = 12 */
    int country;        /* country ID */
    int codepage;       /* codepage ID */
    int reserved[2];
    ULONG offset;       /* offset of country-subfunction-header in file */
  } entry;
  struct subf_hdr { /* subfunction header */
    int length;         /* length of entry, not counting this word, = 6 */
    int id;             /* subfunction ID */
    ULONG offset;       /* offset within file of subfunction data entry */
  };
  static struct {   /* subfunction data */
    char signature[8];  /* \377CTYINFO|UCASE|LCASE|FUCASE|FCHAR|COLLATE|DBCS|YESNO */
    int length;         /* length of following table in bytes */
    UBYTE buffer[256];
  } subf_data;
  struct subf_tbl {
    char sig[8];        /* signature for each subfunction data */
    int idx;            /* index of pointer in nls_hc.asm to be copied to */
  };
  static struct subf_tbl table[9] = {
    {"\377       ", -1},  /* 0, unused */
    {"\377CTYINFO", 5},   /* 1 */
    {"\377UCASE  ", 0},   /* 2 */
    {"\377LCASE  ", -1},  /* 3, not supported [yet] */
    {"\377FUCASE ", 1},   /* 4 */
    {"\377FCHAR  ", 2},   /* 5 */
    {"\377COLLATE", 3},   /* 6 */
    {"\377DBCS   ", 4},   /* 7, not supported [yet] */
    {"\377YESNO  ", -1}   /* 35 */
  };
  static struct subf_hdr hdr[9];
  static int entries, count;
  int fd, i, subf_tbl_ndx;
  char *filename = filenam == NULL ? "\\COUNTRY.SYS" : filenam;
  BOOL rc = FALSE;
  BYTE FAR *ptable;
  void FAR *CharMapFn;

  dos_far_ptr x86_path = x86_FAR_PTR(DOS_PSP, PriPathName);
  strcpy(PriPathName, filename);
  if ((fd = open(x86_path, 0)) < 0)
  {
    if (filenam == NULL)
      return !LoadCountryInfoHardCoded(ctryCode);
    printf("%s not found\n", filename);
    return rc;
  }
  /* /// TODO: 
  if (read(fd, &header, sizeof(header)) != sizeof(header))
  {
    printf("Error reading %s\n", filename);
    goto ret;
  }
  if (memcmp(header.name, "\377COUNTRY", sizeof(header.name)))
  {
err:printf("%s has invalid format\n", filename);
    goto ret;
  }
  if (lseek(fd, header.offset) == 0xffffffffL
    || read(fd, &entries, sizeof(entries)) != sizeof(entries))
    goto err;
  for (i = 0; i < entries; i++)
  {
    if (read(fd, &entry, sizeof(entry)) != sizeof(entry) || entry.length != 12)
      goto err;
    if (entry.country != ctryCode || entry.codepage != codePage && codePage)
      continue;
    if (lseek(fd, entry.offset) == 0xffffffffL
      || read(fd, &count, sizeof(count)) != sizeof(count)
      || count > LENGTH(hdr)
      || read(fd, hdr, sizeof(struct subf_hdr) * count)
                      != sizeof(struct subf_hdr) * count)
      goto err;

    /* Note: we reuse i here as we only process 1 entry, goto after inner for ends outer for * /
    for (i = 0; i < count; i++)
    {
      if (hdr[i].length != 6)
        goto err;
      subf_tbl_ndx = hdr[i].id;
      if (subf_tbl_ndx == 3 || ((subf_tbl_ndx < 1 || subf_tbl_ndx > 7) && subf_tbl_ndx != 35))
        continue;
      if (subf_tbl_ndx == 35)
        subf_tbl_ndx = 8;  /* 0 through 7 match, but subfunction 35 is 9th entry in table[] * /
      if (lseek(fd, hdr[i].offset) == 0xffffffffL
       || read(fd, &subf_data, 10) != 10
       || memcmp(subf_data.signature, table[subf_tbl_ndx].sig, 8) && (hdr[i].id !=4
       || memcmp(subf_data.signature, table[2].sig, 8))  /* UCASE for FUCASE ^* /
       || read(fd, subf_data.buffer, subf_data.length) != subf_data.length)
        goto err;
      if (hdr[i].id == 1)
      {
        if (((struct CountrySpecificInfo *)subf_data.buffer)->CountryID
                                                     != entry.country
         || ((struct CountrySpecificInfo *)subf_data.buffer)->CodePage
                                                     != entry.codepage
         && codePage)
          continue;
        nlsPackageHardcoded.cntry = entry.country;
        nlsPackageHardcoded.cp = entry.codepage;
        subf_data.length =      /* MS-DOS "CTYINFO" is up to 38 bytes * /
                min(subf_data.length, sizeof(struct CountrySpecificInfo));
        CharMapFn = nlsCountryInfoHardcoded.C.CharMapFn;
      }
      if (hdr[i].id == 1)
        ptable = (BYTE FAR *)&nlsPackageHardcoded.nlsExt.size;
      else
        ptable = nlsPackageHardcoded.nlsPointers[table[subf_tbl_ndx].idx].pointer;
      if (hdr[i].id == 7)
      {
        if (subf_data.length == 0)
        {
          /* if DBCS table (in country.sys) is empty, clear internal table * /
          *(DWORD *)(subf_data.buffer) = 0L;
          fmemcpy(ptable, subf_data.buffer, 4);
        }
        else
        {
          fmemcpy(ptable + 2, subf_data.buffer, subf_data.length);
          /* write length * /
          *(UWORD *)(subf_data.buffer) = subf_data.length;
          fmemcpy(ptable, subf_data.buffer, 2);
        }
        continue;
      }

      /* for 0-7 we store COUNTRY.SYS data directly in buffer, but yes/no characters we store in nls package directly * /
      if (hdr[i].id == 35)
      {
        fmemcpy(&nlsPackageHardcoded.yeschar, subf_data.buffer, 2);
        fmemcpy(&nlsPackageHardcoded.nochar, subf_data.buffer + 2, 2);
      } else {
          fmemcpy(ptable + 2, subf_data.buffer,
                  /* skip length ^* /  subf_data.length);
          if (hdr[i].id == 1) {
              /* fixup user callable address in case we overwrote it * /
              ((struct CountrySpecificInfo *)ptable)->CharMapFn = CharMapFn;
          }
      }
    }
    rc = TRUE;
    goto ret;
  }
  */
  printf("could not find country info for country ID %u\n", ctryCode);
ret:
  close(fd);
  return rc;
}

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

  {"DEVICE", 2, CfgNotImplemented},
  {"DEVICEHIGH", 2, CfgNotImplemented},
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
        singleStep = FALSE;     /* and fall through */

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
  /* check if MEMDISK used for LoL->BootDrive, if so check for special appended arguments */
  struct memdiskinfo FAR *mdsk = NULL;
  BYTE FAR *cLine;
  /* memdisk check & usage requires 386+, DO NOT invoke if less than 386 */
  if (LoL->cpu >= 3)
  {
    UBYTE drv = (LoL->BootDrive < 3)?0x0:0x80; /* 1=A,2=B,3=C */
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
    dos_far_ptr x86_path = x86_FAR_PTR(DOS_PSP, PriPathName);

    for (ii = 0; configcommands[ii] != NULL; ++ii) {
      strcpy(PriPathName, configcommands[ii]);
      if ((nFileDesc = open(x86_path, 0)) >= 0) {
        CfgDbgPrintf(("Reading \"%s\"...\n", configcommands[ii]));
        break;
      } else {
        CfgDbgPrintf(("\"%s\" not found, PriPathName=\"%s\"\n",
                      configcommands[ii], PriPathName));
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

  BYTE* szLine = ARM_PTR(x86_szLine);
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
      if (read(nFileDesc, linear_to_far(pLine), 1) == 0)
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
  iregs r;
  ULONG startTime = GetBiosTime();
  if (timeout >= 0)
  {
    do
    {
      /* optionally HLT here - timer will IRQ even if no keypress */
      CPU_AX = 0x0100;             /* are there keys available ? */
      bios_intcall(cpu, 0x16);
      if (!zf) {
        CPU_AX = 0x0000;
        bios_intcall(cpu, 0x16);
        return CPU_AX;
      }
    } while ((unsigned)(GetBiosTime() - startTime) < timeout * 18u);
    return 0xffff;
  }
  CPU_AX = 0x0000;
  bios_intcall(cpu, 0x16);
  return CPU_AX;
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

STATIC VOID mcb_init_copy(UCOUNT seg, UWORD size, mcb *near_mcb)
{
  near_mcb->m_size = size;
  memcpy(ARM_PTR(MK_FP(seg, 0)), near_mcb, sizeof(mcb));
}

STATIC VOID mcb_init(UCOUNT seg, UWORD size, BYTE type)
{
  static mcb near_mcb BSS_INIT({0}); /// TODO: _BSS
  near_mcb.m_type = type;
  mcb_init_copy(seg, size, &near_mcb);
}

STATIC VOID mumcb_init(UCOUNT seg, UWORD size)
{
  static mcb near_mcb = {
    MCB_NORMAL,
    8, 0,
    {0,0,0},
    {"SC"}
  };
  mcb_init_copy(seg, size, &near_mcb);
}

/*
 * PreConfig2() from FreeDOS config.c, ported to the current native/guest
 * pointer split.
 *
 * Original effects kept here:
 *   - initialize LoL->first_mcb/base_seg;
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
  /*
   * Current fixed guest layout comment in this file says:
   *   _BSS 019F4h..0240Dh, size 0A1Ah
   * so DynLast() equivalent is DOS_PSP:240Eh.
   */
  dos_far_ptr x86_dyn_last = MK_FP(DOS_PSP, 0x240E);
  dos_far_ptr x86_first_mcb = AlignParagraph(ADD_OFF(x86_dyn_last, 0x0F));
  dos_far_ptr x86_sft2;
  sfttbl *sp;

  base_seg = LoL->first_mcb = FP_SEG(x86_first_mcb);

  /*
   * ram_top is in Kbytes; MCB size is in paragraphs.
   * The MCB itself occupies first_mcb:0000, so usable size is -1.
   */
  mcb_init(base_seg, ram_top * 64 - LoL->first_mcb - 1, MCB_LAST);

  /*
   * Built-in firstsftt has 5 SFT entries. Original PreConfig2 appends
   * a second 3-entry SFT block, giving the initial 8 entries expected
   * before PostConfig() allocates the final FILES= block.
   */
  sp = (sfttbl *)ARM_PTR(LoL->sfthead);
  x86_sft2 = KernelAlloc(sizeof(sftheader) + 3 * sizeof(sft), 'F', 0);
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
    start = LoL->first_mcb;
  }

  /* create the special DOS data MCB if it doesn't exist yet */
  CfgDbgPrintf(("kernelallocpara: %x %x %x %c %d\n", start, base, nPara, type, mode));

  if (base == start)
  {
    /*mcb*/ dos_far_ptr x86_p = x86_para2far(base);
    mcb* p = (mcb*)ARM_PTR(x86_p);
    base++;
    mcb_init(base, p->m_size - 1, p->m_type);
    mumcb_init(FP_SEG(x86_p), 0);
    p->m_name[1] = 'D';
  }

  nPara++;
  mcb_init(base + nPara, para2far(base)->m_size - nPara, para2far(base)->m_type);
  para2far(start)->m_size += nPara;

  struct submcb* p = (struct submcb*)para2far(base);
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

  if (LoL->first_mcb == 0)
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

  if (Config.cfgDosDataUmb)
  {
    Config.cfgFilesHigh = TRUE;
    Config.cfgLastdriveHigh = TRUE;
    Config.cfgStacksHigh = TRUE;
  }

  LoL->lastdrive = Config.cfgLastdrive;
  if (LoL->lastdrive < LoL->nblkdev)
    LoL->lastdrive = LoL->nblkdev;

  CfgDbgPrintf(("starting FAR allocations at %x\n", base_seg));

  config_init_buffers(Config.cfgBuffers);

  /*
   * PreConfig2() appended the second 3-entry SFT block after the
   * built-in 5-entry block. Now append the final FILES= extension.
   */
  x86_sp = LoL->sfthead;
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

  LoL->CDSp = KernelAlloc(sizeof(struct cds) * LoL->lastdrive,
                          'L', Config.cfgLastdriveHigh);

  CfgDbgPrintf((" sft table %04x:%04x\n",
                FP_SEG(((sfttbl *)ARM_PTR(LoL->sfthead))->sftt_next),
                FP_OFF(((sfttbl *)ARM_PTR(LoL->sfthead))->sftt_next)));
  CfgDbgPrintf((" CDS table %04x:%04x\n", FP_SEG(LoL->CDSp), FP_OFF(LoL->CDSp)));
  CfgDbgPrintf((" DPB table %04x:%04x\n", FP_SEG(LoL->DPBp), FP_OFF(LoL->DPBp)));

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
    para2far(base_seg)->m_type = MCB_LAST;

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
  BYTE filename[128];
  BYTE *args;
  BYTE *d;
  BYTE *cmd = (BYTE *)icmd->buffer;
  exec_blk exb;

  cmd = skipwh(cmd);

  for (args = cmd, d = filename; ; args++, d++)
  {
    *d = *args;
    if (*d <= 0x20 || *d == '/')
      break;
  }
  *d = 0;

  args--;
  *args = strlen((char *)&args[1]);
  args[*args + 1] = '\r';
  args[*args + 2] = 0;

  exb.exec.env_seg = 0;
  exb.exec.cmd_line = (CommandTail FAR *)args;
  exb.exec.fcb_1 = exb.exec.fcb_2 = NULL; /// may be after dos_far_ptr MK_FP(0xffff, 0xffff);

  InstallPrintf(("INSTALL exec file='%s' tail_len=%u tail='%s'\n", filename, *args, args + 1));

  /// TODO:
  /*
INT 21h AH=4Bh EXEC
INT 21h AH=4Ch terminate
INT 21h AH=48h alloc
INT 21h AH=49h free
INT 21h AH=4Ah resize
INT 21h AX=5800/5801 allocation strategy
COM/EXE loader: DosExec / DosComLoader / DosExeLoader
PSP creation
environment block
MCB ownership by PSP  
  */
  ///if (init_DosExec(icmd->mode, &exb, filename) != SUCCESS)
  ///  CfgFailure(cmd);
}

VOID DoInstall(void)
{
  int i;
///  unsigned short installMemory;
  struct instCmds *cmd;

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
