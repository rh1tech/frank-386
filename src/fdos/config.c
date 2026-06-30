#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"
#include "i8254.h"

#include "hdr/kconfig.h"
#include "hdr/portab.h"

#include "hdr/ddate.h"
#include "hdr/dtime.h"
#include "hdr/error.h"
#include "hdr/clock.h"
#include "hdr/device.h"
#include "hdr/sft.h"
#include "hdr/kbd.h"
#include "hdr/fcb.h"
#include "hdr/fat.h"
#include "hdr/pcb.h"
#include "hdr/dirmatch.h"
#include "hdr/fnode.h"
#include "hdr/mcb.h"
#include "hdr/lol.h"
#include "hdr/dcb.h"
#include "hdr/cds.h"
#include "hdr/tail.h"
#include "hdr/process.h"
#include "hdr/version.h"
#include "proto.h"
#include "globals.h"
#include "hdr/debug.h"
#include "hdr/buffer.h"
#include "hdr/file.h"
#include "config.h"
#include "hdr/network.h"
#include "init-mod.h"
#include "dyndata.h"

#define printf(...) dos_printf(__VA_ARGS__)

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
struct config Config = { 0 };
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

/*struct buffer*/dos_far_ptr x86_firstAvailableBuf;

extern const dos_far_ptr x86_szLine;
const size_t szLine_len = 256;

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

STATIC struct table commands[] = {
  /* first = switches! this one is special; some options will
     always be ran, others depends on F5/F8 and ? processing */
  {"SWITCHES", 0, CfgNotImplemented},
/// TODO:  {"SWITCHES", 0, CfgSwitches},

  /* rem is never executed by locking out pass                    */
  {"REM", 0, CfgIgnore},
  {";", 0,   CfgIgnore},

  {"MENUCOLOR",0,CfgNotImplemented},
/// TODO:  {"MENUCOLOR",0,CfgMenuColor},
#if 1
  {"MENUDEFAULT", 0, CfgNotImplemented},
  {"MENU", 0, CfgNotImplemented},      /* lines to print in pass 0 */
  {"ECHO", 2, CfgNotImplemented},      /* lines to print in pass 2 - install(high) */
  {"EECHO", 2, CfgNotImplemented},     /* modified ECHO (ea) */

  {"BREAK", 1, CfgNotImplemented},
#else
  {"MENUDEFAULT", 0, CfgMenuDefault},
  {"MENU", 0, CfgMenu},         /* lines to print in pass 0 */
  {"ECHO", 2, CfgMenu},         /* lines to print in pass 2 - install(high) */
  {"EECHO", 2, CfgMenuEsc},     /* modified ECHO (ea) */

  {"BREAK", 1, CfgBreak},
#endif
 
  {"BUFFERS", 1, Config_Buffers},
  {"BUFFERSHIGH", 1, CfgBuffersHigh}, /* as BUFFERS - we use HMA anyway */

#if 1
  {"COMMAND", 1, CfgNotImplemented},
  {"COUNTRY", 1, CfgNotImplemented},
  {"DOS", 1, CfgNotImplemented},
  {"DOSDATA", 1, CfgNotImplemented},
  {"FCBS", 1, CfgNotImplemented},
  {"KEYBUF", 1, CfgNotImplemented},	/* ea */
  {"NUMLOCK", 1, CfgNotImplemented},
  {"SHELL", 1, CfgNotImplemented},
  {"SHELLHIGH", 1, CfgNotImplemented},
  {"STACKS", 1, Stacks},
  {"STACKSHIGH", 1, StacksHigh},
  {"SWITCHAR", 1, CfgNotImplemented},
  {"SCREEN", 1, CfgNotImplemented},   /* JPP */
  {"VERSION", 1, CfgNotImplemented},     /* JPP */
  {"ANYDOS", 1, CfgNotImplemented},       /* tom */
  {"IDLEHALT", 1, CfgNotImplemented},   /* ea  */

  {"DEVICE", 2, CfgNotImplemented},
  {"DEVICEHIGH", 2, CfgNotImplemented},
  {"INSTALL", 2, CmdInstall},
  {"INSTALLHIGH", 2, CmdInstallHigh},
  {"CHAIN", 2, CfgNotImplemented},
  {"SET", 2, CfgNotImplemented},
#else
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
#endif
  /* default action                                               */
  {"", -1, CfgFailure}
};

BYTE singleStep BSS_INIT(FALSE);        /* F8 processing */
BYTE SkipAllConfig BSS_INIT(FALSE);     /* F5 processing */
BYTE  MenuSelected BSS_INIT(0);

STATIC BOOL SkipLine(char *pLine)
{
  short key;
  COUNT i;
  signed char originalskipconfigseconds = InitKernelConfig.SkipConfigSeconds;

  if (originalskipconfigseconds >= 0)
  {
/// TODO:
#if 0
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
#endif
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

/*
    Files(pLine)/FilesHigh(pLine) - FILES=/FILESHIGH=: set the number
    of SFT entries to allocate.

    /// TODO: nothing in this codebase actually allocates a second
    /// (larger) SFT block sized to Config.cfgFiles yet (PreConfig2()
    /// is not implemented/called - see the comment on
    /// LoL->firstsftt's getddt()-style fixed 5-entry block earlier in
    /// this file), so this only records the requested value; it does
    /// not yet take effect.

    Migrated from config.c verbatim.
*/
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

/*
    CfgLastdrive(pLine)/CfgLastdriveHigh(pLine) - LASTDRIVE=/
    LASTDRIVEHIGH=: set the highest drive letter DOS will recognize.

    /// TODO: LoL->lastdrive/the CDS array are sized once in
    /// PreConfig() (before CONFIG.SYS is even read, see "use largest
    /// possible value for the initial CDS" in init_kernel() - it's
    /// already set to 26), so this only records Config.cfgLastdrive;
    /// nothing currently shrinks the live CDS array to match a
    /// smaller LASTDRIVE= value.

    Migrated from config.c verbatim.
*/
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

#ifdef MEMDISK_ARGS
  for (; !bEof || (mdsk != NULL); nCfgLine++)
#else
  for (; !bEof; nCfgLine++)
#endif
  {
    struct table *pEntry;
    char* szLine = ARM_PTR(x86_szLine);
    pLineStart = szLine;

#ifdef MEMDISK_ARGS
    if (!bEof)
    {
#endif

    /* read in a single line, \n or ^Z terminated */
    for (BYTE *pLine = szLine;;)
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
    BYTE* pLine = scan(szLine, szBuf, 1);

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
    /// TODO:
#if 0
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
#endif

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

    /// TODO: this codebase's C "kernel" runs as code that stands in
    /// for real x86 instructions, rather than as a guest program
    /// being interpreted - it is itself what services IRQ0 (timer)/
    /// IRQ1 (keyboard) on every emulator tick (see kernel.c's
    /// interrupt handlers). A real wait-for-N-seconds-or-keypress
    /// loop in C here would block those handlers from ever running
    /// again, freezing the timer and keyboard for both the guest and
    /// this loop itself - i.e. it would never see a keypress arrive
    /// or the timer advance, making a synchronous wait meaningless
    /// (see the discussion that led to this comment). The "real"
    /// fix is the same kind of CS:IP-parked BIOS callback this
    /// codebase's bios_19h.c (INT 19h, F5/F8-equivalent reboot
    /// timeout) already uses (set_bios_callback(), see i386.h) -
    /// but that requires unwinding this call back out to the
    /// emulator's main loop and resuming DoConfig()/SkipLine() later
    /// (a setjmp()/longjmp() or explicit state-machine
    /// restructuring), which is an architectural change well beyond
    /// this iteration's actual goal (loading CONFIG.SYS). So for
    /// now, this honestly always reports "no key" (the same value a
    /// real keyboard would eventually report on timeout), ignoring
    /// the requested timeout entirely - CONFIG.SYS always loads to
    /// completion with no way to interrupt it via F5/F8, rather than
    /// hanging or busy-looping pretending to wait.

    Migrated from config.c (signature only; body replaced as above).
*/
UWORD GetBiosKey(int timeout)
{
  UNREFERENCED_PARAMETER(timeout);
  return 0xffff;
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
