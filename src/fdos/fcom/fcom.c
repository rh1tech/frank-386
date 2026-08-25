/*
 * Native FreeCOM bootstrap (v.0.86 port to RP2350).
 */
#include <ctype.h>
#include <stddef.h>
#include <string.h>
/// do not include stdio.h, it conflicts with kernel headers
int snprintf(char *s, size_t n, const char *fmt, ...);
void *malloc(size_t n);
void free(void *p);

#include "fdos/hdrs.h"
#include "bios/bios.h"
#include "fdos/fcom/fcom.h"
#include "fdos/fcom/fcom_guest.h"
#include "fdos/mcb_proxy.h"
#include "fcom_help.inc"

#define FCOM_LINE_MAX          126u
#define FCOM_WORK_OFFSET       0x0100u
#define FCOM_STACK_GUARD       16u
#define FCOM_STACK_RESERVE     0x2000u
#define FCOM_ALIGN16(v)        (((v) + 15u) & ~15u)

struct fcom_guest;
static int fcom_write(CPU *cpu, UWORD command_psp, UWORD offset, UWORD count);
/* chkCBreak() modes - include/misc.h */
#define BREAK_BATCHFILE       1
#define BREAK_ENDOFBATCHFILES 2
#define BREAK_INPUT           3
#define BREAK_FORCMD          5
static int fcom_chk_cbreak(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, int mode);
static void build_tail(struct fcom_guest *g, const char *args);
static int exec_once(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                     UBYTE exec_mode);
static int execute_command_line(CPU *cpu, UWORD command_psp, struct fcom_guest *g, char *line);
static int execute_batch_file(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *name, const char *args);
static const char *command_basename(const char *name);
static void fcom_output_resource(CPU *cpu, UWORD command_psp,
                                 struct fcom_guest *g,
                                 const char *text);
static const char *fcom_command_help(const char *name);

#pragma pack(push, 1)
struct fcom_guest {
  keyboard input;
  exec_blk exec_block;
  CommandTail tail;
  fcb exec_fcb1;
  fcb exec_fcb2;
  char filename[128];
  char program[128];
  char init_tail[128];
  char text[160];
  char path[128];
  char path2[128];
  dmatch find;
  UBYTE io[256];
  UWORD owned_env_seg;
  UWORD owned_env_bytes;
  UBYTE persistent;
  UBYTE skip_autoexec;
  char autoexec_path[128];
  char comspec_arg[128];  /* SHELL= path word: COMSPEC source (FreeCom grabComFilename) */
  char batch_line[FCOM_LINE_MAX + 1];
  char batch_name[128];
  char batch_args[128];
  char batch_arg[128];
  char batch_goto[128];
  char if_left[128];
  char if_right[128];
  char for_item[128];
  char for_command[FCOM_LINE_MAX + 1];
  char redirect_command[FCOM_LINE_MAX + 1];
  char redirect_in[128];
  char redirect_out[128];
  char pipe_left[FCOM_LINE_MAX + 1];
  char pipe_right[FCOM_LINE_MAX + 1];
  char pipe_temp[128];
  UBYTE pipe_depth;
  UBYTE batch_active;
  UBYTE cbreak_leave_all;   /* chkCBreak() 'leaveAll': abort ALL batch files */
  UBYTE echo_enabled;
  UBYTE exit_requested;
  UBYTE exit_batch_only;
  UWORD exit_code;
  UBYTE errorlevel;
  UBYTE exit_reason;
  UWORD dir_stack_used;
  UWORD alias_used;
  UWORD history_used;
  UBYTE lfnfor_enabled;
  UBYTE lfncomplete_enabled;
  UBYTE fddebug_enabled;
  UBYTE trace_mode;
  UBYTE batch_depth;
  UBYTE batch_shiftlevel;
  UWORD active_saved_stdin;
  UWORD active_saved_stdout;
  UWORD saved_parent_psp;
  intvec saved_terminate_addr;
  UWORD fddebug_handle;
  char fddebug_name[128];
};
#pragma pack(push, 1)
struct fcom_batch_context {
  char name[128];
  char args[128];
  char goto_label[128];
  UBYTE active;
  UBYTE exit_batch_only;
  UBYTE shiftlevel;
};
#pragma pack(pop)

int fcom_is_command_com(const char *name)
{
  const char *base;
  if (name == NULL)
    return 0;
  base = command_basename(name);
  return strcasecmp(base, "COMMAND.COM") == 0;
}

#define FCOM_BATCH_CONTEXTS    8u
#define FCOM_BATCH_CONTEXT_BYTES \
  (FCOM_BATCH_CONTEXTS * sizeof(struct fcom_batch_context))

#define FCOM_DATA_END          (FCOM_WORK_OFFSET + sizeof(struct fcom_guest))
#define FCOM_DIR_STACK_BYTES   1024u
#define FCOM_ALIAS_BYTES       1024u
#define FCOM_HISTORY_BYTES     2048u
#define FCOM_LOADFIX_SLOTS     256u
#define FCOM_LOADFIX_BYTES     (FCOM_LOADFIX_SLOTS * sizeof(UWORD))
#define FCOM_DIR_STACK_OFFSET  FCOM_ALIGN16(FCOM_DATA_END)
#define FCOM_ALIAS_OFFSET      (FCOM_DIR_STACK_OFFSET + FCOM_DIR_STACK_BYTES)
#define FCOM_HISTORY_OFFSET    (FCOM_ALIAS_OFFSET + FCOM_ALIAS_BYTES)
#define FCOM_LOADFIX_OFFSET    (FCOM_HISTORY_OFFSET + FCOM_HISTORY_BYTES)
#define FCOM_BATCH_CONTEXT_OFFSET   FCOM_ALIGN16(FCOM_LOADFIX_OFFSET + FCOM_LOADFIX_BYTES)

/*
 * Reserve a real two-byte guest entry point after all persistent FCOM data.
 * It must not overlap FCOM_WORK_OFFSET: fcom_process_main() clears the whole
 * struct fcom_guest at startup.
 */
#define FCOM_ENTRY_OFFSET      FCOM_ALIGN16(FCOM_BATCH_CONTEXT_OFFSET + FCOM_BATCH_CONTEXT_BYTES)
#define FCOM_SHADOW_BYTES      (FCOM_ENTRY_OFFSET - FCOM_WORK_OFFSET)

/* One native shadow is enough even for nested COMMAND /C: nested execution is
 * synchronous.  fcom_guest_shadow_enter() flushes the suspended parent mirror
 * and restores it on leave, so parent C pointers continue to refer to this same
 * storage after the child returns. */
static UBYTE *fcom_persistent_shadow;

static void *fcom_shadow_at(UWORD command_psp, UWORD offset, size_t len)
{
  return fcom_guest_shadow_ptr(fcom_guest_linear(command_psp, offset), len);
}

static struct fcom_guest *fcom_state(UWORD command_psp)
{
  return (struct fcom_guest *)fcom_shadow_at(command_psp, FCOM_WORK_OFFSET,
                                              sizeof(struct fcom_guest));
}

#define FCOM_ENTRY_BYTES       2u
/* FreeCOM terminate hook (PSP:0Ah target while COMMAND self-parents):
   a real INT 20h in guest memory. */
#define FCOM_TERM_OFFSET       (FCOM_ENTRY_OFFSET + FCOM_ENTRY_BYTES)
#define FCOM_TERM_BYTES        2u
/*
 * FreeCOM's INT 23h handler (shell/cb_catch.asm cbreak_handler):
 *
 *     inc WORD [CS:CBreakCounter]
 *     clc                 ;; tell DOS to proceed
 *     retf 2
 *
 * The native shell has no code image of its own in guest memory, but the
 * Ctrl-Break handler MUST be genuine guest code: DOS (spawn_int23 /
 * handle_break in this port) invokes it through the IVT in guest context,
 * and its return style (RETF 2 with Carry clear = "resume") is what keeps
 * ^C from terminating the shell.  Nine bytes right after the JMP $ entry:
 *
 *     2E FF 06 ll hh      inc word [cs:FCOM_CBREAK_COUNT_OFFSET]
 *     F8                  clc
 *     CA 02 00            retf 2
 *
 * followed by the CBreakCounter word itself.  fcom_chk_cbreak() (the
 * chkCBreak() port) polls and clears the counter from native code.
 */
#define FCOM_CBREAK_STUB_OFFSET   (FCOM_TERM_OFFSET + FCOM_TERM_BYTES)
#define FCOM_CBREAK_STUB_BYTES    9u
#define FCOM_CBREAK_COUNT_OFFSET  (FCOM_CBREAK_STUB_OFFSET + FCOM_CBREAK_STUB_BYTES)
#define FCOM_CBREAK_AREA_END      (FCOM_CBREAK_COUNT_OFFSET + 2u)
#define FCOM_GUARD_OFFSET      FCOM_ALIGN16(FCOM_CBREAK_AREA_END)
#define FCOM_STACK_BOTTOM      (FCOM_GUARD_OFFSET + FCOM_STACK_GUARD)
#define FCOM_PROCESS_BYTES     FCOM_ALIGN16(FCOM_STACK_BOTTOM + FCOM_STACK_RESERVE)
#define FCOM_PROCESS_PARAS     (FCOM_PROCESS_BYTES >> 4)
#define FCOM_STACK_TOP         FCOM_PROCESS_BYTES
#define FCOM_STACK_BYTES       (FCOM_STACK_TOP - FCOM_STACK_BOTTOM)

_Static_assert(FCOM_PROCESS_BYTES <= 0xfff0u,
               "FCOM compact process exceeds a 16-bit segment");
_Static_assert(FCOM_STACK_BYTES >= FCOM_STACK_RESERVE,
               "FCOM guest stack reserve is too small");
_Static_assert((FCOM_GUARD_OFFSET & 15u) == 0,
               "FCOM stack guard is not paragraph-aligned");
_Static_assert(FCOM_ENTRY_OFFSET >= FCOM_DATA_END,
               "FCOM guest entry overlaps struct fcom_guest");
_Static_assert(FCOM_ENTRY_OFFSET >=
                   FCOM_BATCH_CONTEXT_OFFSET + FCOM_BATCH_CONTEXT_BYTES,
               "FCOM guest entry overlaps persistent context storage");
_Static_assert(FCOM_CBREAK_AREA_END <= FCOM_GUARD_OFFSET,
               "FCOM guest entry/terminate hook/cbreak stub overlaps stack guard");

static int int21_failed(CPU *cpu)
{
  return (cpu_getflags(cpu) & 1u) != 0;
}

static inline uint32_t fcom_gaddr(UWORD command_psp, size_t off)
{
  return fcom_guest_linear(command_psp,
      (UWORD)(FCOM_WORK_OFFSET + (UWORD)off));
}

static inline UBYTE fcom_gread8(UWORD command_psp, size_t off)
{
  return fcom_guest_read8(fcom_gaddr(command_psp, off));
}

static inline void fcom_gwrite8(UWORD command_psp, size_t off, UBYTE value)
{
  fcom_guest_write8(fcom_gaddr(command_psp, off), value);
}

static uint32_t fcom_gskip_space(uint32_t p)
{
  UBYTE c;
  while ((c = fcom_guest_read8(p)) == ' ' || c == '\t')
    ++p;
  return p;
}

static size_t fcom_gstr_copy_from_host(UWORD command_psp, size_t off,
                                      size_t cap, const char *src)
{
  size_t n = strlen(src);
  uint32_t dst = fcom_gaddr(command_psp, off);

  if (n >= cap)
    n = cap - 1u;
  if (n)
    fcom_guest_write(dst, src, n);
  fcom_guest_write8(dst + (uint32_t)n, 0);
  return n;
}

static int fcom_gstr_eq_label(UWORD command_psp, size_t line_off,
                              size_t want_off)
{
  uint32_t p = fcom_gaddr(command_psp, line_off);
  uint32_t want = fcom_gaddr(command_psp, want_off);
  uint32_t end;
  size_t len;
  size_t want_len;

  while (fcom_guest_read8(p) == ' ' || fcom_guest_read8(p) == '\t')
    ++p;
  if (fcom_guest_read8(p) != ':')
    return 0;
  ++p;
  while (fcom_guest_read8(p) == ' ' || fcom_guest_read8(p) == '\t')
    ++p;

  end = p;
  while (fcom_guest_read8(end) != 0 &&
         fcom_guest_read8(end) != ' ' &&
         fcom_guest_read8(end) != '\t')
    ++end;

  len = (size_t)(end - p);
  want_len = fcom_guest_strnlen(want, 128u);
  if (len != want_len)
    return 0;

  while (len--) {
    if (toupper((unsigned char)fcom_guest_read8(p++)) !=
        toupper((unsigned char)fcom_guest_read8(want++)))
      return 0;
  }
  return 1;
}


static char *fcom_dir_stack_storage(UWORD command_psp)
{
  return (char *)fcom_shadow_at(command_psp, FCOM_DIR_STACK_OFFSET, FCOM_DIR_STACK_BYTES);
}


static char *fcom_alias_storage(UWORD command_psp)
{
  return (char *)fcom_shadow_at(command_psp, FCOM_ALIAS_OFFSET, FCOM_ALIAS_BYTES);
}


static char *fcom_history_storage(UWORD command_psp)
{
  return (char *)fcom_shadow_at(command_psp, FCOM_HISTORY_OFFSET, FCOM_HISTORY_BYTES);
}


static UWORD *fcom_loadfix_storage(UWORD command_psp)
{
  return (UWORD *)fcom_shadow_at(command_psp, FCOM_LOADFIX_OFFSET, FCOM_LOADFIX_BYTES);
}


static uint32_t fcom_cbreak_counter_addr(UWORD command_psp)
{
  return fcom_guest_linear(command_psp, FCOM_CBREAK_COUNT_OFFSET);
}

static void init_stack_guard(UWORD command_psp)
{
  fcom_guest_fill(fcom_guest_linear(command_psp, FCOM_GUARD_OFFSET),
                  0xa5, FCOM_STACK_GUARD);
}

static int stack_guard_ok(UWORD command_psp)
{
  uint32_t p = fcom_guest_linear(command_psp, FCOM_GUARD_OFFSET);
  unsigned i;

  for (i = 0; i < FCOM_STACK_GUARD; ++i) {
    if (fcom_guest_read8(p + i) != 0xa5)
      return 0;
  }
  return 1;
}

static void fcom_intcall(CPU *cpu, UWORD command_psp, UBYTE intno,
                         const char *owner)
{
  UWORD old_ss = CPU_SS;
  UWORD old_sp = CPU_SP;

  /*
   * The native shell owns only a compact guest block.  Its guest stack
   * starts at the paragraph-aligned end of that block and grows down
   * toward the guard; it must never address the released tail of the
   * segment.
   */
  SET_SS(command_psp);
  CPU_SP = FCOM_STACK_TOP;
  fcom_guest_shadow_sync_to_guest();
  bios_intcall(cpu, intno, owner);
  fcom_guest_shadow_sync_from_guest();
  SET_SS(old_ss);
  CPU_SP = old_sp;

  if (!stack_guard_ok(command_psp)) {
    dos_printf("FCOM: guest stack overflow below %04x:%04x\n",
               command_psp, (unsigned)FCOM_STACK_BOTTOM);
    for (;;)
      bios_intcall(cpu, 0x28, "FCOM stack overflow idle");
  }
}

static void dos_puts(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                     const char *s)
{
  size_t n = strlen(s);
  if (n >= sizeof(g->text))
    n = sizeof(g->text) - 1;
  memcpy(g->text, s, n);
  g->text[n++] = '$';
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text);
  CPU_AH = 0x09;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM output");
}

static unsigned fcom_readline(CPU *cpu, UWORD command_psp, struct fcom_guest *g)
{
  g->input.kb_size = FCOM_LINE_MAX;
  g->input.kb_count = 0;
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, input);
  CPU_AH = 0x0a;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM input");

  /*
   * cmdinput.c:188: if(cbreak) ch = KEY_CTL_C; - a ^C/^Break during line
   * input abandons the line.  The INT 23h stub incremented CBreakCounter
   * and DOS resumed the read; poll-and-clear here (chkCBreak(BREAK_INPUT))
   * and hand back an empty line so the main loop redisplays the prompt.
   */
  if (fcom_chk_cbreak(cpu, command_psp, g, BREAK_INPUT)) {
    dos_puts(cpu, command_psp, g, "\n");
    g->input.kb_count = 0;
    g->input.kb_buf[0] = '\0';
    return 0;
  }

  /*
   * DOS buffered input echoes Enter as CR. Complete the console
   * newline with LF so command output starts on the next row.
   */
  dos_puts(cpu, command_psp, g, "\n");

  if (g->input.kb_count > FCOM_LINE_MAX)
    g->input.kb_count = FCOM_LINE_MAX;
  g->input.kb_buf[g->input.kb_count] = '\0';
  return g->input.kb_count;
}

static char *skip_space(char *p)
{
  while (*p == ' ' || *p == '\t')
    ++p;
  return p;
}

static int command_is(const char *name, const char *command)
{
  return strcasecmp(name, command) == 0;
}

static const char *command_basename(const char *name)
{
  const char *base = name;
  const char *p;

  for (p = name; *p != '\0'; ++p) {
    if (*p == '\\' || *p == '/' || *p == ':')
      base = p + 1;
  }

  return base;
}

static int is_native_command_name(const char *name)
{
  const char *base = command_basename(name);

  /*
   * Compatibility workaround: COMMAND is always the native shell, even
   * when CONFIG.SYS/BAT/software specifies a path to COMMAND.COM.
   * No other internal command receives this basename-only treatment.
   *
   * TODO(kernel): guest programs may bypass this parser and call
   * INT 21h/AX=4B00h directly with COMSPEC (for example Norton).
   * DosExec() must eventually intercept Load-and-Go of COMMAND.COM,
   * extract ep->exec.cmd_line and invoke one native fcom_run().
   * Do not intercept EXEC_LOAD or EXEC_OVERLAY.
   */
  return strcasecmp(base, "COMMAND") == 0 ||
         strcasecmp(base, "COMMAND.COM") == 0;
}

static int drive_command(const char *name)
{
  return isalpha((unsigned char)name[0]) &&
         name[1] == ':' &&
         name[2] == '\0';
}

static int dos_set_drive(CPU *cpu, UWORD command_psp, unsigned drive)
{
  CPU_DL = (UBYTE)drive;
  CPU_AH = 0x0e;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM set drive");
  return drive < CPU_AL;
}

static unsigned dos_get_drive(CPU *cpu, UWORD command_psp)
{
  CPU_AH = 0x19;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM get drive");
  return CPU_AL;
}

static int dos_change_dir(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, const char *path)
{
  size_t n = strlen(path);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, path, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AH = 0x3b;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CHDIR");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int dos_get_cwd(CPU *cpu, UWORD command_psp,
                       struct fcom_guest *g, unsigned drive)
{
  memset(g->path, 0, sizeof(g->path));
  SET_DS(command_psp);
  CPU_SI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_DL = (UBYTE)drive;
  CPU_AH = 0x47;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM get CWD");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static char *fcom_env_alloc_value(UWORD command_psp, const char *name,
                                  size_t max_size)
{
  const UWORD env_seg = fcom_guest_psp_environment(command_psp);
  const size_t name_len = strlen(name);
  uint32_t env;
  unsigned left = 0x8000u;

  if (max_size == 0 || env_seg == 0)
    return NULL;

  env = fcom_guest_linear(env_seg, 0);
  while (left != 0 && fcom_guest_read8(env) != 0) {
    size_t n = fcom_guest_strnlen(env, left);

    if (n == left)
      break;
    if (n > name_len &&
        fcom_guest_env_name_matches(env, n, name, name_len)) {
      size_t value_len = n - name_len - 1u;
      char *dst;

      /* Keep the old size limits without making transient environment
       * values resident members of struct fcom_guest. */
      if (value_len >= max_size)
        return NULL;

      dst = (char *)malloc(value_len + 1u);
      if (dst == NULL)
        return NULL;

      fcom_guest_read(env + (uint32_t)name_len + 1u, dst, value_len);
      dst[value_len] = '\0';
      return dst;
    }
    env += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }
  return NULL;
}

static void prompt_append_char(char **dst, size_t *left, char ch)
{
  if (*left > 1) {
    *(*dst)++ = ch;
    --*left;
  }
}

static void prompt_append_text(char **dst, size_t *left, const char *text)
{
  while (*text != '\0' && *left > 1) {
    *(*dst)++ = *text++;
    --*left;
  }
}

static void prompt_append_path(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               char **dst, size_t *left)
{
  unsigned drive = dos_get_drive(cpu, command_psp);

  prompt_append_char(dst, left, (char)('A' + drive));
  prompt_append_char(dst, left, ':');
  prompt_append_char(dst, left, '\\');

  if (dos_get_cwd(cpu, command_psp, g, 0) == 0 &&
      g->path[0] != '\0')
    prompt_append_text(dst, left, g->path);
}

static void prompt_append_date(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               char **dst, size_t *left)
{
  int n;

  CPU_AH = 0x2a;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM PROMPT date");

  n = snprintf(g->path, sizeof(g->path), "%02u-%02u-%04u",
               (unsigned)CPU_DH, (unsigned)CPU_DL, (unsigned)CPU_CX);
  if (n > 0)
    prompt_append_text(dst, left, g->path);
}

static void prompt_append_time(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               char **dst, size_t *left)
{
  int n;

  CPU_AH = 0x2c;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM PROMPT time");

  n = snprintf(g->path, sizeof(g->path), "%02u:%02u:%02u.%02u",
               (unsigned)CPU_CH, (unsigned)CPU_CL,
               (unsigned)CPU_DH, (unsigned)CPU_DL);
  if (n > 0)
    prompt_append_text(dst, left, g->path);
}

static void show_prompt(CPU *cpu, UWORD command_psp, struct fcom_guest *g)
{
  char *env_format = fcom_env_alloc_value(command_psp, "PROMPT", 256u);
  const char *format = env_format;
  char *dst = g->text;
  size_t left = sizeof(g->text);

  if (format == NULL || *format == '\0')
    format = "$P$G";

  while (*format != '\0' && left > 1) {
    char code;

    if (*format != '$') {
      prompt_append_char(&dst, &left, *format++);
      continue;
    }

    ++format;
    if (*format == '\0') {
      prompt_append_char(&dst, &left, '$');
      break;
    }

    code = (char)toupper((unsigned char)*format++);
    switch (code) {
    case 'P':
      prompt_append_path(cpu, command_psp, g, &dst, &left);
      break;
    case 'N':
      prompt_append_char(&dst, &left,
                         (char)('A' + dos_get_drive(cpu, command_psp)));
      break;
    case 'G':
      prompt_append_char(&dst, &left, '>');
      break;
    case 'L':
      prompt_append_char(&dst, &left, '<');
      break;
    case 'B':
      prompt_append_char(&dst, &left, '|');
      break;
    case 'Q':
      prompt_append_char(&dst, &left, '=');
      break;
    case '$':
      prompt_append_char(&dst, &left, '$');
      break;
    case '_':
      prompt_append_text(&dst, &left, "\r\n");
      break;
    case 'S':
      prompt_append_char(&dst, &left, ' ');
      break;
    case 'D':
      prompt_append_date(cpu, command_psp, g, &dst, &left);
      break;
    case 'T':
      prompt_append_time(cpu, command_psp, g, &dst, &left);
      break;
    case 'V':
      prompt_append_text(&dst, &left, "FreeCOM 0.86");
      break;
    case 'E':
      prompt_append_char(&dst, &left, 0x1b);
      break;
    case 'H':
      prompt_append_char(&dst, &left, '\b');
      break;
    default:
      prompt_append_char(&dst, &left, '$');
      prompt_append_char(&dst, &left, code);
      break;
    }
  }

  *dst = '\0';
  (void)fcom_write(cpu, command_psp,
                   FCOM_WORK_OFFSET +
                     (UWORD)offsetof(struct fcom_guest, text),
                   (UWORD)(dst - g->text));
  free(env_format);
}

static void ensure_prompt_column_zero(CPU *cpu, UWORD command_psp,
                                      struct fcom_guest *g)
{
  /*
   * FreeCOM starts a new row only when the previous program left the
   * cursor away from column zero. BIOS returns zero-based column in DL.
   */
  CPU_BH = 0;
  CPU_AH = 0x03;
  fcom_intcall(cpu, command_psp, 0x10, "FCOM cursor position");

  if (CPU_DL != 0)
    dos_puts(cpu, command_psp, g, "\n");
}

static int fcom_handle_attributes(CPU *cpu, UWORD command_psp,
                                  UWORD handle, UWORD *attributes)
{
  CPU_BX = handle;
  CPU_AX = 0x4400;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM IOCTL get attributes");

  if (int21_failed(cpu))
    return 0;

  *attributes = CPU_DX;
  return 1;
}

static void clear_screen(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g)
{
  UWORD attributes;

  /*
   * cmd/cls.c always emits form feed first. The BIOS clear is used only
   * when stdout is the standard CON device.
   */
  g->io[0] = 0x0c;
  (void)fcom_write(cpu, command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);

  if (!fcom_handle_attributes(cpu, command_psp, 1, &attributes))
    return;

  if ((attributes & 0x009fu) == 0x0093u) {
    unsigned mode;
    UWORD fill = 0x0700u;

    CPU_AH = 0x0f;
    fcom_intcall(cpu, command_psp, 0x10, "FCOM CLS video mode");
    mode = CPU_AL & 0x7fu;

    switch (mode) {
    case 0x04:
    case 0x05:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x0d:
    case 0x0e:
    case 0x0f:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x59:
      fill = 0;
      break;
    default:
      break;
    }

    CPU_AX = 0x0600;
    CPU_BX = fill;
    CPU_CX = 0x0000;
    CPU_DX = 0x184f;
    fcom_intcall(cpu, command_psp, 0x10, "FCOM CLS scroll");

    CPU_BH = 0;
    CPU_DX = 0;
    CPU_AH = 0x02;
    fcom_intcall(cpu, command_psp, 0x10, "FCOM CLS home");
  } else if ((attributes & 0x009cu) == 0x0080u) {
    g->io[0] = 0x1b;
    g->io[1] = '[';
    g->io[2] = '2';
    g->io[3] = 'J';
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 4);
  }
}

static void pause_command(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, const char *args)
{
  const char *message = skip_space((char *)args);

  if (*message != '\0')
    dos_puts(cpu, command_psp, g, message);
  else
    dos_puts(cpu, command_psp, g,
             "Press any key to continue . . .");

  CPU_AH = 0x08;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM PAUSE");

  dos_puts(cpu, command_psp, g, "\n");
}


static int fcom_write_handle(CPU *cpu, UWORD command_psp,
                             UWORD handle, UWORD offset, UWORD count)
{
  SET_DS(command_psp);
  CPU_DX = offset;
  CPU_BX = handle;
  CPU_CX = count;
  CPU_AH = 0x40;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM write");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int fcom_write(CPU *cpu, UWORD command_psp, UWORD offset, UWORD count)
{
  return fcom_write_handle(cpu, command_psp, 1, offset, count);
}


static void error_bad_command(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    n = sizeof(g->path) - 1;

  memcpy(g->path, name, n);
  g->path[n] = '\0';

  /* FreeCOM 0.86 TEXT_ERROR_BADCOMMAND. */
  dos_puts(cpu, command_psp, g, "Bad command or filename - \"");
  dos_puts(cpu, command_psp, g, g->path);
  dos_puts(cpu, command_psp, g, "\".\r\n");
}

static void restore_default_dta(CPU *cpu, UWORD command_psp)
{
  SET_DS(command_psp);
  CPU_DX = (UWORD)offsetof(psp, ps_cmd);
  CPU_AH = 0x1a;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM restore DTA");
}

static int set_find_dta(CPU *cpu, UWORD command_psp)
{
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, find);
  CPU_AH = 0x1a;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM set find DTA");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int dos_find_first_attr(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               const char *pattern, UWORD attributes)
{
  size_t n = strlen(pattern);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, pattern, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_CX = attributes;
  CPU_AH = 0x4e;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM find first");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int dos_find_first(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, const char *pattern)
{
  return dos_find_first_attr(cpu, command_psp, g, pattern, 0x37);
}

static int dos_find_next(CPU *cpu, UWORD command_psp)
{
  CPU_AH = 0x4f;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM find next");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

struct fcom_dir_options {
  UBYTE bare;
  UBYTE wide;
  UBYTE pause;
  UBYTE required_attr;
  UBYTE excluded_attr;
  char *pattern;
};

static char *fcom_dir_next_argument(char **cursor)
{
  char *p = skip_space(*cursor);
  char *start;

  if (*p == '\0') {
    *cursor = p;
    return NULL;
  }

  if (*p == '"') {
    start = ++p;
    while (*p != '\0' && *p != '"')
      ++p;
    if (*p == '"')
      *p++ = '\0';
  } else {
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
      ++p;
    if (*p != '\0')
      *p++ = '\0';
  }

  *cursor = p;
  return start;
}

static int fcom_dir_attr_bit(int ch, UBYTE *bit)
{
  switch (toupper((unsigned char)ch)) {
  case 'R':
    *bit = 0x01;
    return 1;
  case 'H':
    *bit = 0x02;
    return 1;
  case 'S':
    *bit = 0x04;
    return 1;
  case 'D':
    *bit = 0x10;
    return 1;
  case 'A':
    *bit = 0x20;
    return 1;
  default:
    return 0;
  }
}

static int fcom_parse_dir_attr(char *p, struct fcom_dir_options *options)
{
  int exclude = 0;

  if (*p == ':')
    ++p;

  if (*p == '\0')
    return 1;

  while (*p != '\0') {
    UBYTE bit;

    if (*p == '-') {
      exclude = 1;
      ++p;
      continue;
    }

    if (!fcom_dir_attr_bit((unsigned char)*p++, &bit))
      return 0;

    if (exclude) {
      options->excluded_attr |= bit;
      options->required_attr &= (UBYTE)~bit;
    } else {
      options->required_attr |= bit;
      options->excluded_attr &= (UBYTE)~bit;
    }
  }

  return 1;
}

static int fcom_parse_dir_options(struct fcom_guest *g, const char *args,
                                  struct fcom_dir_options *options)
{
  char *cursor;
  char *arg;

  memset(options, 0, sizeof(*options));

  if (strlen(args) >= sizeof(g->redirect_command))
    return 0;

  strcpy(g->redirect_command, args);
  cursor = g->redirect_command;

  while ((arg = fcom_dir_next_argument(&cursor)) != NULL) {
    if ((arg[0] == '/' || arg[0] == '-') && arg[1] != '\0') {
      char *p = arg + 1;

      switch (toupper((unsigned char)*p++)) {
      case 'B':
        if (*p != '\0')
          return 0;
        options->bare = 1;
        break;

      case 'W':
        if (*p != '\0')
          return 0;
        options->wide = 1;
        break;

      case 'P':
        if (*p != '\0')
          return 0;
        options->pause = 1;
        break;

      case 'A':
        if (!fcom_parse_dir_attr(p, options))
          return 0;
        break;

      default:
        return 0;
      }
    } else {
      if (options->pattern != NULL)
        return 0;
      options->pattern = arg;
    }
  }

  if (options->pattern == NULL)
    options->pattern = "*.*";

  if (options->bare)
    options->wide = 0;

  return 1;
}

static int fcom_dir_attr_matches(const struct fcom_dir_options *options,
                                 UBYTE attributes)
{
  if ((attributes & options->required_attr) != options->required_attr)
    return 0;
  if ((attributes & options->excluded_attr) != 0)
    return 0;
  return 1;
}

static unsigned fcom_dir_write_entry(CPU *cpu, UWORD command_psp,
                                     struct fcom_guest *g,
                                     const struct fcom_dir_options *options,
                                     unsigned wide_column)
{
  int n;

  if (options->bare) {
    n = snprintf(g->text, sizeof(g->text), "%s\r\n",
                 g->find.dm_name);
  } else if (options->wide) {
    if (g->find.dm_attr_fnd & D_DIR)
      n = snprintf(g->text, sizeof(g->text), "[%-12s]",
                   g->find.dm_name);
    else
      n = snprintf(g->text, sizeof(g->text), "%-14s",
                   g->find.dm_name);

    if (n > 0) {
      fcom_write(cpu, command_psp,
                 FCOM_WORK_OFFSET +
                   (UWORD)offsetof(struct fcom_guest, text),
                 (UWORD)n);
    }

    ++wide_column;
    if (wide_column >= 5) {
      dos_puts(cpu, command_psp, g, "\r\n");
      wide_column = 0;
    }
    return wide_column;
  } else if (g->find.dm_attr_fnd & D_DIR) {
    n = snprintf(g->text, sizeof(g->text), "%-13s <DIR>\r\n",
                 g->find.dm_name);
  } else {
    n = snprintf(g->text, sizeof(g->text), "%-13s %10lu\r\n",
                 g->find.dm_name, (unsigned long)g->find.dm_size);
  }

  if (n > 0) {
    if ((size_t)n >= sizeof(g->text))
      n = (int)sizeof(g->text) - 1;
    fcom_write(cpu, command_psp,
               FCOM_WORK_OFFSET +
                 (UWORD)offsetof(struct fcom_guest, text),
               (UWORD)n);
  }

  return wide_column;
}

static void builtin_dir(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, const char *args)
{
  struct fcom_dir_options options;
  unsigned displayed_lines = 0;
  unsigned wide_column = 0;
  int rc;

  if (!fcom_parse_dir_options(g, args, &options)) {
    dos_puts(cpu, command_psp, g, "Invalid DIR parameter.\r\n");
    return;
  }

  memset(&g->find, 0, sizeof(g->find));
  if (set_find_dta(cpu, command_psp) < 0) {
    dos_puts(cpu, command_psp, g, "Unable to set DTA\r\n");
    return;
  }

  rc = dos_find_first(cpu, command_psp, g, options.pattern);
  if (rc < 0) {
    restore_default_dta(cpu, command_psp);
    dos_puts(cpu, command_psp, g, "File not found\r\n");
    return;
  }

  do {
    if (strcmp(g->find.dm_name, ".") != 0 &&
        strcmp(g->find.dm_name, "..") != 0 &&
        fcom_dir_attr_matches(&options, g->find.dm_attr_fnd)) {
      wide_column = fcom_dir_write_entry(
          cpu, command_psp, g, &options, wide_column);

      if (!options.wide || wide_column == 0)
        ++displayed_lines;

      if (options.pause && displayed_lines >= 23) {
        if (options.wide && wide_column != 0) {
          dos_puts(cpu, command_psp, g, "\r\n");
          wide_column = 0;
        }
        pause_command(cpu, command_psp, g, "");
        displayed_lines = 0;
      }
    }

    rc = dos_find_next(cpu, command_psp);
  } while (rc == 0);

  if (options.wide && wide_column != 0)
    dos_puts(cpu, command_psp, g, "\r\n");

  restore_default_dta(cpu, command_psp);
}


static int dos_open_read(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;
  memcpy(g->path, name, n + 1);

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AX = 0x3d00;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM open");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int fcom_read(CPU *cpu, UWORD command_psp,
                    struct fcom_guest *g, UWORD handle)
{
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io);
  CPU_BX = handle;
  CPU_CX = sizeof(g->io);
  CPU_AH = 0x3f;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM read");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static void fcom_close(CPU *cpu, UWORD command_psp, UWORD handle)
{
  CPU_BX = handle;
  CPU_AH = 0x3e;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM close");
}

static char *fcom_type_next_argument(char **cursor)
{
  char *p = skip_space(*cursor);
  char *start;

  if (*p == '\0') {
    *cursor = p;
    return NULL;
  }

  if (*p == '"') {
    start = ++p;
    while (*p != '\0' && *p != '"')
      ++p;
    if (*p == '"')
      *p++ = '\0';
  } else {
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
      ++p;
    if (*p != '\0')
      *p++ = '\0';
  }

  *cursor = p;
  return start;
}

static int fcom_type_one(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, const char *name)
{
  int handle;
  int count;

  handle = dos_open_read(cpu, command_psp, g, name);
  if (handle < 0) {
    dos_puts(cpu, command_psp, g, "File not found.\r\n");
    return handle;
  }

  while ((count = fcom_read(cpu, command_psp, g, (UWORD)handle)) >= 0) {
    UWORD begin = 0;
    UWORD i;

    for (i = 0; i < (UWORD)count; ++i) {
      if (g->io[i] == 0x1a)
        break;

      if (g->io[i] == '\r' || g->io[i] == '\n') {
        if (i > begin) {
          int written = fcom_write(
              cpu, command_psp,
              FCOM_WORK_OFFSET +
                (UWORD)offsetof(struct fcom_guest, io) + begin,
              (UWORD)(i - begin));

          if (written < 0 || written != (int)(i - begin)) {
            fcom_close(cpu, command_psp, (UWORD)handle);
            return -5;
          }
        }

        if (g->io[i] == '\n') {
          g->text[0] = '\r';
          g->text[1] = '\n';
          if (fcom_write(
                  cpu, command_psp,
                  FCOM_WORK_OFFSET +
                    (UWORD)offsetof(struct fcom_guest, text),
                  2) != 2) {
            fcom_close(cpu, command_psp, (UWORD)handle);
            return -5;
          }
        }

        begin = i + 1;
      }
    }

    if (i > begin) {
      int written = fcom_write(
          cpu, command_psp,
          FCOM_WORK_OFFSET +
            (UWORD)offsetof(struct fcom_guest, io) + begin,
          (UWORD)(i - begin));

      if (written < 0 || written != (int)(i - begin)) {
        fcom_close(cpu, command_psp, (UWORD)handle);
        return -5;
      }
    }

    if (count == 0 || i != (UWORD)count)
      break;
  }

  fcom_close(cpu, command_psp, (UWORD)handle);
  return count < 0 ? count : 0;
}

static void builtin_type(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *name;
  unsigned argc = 0;

  while ((name = fcom_type_next_argument(&cursor)) != NULL) {
    ++argc;

    if (fcom_type_one(cpu, command_psp, g, name) < 0)
      break;
  }

  if (argc == 0)
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
}


static uint32_t environment_start_linear(UWORD command_psp)
{
  UWORD env_seg = fcom_guest_psp_environment(command_psp);
  return env_seg ? fcom_guest_linear(env_seg, 0) : FCOM_GUEST_NULL;
}

static int guest_alloc(CPU *cpu, UWORD command_psp, UWORD paras)
{
  CPU_BX = paras;
  CPU_AH = 0x48;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM allocate");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static void guest_free(CPU *cpu, UWORD command_psp, UWORD segment)
{
  SET_ES(segment);
  CPU_AH = 0x49;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM free");
}

static int environment_layout(UWORD command_psp,
                              size_t *variables_bytes,
                              size_t *trailer_bytes,
                              uint32_t program_dst, size_t program_name_size)
{
  uint32_t start = environment_start_linear(command_psp);
  uint32_t p = start;
  unsigned left = 0x8000u;
  UWORD extra_count;
  size_t trailer = 2;
  unsigned i;

  *variables_bytes = 0;
  *trailer_bytes = 0;
  if (program_name_size && program_dst != FCOM_GUEST_NULL)
    fcom_guest_write8(program_dst, 0);

  if (start == FCOM_GUEST_NULL)
    return 0;

  while (left != 0) {
    size_t n = fcom_guest_strnlen(p, left);

    if (n == left)
      return -1;
    p += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
    if (n == 0) {
      if (p == start + 1u) {
        if (left == 0 || fcom_guest_read8(p) != '\0')
          return -1;
        ++p;
        --left;
      }
      break;
    }
  }

  if (left < 2)
    return -1;

  *variables_bytes = (size_t)(p - start);
  extra_count = (UWORD)fcom_guest_read8(p) |
                ((UWORD)fcom_guest_read8(p + 1u) << 8);
  p += 2u;
  left -= 2;

  for (i = 0; i < extra_count; ++i) {
    size_t n = fcom_guest_strnlen(p, left);

    if (n == left)
      return -1;
    if (i == 0 && program_name_size && program_dst != FCOM_GUEST_NULL) {
      size_t copy = n < program_name_size - 1u ? n : program_name_size - 1u;
      fcom_guest_copy(program_dst, p, copy);
      fcom_guest_write8(program_dst + (uint32_t)copy, 0);
    }
    trailer += n + 1u;
    p += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }

  *trailer_bytes = trailer;
  return 0;
}

static int replace_environment_variable(CPU *cpu, UWORD command_psp,
                                        const char *assignment)
{
  char assignment_copy[160];
  const char *equal;
  const char *value;

  {
    size_t assignment_len = strlen(assignment);
    if (assignment_len >= sizeof(assignment_copy))
      return -8;
    memcpy(assignment_copy, assignment, assignment_len + 1u);
    assignment = assignment_copy;
  }

  equal = strchr(assignment, '=');
  uint32_t src;
  size_t name_len;
  size_t value_len;
  size_t variables_bytes;
  size_t trailer_bytes;
  size_t copied_bytes = 0;
  size_t entry_count = 0;
  size_t required;
  unsigned left;
  UWORD paras;
  int allocated;
  UWORD new_seg;
  uint32_t dst;
  size_t pos = 0;
  uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  UWORD old_owned_env = fcom_guest_read16(
      gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_seg));

  if (equal == NULL)
    return -1;

  name_len = (size_t)(equal - assignment);
  value = equal + 1;
  value_len = strlen(value);

  if (name_len == 0 || memchr(assignment, ' ', name_len) != NULL ||
      memchr(assignment, '\t', name_len) != NULL)
    return -1;

  if (environment_layout(command_psp, &variables_bytes,
                         &trailer_bytes, FCOM_GUEST_NULL, 0) < 0)
    return -1;

  src = environment_start_linear(command_psp);
  left = (unsigned)variables_bytes;
  while (src != FCOM_GUEST_NULL && left > 1u && fcom_guest_read8(src)) {
    size_t n = fcom_guest_strnlen(src, left);

    if (n == left)
      return -1;

    if (!fcom_guest_env_name_matches(src, n, assignment, name_len)) {
      copied_bytes += n + 1u;
      ++entry_count;
    }

    src += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }

  if (value_len != 0) {
    copied_bytes += name_len + 1u + value_len + 1u;
    ++entry_count;
  }

  required = copied_bytes + (entry_count ? 1u : 2u) + trailer_bytes;
  if (required > 0xfff0u)
    return -8;

  paras = (UWORD)((required + 15u) >> 4);
  allocated = guest_alloc(cpu, command_psp, paras);
  if (allocated < 0)
    return allocated;

  new_seg = (UWORD)allocated;
  dst = fcom_guest_linear(new_seg, 0);

  src = environment_start_linear(command_psp);
  left = (unsigned)variables_bytes;
  while (src != FCOM_GUEST_NULL && left > 1u && fcom_guest_read8(src)) {
    size_t n = fcom_guest_strnlen(src, left);

    if (n == left) {
      guest_free(cpu, command_psp, new_seg);
      return -1;
    }

    if (!fcom_guest_env_name_matches(src, n, assignment, name_len)) {
      fcom_guest_copy(dst + (uint32_t)pos, src, n + 1u);
      pos += n + 1u;
    }

    src += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }

  if (value_len != 0) {
    fcom_guest_write(dst + (uint32_t)pos, assignment, name_len);
    pos += name_len;
    {
      const char eq = '=';
      fcom_guest_write(dst + (uint32_t)pos, &eq, 1);
    }
    ++pos;
    fcom_guest_write(dst + (uint32_t)pos, value, value_len);
    pos += value_len;
    {
      const char zero = '\0';
      fcom_guest_write(dst + (uint32_t)pos, &zero, 1);
    }
    ++pos;
  }

  {
    const char zero = '\0';
    if (pos == 0)
      fcom_guest_write(dst + (uint32_t)pos++, &zero, 1);
    fcom_guest_write(dst + (uint32_t)pos++, &zero, 1);
  }

  src = environment_start_linear(command_psp);
  if (src != FCOM_GUEST_NULL) {
    fcom_guest_copy(dst + (uint32_t)pos,
                    src + (uint32_t)variables_bytes, trailer_bytes);
    pos += trailer_bytes;
  } else {
    const char zeros[2] = {0, 0};
    fcom_guest_write(dst + (uint32_t)pos, zeros, sizeof(zeros));
    pos += sizeof(zeros);
  }

  fcom_guest_psp_set_environment(command_psp, new_seg);

  if (old_owned_env != 0)
    guest_free(cpu, command_psp, old_owned_env);

  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_seg),
                     new_seg);
  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_bytes),
                     (UWORD)(paras << 4));
  return 0;
}


static int fcom_guest_assignment_name_matches(uint32_t env_entry,
                                               size_t env_len,
                                               uint32_t assignment,
                                               size_t name_len)
{
  size_t i;

  if (env_len <= name_len ||
      fcom_guest_read8(env_entry + (uint32_t)name_len) != '=')
    return 0;

  for (i = 0; i < name_len; ++i) {
    UBYTE a = fcom_guest_read8(env_entry + (uint32_t)i);
    UBYTE b = fcom_guest_read8(assignment + (uint32_t)i);
    if (toupper((unsigned char)a) != toupper((unsigned char)b))
      return 0;
  }
  return 1;
}

static int replace_environment_variable_guest(CPU *cpu, UWORD command_psp,
                                               uint32_t assignment,
                                               size_t assignment_max)
{
  uint32_t src;
  uint32_t equal;
  size_t assignment_len;
  size_t name_len;
  size_t value_len;
  size_t variables_bytes;
  size_t trailer_bytes;
  size_t copied_bytes = 0;
  size_t entry_count = 0;
  size_t required;
  unsigned left;
  UWORD paras;
  int allocated;
  UWORD new_seg;
  uint32_t dst;
  size_t pos = 0;
  uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  UWORD old_owned_env = fcom_guest_read16(
      gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_seg));

  assignment_len = fcom_guest_strnlen(assignment, assignment_max);
  if (assignment_len == assignment_max)
    return -8;

  equal = assignment;
  while (equal < assignment + assignment_len &&
         fcom_guest_read8(equal) != '=')
    ++equal;
  if (equal == assignment || equal >= assignment + assignment_len)
    return -1;

  name_len = (size_t)(equal - assignment);
  value_len = assignment_len - name_len - 1u;

  {
    size_t i;
    for (i = 0; i < name_len; ++i) {
      UBYTE c = fcom_guest_read8(assignment + (uint32_t)i);
      if (c == ' ' || c == '\t')
        return -1;
    }
  }

  if (environment_layout(command_psp, &variables_bytes,
                         &trailer_bytes, FCOM_GUEST_NULL, 0) < 0)
    return -1;

  src = environment_start_linear(command_psp);
  left = (unsigned)variables_bytes;
  while (src != FCOM_GUEST_NULL && left > 1u && fcom_guest_read8(src)) {
    size_t n = fcom_guest_strnlen(src, left);
    if (n == left)
      return -1;

    if (!fcom_guest_assignment_name_matches(src, n, assignment, name_len)) {
      copied_bytes += n + 1u;
      ++entry_count;
    }
    src += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }

  if (value_len != 0) {
    copied_bytes += name_len + 1u + value_len + 1u;
    ++entry_count;
  }

  required = copied_bytes + (entry_count ? 1u : 2u) + trailer_bytes;
  if (required > 0xfff0u)
    return -8;

  paras = (UWORD)((required + 15u) >> 4);
  allocated = guest_alloc(cpu, command_psp, paras);
  if (allocated < 0)
    return allocated;

  new_seg = (UWORD)allocated;
  dst = fcom_guest_linear(new_seg, 0);

  src = environment_start_linear(command_psp);
  left = (unsigned)variables_bytes;
  while (src != FCOM_GUEST_NULL && left > 1u && fcom_guest_read8(src)) {
    size_t n = fcom_guest_strnlen(src, left);
    if (n == left) {
      guest_free(cpu, command_psp, new_seg);
      return -1;
    }

    if (!fcom_guest_assignment_name_matches(src, n, assignment, name_len)) {
      fcom_guest_copy(dst + (uint32_t)pos, src, n + 1u);
      pos += n + 1u;
    }
    src += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }

  if (value_len != 0) {
    fcom_guest_copy(dst + (uint32_t)pos, assignment, assignment_len);
    pos += assignment_len;
    fcom_guest_write8(dst + (uint32_t)pos++, 0);
  }

  if (pos == 0)
    fcom_guest_write8(dst + (uint32_t)pos++, 0);
  fcom_guest_write8(dst + (uint32_t)pos++, 0);

  src = environment_start_linear(command_psp);
  if (src != FCOM_GUEST_NULL) {
    fcom_guest_copy(dst + (uint32_t)pos,
                    src + (uint32_t)variables_bytes, trailer_bytes);
  } else {
    fcom_guest_write8(dst + (uint32_t)pos++, 0);
    fcom_guest_write8(dst + (uint32_t)pos++, 0);
  }

  fcom_guest_psp_set_environment(command_psp, new_seg);

  if (old_owned_env != 0)
    guest_free(cpu, command_psp, old_owned_env);

  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_seg),
                     new_seg);
  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_bytes),
                     (UWORD)(paras << 4));
  return 0;
}

/* Определены ниже, у прочих int21-хелперов редиректа. */
static int dos_open_mode(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g,
                         const char *name, UBYTE mode);

static int fcom_initialize_environment(CPU *cpu, UWORD command_psp)
{
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  const uint32_t program =
      gbase + (uint32_t)offsetof(struct fcom_guest, program);
  const uint32_t assignment =
      gbase + (uint32_t)offsetof(struct fcom_guest, text);
  const uint32_t path =
      gbase + (uint32_t)offsetof(struct fcom_guest, path);
  const uint32_t path2 =
      gbase + (uint32_t)offsetof(struct fcom_guest, path2);
  const uint32_t comspec =
      gbase + (uint32_t)offsetof(struct fcom_guest, comspec_arg);
  UWORD env_seg = fcom_guest_psp_environment(command_psp);
  size_t variables_bytes;
  size_t trailer_bytes;
  size_t n;

  if (env_seg != 0 &&
      fdos_mcb_owner((seg)(env_seg - 1u)) == command_psp) {
    fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_seg),
                       env_seg);
    fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, owned_env_bytes),
                       (UWORD)(fdos_mcb_size((seg)(env_seg - 1u)) << 4));
  }

  if (environment_layout(command_psp, &variables_bytes, &trailer_bytes,
                         program, sizeof(((struct fcom_guest *)0)->program)) < 0)
    fcom_guest_write8(program, 0);

  if (fcom_guest_read8(program) == 0)
    fcom_guest_write(program, "COMMAND.COM", sizeof("COMMAND.COM"));

  n = fcom_guest_strnlen(comspec,
                         sizeof(((struct fcom_guest *)0)->comspec_arg));
  if (n != 0 && n < sizeof(((struct fcom_guest *)0)->path2)) {
    fcom_guest_copy(path, comspec, n + 1u);

    SET_DS(command_psp);
    CPU_SI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
    SET_ES(command_psp);
    CPU_DI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path2);
    CPU_AH = 0x60;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM comspec truename");

    if (!int21_failed(cpu)) {
      int is_dir = 0;
      size_t path2_len =
          fcom_guest_strnlen(path2, sizeof(((struct fcom_guest *)0)->path2));

      if (path2_len < sizeof(((struct fcom_guest *)0)->path2)) {
        fcom_guest_copy(path, path2, path2_len + 1u);
        SET_DS(command_psp);
        CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
        CPU_AX = 0x4300;
        fcom_intcall(cpu, command_psp, 0x21, "FCOM comspec attr");
        if (!int21_failed(cpu) && (CPU_CX & 0x10))
          is_dir = 1;

        if (is_dir) {
          while (path2_len > 0 &&
                 fcom_guest_read8(path2 + (uint32_t)path2_len - 1u) == '\\') {
            --path2_len;
            fcom_guest_write8(path2 + (uint32_t)path2_len, 0);
          }

          if (path2_len + sizeof("\\COMMAND.COM") <=
              sizeof(((struct fcom_guest *)0)->path2)) {
            fcom_guest_write(path2 + (uint32_t)path2_len,
                             "\\COMMAND.COM", sizeof("\\COMMAND.COM"));
            path2_len += sizeof("\\COMMAND.COM") - 1u;
          }
        }

        fcom_guest_copy(path, path2, path2_len + 1u);
        SET_DS(command_psp);
        CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
        CPU_AX = 0x3d00u;
        fcom_intcall(cpu, command_psp, 0x21, "FCOM comspec open");

        if (!int21_failed(cpu)) {
          UWORD fd = CPU_AX;
          CPU_BX = fd;
          CPU_AH = 0x3e;
          fcom_intcall(cpu, command_psp, 0x21, "FCOM comspec close");
          fcom_guest_copy(program, path2, path2_len + 1u);
        } else {
          dos_printf("FCOM: cannot open COMSPEC\n");
        }
      }
    } else {
      dos_printf("FCOM: bad COMSPEC path\n");
    }
  }

  fcom_guest_write(assignment, "COMSPEC=", sizeof("COMSPEC=") - 1u);
  n = fcom_guest_strnlen(program, sizeof(((struct fcom_guest *)0)->program));
  if (n >= sizeof(((struct fcom_guest *)0)->text) - sizeof("COMSPEC="))
    return -1;

  fcom_guest_copy(assignment + sizeof("COMSPEC=") - 1u, program, n);
  fcom_guest_write8(assignment + sizeof("COMSPEC=") - 1u + (uint32_t)n, 0);

  return replace_environment_variable_guest(
      cpu, command_psp, assignment, sizeof(((struct fcom_guest *)0)->text));
}

static int fcom_set_bool_option(const char *arg,
                                int letter, int *value)
{
  const char *p = arg;
  int enabled = 1;

  if (*p != '/' && *p != '-')
    return 0;
  ++p;

  if (*p == '-') {
    enabled = 0;
    ++p;
  }

  if (toupper((unsigned char)p[0]) != letter || p[1] != '\0')
    return 0;

  *value = enabled;
  return 1;
}

static unsigned fcom_environment_bytes(UWORD command_psp)
{
  const UWORD env_seg = fcom_guest_psp_environment(command_psp);

  if (env_seg == 0)
    return 0;

  return (unsigned)fdos_mcb_size((seg)(env_seg - 1u)) << 4;
}

static unsigned fcom_environment_used(UWORD command_psp)
{
  uint32_t env = environment_start_linear(command_psp);
  unsigned used = 0;
  unsigned left = 0xfff0u;

  if (env == FCOM_GUEST_NULL)
    return 0;

  while (left != 0 && fcom_guest_read8(env) != 0) {
    size_t n = fcom_guest_strnlen(env, left);
    if (n == left)
      return used;
    used += (unsigned)n + 1u;
    env += (uint32_t)n + 1u;
    left -= (unsigned)n + 1u;
  }
  if (left != 0)
    ++used;
  return used;
}

static void fcom_uppercase(char *text)
{
  while (*text != '\0') {
    *text = (char)toupper((unsigned char)*text);
    ++text;
  }
}

static int fcom_set_read_prompt(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g,
                                const char *prompt,
                                char **value)
{
  int count;

  dos_puts(cpu, command_psp, g, prompt);

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io);
  CPU_BX = 0;
  CPU_CX = sizeof(g->io) - 1u;
  CPU_AH = 0x3f;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM SET /P read");

  if (int21_failed(cpu))
    return 0;

  count = CPU_AX;
  while (count > 0 &&
         (g->io[count - 1] == '\r' || g->io[count - 1] == '\n'))
    --count;

  g->io[count] = '\0';
  *value = (char *)g->io;
  return 1;
}

static int fcom_set_delete_temp(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g)
{
  size_t n = strlen(g->pipe_temp);

  if (n >= sizeof(g->path))
    return 0;

  memcpy(g->path, g->pipe_temp, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AH = 0x41;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM SET /E unlink");
  return !int21_failed(cpu);
}

static int fcom_set_execute(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g,
                            const char *command,
                            char **value)
{
  int handle;
  int count;
  size_t command_len;

  snprintf(g->pipe_temp, sizeof(g->pipe_temp),
           "FC%04X.$$$", command_psp);
  (void)fcom_set_delete_temp(cpu, command_psp, g);

  command_len = strlen(command);
  if (command_len + 1u + strlen(g->pipe_temp) >= sizeof(g->for_command))
    return 0;

  memcpy(g->for_command, command, command_len);
  g->for_command[command_len++] = '>';
  strcpy(g->for_command + command_len, g->pipe_temp);

  if (execute_command_line(cpu, command_psp, g, g->for_command) < 0) {
    (void)fcom_set_delete_temp(cpu, command_psp, g);
    return 0;
  }

  if (strlen(g->pipe_temp) >= sizeof(g->path))
    return 0;
  strcpy(g->path, g->pipe_temp);

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AX = 0x3d00;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM SET /E open");
  if (int21_failed(cpu)) {
    (void)fcom_set_delete_temp(cpu, command_psp, g);
    return 0;
  }

  handle = CPU_AX;

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io);
  CPU_BX = (UWORD)handle;
  CPU_CX = sizeof(g->io) - 1u;
  CPU_AH = 0x3f;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM SET /E read");

  if (int21_failed(cpu))
    count = -1;
  else
    count = CPU_AX;

  CPU_BX = (UWORD)handle;
  CPU_AH = 0x3e;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM SET /E close");
  (void)fcom_set_delete_temp(cpu, command_psp, g);

  if (count < 0)
    return 0;

  {
    int i;

    for (i = 0; i < count; ++i) {
      if (g->io[i] == '\r' || g->io[i] == '\n')
        break;
    }
    count = i;
  }

  g->io[count] = '\0';
  *value = (char *)g->io;
  return 1;
}

static int fcom_value_is_empty(const char *value)
{
  while (*value != '\0') {
    if (*value != ' ' && *value != '\t')
      return 0;
    ++value;
  }

  return 1;
}

static void fcom_set_error(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g,
                           const char *name)
{
  dos_puts(cpu, command_psp, g,
           "Can not set environment variable '");
  dos_puts(cpu, command_psp, g, name);
  dos_puts(cpu, command_psp, g,
           "'.\r\nEnvironment full?\r\n");
}

static void builtin_set(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  char *cursor = skip_space(args);
  char *assignment;
  char *equal;
  char *value;
  int opt_case = 0;
  int opt_prompt = 0;
  int opt_upper = 0;
  int opt_execute = 0;
  int opt_info = 0;

  while ((*cursor == '/' || *cursor == '-') && cursor[1] != '\0') {
    char *end = cursor;
    char saved;
    int recognized;

    while (*end != '\0' && *end != ' ' && *end != '\t')
      ++end;

    saved = *end;
    *end = '\0';

    recognized =
        fcom_set_bool_option(cursor, 'C', &opt_case) ||
        fcom_set_bool_option(cursor, 'P', &opt_prompt) ||
        fcom_set_bool_option(cursor, 'U', &opt_upper) ||
        fcom_set_bool_option(cursor, 'E', &opt_execute) ||
        fcom_set_bool_option(cursor, 'I', &opt_info);

    *end = saved;

    if (!recognized) {
      dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
      return;
    }

    cursor = skip_space(end);
  }

  assignment = cursor;

  if (opt_info) {
    unsigned bytes = fcom_environment_bytes(command_psp);
    unsigned used = fcom_environment_used(command_psp);
    int n = snprintf(g->text, sizeof(g->text),
                     "Size of environment segment: %u bytes; unused: %u\r\n",
                     bytes, bytes > used ? bytes - used : 0u);

    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);
  }

  if (*assignment == '\0') {
    uint32_t env = environment_start_linear(command_psp);
    unsigned left = 0xfff0u;

    while (env != FCOM_GUEST_NULL && left != 0 && fcom_guest_read8(env) != 0) {
      size_t n = fcom_guest_strnlen(env, left);
      if (n == left)
        break;

      if (fcom_guest_read8(env) > ' ') {
        size_t done = 0;
        while (done < n) {
          size_t chunk = n - done;
          if (chunk > sizeof(g->io))
            chunk = sizeof(g->io);
          fcom_guest_read(env + (uint32_t)done, g->io, chunk);
          if (fcom_write(cpu, command_psp,
                         FCOM_WORK_OFFSET +
                           (UWORD)offsetof(struct fcom_guest, io),
                         (UWORD)chunk) < 0)
            return;
          done += chunk;
        }
        dos_puts(cpu, command_psp, g, "\r\n");
      }
      env += (uint32_t)n + 1u;
      left -= (unsigned)n + 1u;
    }
    return;
  }

  equal = strchr(assignment, '=');
  if (equal == NULL || equal == assignment) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  *equal = '\0';
  value = equal + 1;

  if (!opt_case)
    fcom_uppercase(assignment);

  if (opt_prompt) {
    if (!fcom_set_read_prompt(cpu, command_psp, g, value, &value))
      return;
  }

  if (opt_execute) {
    if (!fcom_set_execute(cpu, command_psp, g, value, &value))
      return;
  }

  if (opt_upper)
    fcom_uppercase(value);

  if (fcom_value_is_empty(value))
    value = "";

  if (strlen(assignment) + 1u + strlen(value) >= sizeof(g->text)) {
    fcom_set_error(cpu, command_psp, g, assignment);
    return;
  }

  strcpy(g->text, assignment);
  strcat(g->text, "=");
  strcat(g->text, value);

  if (replace_environment_variable(cpu, command_psp, g->text) < 0)
    fcom_set_error(cpu, command_psp, g, assignment);
}

static void builtin_path(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  size_t n;

  if (*p == '\0' && strchr(args, ';') == NULL) {
    char *env_path = fcom_env_alloc_value(command_psp, "PATH", 1024u);

    if (env_path != NULL) {
      dos_puts(cpu, command_psp, g, "PATH=");
      dos_puts(cpu, command_psp, g, env_path);
      dos_puts(cpu, command_psp, g, "\r\n");
      free(env_path);
      return;
    }
    dos_puts(cpu, command_psp, g, "No search path defined.\r\n");
    return;
  }

  n = strlen(p);
  while (n != 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
    p[--n] = '\0';

  if (strcmp(p, ";") == 0)
    p = "";

  if (strlen(p) + 5u >= sizeof(g->text)) {
    fcom_set_error(cpu, command_psp, g, "PATH");
    return;
  }

  strcpy(g->text, "PATH=");
  strcat(g->text, p);

  if (replace_environment_variable(cpu, command_psp, g->text) < 0)
    fcom_set_error(cpu, command_psp, g, "PATH");
}

static void builtin_prompt(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);

  if (*p == '=')
    p = skip_space(p + 1);

  if (strlen(p) + 7u >= sizeof(g->text)) {
    fcom_set_error(cpu, command_psp, g, "PROMPT");
    return;
  }

  strcpy(g->text, "PROMPT=");
  strcat(g->text, p);

  if (replace_environment_variable(cpu, command_psp, g->text) < 0)
    fcom_set_error(cpu, command_psp, g, "PROMPT");
}


enum onoff_value {
  FCOM_ONOFF_INVALID = -1,
  FCOM_ONOFF_EMPTY = 0,
  FCOM_ONOFF_OFF,
  FCOM_ONOFF_ON
};

static enum onoff_value parse_onoff(char *args)
{
  char *p = skip_space(args);
  size_t n = strlen(p);

  while (n != 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
    p[--n] = '\0';

  if (n == 0)
    return FCOM_ONOFF_EMPTY;
  if (strcasecmp(p, "OFF") == 0)
    return FCOM_ONOFF_OFF;
  if (strcasecmp(p, "ON") == 0)
    return FCOM_ONOFF_ON;
  return FCOM_ONOFF_INVALID;
}



static void fcom_fddebug_close(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g)
{
  if (g->fddebug_handle > 2)
    fcom_close(cpu, command_psp, g->fddebug_handle);

  g->fddebug_handle = 1;
  strcpy(g->fddebug_name, "stdout");
}

static int fcom_fddebug_open_append(CPU *cpu, UWORD command_psp,
                                    struct fcom_guest *g,
                                    const char *name)
{
  size_t n = strlen(name);
  int handle;

  if (n == 0 || n >= sizeof(g->path) ||
      n >= sizeof(g->fddebug_name))
    return 0;

  memcpy(g->path, name, n + 1);

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AX = 0x3d01;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM FDDEBUG open");

  if (int21_failed(cpu)) {
    SET_DS(command_psp);
    CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
    CPU_CX = 0;
    CPU_AH = 0x3c;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM FDDEBUG create");
    if (int21_failed(cpu))
      return 0;
  }

  handle = CPU_AX;

  CPU_BX = (UWORD)handle;
  CPU_CX = 0;
  CPU_DX = 0;
  CPU_AX = 0x4202;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM FDDEBUG seek end");
  if (int21_failed(cpu)) {
    fcom_close(cpu, command_psp, (UWORD)handle);
    return 0;
  }

  fcom_fddebug_close(cpu, command_psp, g);
  g->fddebug_handle = (UWORD)handle;
  memcpy(g->fddebug_name, name, n + 1);
  return 1;
}

static void fcom_fddebug_write(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               const char *prefix,
                               const char *text)
{
  int n;

  if (!g->fddebug_enabled)
    return;

  n = snprintf(g->text, sizeof(g->text), "%s%s\r\n",
               prefix != NULL ? prefix : "",
               text != NULL ? text : "");
  if (n <= 0)
    return;

  if ((size_t)n >= sizeof(g->text))
    n = (int)sizeof(g->text) - 1;

  (void)fcom_write_handle(
      cpu, command_psp, g->fddebug_handle,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
      (UWORD)n);
}

static void builtin_fddebug(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  char *end;
  enum onoff_value value;

  end = p + strlen(p);
  while (end > p &&
         (end[-1] == ' ' || end[-1] == '\t' ||
          end[-1] == '\r' || end[-1] == '\n'))
    *--end = '\0';

  value = parse_onoff(p);
  switch (value) {
  case FCOM_ONOFF_EMPTY:
    dos_puts(cpu, command_psp, g,
             g->fddebug_enabled
                 ? "DEBUG output is ON.\r\n"
                 : "DEBUG output is OFF.\r\n");
    dos_puts(cpu, command_psp, g,
             "DEBUG output is printed to '");
    dos_puts(cpu, command_psp, g,
             g->fddebug_name[0] != '\0'
                 ? g->fddebug_name : "stdout");
    dos_puts(cpu, command_psp, g, "'.\r\n");
    return;

  case FCOM_ONOFF_ON:
    g->fddebug_enabled = 1;
    return;

  case FCOM_ONOFF_OFF:
    g->fddebug_enabled = 0;
    return;

  default:
    break;
  }

  if (strcasecmp(p, "stderr") == 0 ||
      strcasecmp(p, "stdout") == 0) {
    UWORD handle = strcasecmp(p, "stderr") == 0 ? 2 : 1;

    fcom_fddebug_close(cpu, command_psp, g);
    g->fddebug_handle = handle;
    strcpy(g->fddebug_name,
           handle == 2 ? "stderr" : "stdout");
    g->fddebug_enabled = 1;
    return;
  }

  if (!fcom_fddebug_open_append(cpu, command_psp, g, p)) {
    dos_puts(cpu, command_psp, g, "Unable to open - ");
    dos_puts(cpu, command_psp, g, p);
    dos_puts(cpu, command_psp, g, "\r\n");
    return;
  }

  g->fddebug_enabled = 1;
}

static void builtin_lfnfor(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  enum onoff_value value;

  /*
   * cmd/lfnfor.c checks only the first eight bytes; COMPLETEanything
   * therefore follows the COMPLETE branch in the original.
   */
  if (strncasecmp(p, "COMPLETE", 8) == 0) {
    value = parse_onoff(p + 8);

    switch (value) {
    case FCOM_ONOFF_EMPTY:
      dos_puts(cpu, command_psp, g,
               g->lfncomplete_enabled
                   ? "LFN Complete is ON\r\n"
                   : "LFN Complete is OFF\r\n");
      return;

    case FCOM_ONOFF_OFF:
      g->lfncomplete_enabled = 0;
      return;

    case FCOM_ONOFF_ON:
      g->lfncomplete_enabled = 1;
      return;

    default:
      dos_puts(cpu, command_psp, g,
               "Must specify ON or OFF\r\n");
      return;
    }
  }

  value = parse_onoff(p);
  switch (value) {
  case FCOM_ONOFF_EMPTY:
    dos_puts(cpu, command_psp, g,
             g->lfnfor_enabled
                 ? "LFNFOR is ON\r\n"
                 : "LFNFOR is OFF\r\n");
    return;

  case FCOM_ONOFF_OFF:
    g->lfnfor_enabled = 0;
    return;

  case FCOM_ONOFF_ON:
    g->lfnfor_enabled = 1;
    return;

  default:
    dos_puts(cpu, command_psp, g,
             "Must specify ON or OFF\r\n");
    return;
  }
}

static void builtin_verify(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  enum onoff_value value = parse_onoff(args);

  switch (value) {
  case FCOM_ONOFF_EMPTY:
    CPU_AH = 0x54;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM VERIFY get");
    dos_puts(cpu, command_psp, g,
             CPU_AL ? "VERIFY is ON\r\n" : "VERIFY is OFF\r\n");
    break;

  case FCOM_ONOFF_OFF:
  case FCOM_ONOFF_ON:
    CPU_AL = value == FCOM_ONOFF_ON ? 1 : 0;
    CPU_DL = 0;
    CPU_AH = 0x2e;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM VERIFY set");
    break;

  default:
    dos_puts(cpu, command_psp, g, "Must specify ON or OFF.\r\n");
    break;
  }
}

static void builtin_break(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  enum onoff_value value = parse_onoff(args);

  switch (value) {
  case FCOM_ONOFF_EMPTY:
    CPU_AL = 0;
    CPU_AH = 0x33;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM BREAK get");
    dos_puts(cpu, command_psp, g,
             CPU_DL ? "BREAK is ON\r\n" : "BREAK is OFF\r\n");
    break;

  case FCOM_ONOFF_OFF:
  case FCOM_ONOFF_ON:
    CPU_AL = 1;
    CPU_DL = value == FCOM_ONOFF_ON ? 1 : 0;
    CPU_AH = 0x33;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM BREAK set");
    break;

  default:
    dos_puts(cpu, command_psp, g, "Must specify ON or OFF.\r\n");
    break;
  }
}

static int fcom_truepath(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g,
                         const char *source,
                         char *destination,
                         size_t destination_size)
{
  size_t n = strlen(source);

  if (n >= sizeof(g->path) || destination_size == 0)
    return 0;

  memcpy(g->path, source, n + 1);
  memset(g->text, 0, sizeof(g->text));

  SET_DS(command_psp);
  CPU_SI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  SET_ES(command_psp);
  CPU_DI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text);
  CPU_AH = 0x60;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM TRUENAME");

  if (int21_failed(cpu))
    return 0;

  n = strnlen(g->text, sizeof(g->text));
  if (n >= destination_size)
    return 0;

  memcpy(destination, g->text, n + 1);
  return 1;
}

static void builtin_truename(CPU *cpu, UWORD command_psp,
                             struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);

  if (*p == '\0')
    p = ".";

  if (!fcom_truepath(cpu, command_psp, g, p,
                     g->path2, sizeof(g->path2)))
    return;

  dos_puts(cpu, command_psp, g, g->path2);
  dos_puts(cpu, command_psp, g, "\r\n");
}


static int fcom_get_drive_directory(CPU *cpu, UWORD command_psp,
                                    struct fcom_guest *g,
                                    unsigned drive,
                                    char *dst, size_t dst_size)
{
  size_t used = 0;

  if (drive >= 26u || dst_size < 4)
    return 0;

  dst[used++] = (char)('A' + drive);
  dst[used++] = ':';
  dst[used++] = '\\';

  if (dos_get_cwd(cpu, command_psp, g, drive + 1u) < 0)
    return 0;

  if (g->path[0] != '\0') {
    size_t n = strlen(g->path);

    if (used + n >= dst_size)
      return 0;

    memcpy(dst + used, g->path, n);
    used += n;
  }

  dst[used] = '\0';
  return 1;
}

static int fcom_cd_dir(CPU *cpu, UWORD command_psp,
                       struct fcom_guest *g, char *args,
                       int change_drive, const char *function_name)
{
  char *p = skip_space(args);
  char *end;
  unsigned drive;
  int has_drive;

  if (*p == '\0') {
    drive = dos_get_drive(cpu, command_psp);

    if (!fcom_get_drive_directory(cpu, command_psp, g, drive,
                                  g->path2, sizeof(g->path2)))
      return 1;

    dos_puts(cpu, command_psp, g, g->path2);
    dos_puts(cpu, command_psp, g, "\r\n");
    return 0;
  }

  if (*p == '"') {
    args = ++p;
    while (*p != '\0' && *p != '"')
      ++p;
    if (*p == '"')
      *p++ = '\0';
  } else {
    args = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
      ++p;
    if (*p != '\0')
      *p++ = '\0';
  }

  if (*skip_space(p) != '\0') {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return 1;
  }

  end = args + strlen(args);
  while (end > args + 1 &&
         end[-1] == '\\' &&
         end[-2] != ':')
    *--end = '\0';

  has_drive = isalpha((unsigned char)args[0]) && args[1] == ':';

  if (has_drive) {
    drive = (unsigned)(toupper((unsigned char)args[0]) - 'A');

    if (change_drive) {
      if (!dos_set_drive(cpu, command_psp, drive))
        return 1;

      if (args[2] == '\0')
        return 0;
    } else if (args[2] == '\0') {
      if (!fcom_get_drive_directory(cpu, command_psp, g, drive,
                                    g->path2, sizeof(g->path2)))
        return 1;

      dos_puts(cpu, command_psp, g, g->path2);
      dos_puts(cpu, command_psp, g, "\r\n");
      return 0;
    }
  }

  if (dos_change_dir(cpu, command_psp, g, args) < 0) {
    dos_puts(cpu, command_psp, g, function_name);
    dos_puts(cpu, command_psp, g, " failed for '");
    dos_puts(cpu, command_psp, g, args);
    dos_puts(cpu, command_psp, g, "'.\r\n");
    return 1;
  }

  return 0;
}

static void builtin_chdir(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  (void)fcom_cd_dir(cpu, command_psp, g, args, 0, "CHDIR");
}

static void builtin_cdd(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  (void)fcom_cd_dir(cpu, command_psp, g, args, 1, "CDD");
}

static void fcom_dir_stack_drop_oldest(UWORD command_psp,
                                       struct fcom_guest *g)
{
  char *storage = fcom_dir_stack_storage(command_psp);
  size_t oldest;

  if (g->dir_stack_used == 0)
    return;

  oldest = strlen(storage) + 1;
  if (oldest >= g->dir_stack_used) {
    g->dir_stack_used = 0;
    storage[0] = '\0';
    return;
  }

  memmove(storage, storage + oldest,
          g->dir_stack_used - oldest);
  g->dir_stack_used -= (UWORD)oldest;
}

static int fcom_dir_stack_push(UWORD command_psp,
                               struct fcom_guest *g,
                               const char *directory)
{
  char *storage = fcom_dir_stack_storage(command_psp);
  size_t needed = strlen(directory) + 1;

  if (needed > FCOM_DIR_STACK_BYTES)
    return 0;

  /*
   * chgCtxt() discards the oldest directory-stack entries when the
   * configured stack size is exceeded.
   */
  while ((size_t)g->dir_stack_used + needed >
         FCOM_DIR_STACK_BYTES)
    fcom_dir_stack_drop_oldest(command_psp, g);

  memcpy(storage + g->dir_stack_used, directory, needed);
  g->dir_stack_used += (UWORD)needed;
  return 1;
}

static char *fcom_dir_stack_last(UWORD command_psp,
                                 struct fcom_guest *g)
{
  char *storage = fcom_dir_stack_storage(command_psp);
  size_t pos = 0;
  char *last = NULL;

  while (pos < g->dir_stack_used) {
    last = storage + pos;
    pos += strlen(last) + 1;
  }

  return last;
}

static void fcom_dir_stack_pop_last(UWORD command_psp,
                                    struct fcom_guest *g)
{
  char *storage = fcom_dir_stack_storage(command_psp);
  char *last = fcom_dir_stack_last(command_psp, g);

  if (last == NULL)
    return;

  g->dir_stack_used = (UWORD)(last - storage);
  storage[g->dir_stack_used] = '\0';
}

static void fcom_dir_stack_clear(UWORD command_psp,
                                 struct fcom_guest *g)
{
  char *storage = fcom_dir_stack_storage(command_psp);

  g->dir_stack_used = 0;
  storage[0] = '\0';
}

static void builtin_pushd(CPU *cpu, UWORD command_psp, struct fcom_guest *g, char *args)
{
  int rc;

  if (!fcom_get_drive_directory(cpu, command_psp, g,
                               dos_get_drive(cpu, command_psp),
                               g->path2, sizeof(g->path2)))
    return;

  if (!fcom_dir_stack_push(command_psp, g, g->path2)) {
    dos_puts(cpu, command_psp, g, "Out of memory.\r\n");
    return;
  }

  /*
   * cmd/pushd.c pushes first, then optionally calls CDD. A failed CDD does
   * not remove the newly pushed entry.
   */
  args = skip_space(args);
  rc = *args != '\0'
      ? fcom_cd_dir(cpu, command_psp, g, args, 1, "CDD")
      : 0;
  (void)rc;
}

static void builtin_popd(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  char *directory;

  /*
   * cmd/popd.c tests only the first character. "*" clears the stack;
   * any other parameter is ignored and a normal pop is performed.
   */
  if (*p == '*') {
    fcom_dir_stack_clear(command_psp, g);
    return;
  }

  directory = fcom_dir_stack_last(command_psp, g);
  if (directory == NULL) {
    dos_puts(cpu, command_psp, g,
             "Directory stack empty.\r\n");
    fcom_dir_stack_clear(command_psp, g);
    return;
  }

  strncpy(g->path2, directory, sizeof(g->path2) - 1);
  g->path2[sizeof(g->path2) - 1] = '\0';

  /*
   * ctxtPop() removes the item before cmd_cdd() is called, so a failed
   * directory change still consumes the stack entry.
   */
  fcom_dir_stack_pop_last(command_psp, g);
  (void)fcom_cd_dir(cpu, command_psp, g, g->path2, 1, "CDD");
}

static void builtin_dirs(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *storage = fcom_dir_stack_storage(command_psp);
  size_t pos = 0;
  unsigned count = 0;
  int n;

  /*
   * cmd/dirs.c deliberately ignores its parameter.
   */
  (void)args;

  while (pos < g->dir_stack_used) {
    const char *directory = storage + pos;

    dos_puts(cpu, command_psp, g, directory);
    dos_puts(cpu, command_psp, g, "\r\n");
    pos += strlen(directory) + 1;
    ++count;
  }

  if (count == 0) {
    dos_puts(cpu, command_psp, g,
             "Directory stack empty.\r\n");
    return;
  }

  n = snprintf(g->text, sizeof(g->text),
               "%u items displayed.\r\n", count);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);
}



static char *next_argument(char **cursor)
{
  char *p = skip_space(*cursor);
  char *start;

  if (*p == '\0') {
    *cursor = p;
    return NULL;
  }

  if (*p == '"') {
    start = ++p;
    while (*p != '\0' && *p != '"')
      ++p;
    if (*p == '"')
      *p++ = '\0';
  } else {
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
      ++p;
    if (*p != '\0')
      *p++ = '\0';
  }

  *cursor = p;
  return start;
}

static int fcom_mkdir(CPU *cpu, UWORD command_psp,
                     struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AH = 0x39;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM MKDIR");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int fcom_rmdir(CPU *cpu, UWORD command_psp,
                     struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AH = 0x3a;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM RMDIR");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int dos_unlink(CPU *cpu, UWORD command_psp,
                      struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AH = 0x41;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM DEL");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int dos_rename_file(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g,
                           const char *old_name, const char *new_name)
{
  size_t old_len = strlen(old_name);
  size_t new_len = strlen(new_name);

  if (old_len >= sizeof(g->path) || new_len >= sizeof(g->path2))
    return -3;

  memcpy(g->path, old_name, old_len + 1);
  memcpy(g->path2, new_name, new_len + 1);

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  SET_ES(command_psp);
  CPU_DI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path2);
  CPU_AH = 0x56;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM RENAME");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

/* Complete; waiting on the commands that should report per-file errors
   (COPY/DEL/MOVE error paths). Wire up rather than duplicate. */
KEEP_UNUSED static void report_file_error(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *prefix, const char *name)
{
  dos_puts(cpu, command_psp, g, prefix);
  dos_puts(cpu, command_psp, g, name);
  dos_puts(cpu, command_psp, g, "'\r\n");
}

#define FCOM_RMDIR_SLOTS      256u
#define FCOM_RMDIR_PATH_BYTES 128u

static void fcom_dirfct_error(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *function_name,
                              const char *directory)
{
  int n = snprintf(g->text, sizeof(g->text),
                   "%s failed for '%s'.\r\n",
                   function_name, directory);

  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);
}

static int fcom_md_rd_bool_option(const char *arg,
                                  int letter, int *value)
{
  const char *p = arg;
  int enabled = 1;

  if (*p != '/' && *p != '-')
    return 0;
  ++p;

  if (*p == '-') {
    enabled = 0;
    ++p;
  }

  if (toupper((unsigned char)p[0]) != letter || p[1] != '\0')
    return 0;

  *value = enabled;
  return 1;
}

static int fcom_parse_md_rd(char *args, int mkdir_command,
                            char **directory,
                            int *recursive, int *quiet)
{
  char *cursor = args;
  char *arg;

  *directory = NULL;
  *recursive = 0;
  *quiet = 0;

  while ((arg = next_argument(&cursor)) != NULL) {
    if (fcom_md_rd_bool_option(
            arg, mkdir_command ? 'P' : 'S', recursive))
      continue;

    if (fcom_md_rd_bool_option(arg, 'Q', quiet))
      continue;

    if ((arg[0] == '/' || arg[0] == '-') && arg[1] != '\0')
      return 0;

    if (*directory != NULL)
      return 0;

    *directory = arg;
  }

  return *directory != NULL;
}

static void fcom_cut_backslash(char *path)
{
  size_t n = strlen(path);

  while (n > 0 &&
         (path[n - 1] == '\\' || path[n - 1] == '/')) {
    /*
     * Preserve "X:\" and "\". FreeCOM's cutBackslash() removes only
     * redundant trailing separators.
     */
    if (n == 1 ||
        (n == 3 && path[1] == ':'))
      break;

    path[--n] = '\0';
  }
}
static int fcom_get_file_attr(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *name, UWORD *attributes);

/* fcom_get_file_attr() writes attributes on every zero return; GCC 14
   does not preserve that relationship through the emulated INT 21h call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
static int fcom_recursive_mkdir(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g,
                                const char *directory)
{
  size_t n = strlen(directory);
  size_t i;
  size_t start;

  if (n == 0 || n >= sizeof(g->path2))
    return -3;

  strcpy(g->path2, directory);
  fcom_cut_backslash(g->path2);

  n = strlen(g->path2);
  start = (n >= 2 && g->path2[1] == ':') ? 2u : 0u;

  /*
   * cmd/mkdir.c walks every path component and creates it when absent.
   * A drive prefix and the root separator are not passed to mkdir().
   */
  for (i = start; i <= n; ++i) {
    char saved;
    UWORD attributes;

    if (i != n &&
        g->path2[i] != '\\' &&
        g->path2[i] != '/')
      continue;

    if (i == start ||
        (i == start + 1 &&
         (g->path2[start] == '\\' || g->path2[start] == '/')))
      continue;

    saved = g->path2[i];
    g->path2[i] = '\0';

    if (fcom_get_file_attr(cpu, command_psp, g,
                           g->path2, &attributes) == 0) {
      if (!(attributes & D_DIR)) {
        g->path2[i] = saved;
        return -1;
      }
    } else if (fcom_mkdir(cpu, command_psp, g, g->path2) < 0) {
      g->path2[i] = saved;
      return -1;
    }

    g->path2[i] = saved;
  }

  return 0;
}
#pragma GCC diagnostic pop

static int fcom_rmdir_confirm(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *directory)
{
  int ch;

  dos_puts(cpu, command_psp, g, "All files in '");
  dos_puts(cpu, command_psp, g, directory);
  dos_puts(cpu, command_psp, g,
           "' will be deleted!\r\nAre you sure (Y/N)? ");

  for (;;) {
    CPU_AH = 0x08;
    fcom_intcall(cpu, command_psp, 0x21,
                 "FCOM RMDIR confirmation");
    ch = toupper((unsigned char)CPU_AL);

    if (ch == 'Y' || ch == 'N' ||
        ch == '\r' || ch == '\n' || ch == 0x03)
      break;

    g->io[0] = '\a';
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);
  }

  if (ch == '\r' || ch == '\n' || ch == 0x03)
    ch = 'N';

  g->io[0] = (UBYTE)ch;
  g->io[1] = '\r';
  g->io[2] = '\n';
  (void)fcom_write(cpu, command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 3);

  return ch == 'Y';
}

static char *fcom_rmdir_slot(char *storage, unsigned index)
{
  return storage + (size_t)index * FCOM_RMDIR_PATH_BYTES;
}

static int fcom_rmdir_join(char *dst, size_t dst_size,
                           const char *directory,
                           const char *name)
{
  size_t dn = strlen(directory);
  size_t nn = strlen(name);
  int slash = dn != 0 &&
              directory[dn - 1] != '\\' &&
              directory[dn - 1] != '/';

  if (dn + (size_t)slash + nn >= dst_size)
    return 0;

  memcpy(dst, directory, dn);
  if (slash)
    dst[dn++] = '\\';
  memcpy(dst + dn, name, nn + 1);
  return 1;
}

/* fcom_get_file_attr() writes attributes on every zero return; GCC 14
   does not preserve that relationship through the emulated INT 21h call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
static int fcom_recursive_rmdir(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g,
                                const char *directory,
                                int quiet)
{
  UWORD attributes;
  char *storage;
  unsigned used = 1;
  unsigned scan_index;
  int result = -1;

  if (strlen(directory) >= FCOM_RMDIR_PATH_BYTES)
    return -3;

  if (fcom_get_file_attr(cpu, command_psp, g,
                         directory, &attributes) < 0 ||
      !(attributes & D_DIR))
    return -1;

  if (!quiet &&
      !fcom_rmdir_confirm(cpu, command_psp, g, directory))
    return -1;

  storage = (char *)malloc((size_t)FCOM_RMDIR_SLOTS * FCOM_RMDIR_PATH_BYTES);
  if (storage == NULL)
    return -8;

  memset(storage, 0, (size_t)FCOM_RMDIR_SLOTS * FCOM_RMDIR_PATH_BYTES);
  strcpy(fcom_rmdir_slot(storage, 0), directory);
  fcom_cut_backslash(fcom_rmdir_slot(storage, 0));

  /*
   * Original rmdir_withfiles() recursively visits directories first,
   * then files, then removes the directory. The native port keeps the
   * same order but stores the recursion list in guest DOS memory.
   */
  for (scan_index = 0;
       scan_index < used && scan_index < FCOM_RMDIR_SLOTS;
       ++scan_index) {
    char *current = fcom_rmdir_slot(storage, scan_index);
    int rc;

    if (!fcom_rmdir_join(g->batch_arg, sizeof(g->batch_arg),
                         current, "*.*"))
      goto done;

    memset(&g->find, 0, sizeof(g->find));
    if (set_find_dta(cpu, command_psp) < 0)
      goto done;

    rc = dos_find_first_attr(cpu, command_psp, g,
                             g->batch_arg, D_DIR);
    while (rc == 0) {
      if ((g->find.dm_attr_fnd & D_DIR) &&
          strcmp(g->find.dm_name, ".") != 0 &&
          strcmp(g->find.dm_name, "..") != 0) {
        char *next;

        if (used >= FCOM_RMDIR_SLOTS) {
          restore_default_dta(cpu, command_psp);
          goto done;
        }

        next = fcom_rmdir_slot(storage, used);
        if (!fcom_rmdir_join(next, FCOM_RMDIR_PATH_BYTES,
                             current, g->find.dm_name)) {
          restore_default_dta(cpu, command_psp);
          goto done;
        }
        ++used;
      }

      rc = dos_find_next(cpu, command_psp);
    }

    restore_default_dta(cpu, command_psp);

    if (!fcom_rmdir_join(g->batch_arg, sizeof(g->batch_arg),
                         current, "*.*"))
      goto done;

    memset(&g->find, 0, sizeof(g->find));
    if (set_find_dta(cpu, command_psp) < 0)
      goto done;

    /*
     * FA_NORMAL in the original is zero. It therefore enumerates ordinary
     * files but does not forcibly include hidden or system entries.
     */
    rc = dos_find_first_attr(cpu, command_psp, g,
                             g->batch_arg, 0);
    while (rc == 0) {
      if (!(g->find.dm_attr_fnd & D_DIR) &&
          fcom_rmdir_join(g->path2, sizeof(g->path2),
                           current, g->find.dm_name)) {
        (void)dos_unlink(cpu, command_psp, g, g->path2);

        if (set_find_dta(cpu, command_psp) < 0) {
          restore_default_dta(cpu, command_psp);
          goto done;
        }
      }

      rc = dos_find_next(cpu, command_psp);
    }

    restore_default_dta(cpu, command_psp);
  }

  if (scan_index != used)
    goto done;

  while (used != 0) {
    char *current = fcom_rmdir_slot(storage, --used);

    if (fcom_rmdir(cpu, command_psp, g, current) < 0)
      goto done;
  }

  result = 0;

done:
  free(storage);
  return result;
}
#pragma GCC diagnostic pop

static void builtin_mkdir(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  char *directory;
  int recursive;
  int quiet;
  int rc;

  if (!fcom_parse_md_rd(args, 1,
                        &directory, &recursive, &quiet)) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  fcom_cut_backslash(directory);

  rc = recursive
      ? fcom_recursive_mkdir(cpu, command_psp, g, directory)
      : fcom_mkdir(cpu, command_psp, g, directory);

  if (rc < 0 && !quiet)
    fcom_dirfct_error(cpu, command_psp, g,
                      "MKDIR", directory);
}

static void builtin_rmdir(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  char *directory;
  int recursive;
  int quiet;
  int rc;

  if (!fcom_parse_md_rd(args, 0,
                        &directory, &recursive, &quiet)) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  fcom_cut_backslash(directory);

  rc = recursive
      ? fcom_recursive_rmdir(cpu, command_psp, g,
                             directory, quiet)
      : fcom_rmdir(cpu, command_psp, g, directory);

  if (rc < 0 && !quiet)
    fcom_dirfct_error(cpu, command_psp, g,
                      "RMDIR", directory);
}

static int make_delete_name(struct fcom_guest *g,
                            const char *pattern, const char *found)
{
  const char *slash1 = strrchr(pattern, '\\');
  const char *slash2 = strrchr(pattern, '/');
  const char *slash = slash1;

  if (slash2 != NULL && (slash == NULL || slash2 > slash))
    slash = slash2;

  if (slash != NULL) {
    size_t prefix = (size_t)(slash - pattern + 1);
    size_t name_len = strlen(found);

    if (prefix + name_len >= sizeof(g->path2))
      return 0;
    memcpy(g->path2, pattern, prefix);
    memcpy(g->path2 + prefix, found, name_len + 1);
  } else {
    if (strlen(found) >= sizeof(g->path2))
      return 0;
    strcpy(g->path2, found);
  }

  return 1;
}

struct fcom_del_options {
  int prompt;
  int verbose;
};

static int fcom_del_bool_option(const char *arg,
                                int letter, int *value)
{
  const char *p = arg;
  int enabled = 1;

  if (*p != '/' && *p != '-')
    return 0;
  ++p;

  if (*p == '-') {
    enabled = 0;
    ++p;
  }

  if (toupper((unsigned char)p[0]) != letter || p[1] != '\0')
    return 0;

  *value = enabled;
  return 1;
}

static int fcom_del_confirm_all(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g,
                                const char *directory)
{
  int ch;

  dos_puts(cpu, command_psp, g, "All files in '");
  dos_puts(cpu, command_psp, g, directory);
  dos_puts(cpu, command_psp, g,
           "' will be deleted!\r\nAre you sure (Y/N)? ");

  for (;;) {
    CPU_AH = 0x08;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM DEL all prompt");
    ch = toupper((unsigned char)CPU_AL);

    if (ch == 'Y' || ch == 'N' ||
        ch == '\r' || ch == '\n' || ch == 0x03)
      break;

    g->io[0] = '\a';
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);
  }

  if (ch == '\r' || ch == '\n' || ch == 0x03)
    ch = 'N';

  g->io[0] = (UBYTE)ch;
  g->io[1] = '\r';
  g->io[2] = '\n';
  (void)fcom_write(cpu, command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 3);

  return ch == 'Y';
}

static int fcom_del_confirm_file(CPU *cpu, UWORD command_psp,
                                 struct fcom_guest *g,
                                 const char *filename,
                                 int *all)
{
  int ch;

  if (*all)
    return 1;

  dos_puts(cpu, command_psp, g, "Delete '");
  dos_puts(cpu, command_psp, g, filename);
  dos_puts(cpu, command_psp, g,
           "' (Yes/No/All/Quit) ? ");

  for (;;) {
    CPU_AH = 0x08;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM DEL file prompt");
    ch = toupper((unsigned char)CPU_AL);

    if (ch == 'Y' || ch == 'N' ||
        ch == 'A' || ch == 'Q' ||
        ch == '\r' || ch == '\n' || ch == 0x1b)
      break;

    g->io[0] = '\a';
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);
  }

  if (ch == '\r' || ch == '\n')
    ch = 'Y';
  else if (ch == 0x1b)
    ch = 'Q';

  g->io[0] = (UBYTE)ch;
  g->io[1] = '\r';
  g->io[2] = '\n';
  (void)fcom_write(cpu, command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 3);

  if (ch == 'A') {
    *all = 1;
    return 1;
  }
  if (ch == 'Q')
    return -1;
  return ch == 'Y';
}

static const char *fcom_copy_basename(const char *name)
{
  const char *base = name;
  const char *p;

  for (p = name; *p != '\0'; ++p) {
    if (*p == '\\' || *p == '/' || *p == ':')
      base = p + 1;
  }

  return base;
}

static int fcom_del_is_all_pattern(const char *pattern)
{
  const char *name = fcom_copy_basename(pattern);
  const char *dot;

  if (*name != '*')
    return 0;

  dot = strrchr(name, '.');
  return dot == NULL || strcmp(dot, ".*") == 0;
}

static int fcom_del_directory_text(char *dst, size_t dst_size,
                                   const char *pattern)
{
  const char *slash1 = strrchr(pattern, '\\');
  const char *slash2 = strrchr(pattern, '/');
  const char *colon = strrchr(pattern, ':');
  const char *cut = slash1;
  size_t n;

  if (slash2 != NULL && (cut == NULL || slash2 > cut))
    cut = slash2;
  if (colon != NULL && (cut == NULL || colon > cut))
    cut = colon;

  if (cut == NULL) {
    if (dst_size < 2)
      return 0;
    strcpy(dst, ".");
    return 1;
  }

  n = (size_t)(cut - pattern);
  if (cut == colon)
    ++n;

  if (n == 0)
    n = 1;

  if (n >= dst_size)
    return 0;

  memcpy(dst, pattern, n);
  dst[n] = '\0';
  return 1;
}

static void builtin_del(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  struct fcom_del_options options;
  char *cursor;
  char *arg;
  unsigned argc = 0;
  unsigned count = 0;
  int all = 0;

  memset(&options, 0, sizeof(options));

  if (strlen(args) >= sizeof(g->batch_line)) {
    dos_puts(cpu, command_psp, g, "Filename too long.\r\n");
    return;
  }

  strcpy(g->batch_line, args);
  cursor = args;

  while ((arg = next_argument(&cursor)) != NULL) {
    if (fcom_del_bool_option(arg, 'P', &options.prompt) ||
        fcom_del_bool_option(arg, 'V', &options.verbose))
      continue;

    if ((arg[0] == '/' || arg[0] == '-') && arg[1] != '\0') {
      dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
      return;
    }

    ++argc;
  }

  if (argc == 0) {
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
    return;
  }

  cursor = g->batch_line;
  while ((arg = next_argument(&cursor)) != NULL) {
    int rc;
    int matched = 0;

    if (fcom_del_bool_option(arg, 'P', &options.prompt) ||
        fcom_del_bool_option(arg, 'V', &options.verbose))
      continue;

    if (strlen(arg) >= sizeof(g->batch_arg)) {
      dos_puts(cpu, command_psp, g, "Filename too long. - '");
      dos_puts(cpu, command_psp, g, arg);
      dos_puts(cpu, command_psp, g, "'\r\n");
      return;
    }

    strcpy(g->batch_arg, arg);

    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
      UWORD attributes;
#pragma GCC diagnostic pop
      if (fcom_get_file_attr(cpu, command_psp, g,
                             g->batch_arg, &attributes) == 0 &&
          (attributes & D_DIR)) {
        size_t n = strlen(g->batch_arg);

        if (n + 4 >= sizeof(g->batch_arg)) {
          dos_puts(cpu, command_psp, g, "Filename too long. - '");
          dos_puts(cpu, command_psp, g, arg);
          dos_puts(cpu, command_psp, g, "'\r\n");
          return;
        }

        if (n != 0 &&
            g->batch_arg[n - 1] != '\\' &&
            g->batch_arg[n - 1] != '/')
          g->batch_arg[n++] = '\\';

        strcpy(g->batch_arg + n, "*.*");
      }
    }

    if (!options.prompt &&
        fcom_del_is_all_pattern(g->batch_arg)) {
      if (!fcom_del_directory_text(g->path2, sizeof(g->path2),
                                   g->batch_arg) ||
          !fcom_del_confirm_all(cpu, command_psp, g, g->path2))
        return;
    }

    memset(&g->find, 0, sizeof(g->find));
    if (set_find_dta(cpu, command_psp) < 0)
      return;

    rc = dos_find_first_attr(cpu, command_psp, g,
                             g->batch_arg, 0x20);
    while (rc == 0) {
      if (!(g->find.dm_attr_fnd & D_DIR) &&
          make_delete_name(g, g->batch_arg, g->find.dm_name)) {
        int answer = 1;

        matched = 1;

        if (options.prompt)
          answer = fcom_del_confirm_file(
              cpu, command_psp, g, g->path2, &all);

        if (answer < 0) {
          restore_default_dta(cpu, command_psp);
          return;
        }

        if (answer) {
          if (options.verbose && !options.prompt) {
            dos_puts(cpu, command_psp, g, "Deleting file \"");
            dos_puts(cpu, command_psp, g, g->path2);
            dos_puts(cpu, command_psp, g, "\".\r\n");
          }

          if (dos_unlink(cpu, command_psp, g, g->path2) == 0)
            ++count;
          else {
            dos_puts(cpu, command_psp, g, g->path2);
            dos_puts(cpu, command_psp, g, "\r\n");
          }

          if (set_find_dta(cpu, command_psp) < 0) {
            restore_default_dta(cpu, command_psp);
            return;
          }
        }
      }

      rc = dos_find_next(cpu, command_psp);
    }

    restore_default_dta(cpu, command_psp);

    if (!matched)
      dos_puts(cpu, command_psp, g, "File not found.\r\n");
  }

  if (g->echo_enabled) {
    if (count == 0) {
      dos_puts(cpu, command_psp, g, "no file removed.\r\n");
    } else if (count == 1) {
      dos_puts(cpu, command_psp, g, "one file removed.\r\n");
    } else {
      int n = snprintf(g->text, sizeof(g->text),
                       "%u files removed.\r\n", count);

      if (n > 0)
        (void)fcom_write(cpu, command_psp,
            FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
            (UWORD)n);
    }
  }
}

static int fcom_rename_fill_component(char *dst, size_t dst_size,
                                      const char *pattern,
                                      const char *source)
{
  size_t out = 0;

  while (*pattern != '\0' && out + 1 < dst_size) {
    switch (*pattern++) {
    case '?':
      if (*source != '\0')
        dst[out++] = *source;
      break;

    case '*': {
      size_t n = strlen(source);

      if (n >= dst_size - out)
        n = dst_size - out - 1;

      memcpy(dst + out, source, n);
      out += n;
      source += n;
      dst[out] = '\0';
      return 1;
    }

    default:
      dst[out++] = pattern[-1];
      break;
    }

    if (*source != '\0')
      ++source;
  }

  dst[out] = '\0';
  return *pattern == '\0';
}

static int fcom_rename_apply_mask(char *dst, size_t dst_size,
                                  const char *source_name,
                                  const char *destination_mask)
{
  const char *source_dot = strrchr(source_name, '.');
  const char *mask_dot = strrchr(destination_mask, '.');
  char source_base[13];
  char source_ext[4];
  char mask_base[13];
  char mask_ext[4];
  char result_base[13];
  char result_ext[4];
  size_t n;

  if (source_dot == source_name)
    source_dot = NULL;
  if (mask_dot == destination_mask)
    mask_dot = NULL;

  n = source_dot != NULL
      ? (size_t)(source_dot - source_name)
      : strlen(source_name);
  if (n >= sizeof(source_base))
    return 0;
  memcpy(source_base, source_name, n);
  source_base[n] = '\0';

  if (source_dot != NULL) {
    if (strlen(source_dot + 1) >= sizeof(source_ext))
      return 0;
    strcpy(source_ext, source_dot + 1);
  } else {
    source_ext[0] = '\0';
  }

  n = mask_dot != NULL
      ? (size_t)(mask_dot - destination_mask)
      : strlen(destination_mask);
  if (n >= sizeof(mask_base))
    return 0;
  memcpy(mask_base, destination_mask, n);
  mask_base[n] = '\0';

  if (mask_dot != NULL) {
    if (strlen(mask_dot + 1) >= sizeof(mask_ext))
      return 0;
    strcpy(mask_ext, mask_dot + 1);
  } else {
    mask_ext[0] = '\0';
  }

  if (!fcom_rename_fill_component(
          result_base, sizeof(result_base),
          mask_base, source_base) ||
      !fcom_rename_fill_component(
          result_ext, sizeof(result_ext),
          mask_ext, source_ext))
    return 0;

  n = strlen(result_base);
  if (result_ext[0] != '\0') {
    size_t en = strlen(result_ext);

    if (n + 1 + en >= dst_size)
      return 0;

    memcpy(dst, result_base, n);
    dst[n++] = '.';
    memcpy(dst + n, result_ext, en + 1);
  } else {
    if (n >= dst_size)
      return 0;
    memcpy(dst, result_base, n + 1);
  }

  return 1;
}

static int fcom_rename_build_path(char *dst, size_t dst_size,
                                  const char *source_pattern,
                                  const char *name)
{
  const char *slash1 = strrchr(source_pattern, '\\');
  const char *slash2 = strrchr(source_pattern, '/');
  const char *colon = strrchr(source_pattern, ':');
  const char *cut = slash1;
  size_t prefix;
  size_t n = strlen(name);

  if (slash2 != NULL && (cut == NULL || slash2 > cut))
    cut = slash2;
  if (colon != NULL && (cut == NULL || colon > cut))
    cut = colon;

  prefix = cut != NULL ? (size_t)(cut - source_pattern + 1) : 0;
  if (prefix + n >= dst_size)
    return 0;

  if (prefix != 0)
    memcpy(dst, source_pattern, prefix);
  memcpy(dst + prefix, name, n + 1);
  return 1;
}

static void builtin_rename(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *source = next_argument(&cursor);
  char *destination = next_argument(&cursor);
  int rc;

  if (source == NULL || destination == NULL) {
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
    return;
  }

  if (next_argument(&cursor) != NULL) {
    dos_puts(cpu, command_psp, g, "Too many parameters.\r\n");
    return;
  }

  if (strchr(destination, ':') != NULL ||
      strchr(destination, '\\') != NULL ||
      strchr(destination, '/') != NULL) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  if (strlen(source) >= sizeof(g->batch_name) ||
      strlen(destination) >= sizeof(g->batch_args)) {
    dos_puts(cpu, command_psp, g, "Filename too long.\r\n");
    return;
  }

  strcpy(g->batch_name, source);
  strcpy(g->batch_args, destination);

  memset(&g->find, 0, sizeof(g->find));
  if (set_find_dta(cpu, command_psp) < 0)
    return;

  rc = dos_find_first_attr(cpu, command_psp, g,
                           g->batch_name, 0x37);
  if (rc < 0) {
    restore_default_dta(cpu, command_psp);
    dos_puts(cpu, command_psp, g, "File not found.\r\n");
    return;
  }

  do {
    if (strcmp(g->find.dm_name, ".") != 0 &&
        strcmp(g->find.dm_name, "..") != 0) {
      if (!fcom_rename_apply_mask(
              g->program, sizeof(g->program),
              g->find.dm_name, g->batch_args) ||
          !fcom_rename_build_path(
              g->batch_arg, sizeof(g->batch_arg),
              g->batch_name, g->find.dm_name) ||
          !fcom_rename_build_path(
              g->path2, sizeof(g->path2),
              g->batch_name, g->program)) {
        restore_default_dta(cpu, command_psp);
        dos_puts(cpu, command_psp, g, "Filename too long.\r\n");
        return;
      }

      if (dos_rename_file(cpu, command_psp, g,
                          g->batch_arg, g->path2) < 0) {
        restore_default_dta(cpu, command_psp);
        dos_puts(cpu, command_psp, g, "rename\r\n");
        return;
      }

      if (set_find_dta(cpu, command_psp) < 0) {
        restore_default_dta(cpu, command_psp);
        return;
      }
    }

    rc = dos_find_next(cpu, command_psp);
  } while (rc == 0);

  restore_default_dta(cpu, command_psp);
}


static int fcom_parse_numbers(const char *text, unsigned maximum,
                              unsigned *items, unsigned *values,
                              const char **tail)
{
  const char *p = skip_space((char *)text);
  unsigned count = 0;

  while (*p != '\0' && count < maximum) {
    unsigned value = 0;

    if (!isdigit((unsigned char)*p))
      break;

    do {
      value = value * 10u + (unsigned)(*p - '0');
      ++p;
    } while (isdigit((unsigned char)*p));

    values[count++] = value;

    p = skip_space((char *)p);
    if (*p == '\0' || isalpha((unsigned char)*p))
      break;

    if ((unsigned char)*p < 0x20u ||
        (unsigned char)*p >= 0x7fu)
      return 0;

    ++p;
    p = skip_space((char *)p);
  }

  *items = count;
  *tail = p;
  return 1;
}

static int fcom_leap_year(unsigned year)
{
  return (!(year % 4u) && (year % 100u)) || !(year % 400u);
}

static int fcom_parse_date(CPU *cpu, UWORD command_psp,
                           const char *text,
                           unsigned *month, unsigned *day,
                           unsigned *year)
{
  static const UBYTE month_days[2][13] = {
    {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
  };
  unsigned values[3] = {0, 0, 0};
  unsigned items;
  const char *tail;

  if (!fcom_parse_numbers(text, 3, &items, values, &tail))
    return 0;

  tail = skip_space((char *)tail);
  if (*tail != '\0')
    return 0;

  CPU_AH = 0x2a;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM DATE get");

  *year = CPU_CX;
  *month = CPU_DH;
  *day = CPU_DL;

  switch (items) {
  case 0:
    return 2; /* E_Empty */
  case 1:
    *day = values[0];
    break;
  case 2:
    *month = values[0];
    *day = values[1];
    break;
  case 3:
    *month = values[0];
    *day = values[1];
    *year = values[2];
    break;
  default:
    return 0;
  }

  if (*year < 80u)
    *year += 2000u;
  else if (*year < 200u)
    *year += 1900u;

  if (*month < 1u || *month > 12u ||
      *year < 1980u || *year > 2099u ||
      *day < 1u ||
      *day > month_days[fcom_leap_year(*year)][*month])
    return 0;

  return 1;
}

static int fcom_parse_time(const char *text,
                           unsigned *hour, unsigned *minute,
                           unsigned *second, unsigned *hundredth)
{
  unsigned values[4] = {0, 0, 0, 0};
  unsigned items;
  const char *tail;
  int meridian = 0;

  if (!fcom_parse_numbers(text, 4, &items, values, &tail))
    return 0;

  if (toupper((unsigned char)*tail) == 'P') {
    meridian = 2;
    ++tail;
  } else if (toupper((unsigned char)*tail) == 'A') {
    meridian = 1;
    ++tail;
  }

  if (meridian != 0) {
    if (toupper((unsigned char)tail[0]) == 'M') {
      ++tail;
    } else if (tail[0] == '.' &&
               toupper((unsigned char)tail[1]) == 'M' &&
               tail[2] == '.') {
      tail += 3;
    }
  }

  tail = skip_space((char *)tail);
  if (*tail != '\0' || items == 1 || items > 4)
    return 0;
  if (items == 0)
    return 2; /* E_Empty */

  *hour = values[0];
  *minute = values[1];
  *second = values[2];
  *hundredth = values[3];

  if (meridian == 2 && *hour != 12u)
    *hour += 12u;
  else if (meridian == 1 && *hour == 12u)
    *hour = 0u;

  if (*hour >= 24u || *minute >= 60u ||
      *second >= 60u || *hundredth > 99u)
    return 0;

  return 1;
}

static int fcom_datetime_no_prompt(char **args)
{
  char *p = skip_space(*args);
  char *end;
  int result = 0;

  if ((*p != '/' && *p != '-') || p[1] == '\0')
    return 0;

  end = p;
  while (*end != '\0' && *end != ' ' && *end != '\t')
    ++end;

  if ((toupper((unsigned char)p[1]) != 'D' &&
       toupper((unsigned char)p[1]) != 'T') ||
      (p[2] != '\0' && p + 2 != end))
    return -1;

  result = 1;
  *args = skip_space(end);
  return result;
}

static int fcom_read_stdin_line(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, char **line)
{
  int count;

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io);
  CPU_BX = 0;
  CPU_CX = sizeof(g->io) - 1u;
  CPU_AH = 0x3f;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM date/time input");

  if (int21_failed(cpu))
    return 0;

  count = CPU_AX;
  while (count > 0 &&
         (g->io[count - 1] == '\r' || g->io[count - 1] == '\n'))
    --count;

  g->io[count] = '\0';
  *line = (char *)g->io;
  return 1;
}

static void builtin_date(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = args;
  char *value;
  unsigned month;
  unsigned day;
  unsigned year;
  int no_prompt;
  int parsed;
  int n;

  no_prompt = fcom_datetime_no_prompt(&p);
  if (no_prompt < 0) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  p = skip_space(p);
  value = *p != '\0' ? p : NULL;

  if (value == NULL) {
    CPU_AH = 0x2a;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM DATE get");

    n = snprintf(g->text, sizeof(g->text),
                 "Current date is %02u-%02u-%04u\r\n",
                 (unsigned)CPU_DH, (unsigned)CPU_DL,
                 (unsigned)CPU_CX);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);

    if (no_prompt)
      return;
  }

  for (;;) {
    if (value == NULL) {
      dos_puts(cpu, command_psp, g,
               "Enter new date (mm-dd-[cc]yy): ");
      if (!fcom_read_stdin_line(cpu, command_psp, g, &value))
        return;
    }

    parsed = fcom_parse_date(cpu, command_psp, value,
                             &month, &day, &year);
    if (parsed == 2)
      return;

    if (parsed == 1) {
      CPU_CX = (UWORD)year;
      CPU_DH = (UBYTE)month;
      CPU_DL = (UBYTE)day;
      CPU_AH = 0x2b;
      fcom_intcall(cpu, command_psp, 0x21, "FCOM DATE set");

      if (CPU_AL == 0) {
        /*
         * FreeCOM calls _dos_setdate() twice because WinNT may otherwise
         * move the date by one day.
         */
        CPU_CX = (UWORD)year;
        CPU_DH = (UBYTE)month;
        CPU_DL = (UBYTE)day;
        CPU_AH = 0x2b;
        fcom_intcall(cpu, command_psp, 0x21, "FCOM DATE set again");
        return;
      }
    }

    dos_puts(cpu, command_psp, g, "Invalid date.\r\n");
    value = NULL;
  }
}

static void builtin_time(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = args;
  char *value;
  unsigned hour;
  unsigned minute;
  unsigned second;
  unsigned hundredth;
  int no_prompt;
  int parsed;
  int n;

  no_prompt = fcom_datetime_no_prompt(&p);
  if (no_prompt < 0) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  p = skip_space(p);
  value = *p != '\0' ? p : NULL;

  if (value == NULL) {
    CPU_AH = 0x2c;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM TIME get");

    n = snprintf(g->text, sizeof(g->text),
                 "Current time is %02u:%02u:%02u.%02u\r\n",
                 (unsigned)CPU_CH, (unsigned)CPU_CL,
                 (unsigned)CPU_DH, (unsigned)CPU_DL);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);

    if (no_prompt)
      return;
  }

  for (;;) {
    if (value == NULL) {
      dos_puts(cpu, command_psp, g, "Enter new time: ");
      if (!fcom_read_stdin_line(cpu, command_psp, g, &value))
        return;
    }

    parsed = fcom_parse_time(value, &hour, &minute,
                             &second, &hundredth);
    if (parsed == 2)
      return;

    if (parsed == 1) {
      CPU_CH = (UBYTE)hour;
      CPU_CL = (UBYTE)minute;
      CPU_DH = (UBYTE)second;
      CPU_DL = (UBYTE)hundredth;
      CPU_AH = 0x2d;
      fcom_intcall(cpu, command_psp, 0x21, "FCOM TIME set");

      if (CPU_AL == 0)
        return;
    }

    dos_puts(cpu, command_psp, g, "Invalid time.\r\n");
    value = NULL;
  }
}

static void builtin_chcp(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned current;
  unsigned system;
  unsigned codepage = 0;
  int n;

  CPU_AX = 0x6601;
  CPU_BX = 0xffffu;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CHCP get");

  current = CPU_BX;
  system = CPU_DX;

  if (int21_failed(cpu) || current == 0xffffu) {
    dos_puts(cpu, command_psp, g,
             "Failed to acquire current code page from system.\r\n");
    return;
  }

  if (*p == '\0') {
    n = snprintf(g->text, sizeof(g->text),
                 "The current codepage is %u.\r\n"
                 "The system codepage (properly) is: %u.\r\n",
                 current, system);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);
    return;
  }

  if (!isdigit((unsigned char)*p)) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  do {
    codepage = codepage * 10u + (unsigned)(*p - '0');
  } while (isdigit((unsigned char)*++p));

  p = skip_space(p);
  if (*p != '\0' || codepage > 0xffffu) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  CPU_DX = (UWORD)system;
  CPU_BX = (UWORD)codepage;
  CPU_AX = 0x6602;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CHCP set");

  if (int21_failed(cpu))
    dos_puts(cpu, command_psp, g,
             "Failed to change current code page.\r\n");
}


#pragma pack(push, 1)
struct fcom_media_id {
  UWORD info_level;
  UWORD serial1;
  UWORD serial2;
  char volume_label[11];
  char filesystem[8];
};
#pragma pack(pop)

static void builtin_vol(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned current_drive = dos_get_drive(cpu, command_psp);
  unsigned drive;
  struct fcom_media_id *media =
      (struct fcom_media_id *)g->io;
  int media_ok;
  int label_ok;
  int n;

  /*
   * cmd/dir.c:cmd_vol() trims trailing whitespace and accepts either no
   * argument or exactly "X:".
   */
  if (*p == '\0') {
    drive = current_drive;
  } else {
    char *end = p + strlen(p);

    while (end > p &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n'))
      *--end = '\0';

    if (strlen(p) != 2u || p[1] != ':' ||
        !isalpha((unsigned char)p[0])) {
      dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
      return;
    }

    drive = (unsigned)(toupper((unsigned char)p[0]) - 'A');
  }

  if (drive >= 26u || !dos_set_drive(cpu, command_psp, drive)) {
    (void)dos_set_drive(cpu, command_psp, current_drive);
    return;
  }

  memset(media, 0, sizeof(*media));
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io);
  CPU_BX = (UWORD)(drive + 1u);
  CPU_AX = 0x6900;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM VOL media ID");
  media_ok = !int21_failed(cpu);

  memset(&g->find, 0, sizeof(g->find));
  label_ok = 0;

  if (set_find_dta(cpu, command_psp) == 0) {
    if (dos_find_first_attr(cpu, command_psp, g,
                            "\\*.*", 0x08) == 0 &&
        (g->find.dm_attr_fnd & 0x08u))
      label_ok = 1;

    restore_default_dta(cpu, command_psp);
  }

  (void)dos_set_drive(cpu, command_psp, current_drive);

  n = snprintf(g->text, sizeof(g->text),
               " Volume in drive %c ",
               (int)('A' + drive));
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  if (label_ok) {
    char *dot = strchr(g->find.dm_name, '.');

    /*
     * dir_print_header() removes the synthetic 8.3 dot from labels longer
     * than eight characters and pads the base part with spaces.
     */
    if (dot != NULL) {
      char *target = g->find.dm_name + 8;

      memmove(target, dot + 1, 4);
      while (dot < target)
        *dot++ = ' ';
    }

    dos_puts(cpu, command_psp, g, "is ");
    dos_puts(cpu, command_psp, g, g->find.dm_name);
    dos_puts(cpu, command_psp, g, "\r\n");
  } else {
    dos_puts(cpu, command_psp, g, "has no label\r\n");
  }

  if (media_ok) {
    n = snprintf(g->text, sizeof(g->text),
                 " Volume Serial Number is %04X-%04X\r\n",
                 (unsigned)media->serial2,
                 (unsigned)media->serial1);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);
  }
}



static int fcom_get_file_attr(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *name, UWORD *attributes)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AX = 0x4300;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM ATTRIB get");

  if (int21_failed(cpu))
    return -(int)CPU_AX;

  *attributes = CPU_CX;
  return 0;
}

/* Complete; waiting on an ATTRIB built-in (no command-table entry yet). */
KEEP_UNUSED static int fcom_set_file_attr(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *name, UWORD attributes)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_CX = attributes;
  CPU_AX = 0x4301;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM ATTRIB set");

  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static void builtin_beep(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g)
{
  g->io[0] = '\a';
  (void)fcom_write(cpu, command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);
}

static int fcom_file_exists(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g, const char *name)
{
  int handle = dos_open_read(cpu, command_psp, g, name);

  if (handle < 0)
    return 0;

  fcom_close(cpu, command_psp, (UWORD)handle);
  return 1;
}

static int has_extension(const char *name)
{
  const char *base = name;
  const char *p;
  for (p = name; *p; ++p) {
    if (*p == '\\' || *p == '/' || *p == ':')
      base = p + 1;
  }
  return strchr(base, '.') != NULL;
}

static int fcom_try_which_candidate(CPU *cpu, UWORD command_psp,
                                    struct fcom_guest *g,
                                    const char *base,
                                    char *result, size_t result_size)
{
  static const char *const suffixes[] = {
    "", ".COM", ".EXE", ".BAT", ".CMD"
  };
  unsigned first = has_extension(base) ? 0u : 1u;
  unsigned last = has_extension(base) ? 1u :
                  (unsigned)(sizeof(suffixes) / sizeof(suffixes[0]));
  unsigned i;

  for (i = first; i < last; ++i) {
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffixes[i]);

    if (base_len + suffix_len >= result_size)
      continue;

    memcpy(result, base, base_len);
    memcpy(result + base_len, suffixes[i], suffix_len + 1);

    if (fcom_file_exists(cpu, command_psp, g, result))
      return 1;
  }

  return 0;
}

static int path_is_explicit(const char *name)
{
  return strchr(name, '\\') != NULL ||
         strchr(name, '/') != NULL ||
         strchr(name, ':') != NULL;
}

static int make_path_candidate(char *dst, size_t dst_size,
                               const char *dir, size_t dir_len,
                               const char *program)
{
  size_t pn = strlen(program);
  int need_sep = dir_len != 0 && dir[dir_len - 1] != '\\' &&
                 dir[dir_len - 1] != '/' && dir[dir_len - 1] != ':';

  if (dir_len + (size_t)need_sep + pn >= dst_size)
    return 0;
  memcpy(dst, dir, dir_len);
  if (need_sep)
    dst[dir_len++] = '\\';
  memcpy(dst + dir_len, program, pn + 1);
  return 1;
}

static int fcom_find_which(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g,
                           const char *name,
                           char *result, size_t result_size)
{
  char *path_storage;
  const char *path;

  if (path_is_explicit(name))
    return fcom_try_which_candidate(cpu, command_psp, g,
                                    name, result, result_size);

  if (fcom_try_which_candidate(cpu, command_psp, g,
                               name, result, result_size))
    return 1;

  path_storage = fcom_env_alloc_value(command_psp, "PATH", 1024u);
  path = path_storage;
  while (path != NULL && *path != '\0') {
    const char *end = strchr(path, ';');
    size_t len = end != NULL ? (size_t)(end - path) : strlen(path);
    const char *dir = path;

    while (len != 0 && (*dir == ' ' || *dir == '\t')) {
      ++dir;
      --len;
    }
    while (len != 0 &&
           (dir[len - 1] == ' ' || dir[len - 1] == '\t'))
      --len;

    if (len != 0 &&
        make_path_candidate(g->text, sizeof(g->text),
                            dir, len, name) &&
        fcom_try_which_candidate(cpu, command_psp, g,
                                 g->text, result, result_size)) {
      free(path_storage);
      return 1;
    }

    if (end == NULL)
      break;
    path = end + 1;
  }

  free(path_storage);
  return 0;
}

/* Complete; PATH/executable-search helper, waiting on the search to be
   routed through it (and on a WHICH built-in, if one is added). */
KEEP_UNUSED static int fcom_which_candidate(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g,
                                const char *base,
                                char *result, size_t result_size)
{
  static const char *const suffixes[] = {
    ".COM", ".EXE", ".BAT"
  };
  size_t base_len = strlen(base);
  unsigned i;

  if (has_extension(base)) {
    if (base_len >= result_size)
      return 0;

    memcpy(result, base, base_len + 1);
    return fcom_file_exists(cpu, command_psp, g, result);
  }

  for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    size_t suffix_len = strlen(suffixes[i]);

    if (base_len + suffix_len >= result_size)
      continue;

    memcpy(result, base, base_len);
    memcpy(result + base_len, suffixes[i], suffix_len + 1);

    if (fcom_file_exists(cpu, command_psp, g, result))
      return 1;
  }

  return 0;
}

static void builtin_which(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *name;

  while ((name = next_argument(&cursor)) != NULL) {
    dos_puts(cpu, command_psp, g, name);

    if (fcom_find_which(cpu, command_psp, g, name,
                        g->path2, sizeof(g->path2))) {
      dos_puts(cpu, command_psp, g, "\t");
      dos_puts(cpu, command_psp, g, g->path2);
    }

    dos_puts(cpu, command_psp, g, "\r\n");
  }
}

static int fcom_open_readwrite(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AX = 0x3d02;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CTTY open");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int fcom_force_dup_handle(CPU *cpu, UWORD command_psp,
                                 UWORD source, UWORD target)
{
  CPU_BX = source;
  CPU_CX = target;
  CPU_AH = 0x46;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CTTY duplicate");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int fcom_set_handle_attributes(CPU *cpu, UWORD command_psp,
                                      UWORD handle, UWORD attributes)
{
  CPU_BX = handle;
  CPU_DX = attributes;
  CPU_AX = 0x4401;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CTTY set attributes");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static void builtin_ctty(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *device = next_argument(&cursor);
  const char *help;
  UWORD attributes;
  int handle;
  int failed = 0;

  if (device == NULL) {
    help = fcom_command_help("CTTY");
    if (help != NULL)
      fcom_output_resource(cpu, command_psp, g, help);
    return;
  }

  if (next_argument(&cursor) != NULL) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  handle = fcom_open_readwrite(cpu, command_psp, g, device);
  if (handle < 0 ||
      !fcom_handle_attributes(cpu, command_psp,
                              (UWORD)handle, &attributes) ||
      !(attributes & 0x80u)) {
    if (handle >= 0)
      fcom_close(cpu, command_psp, (UWORD)handle);
    dos_puts(cpu, command_psp, g,
             "Invalid or no read-write device '");
    dos_puts(cpu, command_psp, g, device);
    dos_puts(cpu, command_psp, g, "'.\r\n");
    return;
  }

  g->io[0] = '\r';
  g->io[1] = '\n';
  if (fcom_write_handle(cpu, command_psp, (UWORD)handle,
                        FCOM_WORK_OFFSET +
                          (UWORD)offsetof(struct fcom_guest, io),
                        2) != 2) {
    fcom_close(cpu, command_psp, (UWORD)handle);
    dos_puts(cpu, command_psp, g,
             "Invalid or no read-write device '");
    dos_puts(cpu, command_psp, g, device);
    dos_puts(cpu, command_psp, g, "'.\r\n");
    return;
  }

  (void)fcom_set_handle_attributes(
      cpu, command_psp, (UWORD)handle,
      (attributes & 0x00ffu) | 0x0003u);

  /*
   * When CTTY is executed under redirection, restore_redirections() will
   * later copy active_saved_* back onto handles 0 and 1. Replace those saved
   * handles with the new device, matching FreeCOM's oldinfd/oldoutfd logic.
   */
  if (g->active_saved_stdin != 0xffffu)
    failed |= fcom_force_dup_handle(
        cpu, command_psp, (UWORD)handle,
        g->active_saved_stdin) < 0;
  else
    failed |= fcom_force_dup_handle(
        cpu, command_psp, (UWORD)handle, 0) < 0;

  if (g->active_saved_stdout != 0xffffu)
    failed |= fcom_force_dup_handle(
        cpu, command_psp, (UWORD)handle,
        g->active_saved_stdout) < 0;
  else
    failed |= fcom_force_dup_handle(
        cpu, command_psp, (UWORD)handle, 1) < 0;

  failed |= fcom_force_dup_handle(
      cpu, command_psp, (UWORD)handle, 2) < 0;

  fcom_close(cpu, command_psp, (UWORD)handle);

  if (failed) {
    dos_puts(cpu, command_psp, g,
             "Failed to change file descriptors to TTY '");
    dos_puts(cpu, command_psp, g, device);
    dos_puts(cpu, command_psp, g, "'.\r\n");
  }
}



#define FCOM_COPY_ASCII   0x01u
#define FCOM_COPY_BINARY  0x02u

#pragma pack(push, 1)
struct fcom_copy_item {
  UBYTE flags;
  UBYTE group;
  char name[128];
};
#pragma pack(pop)

struct fcom_copy_parse {
  struct fcom_copy_item *items;
  unsigned count;
  unsigned groups;
  int opt_y;
  int opt_v;
  int opt_a;
  int opt_b;
  int dest_flags;
};

static int fcom_create_mode(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g,
                            const char *name, int append)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);

  if (append) {
    SET_DS(command_psp);
    CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
    CPU_AX = 0x3d01;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM COPY open append");

    if (!int21_failed(cpu)) {
      UWORD handle = CPU_AX;

      CPU_BX = handle;
      CPU_CX = 0;
      CPU_DX = 0;
      CPU_AX = 0x4202;
      fcom_intcall(cpu, command_psp, 0x21, "FCOM COPY seek end");
      if (!int21_failed(cpu))
        return handle;

      fcom_close(cpu, command_psp, handle);
    }
  }

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_CX = 0;
  CPU_AH = 0x3c;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM COPY create");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int fcom_copy_write(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g,
                           UWORD handle, UWORD count)
{
  int written = fcom_write_handle(
      cpu, command_psp, handle,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
      count);

  return written == (int)count ? 0 : -5;
}

static int fcom_copy_stream(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g,
                            UWORD source, UWORD destination,
                            int ascii)
{
  for (;;) {
    int count = fcom_read(cpu, command_psp, g, source);
    UWORD write_count;
    UWORD i;

    if (count < 0)
      return count;
    if (count == 0)
      return 0;

    write_count = (UWORD)count;

    if (ascii) {
      for (i = 0; i < write_count; ++i) {
        if (g->io[i] == 0x1a) {
          write_count = i;
          break;
        }
      }
    }

    if (write_count != 0 &&
        fcom_copy_write(cpu, command_psp, g,
                        destination, write_count) < 0)
      return -5;

    if (ascii && write_count != (UWORD)count)
      return 0;
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
static int fcom_path_is_directory(CPU *cpu, UWORD command_psp,
                                  struct fcom_guest *g,
                                  const char *name)
{
  UWORD attributes;
  return fcom_get_file_attr(cpu, command_psp, g,
                            name, &attributes) == 0 &&
         (attributes & D_DIR) != 0;
}
#pragma GCC diagnostic pop

static int fcom_copy_join_path(char *dst, size_t dst_size,
                               const char *directory,
                               const char *name)
{
  size_t dir_len = strlen(directory);
  size_t name_len = strlen(name);
  int need_slash = dir_len != 0 &&
                   directory[dir_len - 1] != '\\' &&
                   directory[dir_len - 1] != '/' &&
                   directory[dir_len - 1] != ':';

  if (dir_len + (size_t)need_slash + name_len >= dst_size)
    return 0;

  memcpy(dst, directory, dir_len);
  if (need_slash)
    dst[dir_len++] = '\\';
  memcpy(dst + dir_len, name, name_len + 1);
  return 1;
}

static int fcom_copy_build_match(char *dst, size_t dst_size,
                                 const char *pattern,
                                 const char *matched_name)
{
  const char *slash1 = strrchr(pattern, '\\');
  const char *slash2 = strrchr(pattern, '/');
  const char *colon = strrchr(pattern, ':');
  const char *cut = slash1;
  size_t prefix;
  size_t name_len = strlen(matched_name);

  if (slash2 != NULL && (cut == NULL || slash2 > cut))
    cut = slash2;
  if (colon != NULL && (cut == NULL || colon > cut))
    cut = colon;

  prefix = cut != NULL ? (size_t)(cut - pattern + 1) : 0;
  if (prefix + name_len >= dst_size)
    return 0;

  if (prefix != 0)
    memcpy(dst, pattern, prefix);
  memcpy(dst + prefix, matched_name, name_len + 1);
  return 1;
}

static int fcom_copy_apply_component(char *dst, size_t dst_size,
                                     const char *source,
                                     const char *mask)
{
  size_t out = 0;
  size_t src = 0;

  while (*mask != '\0') {
    if (*mask == '*') {
      size_t n = strlen(source + src);

      if (out + n >= dst_size)
        return 0;
      memcpy(dst + out, source + src, n);
      out += n;
      src += n;
      ++mask;
      continue;
    }

    if (*mask == '?') {
      if (source[src] != '\0') {
        if (out + 1 >= dst_size)
          return 0;
        dst[out++] = source[src++];
      }
      ++mask;
      continue;
    }

    if (out + 1 >= dst_size)
      return 0;

    dst[out++] = *mask++;
    if (source[src] != '\0')
      ++src;
  }

  dst[out] = '\0';
  return 1;
}

static int fcom_copy_apply_mask(char *dst, size_t dst_size,
                                const char *source_name,
                                const char *mask)
{
  const char *source_dot = strrchr(source_name, '.');
  const char *mask_dot = strrchr(mask, '.');
  char source_base[13];
  char source_ext[4];
  char mask_base[13];
  char mask_ext[4];
  char result_base[13];
  char result_ext[4];
  size_t n;

  if (source_dot == source_name)
    source_dot = NULL;
  if (mask_dot == mask)
    mask_dot = NULL;

  n = source_dot != NULL
      ? (size_t)(source_dot - source_name)
      : strlen(source_name);
  if (n >= sizeof(source_base))
    return 0;
  memcpy(source_base, source_name, n);
  source_base[n] = '\0';

  if (source_dot != NULL) {
    if (strlen(source_dot + 1) >= sizeof(source_ext))
      return 0;
    strcpy(source_ext, source_dot + 1);
  } else {
    source_ext[0] = '\0';
  }

  n = mask_dot != NULL ? (size_t)(mask_dot - mask) : strlen(mask);
  if (n >= sizeof(mask_base))
    return 0;
  memcpy(mask_base, mask, n);
  mask_base[n] = '\0';

  if (mask_dot != NULL) {
    if (strlen(mask_dot + 1) >= sizeof(mask_ext))
      return 0;
    strcpy(mask_ext, mask_dot + 1);
  } else {
    strcpy(mask_ext, source_ext);
  }

  if (!fcom_copy_apply_component(result_base, sizeof(result_base),
                                  source_base, mask_base))
    return 0;
  if (!fcom_copy_apply_component(result_ext, sizeof(result_ext),
                                  source_ext, mask_ext))
    return 0;

  n = strlen(result_base);
  if (result_ext[0] != '\0') {
    size_t en = strlen(result_ext);

    if (n + 1 + en >= dst_size)
      return 0;
    memcpy(dst, result_base, n);
    dst[n++] = '.';
    memcpy(dst + n, result_ext, en + 1);
  } else {
    if (n >= dst_size)
      return 0;
    memcpy(dst, result_base, n + 1);
  }

  return 1;
}

static int fcom_copy_destination(char *dst, size_t dst_size,
                                 const char *destination,
                                 const char *source_name,
                                 int destination_is_directory)
{
  const char *mask = fcom_copy_basename(destination);
  const char *slash1 = strrchr(destination, '\\');
  const char *slash2 = strrchr(destination, '/');
  const char *colon = strrchr(destination, ':');
  const char *cut = slash1;
  char generated[16];
  size_t prefix;

  if (destination_is_directory)
    return fcom_copy_join_path(dst, dst_size,
                               destination, source_name);

  if (slash2 != NULL && (cut == NULL || slash2 > cut))
    cut = slash2;
  if (colon != NULL && (cut == NULL || colon > cut))
    cut = colon;

  if (strchr(mask, '*') == NULL && strchr(mask, '?') == NULL) {
    if (strlen(destination) >= dst_size)
      return 0;
    strcpy(dst, destination);
    return 1;
  }

  if (!fcom_copy_apply_mask(generated, sizeof(generated),
                            source_name, mask))
    return 0;

  prefix = cut != NULL ? (size_t)(cut - destination + 1) : 0;
  if (prefix + strlen(generated) >= dst_size)
    return 0;

  if (prefix != 0)
    memcpy(dst, destination, prefix);
  strcpy(dst + prefix, generated);
  return 1;
}

static int fcom_copy_confirm_overwrite(CPU *cpu, UWORD command_psp,
                                       struct fcom_guest *g,
                                       const char *destination,
                                       int *all)
{
  UWORD attributes;
  int ch;

  if (*all)
    return 1;

  if (fcom_get_file_attr(cpu, command_psp, g,
                         destination, &attributes) < 0)
    return 1;

  dos_puts(cpu, command_psp, g, "Overwrite ");
  dos_puts(cpu, command_psp, g, destination);
  dos_puts(cpu, command_psp, g, " (Yes/No/All/Quit)? ");

  for (;;) {
    CPU_AH = 0x08;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM COPY confirm");
    ch = toupper((unsigned char)CPU_AL);

    if (ch == 'Y' || ch == 'N' || ch == 'A' || ch == 'Q')
      break;
  }

  g->io[0] = (UBYTE)ch;
  g->io[1] = '\r';
  g->io[2] = '\n';
  (void)fcom_write(cpu, command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 3);

  if (ch == 'A') {
    *all = 1;
    return 1;
  }
  if (ch == 'Q')
    return -1;
  return ch == 'Y';
}

static void fcom_copy_error1(CPU *cpu, UWORD command_psp,
                             struct fcom_guest *g,
                             const char *prefix, const char *name)
{
  dos_puts(cpu, command_psp, g, prefix);
  dos_puts(cpu, command_psp, g, name);
  dos_puts(cpu, command_psp, g, "\r\n");
}

static char *fcom_copy_scan_token(char **cursor, int *plus)
{
  char *p = skip_space(*cursor);
  char *start;
  char *dst;

  *plus = 0;

  while (*p == '+') {
    *plus = 1;
    ++p;
    p = skip_space(p);
  }

  if (*p == '\0') {
    *cursor = p;
    return NULL;
  }

  start = p;
  dst = p;

  if (*p == '"') {
    ++p;
    start = dst = p;
    while (*p != '\0' && *p != '"')
      *dst++ = *p++;
    if (*p == '"')
      ++p;
  } else {
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '+')
      *dst++ = *p++;
  }

  *dst = '\0';

  p = skip_space(p);
  if (*p == '+') {
    *plus = 1;
    while (*p == '+')
      ++p;
  }

  *cursor = p;
  return start;
}

static int fcom_copy_option(const char *arg,
                            struct fcom_copy_parse *parse,
                            int *file_flags)
{
  int enable = 1;
  const char *p = arg;

  if (*p != '/' && *p != '-')
    return 0;
  ++p;

  if (*p == '-') {
    enable = 0;
    ++p;
  }

  if (p[0] == '\0' || p[1] != '\0')
    return 0;

  switch (toupper((unsigned char)p[0])) {
  case 'Y':
    parse->opt_y = enable;
    return 1;
  case 'V':
    parse->opt_v = enable;
    return 1;
  case 'A':
    parse->opt_a = enable;
    if (enable) {
      parse->opt_b = 0;
      *file_flags = FCOM_COPY_ASCII;
    }
    return 1;
  case 'B':
    parse->opt_b = enable;
    if (enable) {
      parse->opt_a = 0;
      *file_flags = FCOM_COPY_BINARY;
    }
    return 1;
  default:
    return 0;
  }
}

static void fcom_copy_parse_env(UWORD command_psp,
                                struct fcom_copy_parse *parse)
{
  char *env = fcom_env_alloc_value(command_psp, "COPYCMD", 256u);
  char *cursor;

  if (env == NULL)
    return;

  if (*env == '\0' ||
      strlen(env) >= sizeof(((struct fcom_guest *)0)->batch_line)) {
    free(env);
    return;
  }

  /*
   * COPYCMD contributes options only. FreeCOM explicitly discards any
   * non-option parameters returned by scanCmdline().
   */
  cursor = (char *)env;
  while (*cursor != '\0') {
    char token[8];
    size_t n = 0;

    cursor = skip_space(cursor);
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
           n + 1 < sizeof(token))
      token[n++] = *cursor++;
    token[n] = '\0';

    if (n != 0) {
      int flags = 0;
      (void)fcom_copy_option(token, parse, &flags);
    }
  }

  free(env);
}

static int fcom_copy_count_items(char *line, unsigned *items,
                                 unsigned *groups)
{
  char *cursor = line;
  char *arg;
  int plus;
  int previous = 0;

  *items = 0;
  *groups = 0;

  while ((arg = fcom_copy_scan_token(&cursor, &plus)) != NULL) {
    if ((arg[0] == '/' || arg[0] == '-') && arg[1] != '\0')
      continue;

    if (plus && !previous)
      return -1;

    ++*items;
    if (!plus)
      ++*groups;
    previous = 1;
  }

  if (plus)
    return -2;
  return 0;
}

static int fcom_copy_parse_items(CPU *cpu, UWORD command_psp,
                                 struct fcom_guest *g,
                                 struct fcom_copy_parse *parse,
                                 char *line)
{
  char *cursor = line;
  char *arg;
  int plus;
  int flags = parse->opt_a
      ? FCOM_COPY_ASCII
      : (parse->opt_b ? FCOM_COPY_BINARY : 0);
  unsigned group = 0;
  unsigned index = 0;

  while ((arg = fcom_copy_scan_token(&cursor, &plus)) != NULL) {
    if (fcom_copy_option(arg, parse, &flags)) {
      if (index != 0)
        parse->items[index - 1].flags = (UBYTE)flags;
      continue;
    }

    if (!plus)
      ++group;

    if (index >= parse->count ||
        strlen(arg) >= sizeof(parse->items[index].name))
      return 0;

    parse->items[index].flags = (UBYTE)flags;
    parse->items[index].group = (UBYTE)group;
    strcpy(parse->items[index].name, arg);
    ++index;
  }

  parse->count = index;
  parse->groups = group;
  (void)cpu;
  (void)command_psp;
  (void)g;
  return 1;
}

static int fcom_copy_source_exists(CPU *cpu, UWORD command_psp,
                                   struct fcom_guest *g,
                                   const char *name)
{
  int handle = dos_open_read(cpu, command_psp, g, name);

  if (handle < 0)
    return 0;
  fcom_close(cpu, command_psp, (UWORD)handle);
  return 1;
}

static int fcom_copy_group(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g,
                           struct fcom_copy_parse *parse,
                           unsigned first, unsigned last,
                           const char *destination,
                           int destination_is_directory,
                           int append_destination,
                           int destination_flags,
                           int *all,
                           unsigned *copied)
{
  struct fcom_copy_item *lead = &parse->items[first];
  int wildcard = strchr(lead->name, '*') != NULL ||
                 strchr(lead->name, '?') != NULL;
  int rc;
  int found = 0;

  if (!wildcard) {
    if (!fcom_copy_source_exists(cpu, command_psp, g, lead->name)) {
      fcom_copy_error1(cpu, command_psp, g,
                       "Unable to open file '", lead->name);
      dos_puts(cpu, command_psp, g, "'\r\n");
      return 0;
    }

    if (strlen(lead->name) >= sizeof(g->batch_arg))
      return 0;
    strcpy(g->batch_arg, lead->name);
    found = 1;
  } else {
    memset(&g->find, 0, sizeof(g->find));
    if (set_find_dta(cpu, command_psp) < 0)
      return 0;

    rc = dos_find_first_attr(cpu, command_psp, g, lead->name, 0x21);
    if (rc < 0) {
      restore_default_dta(cpu, command_psp);
      fcom_copy_error1(cpu, command_psp, g,
                       "File not found - ", lead->name);
      return 0;
    }
  }

  do {
    const char *source_name;
    const char *base;
    unsigned i;
    int output;
    int confirmation;
    int dest_ascii =
        (destination_flags & FCOM_COPY_ASCII) != 0;

    if (wildcard) {
      if (g->find.dm_attr_fnd & D_DIR) {
        rc = dos_find_next(cpu, command_psp);
        continue;
      }

      if (!fcom_copy_build_match(g->batch_arg,
                                 sizeof(g->batch_arg),
                                 lead->name, g->find.dm_name)) {
        restore_default_dta(cpu, command_psp);
        return 0;
      }
      found = 1;
    }

    source_name = g->batch_arg;
    base = fcom_copy_basename(source_name);

    if (!fcom_copy_destination(g->path2, sizeof(g->path2),
                               destination, base,
                               destination_is_directory)) {
      if (wildcard)
        restore_default_dta(cpu, command_psp);
      return 0;
    }

    for (i = first; i <= last; ++i) {
      if (strcasecmp(parse->items[i].name, g->path2) == 0) {
        fcom_copy_error1(cpu, command_psp, g,
                         "Cannot copy '", g->path2);
        dos_puts(cpu, command_psp, g, "' to itself\r\n");
        if (wildcard)
          restore_default_dta(cpu, command_psp);
        return 0;
      }
    }

    confirmation = parse->opt_y
        ? 1
        : fcom_copy_confirm_overwrite(
              cpu, command_psp, g, g->path2, all);
    if (confirmation < 0) {
      if (wildcard)
        restore_default_dta(cpu, command_psp);
      return 0;
    }
    if (!confirmation) {
      if (wildcard)
        rc = dos_find_next(cpu, command_psp);
      continue;
    }

    output = fcom_create_mode(cpu, command_psp, g,
                              g->path2, append_destination);
    if (output < 0) {
      fcom_copy_error1(cpu, command_psp, g,
                       "Unable to open file '", g->path2);
      dos_puts(cpu, command_psp, g, "'\r\n");
      if (wildcard)
        restore_default_dta(cpu, command_psp);
      return 0;
    }

    for (i = first; i <= last; ++i) {
      char source_path[128];
      int input;
      int ascii = (parse->items[i].flags & FCOM_COPY_ASCII) != 0;

      if (i == first) {
        if (strlen(source_name) >= sizeof(source_path)) {
          fcom_close(cpu, command_psp, (UWORD)output);
          if (wildcard)
            restore_default_dta(cpu, command_psp);
          return 0;
        }
        strcpy(source_path, source_name);
      } else if (!fcom_copy_build_match(
                     source_path, sizeof(source_path),
                     parse->items[i].name, base)) {
        fcom_close(cpu, command_psp, (UWORD)output);
        if (wildcard)
          restore_default_dta(cpu, command_psp);
        return 0;
      }

      input = dos_open_read(cpu, command_psp, g, source_path);
      if (input < 0) {
        fcom_copy_error1(cpu, command_psp, g,
                         "Unable to open file '", source_path);
        dos_puts(cpu, command_psp, g, "'\r\n");
        fcom_close(cpu, command_psp, (UWORD)output);
        if (wildcard)
          restore_default_dta(cpu, command_psp);
        return 0;
      }

      {
        int n = snprintf(g->text, sizeof(g->text),
                         "%s %s %s\r\n", source_path,
                         (append_destination || i != first)
                             ? "=>>" : "=>",
                         g->path2);
        if (n > 0)
          (void)fcom_write(cpu, command_psp,
              FCOM_WORK_OFFSET +
                (UWORD)offsetof(struct fcom_guest, text),
              (UWORD)n);
      }

      rc = fcom_copy_stream(cpu, command_psp, g,
                            (UWORD)input, (UWORD)output, ascii);
      fcom_close(cpu, command_psp, (UWORD)input);
      if (rc < 0) {
        fcom_close(cpu, command_psp, (UWORD)output);
        if (wildcard)
          restore_default_dta(cpu, command_psp);
        dos_puts(cpu, command_psp, g, "COPY failed\r\n");
        return 0;
      }

    }

    if (dest_ascii) {
      g->io[0] = 0x1a;
      if (fcom_copy_write(cpu, command_psp, g,
                          (UWORD)output, 1) < 0) {
        fcom_close(cpu, command_psp, (UWORD)output);
        if (wildcard)
          restore_default_dta(cpu, command_psp);
        return 0;
      }
    }

    fcom_close(cpu, command_psp, (UWORD)output);
    ++*copied;

    if (wildcard)
      rc = dos_find_next(cpu, command_psp);
  } while (wildcard && rc == 0);

  if (wildcard)
    restore_default_dta(cpu, command_psp);

  return found;
}

static void builtin_copy(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  /*
   * По результатам инспекции (см. STACK-INVENTORY): fcom_copy_parse -
   * ~36 байт скаляров.  COPY item bookkeeping is native-only state; keep it
   * in the native heap rather than guest RAM so FAT/DOS calls cannot remap
   * a backing page underneath parse->items.  Paths stay in the FCOM shadow.
   */
  struct fcom_copy_parse parse_storage;
  struct fcom_copy_parse *parse = &parse_storage;
  unsigned item_count;
  unsigned group_count;
  unsigned destination_index;
  unsigned group;
  unsigned copied = 0;
  int destination_is_directory;
  int all = 0;
  int count_rc;
  size_t items_bytes;

  memset(parse, 0, sizeof(*parse));
  fcom_copy_parse_env(command_psp, parse);

  if (strlen(args) >= sizeof(g->batch_line) ||
      strlen(args) >= sizeof(g->redirect_command)) {
    dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
    return;
  }

  strcpy(g->batch_line, args);
  count_rc = fcom_copy_count_items(
      g->batch_line, &item_count, &group_count);

  if (count_rc == -1) {
    dos_puts(cpu, command_psp, g,
             "The concatenation character '+' cannot lead the arguments.\r\n");
    return;
  }
  if (count_rc == -2) {
    dos_puts(cpu, command_psp, g,
             "The concatenation character '+' cannot trail the arguments.\r\n");
    return;
  }
  if (item_count == 0) {
    dos_puts(cpu, command_psp, g, "Nothing to do.\r\n");
    return;
  }

  items_bytes = (size_t)item_count * sizeof(struct fcom_copy_item);
  parse->items = (struct fcom_copy_item *)malloc(items_bytes);
  if (parse->items == NULL) {
    dos_puts(cpu, command_psp, g, "Out of memory.\r\n");
    return;
  }

  parse->count = item_count;
  memset(parse->items, 0, items_bytes);

  strcpy(g->redirect_command, args);
  if (!fcom_copy_parse_items(cpu, command_psp, g,
                             parse, g->redirect_command)) {
    free(parse->items);
    dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
    return;
  }

  if (parse->groups > 1) {
    destination_index = parse->count - 1;

    if (destination_index != 0 &&
        parse->items[destination_index].group ==
        parse->items[destination_index - 1].group) {
      free(parse->items);
      dos_puts(cpu, command_psp, g,
               "The COPY destination must not contain plus ('+') characters.\r\n");
      return;
    }

    if (strlen(parse->items[destination_index].name) >= sizeof(g->program)) {
      free(parse->items);
      dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
      return;
    }
    strcpy(g->program, parse->items[destination_index].name);
    parse->dest_flags = parse->items[destination_index].flags;
    --parse->count;
    --parse->groups;
  } else {
    strcpy(g->program, ".\\*.*");
    parse->dest_flags = 0;
  }

  destination_is_directory =
      fcom_path_is_directory(cpu, command_psp, g, g->program);

  for (group = 1; group <= parse->groups; ++group) {
    unsigned first = 0;
    unsigned last;

    while (first < parse->count &&
           parse->items[first].group != group)
      ++first;
    if (first == parse->count)
      continue;

    last = first;
    while (last + 1 < parse->count &&
           parse->items[last + 1].group == group)
      ++last;

    /*
     * FreeCOM copy.c: concatenation defaults to ASCII for every source
     * whose mode was not specified, and for the destination as well.
     */
    if (last != first) {
      unsigned i;

      for (i = first; i <= last; ++i) {
        if (parse->items[i].flags == 0)
          parse->items[i].flags = FCOM_COPY_ASCII;
      }

      if (parse->dest_flags == 0)
        parse->dest_flags = FCOM_COPY_ASCII;
    }

    if (!fcom_copy_group(cpu, command_psp, g, parse,
                         first, last, g->program,
                         destination_is_directory,
                         0, parse->dest_flags,
                         &all, &copied))
      break;
  }

  free(parse->items);

  {
    int n = snprintf(g->text, sizeof(g->text),
                     "%u file(s) copied.\r\n", copied);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);
  }
}



static int fcom_alias_name_char(int ch)
{
  return isalnum((unsigned char)ch) ||
         ch == '_' || ch == '-' || ch == '$';
}

static UWORD fcom_alias_find(UWORD command_psp,
                             struct fcom_guest *g,
                             const char *name)
{
  char *storage = fcom_alias_storage(command_psp);
  size_t pos = 0;

  while (pos < g->alias_used) {
    const char *entry_name = storage + pos;
    size_t name_len = strlen(entry_name);
    const char *value = entry_name + name_len + 1;
    size_t value_len = strlen(value);

    if (strcasecmp(entry_name, name) == 0)
      return (UWORD)pos;

    pos += name_len + 1 + value_len + 1;
  }

  return 0xffffu;
}

static void fcom_alias_remove_at(UWORD command_psp,
                                 struct fcom_guest *g,
                                 UWORD offset)
{
  char *storage = fcom_alias_storage(command_psp);
  size_t name_len = strlen(storage + offset);
  size_t value_offset = (size_t)offset + name_len + 1;
  size_t value_len = strlen(storage + value_offset);
  size_t entry_len = name_len + 1 + value_len + 1;
  size_t tail = g->alias_used - (size_t)offset - entry_len;

  if (tail != 0)
    memmove(storage + offset, storage + offset + entry_len, tail);

  g->alias_used -= (UWORD)entry_len;
  if (g->alias_used < FCOM_ALIAS_BYTES)
    storage[g->alias_used] = '\0';
}

static int fcom_alias_set(UWORD command_psp,
                          struct fcom_guest *g,
                          const char *name, const char *value)
{
  char *storage = fcom_alias_storage(command_psp);
  UWORD old = fcom_alias_find(command_psp, g, name);
  size_t name_len = strlen(name);
  size_t value_len = value != NULL ? strlen(value) : 0;
  size_t needed;

  if (old != 0xffffu)
    fcom_alias_remove_at(command_psp, g, old);

  if (value == NULL || *value == '\0')
    return 1;

  needed = name_len + 1 + value_len + 1;
  if ((size_t)g->alias_used + needed > FCOM_ALIAS_BYTES)
    return 0;

  memcpy(storage + g->alias_used, name, name_len + 1);
  memcpy(storage + g->alias_used + name_len + 1,
         value, value_len + 1);
  g->alias_used += (UWORD)needed;
  return 1;
}

static unsigned fcom_alias_count(UWORD command_psp,
                                 const struct fcom_guest *g)
{
  const char *storage = fcom_alias_storage(command_psp);
  size_t pos = 0;
  unsigned count = 0;

  while (pos < g->alias_used) {
    size_t name_len = strlen(storage + pos);
    size_t value_len = strlen(storage + pos + name_len + 1);

    pos += name_len + 1 + value_len + 1;
    ++count;
  }

  return count;
}

static unsigned fcom_string_table_count(const char *storage,
                                        UWORD used)
{
  size_t pos = 0;
  unsigned count = 0;

  while (pos < used) {
    pos += strlen(storage + pos) + 1;
    ++count;
  }

  return count;
}

static void builtin_alias(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  char *storage = fcom_alias_storage(command_psp);
  char *p = skip_space(args);
  char *equal;
  size_t pos;

  /*
   * cmd/alias.c rejects every option before breakVarAssign().
   */
  if ((*p == '/' || *p == '-') && p[1] != '\0') {
    dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
    return;
  }

  if (*p == '\0') {
    pos = 0;
    while (pos < g->alias_used) {
      const char *name = storage + pos;
      size_t name_len = strlen(name);
      const char *value = name + name_len + 1;
      size_t value_len = strlen(value);

      dos_puts(cpu, command_psp, g, name);
      dos_puts(cpu, command_psp, g, "=");
      dos_puts(cpu, command_psp, g, value);
      dos_puts(cpu, command_psp, g, "\r\n");

      pos += name_len + 1 + value_len + 1;
    }
    return;
  }

  equal = strchr(p, '=');
  if (equal == NULL || equal == p) {
    dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
    return;
  }

  *equal++ = '\0';

  {
    char *name = p;
    char *value;
    char *end;
    size_t i;

    end = name + strlen(name);
    while (end > name && (end[-1] == ' ' || end[-1] == '\t'))
      *--end = '\0';

    if (*name == '\0') {
      dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
      return;
    }

    for (i = 0; name[i] != '\0'; ++i) {
      if (!fcom_alias_name_char((unsigned char)name[i])) {
        dos_puts(cpu, command_psp, g, "Invalid alias name - ");
        dos_puts(cpu, command_psp, g, name);
        dos_puts(cpu, command_psp, g, "\r\n");
        return;
      }
      name[i] = (char)toupper((unsigned char)name[i]);
    }
    
    value = skip_space(equal);
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
      *--end = '\0';

    if (!fcom_alias_set(command_psp, g, name,
                        *value != '\0' ? value : NULL))
      dos_puts(cpu, command_psp, g, "Alias table full.\r\n");
  }
}

static int fcom_alias_was_used(const UWORD *used,
                               unsigned used_count, UWORD offset)
{
  unsigned i;

  for (i = 0; i < used_count; ++i) {
    if (used[i] == offset)
      return 1;
  }

  return 0;
}

static void fcom_expand_aliases(UWORD command_psp,
                                struct fcom_guest *g,
                                char *line, size_t line_size)
{
  UWORD *used = (UWORD *)g->io;
  unsigned used_count = 0;

  for (;;) {
    char *command = skip_space(line);
    char *end;
    char saved;
    UWORD offset;
    char *storage;
    const char *value;
    size_t prefix_len;
    size_t value_len;
    size_t rest_len;

    if (*command == '*') {
      memmove(line, command + 1, strlen(command));
      return;
    }

    end = command;
    while (fcom_alias_name_char((unsigned char)*end) || *end == '.')
      ++end;

    if (end == command || *end == '\\' || *end == '/' ||
        *end == ':' || *end == '"')
      return;

    saved = *end;
    *end = '\0';
    offset = fcom_alias_find(command_psp, g, command);
    *end = saved;

    if (offset == 0xffffu ||
        fcom_alias_was_used(used, used_count, offset))
      return;

    if ((used_count + 1u) * sizeof(UWORD) > sizeof(g->io))
      return;
    used[used_count++] = offset;

    storage = fcom_alias_storage(command_psp);
    value = storage + offset + strlen(storage + offset) + 1;

    prefix_len = (size_t)(command - line);
    value_len = strlen(value);
    rest_len = strlen(end);

    if (prefix_len + value_len + rest_len >= line_size)
      return;

    memmove(g->for_command, line, prefix_len);
    memcpy(g->for_command + prefix_len, value, value_len);
    memcpy(g->for_command + prefix_len + value_len,
           end, rest_len + 1);
    memcpy(line, g->for_command,
           prefix_len + value_len + rest_len + 1);
  }
}



static void fcom_history_drop_oldest(UWORD command_psp,
                                     struct fcom_guest *g)
{
  char *storage = fcom_history_storage(command_psp);
  size_t oldest;

  if (g->history_used == 0)
    return;

  oldest = strlen(storage) + 1;
  if (oldest >= g->history_used) {
    g->history_used = 0;
    storage[0] = '\0';
    return;
  }

  memmove(storage, storage + oldest, g->history_used - oldest);
  g->history_used -= (UWORD)oldest;
}

static const char *fcom_history_last(UWORD command_psp,
                                     struct fcom_guest *g)
{
  char *storage = fcom_history_storage(command_psp);
  size_t pos = 0;
  const char *last = NULL;

  while (pos < g->history_used) {
    last = storage + pos;
    pos += strlen(last) + 1;
  }

  return last;
}

static void fcom_history_add(UWORD command_psp,
                             struct fcom_guest *g,
                             const char *line)
{
  char *storage = fcom_history_storage(command_psp);
  const char *p = line;
  const char *last;
  size_t length;

  while (*p == ' ' || *p == '\t')
    ++p;

  if (*p == '\0')
    return;

  length = strlen(p) + 1;
  if (length > FCOM_HISTORY_BYTES)
    return;

  last = fcom_history_last(command_psp, g);
  if (last != NULL && strcmp(last, p) == 0)
    return;

  while ((size_t)g->history_used + length > FCOM_HISTORY_BYTES)
    fcom_history_drop_oldest(command_psp, g);

  memcpy(storage + g->history_used, p, length);
  g->history_used += (UWORD)length;
}

static void builtin_history(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g, char *args)
{
  char *storage = fcom_history_storage(command_psp);
  char *p = skip_space(args);
  size_t pos = 0;

  if (*p != '\0') {
    unsigned long value = 0;

    /*
     * cmd/history.c uses atol(): a numeric prefix is accepted and trailing
     * text is ignored.
     */
    while (isdigit((unsigned char)*p)) {
      value = value * 10ul + (unsigned long)(*p - '0');
      ++p;
    }

    if (value < 256ul || value > 32768ul) {
      dos_puts(cpu, command_psp, g, "Invalid history size '");
      dos_puts(cpu, command_psp, g, skip_space(args));
      dos_puts(cpu, command_psp, g, "'.\r\n");
      return;
    }

    dos_puts(cpu, command_psp, g,
             "HISTORY: To change to size of the history is not implemented, yet\r\n");
    return;
  }

  if (g->history_used == 0) {
    dos_puts(cpu, command_psp, g, "Command line history empty.\r\n");
    return;
  }

  while (pos < g->history_used) {
    const char *entry = storage + pos;

    dos_puts(cpu, command_psp, g, entry);
    dos_puts(cpu, command_psp, g, "\r\n");
    pos += strlen(entry) + 1;
  }
}



static void fcom_output_resource(CPU *cpu, UWORD command_psp,
                                 struct fcom_guest *g,
                                 const char *text)
{
  size_t used = 0;

  while (*text != '\0') {
    if (*text == '\n') {
      if (used + 2 > sizeof(g->io)) {
        (void)fcom_write(cpu, command_psp,
            FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
            (UWORD)used);
        used = 0;
      }
      g->io[used++] = '\r';
      g->io[used++] = '\n';
      ++text;
      continue;
    }

    if (used == sizeof(g->io)) {
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
          (UWORD)used);
      used = 0;
    }

    g->io[used++] = (UBYTE)*text++;
  }

  if (used != 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
        (UWORD)used);
}

static void fcom_output_guest_resource(CPU *cpu, UWORD command_psp,
                                       struct fcom_guest *g,
                                       dos_far_ptr text)
{
  uint32_t p = fcom_guest_linear(FP_SEG(text), FP_OFF(text));
  size_t used = 0;
  for (;;) {
    UBYTE c = fcom_guest_read8(p++);
    if (c == 0)
      break;
    if (c == '\n') {
      if (used + 2 > sizeof(g->io)) {
        (void)fcom_write(cpu, command_psp,
            FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
            (UWORD)used);
        used = 0;
      }
      g->io[used++] = '\r';
      g->io[used++] = '\n';
      continue;
    }
    if (used == sizeof(g->io)) {
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
          (UWORD)used);
      used = 0;
    }
    g->io[used++] = c;
  }
  if (used != 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
        (UWORD)used);
}

static const char fcom_ver_warranty[] =
  "Copyright (C) 1994-2005 Tim Norman and others.\n"
  "\n"
  "This program is distributed in the hope that it will be useful, but\n"
  "WITHOUT ANY WARRANTY; without even the implied warranty of\n"
  "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n"
  "General Public License for more details.\n"
  "\n"
  "Send bug reports to freedos-devel@lists.sourceforge.net.\n"
  "Updates are available from http://freedos.sourceforge.net/freecom\n";

static const char fcom_ver_redistribution[] =
  "Copyright (C) 1994-2005 Tim Norman and others.\n"
  "\n"
  "This program is free software; you can redistribute it and/or modify\n"
  "it under the terms of the GNU General Public License as published by\n"
  "the Free Software Foundation; either version 2 of the License, or (at\n"
  "your option) any later version.\n"
  "\n"
  "Send bug reports to freedos-devel@lists.sourceforge.net.\n"
  "Updates are available from http://freedos.sourceforge.net/freecom\n";

static const char fcom_ver_developers[] =
  "\n"
  "The FreeDOS Command Shell developed by many developers, please refer\n"
  "to the enclosed HISTORY.TXT file.\n"
  "\n"
  "Send bug reports to freedos-devel@lists.sourceforge.net.\n"
  "Updates are available from http://freedos.sourceforge.net/freecom\n";

static int fcom_ver_option(const char *arg, int letter, int *value)
{
  const char *p = arg;
  int enabled = 1;

  if (*p != '/' && *p != '-')
    return 0;
  ++p;

  if (*p == '-') {
    enabled = 0;
    ++p;
  }

  if (toupper((unsigned char)p[0]) != letter || p[1] != '\0')
    return 0;

  *value = enabled;
  return 1;
}

static void builtin_ver(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *arg;
  int opt_r = 0;
  int opt_w = 0;
  int opt_d = 0;
  int opt_c = 0;
  int n;

  n = snprintf(g->text, sizeof(g->text),
               "\r\nFreeCom version 0.86 - GNUC [%s %s]\r\n",
               __DATE__, __TIME__);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  while ((arg = next_argument(&cursor)) != NULL) {
    if (fcom_ver_option(arg, 'R', &opt_r) ||
        fcom_ver_option(arg, 'W', &opt_w) ||
        fcom_ver_option(arg, 'D', &opt_d) ||
        fcom_ver_option(arg, 'C', &opt_c))
      continue;

    /* Original VER silently ignores non-option arguments. */
    if ((arg[0] == '/' || arg[0] == '-') && arg[1] != '\0') {
      dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
      return;
    }
  }

  if (opt_r) {
    CPU_AX = 0x3000;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM VER DOS version");

    n = snprintf(g->text, sizeof(g->text),
                 "DOS version %u.%u\r\n",
                 (unsigned)CPU_AL, (unsigned)CPU_AH);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);

    if (CPU_BH == 0xfdu) {
      if (CPU_BL == 0xffu) {
        dos_puts(cpu, command_psp, g,
                 "FreeDOS kernel (build 1933 or prior)\r\n");
      } else {
        CPU_AX = 0x33ff;
        fcom_intcall(cpu, command_psp, 0x21,
                     "FCOM VER FreeDOS version");
        if (!int21_failed(cpu)) {
          fcom_output_guest_resource(cpu, command_psp, g,
                                     MK_FP(CPU_DX, CPU_AX));
        }
      }
    }
  }

  if (opt_w)
    fcom_output_resource(cpu, command_psp, g, fcom_ver_warranty);
  if (opt_d)
    fcom_output_resource(cpu, command_psp, g,
                         fcom_ver_redistribution);
  if (opt_c)
    fcom_output_resource(cpu, command_psp, g,
                         fcom_ver_developers);
}


static const char *fcom_command_help(const char *name)
{
  const struct fcom_help_entry *entry;

  for (entry = fcom_help_table; entry->name != NULL; ++entry) {
    if (strcasecmp(entry->name, name) == 0)
      return entry->text;
  }

  return NULL;
}

static void fcom_showcmd_name(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *name, unsigned column)
{
  int n;

  if (column == 7)
    n = snprintf(g->text, sizeof(g->text), "%s\n", name);
  else
    n = snprintf(g->text, sizeof(g->text), "%-10s", name);

  if (n > 0)
    fcom_output_resource(cpu, command_psp, g, g->text);
}

static void builtin_question(CPU *cpu, UWORD command_psp,
                             struct fcom_guest *g, char *args)
{
  static const char *const commands[] = {
    "ALIAS", "BEEP", "BREAK", "CALL", "CD", "CHDIR", "CDD", "CHCP",
    "CLS", "COPY", "CTTY", "DATE", "DEL", "DIR", "DIRS", "DOSKEY",
    "ECHO", "ERASE", "EXIT", "FOR", "GOTO", "HISTORY", "IF", "LFNFOR",
    "LH", "LOADHIGH", "LOADFIX", "MEMORY", "MD", "MKDIR", "PATH", "PAUSE",
    "PROMPT", "PUSHD", "POPD", "RD", "REM", "REN", "RENAME", "RMDIR",
    "SET", "SHIFT", "TIME", "TITLE", "TRUENAME", "TYPE", "VER", "VERIFY",
    "VOL", "?", "FDDEBUG", "WHICH"
  };
  unsigned i;

  (void)args;
  fcom_output_resource(cpu, command_psp, g,
                       fcom_text_msg_showcmd_internal_commands);

  for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
    fcom_showcmd_name(cpu, command_psp, g, commands[i], i & 7u);

  if ((sizeof(commands) / sizeof(commands[0])) & 7u)
    fcom_output_resource(cpu, command_psp, g, "\n");

  fcom_output_resource(cpu, command_psp, g,
                       fcom_text_msg_showcmd_features);
  fcom_output_resource(cpu, command_psp, g,
                       fcom_text_showcmd_feature_aliases);
  fcom_output_resource(cpu, command_psp, g,
                       fcom_text_showcmd_feature_history);
  fcom_output_resource(cpu, command_psp, g,
                       fcom_text_showcmd_feature_dirstack);
  fcom_output_resource(cpu, command_psp, g,
                       fcom_text_showcmd_feature_debug);
  fcom_output_resource(cpu, command_psp, g, "\n");
}


static void builtin_doskey(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  (void)args;
  dos_puts(cpu, command_psp, g,
           "DOSKEY features are already enabled in the shell.\r\n");
}

static void builtin_memory(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  unsigned env_max = fcom_environment_bytes(command_psp);
  unsigned env_used = fcom_environment_used(command_psp);
  unsigned alias_count =
      fcom_alias_count(command_psp, g);
  unsigned history_count =
      fcom_string_table_count(
          fcom_history_storage(command_psp), g->history_used);
  unsigned dirstack_count =
      fcom_string_table_count(
          fcom_dir_stack_storage(command_psp), g->dir_stack_used);
  unsigned context_free =
      FCOM_STACK_BYTES;
  int n;

  (void)args;

  n = snprintf(g->text, sizeof(g->text),
               "Environment segment    : max %5u bytes; free %5u bytes\r\n",
               env_max, env_max > env_used ? env_max - env_used : 0u);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  n = snprintf(g->text, sizeof(g->text),
               "Context segment        : max %5u bytes; free %5u bytes\r\n",
               (unsigned)FCOM_PROCESS_BYTES, context_free);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  n = snprintf(g->text, sizeof(g->text),
               "\tAliases        : limit %5u bytes, current %5u bytes, %5u items\r\n",
               (unsigned)FCOM_ALIAS_BYTES,
               (unsigned)g->alias_used, alias_count);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  n = snprintf(g->text, sizeof(g->text),
               "\tHistory        : limit %5u bytes, current %5u bytes, %5u items\r\n",
               (unsigned)FCOM_HISTORY_BYTES,
               (unsigned)g->history_used, history_count);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  n = snprintf(g->text, sizeof(g->text),
               "\tDirectory stack: limit %5u bytes, current %5u bytes, %5u items\r\n",
               (unsigned)FCOM_DIR_STACK_BYTES,
               (unsigned)g->dir_stack_used, dirstack_count);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  n = snprintf(g->text, sizeof(g->text),
               "\tLast dir cache : used  %5u bytes, %5u items\r\n",
               0u, 0u);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

  n = snprintf(g->text, sizeof(g->text),
               "\tSwapinfo       : used  %5u bytes, %5u items\r\n",
               0u, 0u);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);

}



static void fcom_loadfix_release(UWORD command_psp, unsigned count)
{
  UWORD *blocks = fcom_loadfix_storage(command_psp);

  while (count != 0) {
    UWORD mcb_seg = blocks[--count];

    if (mcb_seg != 0)
      (void)DosMemFree(mcb_seg);
  }
}

static int fcom_loadfix_prepare(UWORD command_psp, unsigned *count)
{
  UWORD *blocks = fcom_loadfix_storage(command_psp);

  *count = 0;

  for (;;) {
    seg mcb_seg = 0;
    UWORD largest = 0;
    UWORD data_seg;
    UWORD wanted;
    UWORD maximum = 0;
    COUNT rc;

    /*
     * The original FreeCOM LOADFIX consumes every free conventional-memory
     * fragment below linear address 10000h.  A subsequently EXECed program
     * therefore cannot be loaded into the first 64 KiB.
     */
    rc = DosMemAlloc(1, FIRST_FIT, &mcb_seg, &largest);
    if (rc < SUCCESS)
      return SUCCESS;

    data_seg = (UWORD)(mcb_seg + 1u);
    if (data_seg >= 0x1000u) {
      (void)DosMemFree(mcb_seg);
      return SUCCESS;
    }

    if (*count >= FCOM_LOADFIX_SLOTS) {
      (void)DosMemFree(mcb_seg);
      return DE_NOMEM;
    }

    wanted = (UWORD)(0x1000u - data_seg);
    rc = DosMemChange(data_seg, wanted, &maximum);
    if (rc < SUCCESS) {
      (void)DosMemFree(mcb_seg);
      return rc;
    }

    blocks[(*count)++] = mcb_seg;
  }
}

static int execute_command_line(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, char *line);
static int execute_command_line_body(CPU *cpu, UWORD command_psp,
                                     struct fcom_guest *g, char *line);
                                     
static int parse_exit_code(char *args, int *batch_only, UWORD *code)
{
  char *p = skip_space(args);
  unsigned value = 0;

  *batch_only = 0;
  *code = 0;

  if (strncasecmp(p, "/B", 2) == 0 &&
      (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
    *batch_only = 1;
    p = skip_space(p + 2);
  }

  if (*p == '\0')
    return 1;

  if (!isdigit((unsigned char)*p))
    return 0;

  do {
    value = value * 10u + (unsigned)(*p - '0');
    ++p;
  } while (isdigit((unsigned char)*p));

  p = skip_space(p);
  if (*p != '\0')
    return 0;

  *code = (UWORD)value;
  return 1;
}

static int builtin_loadfix(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  unsigned block_count = 0;
  int rc;

  args = skip_space(args);
  if (*args == '\0') {
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
    return 0;
  }

  rc = fcom_loadfix_prepare(command_psp, &block_count);
  if (rc < SUCCESS) {
    fcom_loadfix_release(command_psp, block_count);
    dos_puts(cpu, command_psp, g,
             "Unable to reserve the first 64K of memory.\r\n");
    return 0;
  }

  rc = execute_command_line(cpu, command_psp, g, args);
  fcom_loadfix_release(command_psp, block_count);
  return rc;
}


static int run_builtin(CPU *cpu, UWORD command_psp,
                       struct fcom_guest *g, char *args)
{
  if (command_is(g->filename, "?")) {
    builtin_question(cpu, command_psp, g, args);
    return 1;
  }

  /*
   * FreeCOM shell/command.c compares the first two non-blank argument
   * bytes with "/?" and displays the command's TEXT_CMDHELP_* resource.
   */
  {
    char *help_args = skip_space(args);

    if (help_args[0] == '/' && help_args[1] == '?') {
      const char *help = fcom_command_help(g->filename);

      if (help != NULL) {
        fcom_output_resource(cpu, command_psp, g, help);
        return 1;
      }
    }
  }

  if (is_native_command_name(g->filename)) {
    /*
     * Each nested COMMAND has its own MCB/PSP.  /C returns after one
     * command; an interactive child returns to this shell on EXIT.

     * Route both COMMAND and COMMAND.COM through INT 21h/4B00h.  The kernel
     * interceptor then applies normal ChildEnv() semantics instead of sharing
     * this shell's environment block with the nested shell.
     */
    strcpy(g->filename, "COMMAND.COM");
    build_tail(g, args);
    int rc = exec_once(cpu, command_psp, g, 0);
    if (rc < 0)
      dos_puts(cpu, command_psp, g, "Unable to start COMMAND.COM\r\n");
    return 1;
  }

  if (drive_command(g->filename)) {
    unsigned drive = (unsigned)(toupper((unsigned char)g->filename[0]) - 'A');
    if (!dos_set_drive(cpu, command_psp, drive))
      dos_puts(cpu, command_psp, g, "Invalid drive\r\n");
    return 1;
  }

  if (command_is(g->filename, "CD") ||
      command_is(g->filename, "CHDIR")) {
    builtin_chdir(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "CLS")) {
    clear_screen(cpu, command_psp, g);
    return 1;
  }

  if (command_is(g->filename, "ECHO")) {
    enum onoff_value value = parse_onoff(args);

    if (value == FCOM_ONOFF_EMPTY) {
      dos_puts(cpu, command_psp, g,
               g->echo_enabled ? "ECHO is on\r\n"
                               : "ECHO is off\r\n");
    } else if (value == FCOM_ONOFF_ON) {
      g->echo_enabled = 1;
    } else if (value == FCOM_ONOFF_OFF) {
      g->echo_enabled = 0;
    } else {
      dos_puts(cpu, command_psp, g, args);
      dos_puts(cpu, command_psp, g, "\r\n");
    }
    return 1;
  }

  if (command_is(g->filename, "PAUSE")) {
    pause_command(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "DIR")) {
    builtin_dir(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "TYPE")) {
    builtin_type(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "COPY")) {
    builtin_copy(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "SET")) {
    builtin_set(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "ALIAS")) {
    builtin_alias(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "HISTORY")) {
    builtin_history(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "LFNFOR")) {
    builtin_lfnfor(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "FDDEBUG")) {
    builtin_fddebug(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "DOSKEY")) {
    builtin_doskey(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "MEMORY")) {
    builtin_memory(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "PATH")) {
    builtin_path(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "PROMPT")) {
    builtin_prompt(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "BREAK")) {
    builtin_break(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "VERIFY")) {
    builtin_verify(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "TRUENAME")) {
    builtin_truename(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "CDD")) {
    builtin_cdd(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "PUSHD")) {
    builtin_pushd(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "POPD")) {
    builtin_popd(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "DIRS")) {
    builtin_dirs(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "MD") ||
      command_is(g->filename, "MKDIR")) {
    builtin_mkdir(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "RD") ||
      command_is(g->filename, "RMDIR")) {
    builtin_rmdir(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "DEL") ||
      command_is(g->filename, "ERASE")) {
    builtin_del(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "REN") ||
      command_is(g->filename, "RENAME")) {
    builtin_rename(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "BEEP")) {
    builtin_beep(cpu, command_psp, g);
    return 1;
  }

  if (command_is(g->filename, "WHICH")) {
    builtin_which(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "CTTY")) {
    builtin_ctty(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "CHCP")) {
    builtin_chcp(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "VOL")) {
    builtin_vol(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "DATE")) {
    builtin_date(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "TIME")) {
    builtin_time(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "VER")) {
    builtin_ver(cpu, command_psp, g, args);
    return 1;
  }

  if (command_is(g->filename, "EXIT")) {
    int batch_only;
    UWORD code;

    if (!parse_exit_code(args, &batch_only, &code)) {
      dos_puts(cpu, command_psp, g, "Invalid syntax.\r\n");
      return 1;
    }

    g->exit_code = code;

    if (batch_only && g->batch_active) {
      g->exit_batch_only = 1;
      return -1;
    }

    if (g->persistent)
      return 1;

    g->exit_requested = 1;
    return -1;
  }

  return 0;
}

/* Split one DOS command line into executable and raw argument tail.
   Quoted executable names are accepted; argument quoting is left untouched. */
static char *split_command(struct fcom_guest *g)
{
  char *src = skip_space(g->input.kb_buf);
  char *dst = g->filename;
  char *dst_end = g->filename + sizeof(g->filename) - 1;
  int quoted = 0;

  if (*src == '"') {
    quoted = 1;
    ++src;
  }

  while (*src != '\0' && dst < dst_end) {
    if (quoted) {
      if (*src == '"') {
        ++src;
        break;
      }
    } else if (*src == ' ' || *src == '\t') {
      break;
    }
    *dst++ = *src++;
  }
  *dst = '\0';
  return skip_space(src);
}

static void build_tail(struct fcom_guest *g, const char *args)
{
  size_t n = strlen(args);
  if (n > sizeof(g->tail.ctBuffer) - 2)
    n = sizeof(g->tail.ctBuffer) - 2;

  g->tail.ctCount = (UBYTE)(n ? n + 1 : 0);
  if (n) {
    g->tail.ctBuffer[0] = ' ';
    memcpy(g->tail.ctBuffer + 1, args, n);
  }
  g->tail.ctBuffer[g->tail.ctCount] = '\r';
}

static void build_exec_fcbs(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g)
{
  UWORD source = FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, tail.ctBuffer);

  memset(&g->exec_fcb1, 0, sizeof(g->exec_fcb1));
  memset(&g->exec_fcb2, 0, sizeof(g->exec_fcb2));

  SET_DS(command_psp);
  CPU_SI = source;
  SET_ES(command_psp);
  CPU_DI = FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, exec_fcb1);
  CPU_AX = 0x2901;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM parse FCB1");

  SET_ES(command_psp);
  CPU_DI = FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, exec_fcb2);
  CPU_AX = 0x2901;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM parse FCB2");
}

static void capture_exec_status(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, int exec_rc)
{
  if (exec_rc < 0) {
    g->errorlevel = (UBYTE)(-exec_rc);
    g->exit_reason = 0xff;
    return;
  }

  CPU_AH = 0x4d;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM get child status");
  g->errorlevel = CPU_AL;
  g->exit_reason = CPU_AH;
}

static int exec_once(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                     UBYTE exec_mode)
{
  memset(&g->exec_block, 0, sizeof(g->exec_block));
  g->exec_block.exec.env_seg = 0;                 /* inherit process-0 environment */
  g->exec_block.exec.cmd_line = MK_FP(command_psp, FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, tail));
  build_exec_fcbs(cpu, command_psp, g);
  g->exec_block.exec.fcb_1 = MK_FP(command_psp, FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, exec_fcb1));
  g->exec_block.exec.fcb_2 = MK_FP(command_psp, FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, exec_fcb2));

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, filename);
  SET_ES(command_psp);
  CPU_BX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, exec_block);
  CPU_AX = 0x4b00 | exec_mode;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM EXEC");
  {
    int rc = int21_failed(cpu) ? -(int)CPU_AX : 0;
    capture_exec_status(cpu, command_psp, g, rc);
    return rc;
  }
}

static int exec_program(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                        const char *name, UBYTE exec_mode)
{
  static const char *const suffixes[] = { "", ".COM", ".EXE" };
  unsigned first = has_extension(name) ? 0u : 1u;
  unsigned last = has_extension(name) ? 1u : 3u;
  unsigned i;
  int rc = -2;

  for (i = first; i < last; ++i) {
    size_t n = strlen(name);
    size_t e = strlen(suffixes[i]);

    if (n + e >= sizeof(g->filename))
      return -2;
    memcpy(g->filename, name, n);
    memcpy(g->filename + n, suffixes[i], e + 1);

    /*
     * The final candidate cannot fall through to another suffix.  Return it
     * directly so GCC can tail-call exec_once(): while the child is running,
     * exec_program() then contributes no native frame at all.  This covers an
     * explicitly named .COM/.EXE and the final .EXE attempt for a bare name.
     */
    if (i + 1u == last)
      return exec_once(cpu, command_psp, g, exec_mode);

    rc = exec_once(cpu, command_psp, g, exec_mode);
    if (rc == 0)
      return 0;

    /* Only "file/path not found" permits trying another candidate. */
    if (rc != -2 && rc != -3)
      return rc;
  }
  return rc;
}

/*
 * PATH-search scratch is shared rather than placed on the native stack.
 *
 * This code runs only on core0. Logical re-entry is also safe: when an
 * exec_program() call actually starts a child, the outer invocation goes
 * directly to "resolved" after the child returns and no longer reads the
 * saved PATH cursor. A "not found" return cannot have run a child, so no
 * nested FCOM invocation could have overwritten the cursor. BAT recursion
 * starts only after the PATH-search state is dead.
 */
struct fcom_exec_search_workspace {
  const char *path;
  const char *end;
  const char *dir;
  size_t len;
  int rc;
};

static struct fcom_exec_search_workspace fcom_exec_search;

static int exec_external(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                         const char *args, UBYTE exec_mode)
{
  char *path_storage = NULL;

  build_tail(g, args);
  strcpy(g->program, g->filename);

  /*
   * INT 21h/AH=0Ah leaves the entered command visible on the current
   * console row.  Terminate the parent command line before EXEC: exec_program()
   * is synchronous, so doing this after it returns is already too late for
   * the child's first output.
   */
  dos_puts(cpu, command_psp, g, "\r\n");

  /* Explicit drive/path names are never searched through PATH. */
  if (path_is_explicit(g->program)) {
    fcom_exec_search.rc = exec_program(cpu, command_psp, g, g->program,
                                       exec_mode);
    goto resolved;
  }

  /* DOS shells search the current directory before PATH. */
  fcom_exec_search.rc = exec_program(cpu, command_psp, g, g->program,
                                     exec_mode);
  if (fcom_exec_search.rc == 0 ||
      (fcom_exec_search.rc != -2 && fcom_exec_search.rc != -3))
    goto resolved;

  path_storage = fcom_env_alloc_value(command_psp, "PATH", 1024u);
  fcom_exec_search.path = path_storage;
  while (fcom_exec_search.path && *fcom_exec_search.path) {
    fcom_exec_search.end = strchr(fcom_exec_search.path, ';');
    fcom_exec_search.len = fcom_exec_search.end
        ? (size_t)(fcom_exec_search.end - fcom_exec_search.path)
        : strlen(fcom_exec_search.path);
    fcom_exec_search.dir = fcom_exec_search.path;

    while (fcom_exec_search.len &&
           (*fcom_exec_search.dir == ' ' ||
            *fcom_exec_search.dir == '\t')) {
      ++fcom_exec_search.dir;
      --fcom_exec_search.len;
    }
    while (fcom_exec_search.len &&
           (fcom_exec_search.dir[fcom_exec_search.len - 1] == ' ' ||
            fcom_exec_search.dir[fcom_exec_search.len - 1] == '\t'))
      --fcom_exec_search.len;

    /* Empty PATH components denote the current directory, already tried. */
    if (fcom_exec_search.len &&
        make_path_candidate(g->text, sizeof(g->text),
                            fcom_exec_search.dir,
                            fcom_exec_search.len, g->program)) {
      fcom_exec_search.rc = exec_program(cpu, command_psp, g, g->text,
                                         exec_mode);
      if (fcom_exec_search.rc == 0 ||
          (fcom_exec_search.rc != -2 && fcom_exec_search.rc != -3))
        goto resolved;
    }

    if (!fcom_exec_search.end)
      break;
    fcom_exec_search.path = fcom_exec_search.end + 1;
  }
resolved:
  free(path_storage);
  /*
   * Keep BAT fallback and bad-command reporting here. A separate wrapper
   * remained live for the whole exec_program()->DosExec() path and cost
   * another 48 bytes of native stack without owning independent state.
   */
  if (fcom_exec_search.rc == -2 || fcom_exec_search.rc == -3) {
    int batch_rc = execute_batch_file(cpu, command_psp, g, g->filename, args);

    if (batch_rc == 0)
      return 0;
    if (batch_rc == -1)
      return -1;
  }

  if (fcom_exec_search.rc < 0)
    error_bad_command(cpu, command_psp, g, g->program);
  return 0;
}


/* --- LOADHIGH: port of shell/loadhigh.c --------------------------------
 *
 * FreeCOM 0.86 ships with INCLUDE_CMD_LOADHIGH enabled (config.h:148) and
 * INCLUDE_CMD_FAKELOADHIGH commented out - LH/LOADHIGH are the REAL
 * command that arranges upper-memory loading, not an alias of CALL.  The
 * mechanism is pure DOS API state:
 *
 *   cmd_loadhigh(): save UMB link, call lh_lf(), restore UMB link.
 *   lh_lf():        save alloc strategy; findUMBRegions(); parseArgs();
 *                   loadhigh_prepare(); execute(fnam, args, 1);
 *                   free blocker blocks; restore strategy.
 *   loadhigh_prepare(): link UMBs, strategy=first-fit; walk every region,
 *                   DosAlloc() each free block; blocks the program may
 *                   use are freed again, blocks it must not touch stay
 *                   allocated ("blockers"); finally strategy = 0x80
 *                   (first fit high) when a usable UMB exists.
 *
 * DosExec()'s ExecMemAlloc honours mem_access_mode (task.c), so the child
 * really lands in the UMB.  LOADFIX keeps its earlier separate port
 * (builtin_loadfix/fcom_loadfix_prepare); upstream funnels both commands
 * through lh_lf(), the difference being only *_prepare() - noted, not
 * unified here to avoid touching the proven LOADFIX path.
 *
 * Bookkeeping arrays were malloc()ed in FreeCOM's private heap - equally
 * invisible to other guest programs - so native static storage is a
 * faithful home for them (one shell invocation at a time, like the
 * original globals).
 */

enum {
  LH_OK = 0,
  LH_ERR_SILENT = -3,
  LH_ERR_FILE_NOT_FOUND = 2,
  LH_ERR_MCB_CHAIN = 7,
  LH_ERR_OUT_OF_MEMORY = 8,
  LH_ERR_INVALID_PARMS = 87
};

struct lh_umbregion {
  UWORD start;              /* start of the region */
  UWORD end;                /* end of the region */
  UWORD minSize;            /* minimum free size, given by the L switch */
  int access;               /* does the program have access to this region? */
};

#define LH_MAX_BLOCKS  256
#define LH_MAX_REGIONS 64

static UWORD lh_block[LH_MAX_BLOCKS];   /* blocks the program can't use  */
static int   lh_allocatedBlocks;
static struct lh_umbregion lh_umbRegion[LH_MAX_REGIONS];
static int   lh_umbRegions;             /* region 0 = conventional memory */
static int   lh_upper_flag;             /* load the program high?         */
static int   lh_optS;
static char  lh_optL[64];
static int   lh_optL_present;
static char  lh_fnam[128];

static BYTE lh_mcb_type(UWORD mseg)
{
  return fdos_mcb_type((seg)mseg);
}

static UWORD lh_mcb_owner(UWORD mseg)
{
  return fdos_mcb_owner((seg)mseg);
}

static UWORD lh_mcb_size(UWORD mseg)
{
  return fdos_mcb_size((seg)mseg);
}

static int lh_mcb_name_eq(UWORD mseg, const char *name, unsigned len)
{
  unsigned i;
  for (i = 0; i < len; ++i)
    if (fdos_mcb_name_byte((seg)mseg, i) != (BYTE)name[i])
      return 0;
  return 1;
}

/* suppl DosAlloc(): INT 21h/48h - returns the DATA segment (MCB+1) or 0 */
static UWORD lh_DosAlloc(UWORD size)
{
  seg mseg = 0;
  UWORD largest = 0;

  if (DosMemAlloc(size, fcom_guest_mem_access_mode(), &mseg, &largest)
      < SUCCESS)
    return 0;
  return (UWORD)(mseg + 1);
}

/* suppl DOSfree(): INT 21h/49h - argument is the DATA segment */
static void lh_DOSfree(UWORD bl)
{
  (void)DosMemFree((UWORD)(bl - 1));
}

/* suppl DOSresize(): INT 21h/4Ah - argument is the DATA segment */
static void lh_DOSresize(UWORD bl, UWORD size)
{
  UWORD maximum = 0;

  (void)DosMemChange(bl, size, &maximum);
}

/* strtol(c, &c, 10) for the /L list; -1 on no digits (matches the
   original's -1 minsize sentinel probing) */
static long lh_scan_long(char *p, char **endp)
{
  long v = 0;
  int neg = 0;
  int any = 0;

  if (*p == '-') { neg = 1; ++p; }
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    any = 1;
    ++p;
  }
  *endp = p;
  if (!any)
    return -1;
  return neg ? -v : v;
}

/* findUMBRegions(): scan the MCB chain for all active memory regions.
   Region 0 is the conventional memory. */
static int lh_findUMBRegions(void)
{
  struct lh_umbregion *region = lh_umbRegion;
  UWORD mseg = fcom_guest_lol_first_mcb();          /* GetFirstMCB() */
  char sig;
  int i;

  lh_umbRegions = 0;
  region->start = mseg;

  /* First, find the end of the conventional memory:
   * Turn UMB link off, and track the MCB chain to the end. */

  DosUmbLink(0);

  while (lh_mcb_type(mseg) == 'M')
    mseg = (UWORD)(mseg + lh_mcb_size(mseg) + 1);

  if (lh_mcb_type(mseg) != 'Z')
    return LH_ERR_MCB_CHAIN;

  /* If the last memory block in conventional memory is "reserved",
   * conventional memory ends at the paragraph before the block. If
   * the last block is an ordinary one, conventional memory ends at
   * the last paragraph of the block. */

  if (lh_mcb_owner(mseg) == 8 && lh_mcb_name_eq(mseg, "SC", 2))
    region->end = (UWORD)(mseg - 1);
  else
    region->end = (UWORD)(mseg + lh_mcb_size(mseg));

  region++;
  region->start = 0;

  /* Turn UMB link on. If MS-DOS UMBs are available, the signature of
   * the last conventional memory block will change from 'Z' to 'M'. */

  DosUmbLink(1);

  if (lh_mcb_type(mseg) == 'M')
  {                             /* UMBs are available */
    mseg = (UWORD)(mseg + lh_mcb_size(mseg) + 1);

    /* This loop searches for the regions, by searching either for
     * special MCBs or 'reserved' memory regions. */

    do
    {
      UWORD size;
      UWORD owner;

      /* port safety net: the original trusted its 64-entry array */
      if (region - lh_umbRegion >= LH_MAX_REGIONS - 1)
        return LH_ERR_MCB_CHAIN;

      sig = (char)lh_mcb_type(mseg);
      size = lh_mcb_size(mseg);
      owner = lh_mcb_owner(mseg);

      if (owner == 8 && lh_mcb_name_eq(mseg, "SC", 2))
      {
        /* this is a 'hole' in memory */
        if (region->start)
        {
          region->end = (UWORD)(mseg - 1);
          region++;
          region->start = 0;
        }
      }
      else
      {
        /* In MS-DOS 6.x, each UMB region starts with a 'major' mcb
         * that is outside the ordinary MCB chain. This mcb defines
         * the size of the whole region. */
        const UWORD umb_seg = (UWORD)(mseg - 1);
        const BYTE umb_type = lh_mcb_type(umb_seg);

        if ((umb_type == 'Z' || umb_type == 'M') &&
            lh_mcb_name_eq(umb_seg, "UMB     ", 8))
        {
          const UWORD umb_owner = lh_mcb_owner(umb_seg);
          const UWORD umb_size = lh_mcb_size(umb_seg);

          /* This is the signature of the special MS-DOS MCBs */
          region->start = umb_owner;
          region->end = (UWORD)(umb_owner + umb_size - 1);
          if ((sig = (char)umb_type) == 'M')
            region->end--;
          region++;
          region->start = 0;
          mseg = (UWORD)(umb_seg + umb_size);
          if (sig == 'Z')
            break;
          continue;
        }
        if (!region->start)
          region->start = mseg;
      }

      if (sig == 'Z')
      {
        region->end = (UWORD)(mseg + size);
        region++;
        break;
      }

      mseg = (UWORD)(mseg + size + 1);
    }
    while (sig == 'M');

    if (sig != 'Z')
      return LH_ERR_MCB_CHAIN;
  }
  lh_umbRegions = (int)(region - lh_umbRegion);

  /* By default, the program will have access to all UMB regions. This
   * may be modified by command-line arguments. */

  for (i = 0; i < lh_umbRegions; i++)
  {
    lh_umbRegion[i].access = 1;
    lh_umbRegion[i].minSize = 0xffff;
  }

  return LH_OK;
}

/* loadhigh_prepare(): allocate memory as necessary.  All memory that
   the program is not allowed to access must be temporarily allocated
   while the program is running. */
static int lh_loadhigh_prepare(void)
{
  int i;
  struct lh_umbregion *region = lh_umbRegion;
  static UWORD availBlock[LH_MAX_BLOCKS];
  UWORD availBlocks = 0;

  /* Set the UMB link and malloc strategy */
  DosUmbLink(1);
  fcom_guest_set_mem_access_mode(FIRST_FIT);

  /* Call to force DOS to catenate any successive free memory blocks */
  (void)lh_DosAlloc(0xffff);

  for (i = 0; i < lh_umbRegions; i++, region++)
  {
    UWORD mseg = region->start;
    int startBlock = lh_allocatedBlocks;
    int found_one = 0;

    while (mseg < region->end)
    {
      UWORD size = lh_mcb_size(mseg);
      BYTE type = lh_mcb_type(mseg);
      UWORD owner = lh_mcb_owner(mseg);

      if (type != 'M' || size == 0)
        break;

      if (!owner)
      {
        /* Found a free memory block: allocate it. */
        UWORD bl = lh_DosAlloc(size);

        if (bl != (UWORD)(mseg + 1))
          return LH_ERR_MCB_CHAIN;

        if (lh_allocatedBlocks >= LH_MAX_BLOCKS ||
            availBlocks >= LH_MAX_BLOCKS)
          return LH_ERR_OUT_OF_MEMORY;

        if (region->access)
        {
          if (region->minSize == 0xffff ||
              (!lh_optS && found_one) ||
              (!(lh_optS && found_one) && size >= region->minSize))
          {
            availBlock[availBlocks++] = bl;

            if (lh_optS)
              lh_DOSresize(bl, region->minSize);
            else if (lh_allocatedBlocks > startBlock)
            {
              memcpy(availBlock + availBlocks,
                     lh_block + startBlock,
                     (size_t)(lh_allocatedBlocks - startBlock) *
                         sizeof(*lh_block));
              availBlocks = (UWORD)(availBlocks +
                  (lh_allocatedBlocks - startBlock));
              lh_allocatedBlocks = startBlock;
            }
            found_one = 1;
          }
          else
            lh_block[lh_allocatedBlocks++] = bl;
        }
        else
          lh_block[lh_allocatedBlocks++] = bl;

        /* Allocation/resize may have changed this MCB's size. */
        size = lh_mcb_size(mseg);
      }

      mseg = (UWORD)(mseg + size + 1);
    }
  }

  for (i = 0; i < (int)availBlocks; i++)
    lh_DOSfree(availBlock[i]);

  fcom_guest_set_mem_access_mode(lh_upper_flag ? FIRST_FIT_U : FIRST_FIT);
  return LH_OK;
}

/* parseArgs(): only '/' is a switch character; the first argument not
   starting with '/' is the filename; LOADHIGH accepts /L and /S. */
static int lh_parseArgs(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                        char *cmdline, char **fnam, char **rest)
{
  char *c;

  /* leadOptions(opt_lh) */
  cmdline = skip_space(cmdline);
  while (*cmdline == '/')
  {
    char opt = (char)toupper((unsigned char)cmdline[1]);

    if (opt == '\0')
      return LH_ERR_INVALID_PARMS;
    cmdline += 2;
    if (opt == 'S')
      lh_optS = 1;
    else if (opt == 'L')
    {
      size_t n = 0;

      if (*cmdline == ':' || *cmdline == '=')
        ++cmdline;
      while (*cmdline != '\0' && *cmdline != ' ' && *cmdline != '\t')
      {
        if (n < sizeof(lh_optL) - 1)
          lh_optL[n++] = *cmdline;
        ++cmdline;
      }
      lh_optL[n] = '\0';
      lh_optL_present = 1;
    }
    else
      return LH_ERR_INVALID_PARMS;      /* optErr(): unknown switch */
    cmdline = skip_space(cmdline);
  }

  if (lh_optL_present)
  {
    int i, r;
    char *lc = lh_optL;

    /* (swapContext = FALSE is a no-op: the native shell never swaps) */

    /* Disable access to all UMB regions not listed here */
    for (i = 1; i < lh_umbRegions; i++)
      lh_umbRegion[i].access = 0;

    r = 0;

    do
    {
      long region_minSize = -1;   /* flag: no minsize was specified */
      int region_number = (int)lh_scan_long(lc, &lc);

      if (*lc == ',')
      {
        long larg;

        ++lc;
        if ((larg = lh_scan_long(lc, &lc)) != -1L)
          region_minSize = (larg + 15) >> 4;    /* topara() */
      }

      if (region_number >= lh_umbRegions || region_number < 0)
      {
        /* TEXT_ERROR_REGION_WARNING */
        char msg[48];

        snprintf(msg, sizeof(msg),
                 "Illegal memory region %d - ignored.\r\n", region_number);
        dos_puts(cpu, command_psp, g, msg);
      }
      else
      {
        if (!r && !region_number)
          lh_upper_flag = 0;

        r++;
        lh_umbRegion[region_number].minSize =
            region_minSize < 0 ? 0xffff : (UWORD)region_minSize;
        lh_umbRegion[region_number].access = 1;
      }
    }
    while (*lc++ == ';');
    if (lc[-1])
      cmdline = (char *)"";     /* to cause an error later */
  }

  /* does a file name follow? */
  if (!*cmdline)
    return LH_ERR_INVALID_PARMS;

  if (lh_optS && !lh_optL_present)
    return LH_ERR_INVALID_PARMS;

  /* The next argument is the file name (unquote()); the rest of the
   * command line is passed as parameters to the new program. */

  {
    size_t n = 0;

    if (*cmdline == '"')
    {
      ++cmdline;
      while (*cmdline != '\0' && *cmdline != '"')
      {
        if (n < sizeof(lh_fnam) - 1)
          lh_fnam[n++] = *cmdline;
        ++cmdline;
      }
      if (*cmdline == '"')
        ++cmdline;
    }
    else
    {
      while (*cmdline != '\0' && *cmdline != ' ' && *cmdline != '\t')
      {
        if (n < sizeof(lh_fnam) - 1)
          lh_fnam[n++] = *cmdline;
        ++cmdline;
      }
    }
    lh_fnam[n] = '\0';
    c = cmdline;
  }

  *fnam = lh_fnam;
  *rest = skip_space(c);        /* skipdm() */

  return LH_OK;
}

/* lh_error(): print error messages */
static void lh_error(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                     int errcode)
{
  switch (errcode)
  {
    case LH_ERR_INVALID_PARMS:
      dos_puts(cpu, command_psp, g, "Syntax error\r\n");
      break;

    case LH_ERR_FILE_NOT_FOUND:
      dos_puts(cpu, command_psp, g, "File not found\r\n");
      break;

    case LH_ERR_OUT_OF_MEMORY:
      dos_puts(cpu, command_psp, g, "Out of memory\r\n");
      break;

    case LH_ERR_MCB_CHAIN:
      dos_puts(cpu, command_psp, g, "Bad MCB chain\r\n");
      break;

    case LH_ERR_SILENT:
      break;

    default:
      dos_puts(cpu, command_psp, g, "LOADHIGH: internal error\r\n");
      break;
  }
}

/* cmd_loadhigh() + lh_lf() */
static int builtin_loadhigh(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g, char *args)
{
  int old_link = (int)(fcom_guest_lol_uppermem_link() & 1);   /* dosGetUMBLinkState() */
  COUNT old_strat = fcom_guest_mem_access_mode(); /* dosGetAllocStrategy() */
  char *fnam = NULL;
  char *rest = NULL;
  int rc;
  int i;

  /* initialise() */
  lh_allocatedBlocks = 0;
  lh_upper_flag = 1;
  lh_optS = 0;
  lh_optL_present = 0;
  lh_optL[0] = '\0';
  /* (FEATURE_XMS_SWAP transient relocation: no-op for the native shell) */

  rc = lh_findUMBRegions();
  if (rc == LH_OK)
  {
    rc = lh_parseArgs(cpu, command_psp, g, args, &fnam, &rest);
    if (rc == LH_OK)
    {
      /* command line was OK - allocate the memory */
      rc = lh_loadhigh_prepare();

      /* finally, execute the file */
      if (!rc)
      {
        int exec_rc;

        /* execute(fnam, args, 1): externals only -
           command.c:206 "loadhigh/loadfix don't do batch files" */
        strncpy(g->filename, fnam, sizeof(g->filename) - 1);
        g->filename[sizeof(g->filename) - 1] = '\0';
        exec_rc = exec_external(cpu, command_psp, g, rest, 0x80);
        if (exec_rc == -2 || exec_rc == -3)
          rc = LH_ERR_FILE_NOT_FOUND;
        else if (exec_rc < 0)
          rc = LH_ERR_SILENT;   /* exec path reported its own error */
      }
    }
  }

  /** Clean Up **/
  /* free any memory that was allocated to prevent the program from
     using it */
  for (i = 0; i < lh_allocatedBlocks; i++)
    lh_DOSfree(lh_block[i]);
  lh_allocatedBlocks = 0;

  /* Restore DOS malloc strategy to its original value. */
  fcom_guest_set_mem_access_mode((uint8_t)old_strat);

  /* cmd_loadhigh(): restore UMB link state to its original value. */
  DosUmbLink((unsigned)old_link);

  /* if any error occurred, print it */
  if (rc)
    lh_error(cpu, command_psp, g, rc);

  return 0;
}


static int name_has_extension_ci(const char *name, const char *extension)
{
  const char *dot = NULL;
  const char *p;

  for (p = name; *p != '\0'; ++p) {
    if (*p == '\\' || *p == '/' || *p == ':')
      dot = NULL;
    else if (*p == '.')
      dot = p;
  }

  return dot != NULL && strcasecmp(dot, extension) == 0;
}

static int open_batch_candidate(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (name_has_extension_ci(name, ".BAT") ||
      name_has_extension_ci(name, ".CMD"))
    return dos_open_read(cpu, command_psp, g, name);

  if (has_extension(name) || n + 4 >= sizeof(g->text))
    return -2;

  memcpy(g->text, name, n);
  memcpy(g->text + n, ".BAT", 5);
  {
    int handle = dos_open_read(cpu, command_psp, g, g->text);

    if (handle >= 0 || (handle != -2 && handle != -3))
      return handle;
  }

  memcpy(g->text + n, ".CMD", 5);
  return dos_open_read(cpu, command_psp, g, g->text);
}

static int find_batch_file(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, const char *name)
{
  char *path_storage;
  const char *path;
  int handle;

  if (path_is_explicit(name))
    return open_batch_candidate(cpu, command_psp, g, name);

  handle = open_batch_candidate(cpu, command_psp, g, name);
  if (handle >= 0 || (handle != -2 && handle != -3))
    return handle;

  path_storage = fcom_env_alloc_value(command_psp, "PATH", 1024u);
  path = path_storage;
  while (path && *path) {
    const char *end = strchr(path, ';');
    size_t len = end ? (size_t)(end - path) : strlen(path);
    const char *dir = path;

    while (len && (*dir == ' ' || *dir == '\t')) {
      ++dir;
      --len;
    }
    while (len && (dir[len - 1] == ' ' || dir[len - 1] == '\t'))
      --len;

    if (len && make_path_candidate(g->text, sizeof(g->text),
                                   dir, len, name)) {
      handle = open_batch_candidate(cpu, command_psp, g, g->text);
      if (handle >= 0 || (handle != -2 && handle != -3)) {
        free(path_storage);
        return handle;
      }
    }

    if (!end)
      break;
    path = end + 1;
  }

  free(path_storage);
  return -2;
}

static int read_batch_byte(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, UWORD handle)
{
  (void)g;
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io);
  CPU_BX = handle;
  CPU_CX = 1;
  CPU_AH = 0x3f;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM BAT read");

  if (int21_failed(cpu))
    return -(int)CPU_AX;
  if (CPU_AX == 0)
    return -1;
  return fcom_guest_read8(
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, io)));
}



static int seek_batch_start(CPU *cpu, UWORD command_psp, UWORD handle)
{
  CPU_BX = handle;
  CPU_CX = 0;
  CPU_DX = 0;
  CPU_AX = 0x4200;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM BAT seek");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static uint32_t batch_argument_at_guest(UWORD command_psp,
                                       unsigned index)
{
  const uint32_t args =
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, batch_args));
  const uint32_t out =
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, batch_arg));
  uint32_t p = args;
  unsigned logical = index +
      fcom_gread8(command_psp, offsetof(struct fcom_guest, batch_shiftlevel));

  while (fcom_guest_read8(p) != 0) {
    uint32_t begin;

    p = fcom_gskip_space(p);
    if (fcom_guest_read8(p) == 0)
      break;

    if (fcom_guest_read8(p) == '"') {
      begin = ++p;
      while (fcom_guest_read8(p) != 0 && fcom_guest_read8(p) != '"')
        ++p;
    } else {
      begin = p;
      while (fcom_guest_read8(p) != 0 &&
             fcom_guest_read8(p) != ' ' &&
             fcom_guest_read8(p) != '\t')
        ++p;
    }

    if (logical == 0) {
      size_t n = (size_t)(p - begin);
      if (n >= sizeof(((struct fcom_guest *)0)->batch_arg))
        n = sizeof(((struct fcom_guest *)0)->batch_arg) - 1u;
      fcom_guest_copy(out, begin, n);
      fcom_guest_write8(out + (uint32_t)n, 0);
      return out;
    }

    if (fcom_guest_read8(p) == '"')
      ++p;
    --logical;
  }

  fcom_guest_write8(out, 0);
  return out;
}

static void batch_shift_args_guest(UWORD command_psp, const char *args)
{
  const char *p = skip_space((char *)args);
  UBYTE level =
      fcom_gread8(command_psp, offsetof(struct fcom_guest, batch_shiftlevel));

  if (strcasecmp(p, "DOWN") == 0) {
    if (level != 0)
      --level;
  } else {
    ++level;
  }

  fcom_gwrite8(command_psp,
               offsetof(struct fcom_guest, batch_shiftlevel), level);
}

static void split_call_target(char *args, char **target, char **tail)
{
  char *p = skip_space(args);
  char *start;

  *target = p;
  *tail = p;

  if (*p == '"') {
    start = ++p;
    while (*p != '\0' && *p != '"')
      ++p;
    if (*p == '"')
      *p++ = '\0';
    *target = start;
    *tail = skip_space(p);
    return;
  }

  while (*p != '\0' && *p != ' ' && *p != '\t')
    ++p;
  if (*p != '\0')
    *p++ = '\0';
  *tail = skip_space(p);
}

static int expand_batch_parameters_guest(UWORD command_psp)
{
  const uint32_t line =
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, batch_line));
  const uint32_t name =
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, batch_name));
  const uint32_t args =
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, batch_args));
  const uint32_t out =
      fcom_gaddr(command_psp, offsetof(struct fcom_guest, for_command));
  uint32_t src = line;
  uint32_t dst = out;
  const uint32_t end = out + FCOM_LINE_MAX;

  while (fcom_guest_read8(src) != 0 && dst < end) {
    UBYTE c = fcom_guest_read8(src);

    if (c == '%' && fcom_guest_read8(src + 1u) != 0) {
      uint32_t value = FCOM_GUEST_NULL;
      UBYTE code = fcom_guest_read8(src + 1u);

      if (code == '0') {
        value = name;
      } else if (code >= '1' && code <= '9') {
        value = batch_argument_at_guest(command_psp,
                                        (unsigned)(code - '1'));
      } else if (code == '*') {
        uint32_t p = args;
        unsigned skip =
            fcom_gread8(command_psp,
                        offsetof(struct fcom_guest, batch_shiftlevel));

        while (skip-- != 0 && fcom_guest_read8(p) != 0) {
          p = fcom_gskip_space(p);
          if (fcom_guest_read8(p) == '"') {
            ++p;
            while (fcom_guest_read8(p) != 0 &&
                   fcom_guest_read8(p) != '"')
              ++p;
            if (fcom_guest_read8(p) == '"')
              ++p;
          } else {
            while (fcom_guest_read8(p) != 0 &&
                   fcom_guest_read8(p) != ' ' &&
                   fcom_guest_read8(p) != '\t')
              ++p;
          }
        }
        value = fcom_gskip_space(p);
      } else if (code == '%') {
        fcom_guest_write8(dst++, '%');
        src += 2u;
        continue;
      }

      if (value != FCOM_GUEST_NULL) {
        while (fcom_guest_read8(value) != 0 && dst < end)
          fcom_guest_write8(dst++, fcom_guest_read8(value++));
        src += 2u;
        continue;
      }
    }

    fcom_guest_write8(dst++, c);
    ++src;
  }

  if (fcom_guest_read8(src) != 0)
    return 0;

  fcom_guest_write8(dst, 0);
  fcom_guest_copy(line, out, (size_t)(dst - out) + 1u);
  return 1;
}

/*
 * One explicit remaining legacy boundary. Batch storage and parameter
 * expansion are guest-offset based before this point.
 */
static int execute_batch_command_legacy(CPU *cpu, UWORD command_psp)
{
  struct fcom_guest *fresh =
      fcom_state(command_psp);
  char *line = (char *)fcom_shadow_at(
      command_psp,
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, batch_line),
      sizeof(((struct fcom_guest *)0)->batch_line));

  return execute_command_line(cpu, command_psp, fresh, line);
}

/*
 * Port of lib/cbreak.c chkCBreak().
 *
 * ctrlBreak (CBreakCounter) lives in guest memory next to the INT 23h
 * stub; 'leaveAll' lives in struct fcom_guest (one shell instance = one
 * guest block, same lifetime as the original's static).
 *
 * The prompt is PROMPT_CANCEL_BATCH from strings/DEFAULT.lng:
 * "Terminate batch file '%s' (Yes/No/All) ? " with metakeys YNA.
 * The key is read with AH=07h (direct input, no ^C check) so the prompt
 * itself cannot recurse into the break machinery; upstream cgetchar()
 * differs only in that detail.  BREAK_* constants (include/misc.h) are
 * defined at the top of this file next to the forward declaration.
 */
static int fcom_chk_cbreak(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, int mode)
{
  const uint32_t counter = fcom_cbreak_counter_addr(command_psp);
  const size_t leave_off = offsetof(struct fcom_guest, cbreak_leave_all);
  const size_t active_off = offsetof(struct fcom_guest, batch_active);

  (void)g;

  switch (mode)
  {
    case BREAK_ENDOFBATCHFILES:
      fcom_gwrite8(command_psp, leave_off, 0);
      return 0;

    case 0:
      if (!fcom_gread8(command_psp, active_off))
        goto justCheck;
      /* fall through */

    case BREAK_BATCHFILE:
      if (fcom_gread8(command_psp, leave_off))
        return 1;
      if (fcom_guest_read16(counter) == 0)
        return 0;

      {
        struct fcom_guest *fresh =
            fcom_state(command_psp);
        dos_puts(cpu, command_psp, fresh, "Terminate batch file '");
        {
          size_t batch_len = fcom_guest_strnlen(
              fcom_gaddr(command_psp, offsetof(struct fcom_guest, batch_name)),
              sizeof(((struct fcom_guest *)0)->batch_name));
          if (batch_len != 0)
            (void)fcom_write(cpu, command_psp,
                FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, batch_name),
                (UWORD)batch_len);
          else
            dos_puts(cpu, command_psp, fresh, "<<unknown>>");
        }
        fresh = fcom_state(command_psp);
        dos_puts(cpu, command_psp, fresh, "' (Yes/No/All) ? ");
      }

      for (;;) {
        int ch;

        CPU_AH = 0x07;
        fcom_intcall(cpu, command_psp, 0x21, "FCOM ^Break prompt");
        ch = toupper((unsigned char)CPU_AL);

        if (ch == 'Y' || ch == 'N' || ch == 'A') {
          struct fcom_guest *fresh =
              fcom_state(command_psp);
          dos_puts(cpu, command_psp, fresh, "\r\n");
          if (ch == 'N') {
            fcom_guest_write16(counter, 0);
            return 0;
          }
          if (ch == 'A')
            fcom_gwrite8(command_psp, leave_off, 1);
          break;
        }

        fcom_guest_write8(fcom_gaddr(command_psp, offsetof(struct fcom_guest, io)),
                '\a');
        (void)fcom_write(cpu, command_psp,
            FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);
      }
      break;

    justCheck:
    case BREAK_FORCMD:
      if (fcom_gread8(command_psp, leave_off))
        return 1;
      /* fall through */

    case BREAK_INPUT:
      if (fcom_guest_read16(counter) == 0)
        return 0;
      break;
  }

  fcom_guest_write16(counter, 0);
  return 1;
}

static int execute_batch_handle(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, UWORD handle)
{
  const size_t line_off = offsetof(struct fcom_guest, batch_line);
  const size_t goto_off = offsetof(struct fcom_guest, batch_goto);
  size_t used = 0;
  int skip_lf = 0;
  int searching = 0;

  for (;;) {
    int ch = read_batch_byte(cpu, command_psp, g, handle);

    if (ch < 0) {
      if (used != 0) {
        fcom_guest_write8(fcom_gaddr(command_psp, line_off) + (uint32_t)used, 0);

        if (searching) {
          if (fcom_gstr_eq_label(command_psp, line_off, goto_off)) {
            fcom_guest_write8(fcom_gaddr(command_psp, goto_off), 0);
            return 0;
          }
        } else {
          struct fcom_guest *fresh =
              fcom_state(command_psp);

          if (fcom_chk_cbreak(cpu, command_psp, fresh, BREAK_BATCHFILE))
            return 0;

          if (!expand_batch_parameters_guest(command_psp)) {
            fresh = fcom_state(command_psp);
            dos_puts(cpu, command_psp, fresh, "Line too long.\r\n");
            return 1;
          }

          if (execute_batch_command_legacy(cpu, command_psp) < 0)
            return -1;
        }
      }

      if (searching) {
        fcom_guest_write8(fcom_gaddr(command_psp, goto_off), 0);
        {
          struct fcom_guest *fresh =
              fcom_state(command_psp);
          dos_puts(cpu, command_psp, fresh, "Label not found\r\n");
        }
        return 0;
      }
      return ch == -1 ? 0 : ch;
    }

    if (skip_lf) {
      skip_lf = 0;
      if (ch == '\n')
        continue;
    }

    if (ch == '\r' || ch == '\n') {
      fcom_guest_write8(fcom_gaddr(command_psp, line_off) + (uint32_t)used, 0);
      if (ch == '\r')
        skip_lf = 1;

      if (searching) {
        if (fcom_gstr_eq_label(command_psp, line_off, goto_off)) {
          searching = 0;
          fcom_guest_write8(fcom_gaddr(command_psp, goto_off), 0);
        }
      } else {
        uint32_t line = fcom_gaddr(command_psp, line_off);

        while (fcom_guest_read8(line) == ' ' || fcom_guest_read8(line) == '\t')
          ++line;

        if (fcom_guest_read8(line) != ':') {
          struct fcom_guest *fresh =
              fcom_state(command_psp);

          if (fcom_chk_cbreak(cpu, command_psp, fresh, BREAK_BATCHFILE))
            return 0;

          if (!expand_batch_parameters_guest(command_psp)) {
            fresh = fcom_state(command_psp);
            dos_puts(cpu, command_psp, fresh, "Line too long.\r\n");
            return 1;
          }

          if (execute_batch_command_legacy(cpu, command_psp) < 0)
            return -1;
        }

        if (fcom_guest_read8(fcom_gaddr(command_psp, goto_off)) != 0) {
          int rc = seek_batch_start(cpu, command_psp, handle);
          if (rc < 0) {
            fcom_guest_write8(fcom_gaddr(command_psp, goto_off), 0);
            return rc;
          }
          searching = 1;
          skip_lf = 0;
        }
      }

      used = 0;
      continue;
    }

    if (used < FCOM_LINE_MAX)
      fcom_guest_write8(fcom_gaddr(command_psp, line_off) + (uint32_t)used++,
              (UBYTE)ch);
  }
}

static int fcom_batch_push(UWORD command_psp, const char *host_name,
                           uint32_t guest_name, const char *args,
                           UBYTE *depth_out, uint32_t *saved_addr_out)
{
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  const size_t name_off = offsetof(struct fcom_guest, batch_name);
  const size_t args_off = offsetof(struct fcom_guest, batch_args);
  const size_t goto_off = offsetof(struct fcom_guest, batch_goto);
  UBYTE depth = fcom_guest_read8(
      gbase + (uint32_t)offsetof(struct fcom_guest, batch_depth));
  uint32_t saved_addr;

  if (depth >= FCOM_BATCH_CONTEXTS)
    return -8;

  saved_addr = fcom_guest_linear(
      command_psp,
      (UWORD)(FCOM_BATCH_CONTEXT_OFFSET +
              (UWORD)(depth * sizeof(struct fcom_batch_context))));

  fcom_guest_copy(saved_addr + offsetof(struct fcom_batch_context, name),
                  fcom_gaddr(command_psp, name_off),
                  sizeof(((struct fcom_batch_context *)0)->name));
  fcom_guest_copy(saved_addr + offsetof(struct fcom_batch_context, args),
                  fcom_gaddr(command_psp, args_off),
                  sizeof(((struct fcom_batch_context *)0)->args));
  fcom_guest_copy(saved_addr + offsetof(struct fcom_batch_context, goto_label),
                  fcom_gaddr(command_psp, goto_off),
                  sizeof(((struct fcom_batch_context *)0)->goto_label));
  fcom_guest_write8(saved_addr + offsetof(struct fcom_batch_context, active),
          fcom_guest_read8(gbase +
              (uint32_t)offsetof(struct fcom_guest, batch_active)));
  fcom_guest_write8(saved_addr + offsetof(struct fcom_batch_context, exit_batch_only),
          fcom_guest_read8(gbase +
              (uint32_t)offsetof(struct fcom_guest, exit_batch_only)));
  fcom_guest_write8(saved_addr + offsetof(struct fcom_batch_context, shiftlevel),
          fcom_guest_read8(gbase +
              (uint32_t)offsetof(struct fcom_guest, batch_shiftlevel)));

  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, batch_depth),
          (UBYTE)(depth + 1u));

  if (guest_name != FCOM_GUEST_NULL) {
    size_t n = fcom_guest_strnlen(
        guest_name, sizeof(((struct fcom_guest *)0)->batch_name) - 1u);
    fcom_guest_copy(fcom_gaddr(command_psp, name_off), guest_name, n);
    fcom_guest_write8(fcom_gaddr(command_psp, name_off) + (uint32_t)n, 0);
  } else {
    fcom_gstr_copy_from_host(command_psp, name_off,
                             sizeof(((struct fcom_guest *)0)->batch_name),
                             host_name ? host_name : "");
  }

  fcom_gstr_copy_from_host(command_psp, args_off,
                           sizeof(((struct fcom_guest *)0)->batch_args),
                           args ? args : "");
  fcom_guest_write8(fcom_gaddr(command_psp, goto_off), 0);
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, batch_active), 1);
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, exit_batch_only), 0);
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, batch_shiftlevel), 0);

  *depth_out = depth;
  *saved_addr_out = saved_addr;
  return 0;
}

static void fcom_batch_pop(UWORD command_psp, UBYTE depth,
                           uint32_t saved_addr)
{
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);

  fcom_guest_copy(fcom_gaddr(command_psp,
                             offsetof(struct fcom_guest, batch_name)),
                  saved_addr + offsetof(struct fcom_batch_context, name),
                  sizeof(((struct fcom_batch_context *)0)->name));
  fcom_guest_copy(fcom_gaddr(command_psp,
                             offsetof(struct fcom_guest, batch_args)),
                  saved_addr + offsetof(struct fcom_batch_context, args),
                  sizeof(((struct fcom_batch_context *)0)->args));
  fcom_guest_copy(fcom_gaddr(command_psp,
                             offsetof(struct fcom_guest, batch_goto)),
                  saved_addr + offsetof(struct fcom_batch_context, goto_label),
                  sizeof(((struct fcom_batch_context *)0)->goto_label));
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, batch_active),
          fcom_guest_read8(saved_addr +
              offsetof(struct fcom_batch_context, active)));
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, exit_batch_only),
          fcom_guest_read8(saved_addr +
              offsetof(struct fcom_batch_context, exit_batch_only)));
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, batch_shiftlevel),
          fcom_guest_read8(saved_addr +
              offsetof(struct fcom_batch_context, shiftlevel)));
  fcom_guest_fill(saved_addr, 0, sizeof(struct fcom_batch_context));
  fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, batch_depth), depth);
}

static int fcom_open_guest_path(CPU *cpu, UWORD command_psp, UWORD path_off)
{
  SET_DS(command_psp);
  CPU_DX = path_off;
  CPU_AX = 0x3d00u;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM BAT open guest path");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int execute_batch_opened(CPU *cpu, UWORD command_psp, int handle,
                                const char *host_name, uint32_t guest_name,
                                const char *args)
{
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  UBYTE depth;
  uint32_t saved_addr;
  int rc;

  rc = fcom_batch_push(command_psp, host_name, guest_name, args,
                       &depth, &saved_addr);
  if (rc < 0) {
    fcom_close(cpu, command_psp, (UWORD)handle);
    {
      struct fcom_guest *fresh =
          fcom_state(command_psp);
      dos_puts(cpu, command_psp, fresh, "Out of memory.\r\n");
    }
    return rc;
  }

  rc = execute_batch_handle(cpu, command_psp, NULL, (UWORD)handle);
  fcom_close(cpu, command_psp, (UWORD)handle);

  if (fcom_guest_read8(
          gbase + (uint32_t)offsetof(struct fcom_guest, exit_batch_only)))
    rc = 0;

  fcom_batch_pop(command_psp, depth, saved_addr);

  if (!fcom_guest_read8(
          gbase + (uint32_t)offsetof(struct fcom_guest, batch_active))) {
    struct fcom_guest *fresh =
        fcom_state(command_psp);
    (void)fcom_chk_cbreak(cpu, command_psp, fresh, BREAK_ENDOFBATCHFILES);
  }

  return rc;
}

static int execute_batch_file(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g, const char *name,
                              const char *args)
{
  int handle;

  {
    struct fcom_guest *fresh =
        fcom_state(command_psp);
    handle = find_batch_file(cpu, command_psp, fresh, name);
  }
  if (handle < 0)
    return handle;

  (void)g;
  return execute_batch_opened(cpu, command_psp, handle,
                              name, FCOM_GUEST_NULL, args);
}

static int execute_batch_guest_path(CPU *cpu, UWORD command_psp,
                                    UWORD path_off)
{
  uint32_t path = fcom_guest_linear(command_psp, path_off);
  int handle = fcom_open_guest_path(cpu, command_psp, path_off);

  if (handle < 0)
    return handle;

  return execute_batch_opened(cpu, command_psp, handle,
                              NULL, path, "");
}

static int execute_default_autoexec(CPU *cpu, UWORD command_psp,
                                    struct fcom_guest *g)
{
  const UWORD path_off =
      FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, autoexec_path);
  uint32_t path = fcom_guest_linear(command_psp, path_off);
  unsigned drive = dos_get_drive(cpu, command_psp);
  int rc;

  (void)g;
  fcom_guest_write8(path + 0u, (UBYTE)('A' + drive));
  fcom_guest_write8(path + 1u, ':');
  fcom_guest_write8(path + 2u, '\\');
  fcom_guest_write(path + 3u, "FDAUTO.BAT", 11u);

  rc = execute_batch_guest_path(cpu, command_psp, path_off);
  if (rc != -2 && rc != -3)
    return rc;

  fcom_guest_write(path + 3u, "AUTOEXEC.BAT", 13u);
  return execute_batch_guest_path(cpu, command_psp, path_off);
}


enum fcom_start_action {
  FCOM_START_NONE = 0,
  FCOM_START_KEEP,
  FCOM_START_EXIT
};


static char *fcom_if_match_token(char *p, const char *token)
{
  size_t n = strlen(token);

  p = skip_space(p);
  if (strncasecmp(p, token, n) != 0)
    return NULL;
  if (p[n] != '\0' && p[n] != ' ' && p[n] != '\t')
    return NULL;
  return skip_space(p + n);
}

static char *fcom_if_skip_quoted_word(char *p, const char *stop)
{
  int quote = 0;

  while (*p != '\0') {
    if (*p == '"' || *p == '\'') {
      if (quote == *p)
        quote = 0;
      else if (quote == 0)
        quote = *p;
      ++p;
      continue;
    }

    if (quote == 0) {
      if (stop != NULL && p[0] == stop[0] && p[1] == stop[1])
        break;
      if (stop == NULL && (*p == ' ' || *p == '\t'))
        break;
    }
    ++p;
  }

  return p;
}

static int dos_file_exists(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, const char *name)
{
  int rc;

  memset(&g->find, 0, sizeof(g->find));
  if (set_find_dta(cpu, command_psp) < 0)
    return 0;

  rc = dos_find_first_attr(cpu, command_psp, g, name,
                           0x01 | 0x02 | 0x04 | 0x20);
  restore_default_dta(cpu, command_psp);
  return rc == 0;
}

static int execute_if(CPU *cpu, UWORD command_psp,
                      struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  char *next;
  char *command = NULL;
  int negate = 0;
  int ignore_case = 0;
  int condition = 0;

  /* FreeCOM accepts both "IF /I NOT" and "IF NOT /I". */
  next = fcom_if_match_token(p, "NOT");
  if (next != NULL) {
    negate = 1;
    p = next;
  }

  next = fcom_if_match_token(p, "/I");
  if (next != NULL) {
    ignore_case = 1;
    p = next;
  }

  next = fcom_if_match_token(p, "NOT");
  if (next != NULL) {
    negate = 1;
    p = next;
  }

  next = fcom_if_match_token(p, "EXIST");
  if (next != NULL) {
    char *end;

    p = next;
    if (*p == '\0') {
      dos_puts(cpu, command_psp, g,
               "IF EXIST requires a filename.\r\n");
      return 0;
    }

    end = fcom_if_skip_quoted_word(p, NULL);
    if ((size_t)(end - p) >= sizeof(g->if_left)) {
      dos_puts(cpu, command_psp, g, "Line too long.\r\n");
      return 0;
    }

    memcpy(g->if_left, p, (size_t)(end - p));
    g->if_left[end - p] = '\0';

    if (g->if_left[0] == '"' &&
        strlen(g->if_left) >= 2 &&
        g->if_left[strlen(g->if_left) - 1] == '"') {
      memmove(g->if_left, g->if_left + 1,
              strlen(g->if_left));
      g->if_left[strlen(g->if_left) - 1] = '\0';
    }

    condition = dos_file_exists(cpu, command_psp, g, g->if_left);
    command = skip_space(end);
  } else {
    next = fcom_if_match_token(p, "ERRORLEVEL");
    if (next != NULL) {
      unsigned n = 0;
      char *q;

      p = next;
      if (*p == '\0') {
        dos_puts(cpu, command_psp, g,
                 "IF ERRORLEVEL requires a number.\r\n");
        return 0;
      }

      q = p;
      do {
        n = n * 10u + (unsigned)((unsigned char)*q - '0');
      } while (*++q != '\0' && *q != ' ' && *q != '\t');

      n &= 0xffu;
      condition = g->errorlevel >= n;
      command = skip_space(q);
    } else {
      char *equal;
      char *right;
      char *right_end;
      size_t left_len;
      size_t right_len;

      equal = fcom_if_skip_quoted_word(p, "==");
      while (*equal != '\0' && *equal != '=' &&
             *equal != ' ' && *equal != '\t')
        ++equal;

      if (equal[0] != '=' || equal[1] != '=') {
        dos_puts(cpu, command_psp, g, "Syntax error.\r\n");
        return 0;
      }

      right = skip_space(equal + 2);
      right_end = fcom_if_skip_quoted_word(right, NULL);

      while (equal > p &&
             (equal[-1] == ' ' || equal[-1] == '\t'))
        --equal;

      left_len = (size_t)(equal - p);
      right_len = (size_t)(right_end - right);

      condition = left_len == right_len &&
          (ignore_case
              ? strncasecmp(p, right, left_len) == 0
              : memcmp(p, right, left_len) == 0);
      command = skip_space(right_end);
    }
  }

  if (condition ^ negate) {
    if (command == NULL || *command == '\0') {
      dos_puts(cpu, command_psp, g,
               "IF requires a command.\r\n");
      return 0;
    }
    return execute_command_line(cpu, command_psp, g, command);
  }

  return 0;
}


static void expand_for_variable(const char *command, char variable,
                                const char *value,
                                char *dst, size_t dst_size)
{
  char *out = dst;
  char *end = dst + dst_size - 1;

  while (*command != '\0' && out < end) {
    if (command[0] == '%' &&
        toupper((unsigned char)command[1]) ==
          toupper((unsigned char)variable)) {
      const char *p = value;

      while (*p != '\0' && out < end)
        *out++ = *p++;
      command += 2;
      continue;
    }

    *out++ = *command++;
  }

  *out = '\0';
}

static int for_pattern_has_wildcards(const char *s)
{
  return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

static int execute_for_value(CPU *cpu, UWORD command_psp,
                             struct fcom_guest *g,
                             char variable, const char *value,
                             const char *command)
{
  strncpy(g->for_item, value, sizeof(g->for_item) - 1);
  g->for_item[sizeof(g->for_item) - 1] = '\0';

  expand_for_variable(command, variable, g->for_item,
                      g->for_command, sizeof(g->for_command));
  return execute_command_line(cpu, command_psp, g, g->for_command);
}

static int execute_for_pattern(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               char variable, const char *pattern,
                               const char *command)
{
  int rc;

  memset(&g->find, 0, sizeof(g->find));
  if (set_find_dta(cpu, command_psp) < 0)
    return 0;

  rc = dos_find_first(cpu, command_psp, g, pattern);
  if (rc < 0) {
    restore_default_dta(cpu, command_psp);
    return 0;
  }

  do {
    /* batch.c:406 chkCBreak(BREAK_FORCMD) - see execute_for() */
    if (fcom_chk_cbreak(cpu, command_psp, g, BREAK_FORCMD)) {
      restore_default_dta(cpu, command_psp);
      return 0;
    }

    if (execute_for_value(cpu, command_psp, g, variable,
                          g->find.dm_name, command) < 0) {
      restore_default_dta(cpu, command_psp);
      return -1;
    }

    if (set_find_dta(cpu, command_psp) < 0) {
      restore_default_dta(cpu, command_psp);
      return -1;
    }

    rc = dos_find_next(cpu, command_psp);
  } while (rc == 0);

  restore_default_dta(cpu, command_psp);
  return 0;
}

static int execute_for(CPU *cpu, UWORD command_psp,
                       struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  char variable;
  char *list_start;
  char *list_end;
  char *command;
  int quote = 0;

  if (*p != '%') {
    dos_puts(cpu, command_psp, g,
             "FOR variable must be a single letter.\r\n");
    return 1;
  }

  while (*p == '%')
    ++p;

  if (!isalpha((unsigned char)*p) ||
      (p[1] != ' ' && p[1] != '\t')) {
    dos_puts(cpu, command_psp, g,
             "FOR variable must be a single letter.\r\n");
    return 1;
  }

  variable = *p++;
  p = skip_space(p);

  if (strncasecmp(p, "IN", 2) != 0 ||
      (p[2] != ' ' && p[2] != '\t' && p[2] != '(')) {
    dos_puts(cpu, command_psp, g,
             "FOR requires IN.\r\n");
    return 1;
  }

  p = skip_space(p + 2);
  if (*p != '(') {
    dos_puts(cpu, command_psp, g,
             "FOR requires parentheses around the set.\r\n");
    return 1;
  }

  list_start = ++p;
  list_end = NULL;

  while (*p != '\0') {
    if (*p == '"' || *p == '\'') {
      if (quote == *p)
        quote = 0;
      else if (quote == 0)
        quote = *p;
    } else if (*p == ')' && quote == 0 &&
               (p[1] == '\0' || p[1] == ' ' || p[1] == '\t')) {
      list_end = p;
      break;
    }
    ++p;
  }

  if (list_end == NULL) {
    dos_puts(cpu, command_psp, g,
             "FOR requires parentheses around the set.\r\n");
    return 1;
  }

  *list_end = '\0';
  p = skip_space(list_end + 1);

  if (strncasecmp(p, "DO", 2) != 0 ||
      (p[2] != '\0' && p[2] != ' ' && p[2] != '\t')) {
    dos_puts(cpu, command_psp, g,
             "FOR requires DO.\r\n");
    return 1;
  }

  command = skip_space(p + 2);
  if (*command == '\0') {
    dos_puts(cpu, command_psp, g,
             "FOR requires a command.\r\n");
    return 1;
  }

  p = list_start;
  while (*p != '\0') {
    char *item;
    char *end;
    int quoted = 0;
    int rc;

    p = skip_space(p);
    if (*p == '\0')
      break;

    /* batch.c:406 chkCBreak(BREAK_FORCMD): user break exits just this FOR */
    if (fcom_chk_cbreak(cpu, command_psp, g, BREAK_FORCMD))
      return 0;

    if (*p == '"') {
      quoted = 1;
      item = ++p;
      while (*p != '\0' && *p != '"')
        ++p;
      end = p;
      if (*p == '"')
        ++p;
    } else {
      item = p;
      while (*p != '\0' && *p != ' ' && *p != '\t')
        ++p;
      end = p;
    }

    if (*end != '\0')
      *end = '\0';

    if (for_pattern_has_wildcards(item))
      rc = execute_for_pattern(cpu, command_psp, g,
                               variable, item, command);
    else
      rc = execute_for_value(cpu, command_psp, g,
                             variable, item, command);

    if (rc < 0)
      return rc;

    if (quoted && *p == '"')
      ++p;
  }

  return 0;
}


static int dos_dup_handle(CPU *cpu, UWORD command_psp, UWORD handle)
{
  CPU_BX = handle;
  CPU_AH = 0x45;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM duplicate handle");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int dos_force_dup(CPU *cpu, UWORD command_psp,
                         UWORD source, UWORD target)
{
  CPU_BX = source;
  CPU_CX = target;
  CPU_AH = 0x46;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM force duplicate handle");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int dos_open_mode(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g,
                         const char *name, UBYTE mode)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AX = 0x3d00u | mode;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM redirect open");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int dos_create_file(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_CX = 0;
  CPU_AH = 0x3c;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM redirect create");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
}

static int dos_seek_end(CPU *cpu, UWORD command_psp, UWORD handle)
{
  CPU_BX = handle;
  CPU_CX = 0;
  CPU_DX = 0;
  CPU_AX = 0x4202;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM redirect seek end");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static char *parse_redirect_name(char *p, char *dst, size_t dst_size)
{
  char *d = dst;
  char *end = dst + dst_size - 1;
  int quoted = 0;

  p = skip_space(p);
  if (*p == '"') {
    quoted = 1;
    ++p;
  }

  while (*p != '\0' && d < end) {
    if (quoted) {
      if (*p == '"') {
        ++p;
        break;
      }
    } else if (*p == ' ' || *p == '\t' ||
               *p == '<' || *p == '>') {
      break;
    }
    *d++ = *p++;
  }

  *d = '\0';
  return p;
}

static int parse_redirections(struct fcom_guest *g, const char *line,
                              int *append_output)
{
  const char *srcp = line;
  char *dst = g->redirect_command;
  char *end = dst + sizeof(g->redirect_command) - 1;
  int quoted = 0;

  g->redirect_in[0] = '\0';
  g->redirect_out[0] = '\0';
  *append_output = 0;

  while (*srcp != '\0' && dst < end) {
    if (*srcp == '"') {
      quoted = !quoted;
      *dst++ = *srcp++;
      continue;
    }

    if (!quoted && (*srcp == '<' || *srcp == '>')) {
      int output = *srcp == '>';
      char *after;
      char *target;

      ++srcp;
      if (output && *srcp == '>') {
        *append_output = 1;
        ++srcp;
      }

      target = output ? g->redirect_out : g->redirect_in;
      after = parse_redirect_name((char *)srcp, target,
                                  output ? sizeof(g->redirect_out)
                                         : sizeof(g->redirect_in));
      if (target[0] == '\0')
        return -1;
      srcp = after;
      continue;
    }

    *dst++ = *srcp++;
  }

  while (dst > g->redirect_command &&
         (dst[-1] == ' ' || dst[-1] == '\t'))
    --dst;
  *dst = '\0';
  return 0;
}

static int apply_redirections(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              int append_output,
                              int *saved_stdin,
                              int *saved_stdout)
{
  int handle;
  int rc;

  *saved_stdin = -1;
  *saved_stdout = -1;

  if (g->redirect_in[0] != '\0') {
    *saved_stdin = dos_dup_handle(cpu, command_psp, 0);
    if (*saved_stdin < 0)
      return *saved_stdin;

    handle = dos_open_mode(cpu, command_psp, g, g->redirect_in, 0);
    if (handle < 0)
      return handle;

    rc = dos_force_dup(cpu, command_psp, (UWORD)handle, 0);
    fcom_close(cpu, command_psp, (UWORD)handle);
    if (rc < 0)
      return rc;
  }

  if (g->redirect_out[0] != '\0') {
    *saved_stdout = dos_dup_handle(cpu, command_psp, 1);
    if (*saved_stdout < 0)
      return *saved_stdout;

    if (append_output) {
      handle = dos_open_mode(cpu, command_psp, g, g->redirect_out, 1);
      if (handle == -2 || handle == -3)
        handle = dos_create_file(cpu, command_psp, g, g->redirect_out);
      if (handle >= 0 && dos_seek_end(cpu, command_psp,
                                      (UWORD)handle) < 0) {
        fcom_close(cpu, command_psp, (UWORD)handle);
        return -1;
      }
    } else {
      handle = dos_create_file(cpu, command_psp, g, g->redirect_out);
    }

    if (handle < 0)
      return handle;

    rc = dos_force_dup(cpu, command_psp, (UWORD)handle, 1);
    fcom_close(cpu, command_psp, (UWORD)handle);
    if (rc < 0)
      return rc;
  }

  return 0;
}

static void restore_redirections(CPU *cpu, UWORD command_psp,
                                 int saved_stdin, int saved_stdout)
{
  if (saved_stdin >= 0) {
    (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdin, 0);
    fcom_close(cpu, command_psp, (UWORD)saved_stdin);
  }

  if (saved_stdout >= 0) {
    (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdout, 1);
    fcom_close(cpu, command_psp, (UWORD)saved_stdout);
  }
}

static int execute_compact_echo(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, const char *command)
{
  const char *text;
  char delimiter;

  if (strncasecmp(command, "ECHO", 4) != 0)
    return 0;

  delimiter = command[4];
  if (delimiter != '.' && delimiter != ':' &&
      delimiter != ',' && delimiter != ';' &&
      delimiter != '=')
    return 0;

  text = command + 5;

  if (*text == '\0') {
    dos_puts(cpu, command_psp, g, "\n");
  } else {
    dos_puts(cpu, command_psp, g, text);
    dos_puts(cpu, command_psp, g, "\r\n");
  }

  return 1;
}

/*
 * Keep command dispatch out of execute_redirected_command().
 *
 * GCC otherwise inlines this large dispatcher into the redirection
 * coordinator.  That recreates a large frame which remains live across
 * synchronous DosExec(), even though redirection setup/teardown were already
 * split out.  noinline is therefore part of the native-stack lifetime model.
 */
static __attribute__((noinline))
int execute_command_core(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *line)
{
  char *args;
  char *command;
  int suppress_echo = 0;
  int builtin;
  int rc;
  size_t n;

  command = skip_space(line);
  if (*command == '@') {
    suppress_echo = 1;
    command = skip_space(command + 1);
  }

  if (g->batch_active && g->echo_enabled &&
      !suppress_echo && *command != '\0' && *command != ':') {
    dos_puts(cpu, command_psp, g, command);
    dos_puts(cpu, command_psp, g, "\r\n");
  }

  if (strncasecmp(command, "REM", 3) == 0 &&
      (command[3] == '\0' || command[3] == ' ' || command[3] == '\t'))
    return 0;

  /*
   * FreeCOM 0.86 maps TITLE to cmd_rem. It is accepted for compatibility
   * but does not change a host window title.
   */
  if (strncasecmp(command, "TITLE", 5) == 0 &&
      (command[5] == '\0' || command[5] == ' ' || command[5] == '\t'))
    return 0;

  if (execute_compact_echo(cpu, command_psp, g, command))
    return 0;

  if (command != g->input.kb_buf) {
    n = strlen(command);
    if (n > FCOM_LINE_MAX)
      n = FCOM_LINE_MAX;
    memcpy(g->input.kb_buf, command, n);
    g->input.kb_buf[n] = '\0';
    g->input.kb_count = (UBYTE)n;
  }

  args = split_command(g);
  if (g->filename[0] == '\0')
    return 0;

  if (command_is(g->filename, "IF"))
    return execute_if(cpu, command_psp, g, args);

  if (command_is(g->filename, "FOR"))
    return execute_for(cpu, command_psp, g, args);

  /*
   * FreeCOM 0.86 builds with INCLUDE_CMD_LOADHIGH (config.h): LH and
   * LOADHIGH are cmd_loadhigh from shell/loadhigh.c, NOT the
   * FAKELOADHIGH alias of CALL that an earlier revision of this port
   * implemented - that variant is commented out upstream and simply
   * executed the command in low memory.
   */
  if (command_is(g->filename, "LH") ||
      command_is(g->filename, "LOADHIGH")) {
    args = skip_space(args);
    if (*args == '\0') {
      error_bad_command(cpu, command_psp, g, g->filename);
      return 0;
    }
    return builtin_loadhigh(cpu, command_psp, g, args);
  }

  if (command_is(g->filename, "LOADFIX"))
    return builtin_loadfix(cpu, command_psp, g, args);

  if (command_is(g->filename, "GOTO")) {
    char *end;

    if (!g->batch_active)
      return 1;

    args = skip_space(args);
    if (*args == ':')
      args = skip_space(args + 1);

    if (*args == '\0') {
      dos_puts(cpu, command_psp, g,
               "GOTO requires a label.\r\n");
      g->exit_batch_only = 1;
      return -1;
    }

    end = args;
    while (*end != '\0' && *end != ' ' && *end != '\t')
      ++end;
    *end = '\0';

    if (strcasecmp(args, "EOF") == 0) {
      g->exit_batch_only = 1;
      return -1;
    }

    strncpy(g->batch_goto, args, sizeof(g->batch_goto) - 1);
    g->batch_goto[sizeof(g->batch_goto) - 1] = '\0';
    return 0;
  }

  if (command_is(g->filename, "SHIFT")) {
    if (!g->batch_active)
      return 1;

    batch_shift_args_guest(command_psp, args);
    return 0;
  }

  if (command_is(g->filename, "CALL")) {
    char *target;
    char *call_args;
    int call_rc;
    int force_trace = 0;

    if (!g->batch_active)
      return 1;

    args = skip_space(args);
    while ((args[0] == '/' || args[0] == '-') &&
           args[1] != '\0') {
      char *option = args;
      char *next = option;

      while (*next != '\0' && *next != ' ' && *next != '\t')
        ++next;
      if (*next != '\0')
        *next++ = '\0';

      if (strcasecmp(option, "/Y") == 0 ||
          strcasecmp(option, "-Y") == 0) {
        force_trace = 1;
      } else if (strcasecmp(option, "/S") != 0 &&
                 strcasecmp(option, "/N") != 0 &&
                 strcasecmp(option, "-S") != 0 &&
                 strcasecmp(option, "-N") != 0) {
        dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
        return 1;
      }

      args = skip_space(next);
    }

    split_call_target(args, &target, &call_args);
    if (*target == '\0') {
      dos_puts(cpu, command_psp, g,
               "Required parameter missing.\r\n");
      return 1;
    }

    if (force_trace)
      ++g->trace_mode;

    call_rc = execute_batch_file(cpu, command_psp, g,
                                 target, call_args);

    if (force_trace)
      --g->trace_mode;

    if (call_rc < 0 && call_rc != -1)
      error_bad_command(cpu, command_psp, g, target);
    return call_rc == -1 ? -1 : 0;
  }

  builtin = run_builtin(cpu, command_psp, g, args);
  if (builtin < 0)
    return -1;
  if (builtin > 0)
    return 0;

  if (strchr(g->filename, '?') != NULL ||
      strchr(g->filename, '*') != NULL) {
    error_bad_command(cpu, command_psp, g, g->filename);
    return 0;
  }

  if (name_has_extension_ci(g->filename, ".BAT") ||
      name_has_extension_ci(g->filename, ".CMD")) {
    rc = execute_batch_file(cpu, command_psp, g, g->filename, args);
    if (rc < 0 && rc != -1)
      error_bad_command(cpu, command_psp, g, g->filename);
    return rc == -1 ? -1 : 0;
  }

  /*
   * Direct return is intentional.  The large command-dispatch frame must end
   * before an external program enters synchronous DosExec().
   */
  return exec_external(cpu, command_psp, g, args, 0);
}



static int split_pipe_command(struct fcom_guest *g, const char *line)
{
  const char *p = line;
  const char *pipe = NULL;
  int quoted = 0;
  size_t left_len;
  size_t right_len;

  while (*p != '\0') {
    if (*p == '"') {
      quoted = !quoted;
    } else if (!quoted && *p == '|') {
      pipe = p;
      break;
    }
    ++p;
  }

  if (pipe == NULL)
    return 0;

  left_len = (size_t)(pipe - line);
  while (left_len != 0 &&
         (line[left_len - 1] == ' ' || line[left_len - 1] == '\t'))
    --left_len;

  ++pipe;
  while (*pipe == ' ' || *pipe == '\t')
    ++pipe;
  right_len = strlen(pipe);

  if (left_len == 0 || right_len == 0 ||
      left_len >= sizeof(g->pipe_left) ||
      right_len >= sizeof(g->pipe_right))
    return -1;

  memcpy(g->pipe_left, line, left_len);
  g->pipe_left[left_len] = '\0';
  memcpy(g->pipe_right, pipe, right_len + 1);
  return 1;
}

static int dos_delete_file(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, const char *name)
{
  size_t n = strlen(name);

  if (n >= sizeof(g->path))
    return -3;

  memcpy(g->path, name, n + 1);
  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  CPU_AH = 0x41;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM delete pipe temp");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static void build_pipe_temp_name(UWORD command_psp, struct fcom_guest *g)
{
  snprintf(g->pipe_temp, sizeof(g->pipe_temp),
           "FC%04X%02X.TMP", command_psp, (unsigned)g->pipe_depth);
}

static int execute_pipeline(CPU *cpu, UWORD command_psp,
                            struct fcom_guest *g,
                            const char *left, const char *right)
{
  int saved_stdout = -1;
  int saved_stdin = -1;
  int handle;
  int rc = 0;
  int left_rc;
  int right_rc;

  ++g->pipe_depth;
  build_pipe_temp_name(command_psp, g);
  (void)dos_delete_file(cpu, command_psp, g, g->pipe_temp);

  saved_stdout = dos_dup_handle(cpu, command_psp, 1);
  if (saved_stdout < 0)
    goto failed;

  handle = dos_create_file(cpu, command_psp, g, g->pipe_temp);
  if (handle < 0)
    goto failed;

  if (dos_force_dup(cpu, command_psp, (UWORD)handle, 1) < 0) {
    fcom_close(cpu, command_psp, (UWORD)handle);
    goto failed;
  }
  fcom_close(cpu, command_psp, (UWORD)handle);

  left_rc = execute_command_line(cpu, command_psp, g, (char *)left);

  (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdout, 1);
  fcom_close(cpu, command_psp, (UWORD)saved_stdout);
  saved_stdout = -1;

  if (left_rc < 0) {
    rc = left_rc;
    goto cleanup;
  }

  saved_stdin = dos_dup_handle(cpu, command_psp, 0);
  if (saved_stdin < 0)
    goto failed;

  handle = dos_open_mode(cpu, command_psp, g, g->pipe_temp, 0);
  if (handle < 0)
    goto failed;

  if (dos_force_dup(cpu, command_psp, (UWORD)handle, 0) < 0) {
    fcom_close(cpu, command_psp, (UWORD)handle);
    goto failed;
  }
  fcom_close(cpu, command_psp, (UWORD)handle);

  right_rc = execute_command_line(cpu, command_psp, g, (char *)right);
  rc = right_rc;

cleanup:
  if (saved_stdin >= 0) {
    (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdin, 0);
    fcom_close(cpu, command_psp, (UWORD)saved_stdin);
  }
  if (saved_stdout >= 0) {
    (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdout, 1);
    fcom_close(cpu, command_psp, (UWORD)saved_stdout);
  }
  build_pipe_temp_name(command_psp, g);
  (void)dos_delete_file(cpu, command_psp, g, g->pipe_temp);
  --g->pipe_depth;
  return rc;

failed:
  if (saved_stdin >= 0) {
    (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdin, 0);
    fcom_close(cpu, command_psp, (UWORD)saved_stdin);
  }
  if (saved_stdout >= 0) {
    (void)dos_force_dup(cpu, command_psp, (UWORD)saved_stdout, 1);
    fcom_close(cpu, command_psp, (UWORD)saved_stdout);
  }
  build_pipe_temp_name(command_psp, g);
  (void)dos_delete_file(cpu, command_psp, g, g->pipe_temp);
  --g->pipe_depth;
  dos_puts(cpu, command_psp, g, "Pipe failed\r\n");
  return 0;
}


static int execute_command_line(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, char *line)
{
  char *p = skip_space(line);

  /*
   * FreeCOM shell/command.c:
   *   "?" alone is the short-help command;
   *   "? command" enables trace mode for this line only.
   *
   * Keep the ordinary path as a direct return. External commands use this
   * path, so GCC can tail-call execute_command_line_body() instead of keeping
   * this wrapper's native ARM frame alive for the complete nested DOS EXEC.
   * The traced path still needs a small wrapper because trace_mode must be
   * restored after the command returns.
   */
  if (*p == '?') {
    char *command = skip_space(p + 1);

    if (*command != '\0') {
      int rc;

      memmove(line, command, strlen(command) + 1);
      ++g->trace_mode;
      rc = execute_command_line_body(cpu, command_psp, g, line);
      --g->trace_mode;
      return rc;
    }
  }

  return execute_command_line_body(cpu, command_psp, g, line);
}


/*
 * Redirection setup and teardown must not remain on the native stack while
 * execute_command_core() synchronously runs a DOS child.  The saved handles
 * already have a process-owned home in struct fcom_guest; use that instead of
 * keeping saved_stdin/saved_stdout in the coordinator's ARM frame.
 *
 * Keep both helpers out of line. Their frames are needed only before or after
 * the child lifetime, never during it.
 */
static __attribute__((noinline))
int prepare_redirections(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, int append_output)
{
  int saved_stdin = -1;
  int saved_stdout = -1;
  int rc = apply_redirections(cpu, command_psp, g, append_output,
                              &saved_stdin, &saved_stdout);
  g->active_saved_stdin =
      saved_stdin >= 0 ? (UWORD)saved_stdin : 0xffffu;
  g->active_saved_stdout =
      saved_stdout >= 0 ? (UWORD)saved_stdout : 0xffffu;
  if (rc < 0) {
    restore_redirections(cpu, command_psp,
                         saved_stdin, saved_stdout);
    g->active_saved_stdin = 0xffffu;
    g->active_saved_stdout = 0xffffu;
    dos_puts(cpu, command_psp, g, "Redirection failed\r\n");
    return 0;
  }

  return 1;
}


static __attribute__((noinline))
void finish_redirected_command(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g, int rc)
{
  int saved_stdin =
      g->active_saved_stdin != 0xffffu ? (int)g->active_saved_stdin : -1;
  int saved_stdout =
      g->active_saved_stdout != 0xffffu ? (int)g->active_saved_stdout : -1;

  restore_redirections(cpu, command_psp,
                       saved_stdin, saved_stdout);
  g->active_saved_stdin = 0xffffu;
  g->active_saved_stdout = 0xffffu;

  {
    int n = snprintf(g->path, sizeof(g->path), "%d", rc);
    if (n > 0)
      fcom_fddebug_write(cpu, command_psp, g, "RESULT: ", g->path);
  }
}
 
static __attribute__((noinline))
int execute_redirected_command(CPU *cpu, UWORD command_psp,
                               struct fcom_guest *g,
                               int append_output)
{
  int rc;

  /*
   * Only this small coordinator frame remains live across the child EXEC.
   * Parser state ended in execute_command_line_body(); redirection setup state
   * is process-owned in g; setup/teardown helper frames have already returned
   * or have not yet been entered.
   */
  if (!prepare_redirections(cpu, command_psp, g, append_output))
    return 0;

  rc = execute_command_core(cpu, command_psp, g, g->redirect_command);
  finish_redirected_command(cpu, command_psp, g, rc);

  return rc;
}


static int execute_command_line_body(CPU *cpu, UWORD command_psp,
                                     struct fcom_guest *g, char *line)
{
  int append_output;
  int pipe_state;

  fcom_fddebug_write(cpu, command_psp, g, "COMMAND: ", line);
  fcom_expand_aliases(command_psp, g, line, FCOM_LINE_MAX + 1);

  if (g->trace_mode) {
    int answer;

    show_prompt(cpu, command_psp, g);
    fcom_output_resource(cpu, command_psp, g, line);
    fcom_output_resource(cpu, command_psp, g,
                         " [Yes=ENTER, No=ESC] ? ");

    for (;;) {
      CPU_AH = 0x08;
      fcom_intcall(cpu, command_psp, 0x21, "FCOM trace prompt");
      answer = CPU_AL;

      if (answer == 'Y' || answer == 'y' ||
          answer == '\r' || answer == '\n') {
        answer = 1;
        break;
      }

      if (answer == 'N' || answer == 'n' || answer == 0x1b) {
        answer = 0;
        break;
      }

      g->io[0] = '\a';
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io), 1);
    }

    fcom_output_resource(cpu, command_psp, g, "\n");
    if (!answer)
      return 0;
  }

  pipe_state = split_pipe_command(g, line);
  if (pipe_state < 0) {
    dos_puts(cpu, command_psp, g, "Invalid pipe\r\n");
    return 0;
  }
  if (pipe_state > 0)
    return execute_pipeline(cpu, command_psp, g,
                            g->pipe_left, g->pipe_right);

  if (parse_redirections(g, line, &append_output) < 0) {
    dos_puts(cpu, command_psp, g, "Invalid redirection\r\n");
    return 0;
  }

  /* Direct return is intentional: for an ordinary external command the
     compiler can tail-call the helper, so this parser frame is not retained
     while DosExec() and the child run below it. */
  return execute_redirected_command(cpu, command_psp, g, append_output);
}


static uint32_t fcom_guest_skip_space(uint32_t p)
{
  UBYTE c;
  while ((c = fcom_guest_read8(p)) == ' ' || c == '\t')
    ++p;
  return p;
}

static uint32_t fcom_guest_skip_option_argument(uint32_t p)
{
  UBYTE c = fcom_guest_read8(p);

  if (c == ':' || c == '=')
    ++p;
  while ((c = fcom_guest_read8(p)) != 0 && c != ' ' && c != '\t')
    ++p;
  return p;
}

static enum fcom_start_action parse_init_tail_guest(
    UWORD command_psp, UWORD *command_offset)
{
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  const uint32_t init_base =
      gbase + (uint32_t)offsetof(struct fcom_guest, init_tail);
  const uint32_t comspec_base =
      gbase + (uint32_t)offsetof(struct fcom_guest, comspec_arg);
  const uint32_t autoexec_base =
      gbase + (uint32_t)offsetof(struct fcom_guest, autoexec_path);
  uint32_t p = fcom_guest_skip_space(init_base);
  enum fcom_start_action action = FCOM_START_NONE;

  *command_offset = 0xffffu;

  while (fcom_guest_read8(p) != 0) {
    UBYTE c = fcom_guest_read8(p);
    UBYTE option;

    if (c != '/' && c != '-') {
      uint32_t w = p;
      size_t n;

      while ((c = fcom_guest_read8(p)) != 0 && c != ' ' && c != '\t')
        ++p;
      n = (size_t)(p - w);

      if (fcom_guest_read8(comspec_base) == 0 && n != 0) {
        if (n >= sizeof(((struct fcom_guest *)0)->comspec_arg))
          n = sizeof(((struct fcom_guest *)0)->comspec_arg) - 1u;
        fcom_guest_copy(comspec_base, w, n);
        fcom_guest_write8(comspec_base + (uint32_t)n, 0);
      }

      p = fcom_guest_skip_space(p);
      continue;
    }

    ++p;
    option = (UBYTE)toupper((unsigned char)fcom_guest_read8(p));
    if (fcom_guest_read8(p) != 0)
      ++p;

    switch (option) {
    case 'P':
      fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, persistent), 1);
      c = fcom_guest_read8(p);
      if (c == ':' || c == '=') {
        uint32_t word;
        size_t n;

        ++p;
        word = p;
        while ((c = fcom_guest_read8(p)) != 0 && c != ' ' && c != '\t')
          ++p;
        n = (size_t)(p - word);
        if (n >= sizeof(((struct fcom_guest *)0)->autoexec_path))
          n = sizeof(((struct fcom_guest *)0)->autoexec_path) - 1u;
        fcom_guest_copy(autoexec_base, word, n);
        fcom_guest_write8(autoexec_base + (uint32_t)n, 0);
      } else {
        p = fcom_guest_skip_option_argument(p);
      }
      break;

    case 'D':
      fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, skip_autoexec), 1);
      p = fcom_guest_skip_option_argument(p);
      break;

    case 'C':
    case 'K':
      action = option == 'C' ? FCOM_START_EXIT : FCOM_START_KEEP;
      c = fcom_guest_read8(p);
      if (c == '=' || c == ':')
        ++p;
      p = fcom_guest_skip_space(p);
      *command_offset = (UWORD)(FCOM_WORK_OFFSET +
          offsetof(struct fcom_guest, init_tail) + (p - init_base));
      return action;

    case 'E':
    case 'L':
    case 'U':
    case 'Y':
    case 'F':
    case '!':
    default:
      p = fcom_guest_skip_option_argument(p);
      break;
    }

    p = fcom_guest_skip_space(p);
  }

  return action;
}

static UWORD fcom_create_process_common(const char *init_tail,
                          dos_far_ptr guest_tail, UBYTE guest_tail_valid,
                          UBYTE start_mode, UWORD parent_psp,
                          UWORD environment_seg)
{
  seg mcb_seg;
  UWORD largest = 0;
  UWORD command_psp;
  size_t n = 0;

  /*
   * Upstream prep_shell() computes Cmd.ctCount by scanning up to the '\r'
   * terminator - Config.cfgInitTail always carries a trailing "\r\n"
   * appended by InitPgm().  strlen() here used to include that "\r\n" in
   * ctCount, so PSP:80h (and thus parse_init_tail()) saw a tail ending in
   * CR/LF.  The /P=<file> scanner only stops at space/TAB/NUL, so the
   * autoexec path became "\FDAUTO.BAT\r\n" and the open silently failed -
   * /P armed the permanent shell but FDAUTO.BAT never ran.
   */
  if (guest_tail_valid &&
      !far_is_null(guest_tail) && !far_is_end(guest_tail))
  {
    uint32_t tail_base = fcom_guest_linear(FP_SEG(guest_tail),
                                           FP_OFF(guest_tail));
    n = fcom_guest_read8(tail_base + offsetof(CommandTail, ctCount));
    if (n > sizeof(((CommandTail *)0)->ctBuffer))
      n = sizeof(((CommandTail *)0)->ctBuffer);
  }
  else if (init_tail)
  {
    for (; init_tail[n] != '\0'; n++)
      if (init_tail[n] == '\r')
        break;
  }

  {
    UBYTE old_umb_link = fcom_guest_lol_uppermem_link();
    COUNT rc;

    if (start_mode & FIRST_FIT_U) {
      /*
       * FIRST_FIT_U means UMB first, then conventional fallback in the
       * current memory manager.  Use UO here so we can report and control
       * the fallback explicitly.
       */
      DosUmbLink(1);
      rc = DosMemAlloc(FCOM_PROCESS_PARAS, FIRST_FIT_UO,
                       &mcb_seg, &largest);
      DosUmbLink(old_umb_link);

      if (rc < SUCCESS) {
        dos_printf("FCOM: no UMB block of %u paragraphs; trying low memory\n",
                   (unsigned)FCOM_PROCESS_PARAS);
        rc = DosMemAlloc(FCOM_PROCESS_PARAS, FIRST_FIT,
                         &mcb_seg, &largest);
      }
    } else {
      rc = DosMemAlloc(FCOM_PROCESS_PARAS, FIRST_FIT,
                       &mcb_seg, &largest);
    }

    if (rc < SUCCESS)
      return 0;
  }

  command_psp = mcb_seg + 1;
  fdos_mcb_set_owner(mcb_seg, command_psp);
  fdos_mcb_set_name8(mcb_seg, "COMMAND ");

  child_psp(command_psp, parent_psp, command_psp + FCOM_PROCESS_PARAS);
  {
    uint32_t pspbase = fcom_guest_linear(command_psp, 0);
    dos_far_ptr prevpsp = MK_FP(parent_psp, 0);
    UBYTE count;

    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_parent),
                     &parent_psp, sizeof(parent_psp));
    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_prevpsp),
                     &prevpsp, sizeof(prevpsp));
    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_environ),
                     &environment_seg, sizeof(environment_seg));

    if (n > sizeof(((psp *)0)->ps_cmd.ctBuffer) - 1u)
      n = sizeof(((psp *)0)->ps_cmd.ctBuffer) - 1u;
    count = (UBYTE)n;
    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_cmd.ctCount),
                     &count, sizeof(count));
    if (n)
    {
      if (guest_tail_valid &&
          !far_is_null(guest_tail) && !far_is_end(guest_tail))
      {
        uint32_t src =
            fcom_guest_linear(FP_SEG(guest_tail), FP_OFF(guest_tail)) +
            (uint32_t)offsetof(CommandTail, ctBuffer);
        size_t i;
        for (i = 0; i < n; ++i) {
          uint8_t c = fcom_guest_read8(src + (uint32_t)i);
          fcom_guest_write(pspbase +
                               (uint32_t)offsetof(psp, ps_cmd.ctBuffer) +
                               (uint32_t)i,
                           &c, 1u);
        }
      }
      else
      {
        fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_cmd.ctBuffer),
                         init_tail, n);
      }
    }
    {
      const char cr = '\r';
      fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_cmd.ctBuffer) +
                           (uint32_t)n,
                       &cr, 1u);
    }
  }

  /*
   * Native COMMAND still has an ordinary guest execution identity.
   * Nothing normally executes this code, but CS:IP must not continue to
   * identify the suspended parent process.  A tight JMP loop is harmless
   * if the CPU core is stepped accidentally outside bios_intcall().
   */
  {
    const UBYTE entry[2] = {0xeb, 0xfe}; /* JMP SHORT -2 */
    const UBYTE term[2] = {0xcd, 0x20};  /* INT 20h */
    fcom_guest_write(fcom_guest_linear(command_psp, FCOM_ENTRY_OFFSET),
                     entry, sizeof(entry));
    fcom_guest_write(fcom_guest_linear(command_psp, FCOM_TERM_OFFSET),
                     term, sizeof(term));
  }

  /*
   * cb_catch.asm cbreak_handler as real guest code - see the layout
   * comment at FCOM_CBREAK_STUB_OFFSET.  fcom_process_main() installs
   * the INT 23h vector; the previous vector was already captured into
   * ps_isv23 by child_psp() above and is restored by process
   * termination (task.c), exactly like a real FreeCOM instance.
   */
  {
    UBYTE stub[11] = {
      0x2e, 0xff, 0x06,
      (UBYTE)(FCOM_CBREAK_COUNT_OFFSET & 0xff),
      (UBYTE)(FCOM_CBREAK_COUNT_OFFSET >> 8),
      0xf8, 0xca, 0x02, 0x00, 0x00, 0x00
    };
    fcom_guest_write(fcom_guest_linear(command_psp, FCOM_CBREAK_STUB_OFFSET),
                     stub, sizeof(stub));
  }

  /*
   * Do not transfer ownership of environment_seg here.  For normal EXEC,
   * patchPSP() owns that operation because ChildEnv() created a private MCB.
   * Process 0 instead points at DOS_PSP's permanent environment and must not
   * make it part of COMMAND's FreeProcessMem() ownership chain.
   */
  return command_psp;
}

UWORD fcom_create_process(const char *init_tail, UBYTE start_mode,
                          UWORD parent_psp, UWORD environment_seg)
{
  return fcom_create_process_common(init_tail, MK_FP(0, 0), 0,
                                    start_mode, parent_psp, environment_seg);
}

UWORD fcom_create_process_guest_tail(dos_far_ptr command_tail,
                          UBYTE start_mode, UWORD parent_psp,
                          UWORD environment_seg)
{
  return fcom_create_process_common(NULL, command_tail, 1,
                                    start_mode, parent_psp, environment_seg);
}

UWORD fcom_process_stack_top(void)
{
  return FCOM_STACK_TOP;
}

UWORD fcom_process_entry_offset(void)
{
  return FCOM_ENTRY_OFFSET;
}

/*
 * The interactive/session phase is kept separate from bootstrap.  All values
 * needed after the session (the original parent and INT 22h vector) live in the
 * process-owned guest block, so fcom_process_main() can tail-call this helper
 * instead of retaining its bootstrap frame for every nested EXEC.
 */
static __attribute__((noinline))
UBYTE fcom_process_session(CPU *cpu, UWORD command_psp,
                           enum fcom_start_action start_action,
                           UWORD start_command_offset)
{
  const uint32_t pspbase = fcom_guest_linear(command_psp, 0);
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);

  if (fcom_guest_read8(gbase +
          (uint32_t)offsetof(struct fcom_guest, persistent)) &&
      !fcom_guest_read8(gbase +
          (uint32_t)offsetof(struct fcom_guest, skip_autoexec)) &&
      start_action == FCOM_START_NONE) {
    const UWORD autoexec_off =
        FCOM_WORK_OFFSET +
        (UWORD)offsetof(struct fcom_guest, autoexec_path);
    uint32_t autoexec = fcom_guest_linear(command_psp, autoexec_off);

    if (fcom_guest_read8(autoexec) != 0)
      (void)execute_batch_guest_path(cpu, command_psp, autoexec_off);
    else
      (void)execute_default_autoexec(cpu, command_psp, NULL);
  }

  if (start_command_offset != 0xffffu &&
      fcom_guest_read8(fcom_guest_linear(command_psp,
                                         start_command_offset)) != 0) {
    struct fcom_guest *g = fcom_state(command_psp);
    char *start_command = (char *)fcom_shadow_at(
        command_psp, start_command_offset, 1);
    int rc = execute_command_line(cpu, command_psp, g, start_command);

    if (rc < 0 &&
        !fcom_guest_read8(gbase +
            (uint32_t)offsetof(struct fcom_guest, persistent)))
      goto done;

    if (start_action == FCOM_START_EXIT &&
        !fcom_guest_read8(gbase +
            (uint32_t)offsetof(struct fcom_guest, persistent)))
      goto done;
  }

  for (;;) {
    int rc;
    struct fcom_guest *g;

    g = fcom_state(command_psp);
    ensure_prompt_column_zero(cpu, command_psp, g);

    g = fcom_state(command_psp);
    show_prompt(cpu, command_psp, g);

    g = fcom_state(command_psp);
    if (fcom_readline(cpu, command_psp, g) == 0)
      continue;

    g = fcom_state(command_psp);
    fcom_history_add(command_psp, g, g->input.kb_buf);

    g = fcom_state(command_psp);
    rc = execute_command_line(cpu, command_psp, g, g->input.kb_buf);
    if (rc < 0)
      break;
  }

done:
  {
    struct fcom_guest *g =
        fcom_state(command_psp);
    fcom_fddebug_close(cpu, command_psp, g);
  }

  {
    UWORD parent = fcom_guest_read16(
        gbase + (uint32_t)offsetof(struct fcom_guest, saved_parent_psp));
    intvec terminate_addr;

    fcom_guest_read(
        gbase + (uint32_t)offsetof(struct fcom_guest, saved_terminate_addr),
        &terminate_addr, sizeof(terminate_addr));
    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_parent),
                     &parent, sizeof(parent));
    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_isv22),
                     &terminate_addr, sizeof(terminate_addr));
  }

  return (UBYTE)(fcom_guest_read8(
      gbase + (uint32_t)offsetof(struct fcom_guest, exit_requested))
      ? fcom_guest_read16(
          gbase + (uint32_t)offsetof(struct fcom_guest, exit_code))
      : fcom_guest_read8(
          gbase + (uint32_t)offsetof(struct fcom_guest, errorlevel)));
}

UBYTE fcom_process_main(CPU *cpu, UWORD command_psp)
{
  const uint32_t pspbase = fcom_guest_linear(command_psp, 0);
  const uint32_t gbase = fcom_guest_linear(command_psp, FCOM_WORK_OFFSET);
  UWORD saved_parent_psp;
  intvec saved_terminate_addr;
  enum fcom_start_action start_action;
  UWORD start_command_offset;
  UWORD word;
  UBYTE byte;
  UBYTE session_rc;

  /* Keep the large FCOM mirror out of static BSS.  PC/CPU and the core
   * devices are allocated from the native heap before COMMAND starts; a
   * static FCOM_SHADOW_BYTES array shifts those hot allocations in SRAM even
   * though the shadow is not used while a guest program runs. */
  if (fcom_persistent_shadow == NULL) {
    fcom_persistent_shadow = (UBYTE *)malloc(FCOM_SHADOW_BYTES);
    if (fcom_persistent_shadow == NULL) {
      dos_printf("FCOM: cannot allocate native shadow\n");
      return 8;
    }
  }

  if (!fcom_guest_shadow_enter(gbase, fcom_persistent_shadow,
                               FCOM_SHADOW_BYTES)) {
    dos_printf("FCOM: native shadow nesting overflow\n");
    return 8;
  }

  fcom_guest_read(pspbase + (uint32_t)offsetof(psp, ps_parent),
                  &saved_parent_psp, sizeof(saved_parent_psp));
  fcom_guest_read(pspbase + (uint32_t)offsetof(psp, ps_isv22),
                  &saved_terminate_addr, sizeof(saved_terminate_addr));

  fcom_guest_fill(gbase, 0, sizeof(struct fcom_guest));
  fcom_guest_fill(fcom_guest_linear(command_psp, FCOM_DIR_STACK_OFFSET),
                  0, FCOM_DIR_STACK_BYTES);
  fcom_guest_fill(fcom_guest_linear(command_psp, FCOM_ALIAS_OFFSET),
                  0, FCOM_ALIAS_BYTES);
  fcom_guest_fill(fcom_guest_linear(command_psp, FCOM_HISTORY_OFFSET),
                  0, FCOM_HISTORY_BYTES);
  fcom_guest_fill(fcom_guest_linear(command_psp, FCOM_LOADFIX_OFFSET),
                  0, FCOM_LOADFIX_BYTES);
  fcom_guest_fill(fcom_guest_linear(command_psp, FCOM_BATCH_CONTEXT_OFFSET),
                  0, FCOM_BATCH_CONTEXT_BYTES);

  word = 0xffffu;
  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, active_saved_stdin),
                     word);
  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, active_saved_stdout),
                     word);
  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, saved_parent_psp),
                     saved_parent_psp);
  fcom_guest_write(gbase + (uint32_t)offsetof(struct fcom_guest, saved_terminate_addr),
                   &saved_terminate_addr, sizeof(saved_terminate_addr));
  word = 1u;
  fcom_guest_write16(gbase + (uint32_t)offsetof(struct fcom_guest, fddebug_handle),
                     word);
  fcom_guest_write(gbase + (uint32_t)offsetof(struct fcom_guest, fddebug_name),
                   "stdout", sizeof("stdout"));
  byte = 1u;
  fcom_guest_write(gbase + (uint32_t)offsetof(struct fcom_guest, echo_enabled),
                   &byte, sizeof(byte));
  init_stack_guard(command_psp);

  /* FreeCOM initialize(): COMMAND temporarily owns itself as parent and
     replaces PSP:0Ah with its terminate hook. */
  fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_parent),
                   &command_psp, sizeof(command_psp));
  {
    intvec term = MK_FP(command_psp, FCOM_TERM_OFFSET);
    fcom_guest_write(pspbase + (uint32_t)offsetof(psp, ps_isv22),
                     &term, sizeof(term));
  }

  /*
   * shell/init.c:193: set_isrfct(0x23, cbreak_handler).
   * The stub bytes were written by fcom_create_process(); the previous
   * vector is in ps_isv23 and comes back at process termination.
   */
  setvec(0x23, MK_FP(command_psp, FCOM_CBREAK_STUB_OFFSET));

  /*
   * PSP:80h is the canonical source.  Parse it before COMSPEC
   * initialization: a non-option word in SHELL= is the COMSPEC source.
   */
  {
    UBYTE count;

    fcom_guest_read(pspbase + (uint32_t)offsetof(psp, ps_cmd.ctCount),
                    &count, sizeof(count));
    if (count >= sizeof(((struct fcom_guest *)0)->init_tail))
      count = sizeof(((struct fcom_guest *)0)->init_tail) - 1u;
    if (count != 0)
      fcom_guest_copy(gbase + (uint32_t)offsetof(struct fcom_guest, init_tail),
                      pspbase + (uint32_t)offsetof(psp, ps_cmd.ctBuffer),
                      count);
    fcom_guest_write8(gbase + (uint32_t)offsetof(struct fcom_guest, init_tail) +
                (uint32_t)count, 0);
  }

  start_action = parse_init_tail_guest(command_psp, &start_command_offset);

  /* FreeCOM initialize(): publish COMSPEC before any startup command runs. */
  if (fcom_initialize_environment(cpu, command_psp) < 0)
    dos_printf("FCOM: cannot set COMSPEC in environment\n");

#if FCOM_DEBUG
  dos_printf("FCOM: PSP=%04x parent=%04x block=%u paras (%u bytes) "
             "data=%04x..%04x stack=%04x..%04x (%u bytes) %s\n",
             command_psp,
             fcom_guest_read16(gbase +
                 (uint32_t)offsetof(struct fcom_guest, saved_parent_psp)),
             (unsigned)FCOM_PROCESS_PARAS,
             (unsigned)FCOM_PROCESS_BYTES,
             FCOM_WORK_OFFSET,
             (unsigned)(FCOM_DATA_END - 1u),
             (unsigned)FCOM_STACK_BOTTOM,
             (unsigned)(FCOM_STACK_TOP - 1u),
             (unsigned)FCOM_STACK_BYTES,
             command_psp >= 0xa000u ? "HIGH" : "LOW");
#else
  /* Баннер печатает только процесс #0 (родитель - PSP ядра):
     COMSPEC-дети (COMMAND /C для внешних команд) стартуют молча,
     как транзиентные экземпляры COMMAND.COM в MS-DOS. */
  if (fcom_guest_read16(gbase +
          (uint32_t)offsetof(struct fcom_guest, saved_parent_psp)) == DOS_PSP)
    dos_printf("FreeCom v.0.86 (for RP2350) @ %04Xh [%s]\n",
      command_psp, command_psp >= 0xa000u ? "UMB" : "LOW");
#endif
  /*
   * Direct return is intentional: bootstrap locals and saved PSP values are
   * no longer live while commands and their synchronous children execute.
   */
  session_rc = fcom_process_session(cpu, command_psp,
                                    start_action, start_command_offset);
  fcom_guest_shadow_leave();
  return session_rc;
}

void fcom_run(CPU *cpu, const char *init_tail, UBYTE start_mode,
              UWORD environment_seg, UBYTE own_environment)
{
  UWORD parent_psp = fcom_guest_current_psp();
  UWORD command_psp =
      fcom_create_process(init_tail, start_mode, parent_psp, environment_seg);

  (void)cpu;

  if (command_psp == 0) {
    dos_printf("FCOM: cannot allocate COMMAND process\n");
    return;
  }

  if (own_environment && environment_seg != 0)
    fdos_mcb_set_owner((seg)(environment_seg - 1u), command_psp);

  /*
   * Process 0 now uses the same enter/run/leave path as a nested native
   * COMMAND.  Its only special property is that its environment is borrowed
   * from DOS_PSP and therefore was not transferred to the child MCB chain.
   */
  (void)exec_run_native_command(command_psp, 0);
}
