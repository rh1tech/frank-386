/*
 * Native FreeCOM bootstrap (v.0.86 port to RP2350).
 */
#include <ctype.h>
#include <stddef.h>
#include <string.h>
/// do not include stdio.h, it conflicts with kernel headers
int snprintf(char *s, size_t n, const char *fmt, ...);

#include "fdos/hdrs.h"
#include "bios/bios.h"
#include "fdos/fcom/fcom.h"

#define FCOM_LINE_MAX       126u
#define FCOM_PROCESS_PARAS  0x1000u
#define FCOM_WORK_OFFSET    0x0100u
#define FCOM_STACK_TOP      0x0000u
#define FCOM_STACK_GUARD    16u
#define FCOM_ALIGN16(v)     (((v) + 15u) & ~15u)

static int fcom_write(CPU *cpu, UWORD command_psp, UWORD offset, UWORD count);

#pragma pack(push, 1)
struct fcom_guest {
  keyboard input;
  exec_blk exec_block;
  CommandTail tail;
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
  UBYTE echo_enabled;
  UBYTE exit_requested;
  UBYTE exit_batch_only;
  UWORD exit_code;
};
#pragma pack(pop)

#define FCOM_DATA_END       (FCOM_WORK_OFFSET + sizeof(struct fcom_guest))
#define FCOM_GUARD_OFFSET   FCOM_ALIGN16(FCOM_DATA_END)
#define FCOM_STACK_BOTTOM   (FCOM_GUARD_OFFSET + FCOM_STACK_GUARD)
#define FCOM_STACK_BYTES    (0x10000u - FCOM_STACK_BOTTOM)

_Static_assert(FCOM_STACK_BOTTOM < 0x10000u,
               "FCOM guest data leaves no room for guest stack");
_Static_assert((FCOM_GUARD_OFFSET & 15u) == 0,
               "FCOM stack guard is not paragraph-aligned");

static int int21_failed(CPU *cpu)
{
  return (cpu_getflags(cpu) & 1u) != 0;
}

static UBYTE *stack_guard(UWORD command_psp)
{
  return (UBYTE *)ARM_PTR(MK_FP(command_psp, FCOM_GUARD_OFFSET));
}

static void init_stack_guard(UWORD command_psp)
{
  memset(stack_guard(command_psp), 0xa5, FCOM_STACK_GUARD);
}

static int stack_guard_ok(UWORD command_psp)
{
  const UBYTE *p = stack_guard(command_psp);
  unsigned i;

  for (i = 0; i < FCOM_STACK_GUARD; ++i) {
    if (p[i] != 0xa5)
      return 0;
  }
  return 1;
}

static void fcom_intcall(CPU *cpu, UWORD command_psp, UBYTE intno,
                         const char *owner)
{
  UWORD old_ss = CPU_SS;
  UWORD old_sp = CPU_SP;

  /* SP=0000h is the canonical top of a 64-KiB 16-bit stack: the first
     PUSH wraps to FFFEh.  Static data occupies the low part of this
     segment; the guard below it detects a stack collision. */
  SET_SS(command_psp);
  CPU_SP = FCOM_STACK_TOP;
  bios_intcall(cpu, intno, owner);
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

static const char *fcom_env_value(UWORD command_psp, const char *name)
{
  const psp *process = (const psp *)ARM_PTR(MK_FP(command_psp, 0));
  const char *env;
  size_t name_len = strlen(name);
  unsigned left = 0x8000u;

  if (process->ps_environ == 0)
    return NULL;

  env = (const char *)ARM_PTR(MK_FP(process->ps_environ, 0));
  while (left && *env) {
    size_t n = strnlen(env, left);

    if (n == left)
      break;
    if (n > name_len && env[name_len] == '=' &&
        strncasecmp(env, name, name_len) == 0)
      return env + name_len + 1;

    env += n + 1;
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
  const char *format = fcom_env_value(command_psp, "PROMPT");
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

static void clear_screen(CPU *cpu, UWORD command_psp)
{
  CPU_AX = 0x0003;
  fcom_intcall(cpu, command_psp, 0x10, "FCOM CLS");
}


static void pause_command(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g)
{
  dos_puts(cpu, command_psp, g,
           "Press any key to continue . . .");

  CPU_AH = 0x08;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM PAUSE");

  dos_puts(cpu, command_psp, g, "\r\n");
}


static int fcom_write(CPU *cpu, UWORD command_psp, UWORD offset, UWORD count)
{
  SET_DS(command_psp);
  CPU_DX = offset;
  CPU_BX = 1;
  CPU_CX = count;
  CPU_AH = 0x40;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM write");
  return int21_failed(cpu) ? -(int)CPU_AX : (int)CPU_AX;
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

static void dir_line(CPU *cpu, UWORD command_psp, struct fcom_guest *g)
{
  int n;

  if (g->find.dm_attr_fnd & D_DIR) {
    n = snprintf(g->text, sizeof(g->text), "%-13s <DIR>\r\n",
                 g->find.dm_name);
  } else {
    n = snprintf(g->text, sizeof(g->text), "%-13s %10lu\r\n",
                 g->find.dm_name, (unsigned long)g->find.dm_size);
  }

  if (n < 0)
    return;
  if ((size_t)n >= sizeof(g->text))
    n = sizeof(g->text) - 1;
  fcom_write(cpu, command_psp,
            FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
            (UWORD)n);
}

static void builtin_dir(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, const char *args)
{
  const char *pattern = *args ? args : "*.*";
  int rc;

  memset(&g->find, 0, sizeof(g->find));
  if (set_find_dta(cpu, command_psp) < 0) {
    dos_puts(cpu, command_psp, g, "Unable to set DTA\r\n");
    return;
  }

  rc = dos_find_first(cpu, command_psp, g, pattern);
  if (rc < 0) {
    restore_default_dta(cpu, command_psp);
    dos_puts(cpu, command_psp, g, "File not found\r\n");
    return;
  }

  do {
    if (strcmp(g->find.dm_name, ".") != 0 &&
        strcmp(g->find.dm_name, "..") != 0)
      dir_line(cpu, command_psp, g);
    rc = dos_find_next(cpu, command_psp);
  } while (rc == 0);

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

static void builtin_type(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, const char *args)
{
  int handle;
  int count;

  if (*args == '\0') {
    dos_puts(cpu, command_psp, g, "Required parameter missing\r\n");
    return;
  }

  handle = dos_open_read(cpu, command_psp, g, args);
  if (handle < 0) {
    dos_puts(cpu, command_psp, g, "File not found\r\n");
    return;
  }

  while ((count = fcom_read(cpu, command_psp, g, (UWORD)handle)) > 0) {
    int written = fcom_write(
        cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, io),
        (UWORD)count);
    if (written < 0 || written != count)
      break;
  }

  fcom_close(cpu, command_psp, (UWORD)handle);
}

static char *environment_start(UWORD command_psp)
{
  const psp *process = (const psp *)ARM_PTR(MK_FP(command_psp, 0));

  if (process->ps_environ == 0)
    return NULL;
  return (char *)ARM_PTR(MK_FP(process->ps_environ, 0));
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

static int env_name_matches(const char *entry, size_t entry_len,
                            const char *name, size_t name_len)
{
  return entry_len > name_len &&
         entry[name_len] == '=' &&
         strncasecmp(entry, name, name_len) == 0;
}

static int replace_environment_variable(CPU *cpu, UWORD command_psp,
                                        struct fcom_guest *g,
                                        const char *assignment)
{
  const char *equal = strchr(assignment, '=');
  const char *value;
  const char *src;
  size_t name_len;
  size_t value_len;
  size_t copied_bytes = 0;
  size_t entry_count = 0;
  size_t required;
  unsigned left = 0x8000u;
  UWORD paras;
  int allocated;
  UWORD new_seg;
  char *dst;
  size_t pos = 0;
  psp *process;

  if (equal == NULL)
    return -1;

  name_len = (size_t)(equal - assignment);
  value = equal + 1;
  value_len = strlen(value);

  if (name_len == 0 || memchr(assignment, ' ', name_len) != NULL ||
      memchr(assignment, '\t', name_len) != NULL)
    return -1;

  src = environment_start(command_psp);
  while (src && left && *src) {
    size_t n = strnlen(src, left);

    if (n == left)
      return -1;

    if (!env_name_matches(src, n, assignment, name_len)) {
      copied_bytes += n + 1;
      ++entry_count;
    }

    src += n + 1;
    left -= (unsigned)n + 1u;
  }

  if (value_len != 0) {
    copied_bytes += name_len + 1 + value_len + 1;
    ++entry_count;
  }

  /*
   * Environment variables are followed by a double NUL.  After it,
   * keep a zero extra-string count, which is a valid DOS environment
   * trailer for this native COMMAND process.
   */
  required = copied_bytes + (entry_count ? 1u : 2u) + 2u;
  if (required > 0xfff0u)
    return -8;

  paras = (UWORD)((required + 15u) >> 4);
  allocated = guest_alloc(cpu, command_psp, paras);
  if (allocated < 0)
    return allocated;

  new_seg = (UWORD)allocated;
  dst = (char *)ARM_PTR(MK_FP(new_seg, 0));

  src = environment_start(command_psp);
  left = 0x8000u;
  while (src && left && *src) {
    size_t n = strnlen(src, left);

    if (n == left) {
      guest_free(cpu, command_psp, new_seg);
      return -1;
    }

    if (!env_name_matches(src, n, assignment, name_len)) {
      memcpy(dst + pos, src, n + 1);
      pos += n + 1;
    }

    src += n + 1;
    left -= (unsigned)n + 1u;
  }

  if (value_len != 0) {
    memcpy(dst + pos, assignment, name_len);
    pos += name_len;
    dst[pos++] = '=';
    memcpy(dst + pos, value, value_len);
    pos += value_len;
    dst[pos++] = '\0';
  }

  if (pos == 0)
    dst[pos++] = '\0';
  dst[pos++] = '\0';
  dst[pos++] = '\0'; /* extra-string count, low byte */
  dst[pos++] = '\0'; /* extra-string count, high byte */

  process = (psp *)ARM_PTR(MK_FP(command_psp, 0));
  process->ps_environ = new_seg;

  if (g->owned_env_seg != 0)
    guest_free(cpu, command_psp, g->owned_env_seg);

  g->owned_env_seg = new_seg;
  g->owned_env_bytes = (UWORD)(paras << 4);
  return 0;
}

static void builtin_set(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, const char *args)
{
  char *env;
  size_t key_len;
  unsigned left;
  int found = 0;

  if (strchr(args, '=') != NULL) {
    int rc = replace_environment_variable(cpu, command_psp, g, args);

    if (rc < 0)
      dos_puts(cpu, command_psp, g, "Unable to set environment variable\r\n");
    return;
  }

  env = environment_start(command_psp);
  key_len = strlen(args);
  left = 0x8000u;

  while (env && left && *env) {
    size_t n = strnlen(env, left);

    if (n == left)
      break;

    if (key_len == 0 ||
        env_name_matches(env, n, args, key_len)) {
      size_t done = 0;

      while (done < n) {
        size_t chunk = n - done;

        if (chunk > sizeof(g->io))
          chunk = sizeof(g->io);
        memcpy(g->io, env + done, chunk);
        if (fcom_write(cpu, command_psp,
                       FCOM_WORK_OFFSET +
                         (UWORD)offsetof(struct fcom_guest, io),
                       (UWORD)chunk) < 0)
          return;
        done += chunk;
      }

      dos_puts(cpu, command_psp, g, "\r\n");
      found = 1;
    }

    env += n + 1;
    left -= (unsigned)n + 1u;
  }

  if (key_len != 0 && !found)
    dos_puts(cpu, command_psp, g, "Environment variable not defined\r\n");
}


static void builtin_path(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  char *env;
  size_t n;

  while (*p != '\0') {
    n = strlen(p);
    if (n == 0 || (p[n - 1] != ' ' && p[n - 1] != '\t'))
      break;
    p[n - 1] = '\0';
  }

  /*
   * FreeCOM cmd/path.c:
   *   PATH with no argument displays PATH or "No search path defined."
   *   PATH ; is not treated as the display form.
   */
  if (*p == '\0' && strchr(args, ';') == NULL) {
    env = environment_start(command_psp);
    while (env && *env) {
      n = strlen(env);
      if (n >= 5 && strncasecmp(env, "PATH=", 5) == 0) {
        dos_puts(cpu, command_psp, g, "PATH=");
        dos_puts(cpu, command_psp, g, env + 5);
        dos_puts(cpu, command_psp, g, "\r\n");
        return;
      }
      env += n + 1;
    }

    dos_puts(cpu, command_psp, g, "No search path defined.\r\n");
    return;
  }

  if (strlen(p) + 5 >= sizeof(g->text)) {
    dos_puts(cpu, command_psp, g,
             "Unable to set environment variable\r\n");
    return;
  }

  memcpy(g->text, "PATH=", 5);
  strcpy(g->text + 5, p);
  if (replace_environment_variable(cpu, command_psp,
                                   g, g->text) < 0)
    dos_puts(cpu, command_psp, g,
             "Unable to set environment variable\r\n");
}



static void builtin_prompt(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  size_t n = strlen(p);

  if (n + 7 >= sizeof(g->text)) {
    dos_puts(cpu, command_psp, g,
             "Unable to set environment variable\r\n");
    return;
  }

  memcpy(g->text, "PROMPT=", 7);
  memcpy(g->text + 7, p, n + 1);

  if (replace_environment_variable(cpu, command_psp,
                                   g, g->text) < 0)
    dos_puts(cpu, command_psp, g,
             "Unable to set environment variable\r\n");
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

static void builtin_truename(CPU *cpu, UWORD command_psp,
                             struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  size_t n;

  if (*p == '\0')
    p = ".";

  n = strlen(p);
  if (n >= sizeof(g->path)) {
    dos_puts(cpu, command_psp, g, "Invalid path.\r\n");
    return;
  }

  memcpy(g->path, p, n + 1);

  SET_DS(command_psp);
  CPU_SI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, path);
  SET_ES(command_psp);
  CPU_DI = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text);
  CPU_AH = 0x60;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM TRUENAME");

  if (int21_failed(cpu)) {
    dos_puts(cpu, command_psp, g, "Invalid path.\r\n");
    return;
  }

  dos_puts(cpu, command_psp, g, g->text);
  dos_puts(cpu, command_psp, g, "\r\n");
}

static void builtin_cdd(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned old_drive;
  unsigned new_drive;
  int has_drive;

  if (*p == '\0') {
    dos_puts(cpu, command_psp, g, "Required parameter missing\r\n");
    return;
  }

  old_drive = dos_get_drive(cpu, command_psp);
  has_drive = isalpha((unsigned char)p[0]) && p[1] == ':';
  new_drive = has_drive
      ? (unsigned)(toupper((unsigned char)p[0]) - 'A')
      : old_drive;

  if (has_drive && !dos_set_drive(cpu, command_psp, new_drive)) {
    dos_puts(cpu, command_psp, g, "Invalid drive\r\n");
    return;
  }

  if (has_drive)
    p += 2;
  p = skip_space(p);

  if (*p != '\0' && dos_change_dir(cpu, command_psp, g, p) < 0) {
    if (has_drive)
      (void)dos_set_drive(cpu, command_psp, old_drive);
    dos_puts(cpu, command_psp, g, "Invalid directory\r\n");
  }
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

static void report_file_error(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g,
                              const char *prefix, const char *name)
{
  dos_puts(cpu, command_psp, g, prefix);
  dos_puts(cpu, command_psp, g, name);
  dos_puts(cpu, command_psp, g, "'\r\n");
}

static void builtin_mkdir(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *name = next_argument(&cursor);

  if (name == NULL) {
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
    return;
  }
  if (next_argument(&cursor) != NULL) {
    dos_puts(cpu, command_psp, g, "Too many parameters.\r\n");
    return;
  }

  if (fcom_mkdir(cpu, command_psp, g, name) < 0)
    report_file_error(cpu, command_psp, g,
                      "Unable to create directory. - '", name);
}

static void builtin_rmdir(CPU *cpu, UWORD command_psp,
                          struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *name = next_argument(&cursor);

  if (name == NULL) {
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
    return;
  }
  if (next_argument(&cursor) != NULL) {
    dos_puts(cpu, command_psp, g, "Too many parameters.\r\n");
    return;
  }

  if (fcom_rmdir(cpu, command_psp, g, name) < 0)
    report_file_error(cpu, command_psp, g,
                      "Unable to remove directory. - '", name);
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

static void builtin_del(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *pattern;
  int any = 0;

  while ((pattern = next_argument(&cursor)) != NULL) {
    int rc;

    memset(&g->find, 0, sizeof(g->find));
    if (set_find_dta(cpu, command_psp) < 0)
      break;

    rc = dos_find_first(cpu, command_psp, g, pattern);
    if (rc < 0) {
      restore_default_dta(cpu, command_psp);
      report_file_error(cpu, command_psp, g,
                        "File not found. - '", pattern);
      continue;
    }

    do {
      if (!(g->find.dm_attr_fnd & D_DIR) &&
          make_delete_name(g, pattern, g->find.dm_name)) {
        if (dos_unlink(cpu, command_psp, g, g->path2) == 0)
          any = 1;
      }
      rc = dos_find_next(cpu, command_psp);
    } while (rc == 0);

    restore_default_dta(cpu, command_psp);
  }

  if (!any && *skip_space(args) == '\0')
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
}

static void builtin_rename(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *old_name = next_argument(&cursor);
  char *new_name = next_argument(&cursor);

  if (old_name == NULL || new_name == NULL) {
    dos_puts(cpu, command_psp, g, "Required parameter missing.\r\n");
    return;
  }
  if (next_argument(&cursor) != NULL) {
    dos_puts(cpu, command_psp, g, "Too many parameters.\r\n");
    return;
  }

  /*
   * FreeCOM rejects a drive or directory in the destination argument.
   * DOS AH=56h also expects the destination directory to be implied by
   * the source path for the normal REN syntax.
   */
  if (strchr(new_name, ':') != NULL ||
      strchr(new_name, '\\') != NULL ||
      strchr(new_name, '/') != NULL) {
    report_file_error(cpu, command_psp, g,
                      "Syntax error. - '", new_name);
    return;
  }

  if (dos_rename_file(cpu, command_psp, g, old_name, new_name) < 0)
    report_file_error(cpu, command_psp, g,
                      "File not found. - '", old_name);
}



static int parse_uint_field(const char **cursor, unsigned *value)
{
  const char *p = *cursor;
  unsigned result = 0;

  if (!isdigit((unsigned char)*p))
    return 0;

  while (isdigit((unsigned char)*p)) {
    result = result * 10u + (unsigned)(*p - '0');
    ++p;
  }

  *cursor = p;
  *value = result;
  return 1;
}

static int parse_date_value(const char *text,
                            unsigned *month, unsigned *day, unsigned *year)
{
  const char *p = text;

  if (!parse_uint_field(&p, month))
    return 0;
  if (*p != '-' && *p != '/' && *p != '.')
    return 0;
  ++p;

  if (!parse_uint_field(&p, day))
    return 0;
  if (*p != '-' && *p != '/' && *p != '.')
    return 0;
  ++p;

  if (!parse_uint_field(&p, year))
    return 0;

  p = skip_space((char *)p);
  if (*p != '\0')
    return 0;

  if (*year < 100u)
    *year += *year >= 80u ? 1900u : 2000u;

  return *month >= 1u && *month <= 12u &&
         *day >= 1u && *day <= 31u &&
         *year >= 1980u && *year <= 2099u;
}

static int parse_time_value(const char *text,
                            unsigned *hour, unsigned *minute,
                            unsigned *second, unsigned *hundredth)
{
  const char *p = text;

  *second = 0;
  *hundredth = 0;

  if (!parse_uint_field(&p, hour) || *p != ':')
    return 0;
  ++p;

  if (!parse_uint_field(&p, minute))
    return 0;

  if (*p == ':') {
    ++p;
    if (!parse_uint_field(&p, second))
      return 0;
  }

  if (*p == '.' || *p == ',') {
    ++p;
    if (!parse_uint_field(&p, hundredth))
      return 0;
  }

  p = skip_space((char *)p);
  if (*p != '\0')
    return 0;

  return *hour <= 23u && *minute <= 59u &&
         *second <= 59u && *hundredth <= 99u;
}

static void builtin_date(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned month;
  unsigned day;
  unsigned year;
  int n;

  if (*p == '\0') {
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
    return;
  }

  if (!parse_date_value(p, &month, &day, &year)) {
    dos_puts(cpu, command_psp, g, "Invalid date.\r\n");
    return;
  }

  CPU_CX = (UWORD)year;
  CPU_DH = (UBYTE)month;
  CPU_DL = (UBYTE)day;
  CPU_AH = 0x2b;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM DATE set");

  if (CPU_AL != 0)
    dos_puts(cpu, command_psp, g, "Invalid date.\r\n");
}

static void builtin_time(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned hour;
  unsigned minute;
  unsigned second;
  unsigned hundredth;
  int n;

  if (*p == '\0') {
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
    return;
  }

  if (!parse_time_value(p, &hour, &minute, &second, &hundredth)) {
    dos_puts(cpu, command_psp, g, "Invalid time.\r\n");
    return;
  }

  CPU_CH = (UBYTE)hour;
  CPU_CL = (UBYTE)minute;
  CPU_DH = (UBYTE)second;
  CPU_DL = (UBYTE)hundredth;
  CPU_AH = 0x2d;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM TIME set");

  if (CPU_AL != 0)
    dos_puts(cpu, command_psp, g, "Invalid time.\r\n");
}



static int parse_exit_code(char *args, int *batch_only, UWORD *code)
{
  char *p = skip_space(args);
  unsigned value = 0;
  int have_value = 0;

  *batch_only = 0;
  *code = 0;

  if (*p == '/') {
    if ((p[1] == 'B' || p[1] == 'b') &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
      *batch_only = 1;
      p = skip_space(p + 2);
    } else {
      return 0;
    }
  }

  while (isdigit((unsigned char)*p)) {
    have_value = 1;
    value = value * 10u + (unsigned)(*p - '0');
    ++p;
  }

  p = skip_space(p);
  if (*p != '\0')
    return 0;

  if (have_value)
    *code = (UWORD)value;

  return 1;
}



static void builtin_chcp(CPU *cpu, UWORD command_psp,
                         struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned codepage = 0;
  int n;

  if (*p == '\0') {
    CPU_AX = 0x6601;
    fcom_intcall(cpu, command_psp, 0x21, "FCOM CHCP get");

    if (int21_failed(cpu)) {
      dos_puts(cpu, command_psp, g, "Code page operation not supported.\r\n");
      return;
    }

    n = snprintf(g->text, sizeof(g->text),
                 "Active code page: %u\r\n", (unsigned)CPU_BX);
    if (n > 0)
      (void)fcom_write(cpu, command_psp,
          FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
          (UWORD)n);
    return;
  }

  while (isdigit((unsigned char)*p)) {
    codepage = codepage * 10u + (unsigned)(*p - '0');
    ++p;
  }

  p = skip_space(p);
  if (codepage == 0 || codepage > 0xffffu || *p != '\0') {
    dos_puts(cpu, command_psp, g, "Invalid code page.\r\n");
    return;
  }

  CPU_BX = (UWORD)codepage;
  CPU_DX = (UWORD)codepage;
  CPU_AX = 0x6602;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM CHCP set");

  if (int21_failed(cpu)) {
    dos_puts(cpu, command_psp, g, "Invalid code page.\r\n");
    return;
  }

  n = snprintf(g->text, sizeof(g->text),
               "Active code page: %u\r\n", codepage);
  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);
}

static void builtin_vol(CPU *cpu, UWORD command_psp,
                        struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  unsigned drive;
  int rc;
  int n;

  if (*p == '\0') {
    drive = dos_get_drive(cpu, command_psp);
  } else if (isalpha((unsigned char)p[0]) && p[1] == ':' &&
             skip_space(p + 2)[0] == '\0') {
    drive = (unsigned)(toupper((unsigned char)p[0]) - 'A');
  } else {
    dos_puts(cpu, command_psp, g, "Invalid drive specification.\r\n");
    return;
  }

  if (drive >= 26u) {
    dos_puts(cpu, command_psp, g, "Invalid drive specification.\r\n");
    return;
  }

  g->path2[0] = (char)('A' + drive);
  g->path2[1] = ':';
  g->path2[2] = '\\';
  g->path2[3] = '*';
  g->path2[4] = '.';
  g->path2[5] = '*';
  g->path2[6] = '\0';

  memset(&g->find, 0, sizeof(g->find));
  if (set_find_dta(cpu, command_psp) < 0)
    return;

  rc = dos_find_first_attr(cpu, command_psp, g, g->path2, 0x08);
  restore_default_dta(cpu, command_psp);

  if (rc < 0 || !(g->find.dm_attr_fnd & 0x08)) {
    n = snprintf(g->text, sizeof(g->text),
                 "Volume in drive %c has no label\r\n",
                 (int)('A' + drive));
  } else {
    n = snprintf(g->text, sizeof(g->text),
                 "Volume in drive %c is %s\r\n",
                 (int)('A' + drive), g->find.dm_name);
  }

  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);
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

static int fcom_set_file_attr(CPU *cpu, UWORD command_psp,
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

static void fcom_print_attr_line(CPU *cpu, UWORD command_psp,
                                 struct fcom_guest *g,
                                 UWORD attributes, const char *name)
{
  int n = snprintf(g->text, sizeof(g->text),
                   "%c %c %c %c     %s\r\n",
                   (attributes & 0x20) ? 'A' : ' ',
                   (attributes & 0x04) ? 'S' : ' ',
                   (attributes & 0x02) ? 'H' : ' ',
                   (attributes & 0x01) ? 'R' : ' ',
                   name);

  if (n > 0)
    (void)fcom_write(cpu, command_psp,
        FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, text),
        (UWORD)n);
}

static int fcom_build_matched_path(struct fcom_guest *g,
                                   const char *pattern,
                                   const char *matched_name)
{
  const char *slash = strrchr(pattern, '\\');
  const char *slash2 = strrchr(pattern, '/');
  const char *colon = strrchr(pattern, ':');
  const char *cut = slash;
  size_t prefix;
  size_t name_len = strlen(matched_name);

  if (slash2 != NULL && (cut == NULL || slash2 > cut))
    cut = slash2;
  if (colon != NULL && (cut == NULL || colon > cut))
    cut = colon;

  prefix = cut != NULL ? (size_t)(cut - pattern + 1) : 0;
  if (prefix + name_len >= sizeof(g->path2))
    return 0;

  if (prefix != 0)
    memcpy(g->path2, pattern, prefix);
  memcpy(g->path2 + prefix, matched_name, name_len + 1);
  return 1;
}

static int fcom_parse_attrib_switch(const char *arg,
                                    UWORD *set_mask, UWORD *clear_mask)
{
  UWORD bit;

  if ((arg[0] != '+' && arg[0] != '-') ||
      arg[1] == '\0' || arg[2] != '\0')
    return 0;

  switch (toupper((unsigned char)arg[1])) {
  case 'R':
    bit = 0x01;
    break;
  case 'H':
    bit = 0x02;
    break;
  case 'S':
    bit = 0x04;
    break;
  case 'A':
    bit = 0x20;
    break;
  default:
    return 0;
  }

  if (arg[0] == '+') {
    *set_mask |= bit;
    *clear_mask &= (UWORD)~bit;
  } else {
    *clear_mask |= bit;
    *set_mask &= (UWORD)~bit;
  }

  return 1;
}

static void builtin_attrib(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, char *args)
{
  char *cursor = args;
  char *arg;
  char *patterns[16];
  unsigned pattern_count = 0;
  UWORD set_mask = 0;
  UWORD clear_mask = 0;
  unsigned i;

  while ((arg = next_argument(&cursor)) != NULL) {
    if (arg[0] == '+' || arg[0] == '-') {
      if (!fcom_parse_attrib_switch(arg, &set_mask, &clear_mask)) {
        dos_puts(cpu, command_psp, g, "Invalid parameter.\r\n");
        return;
      }
    } else {
      if (pattern_count >= sizeof(patterns) / sizeof(patterns[0])) {
        dos_puts(cpu, command_psp, g, "Too many parameters.\r\n");
        return;
      }
      patterns[pattern_count++] = arg;
    }
  }

  if (pattern_count == 0)
    patterns[pattern_count++] = "*.*";

  for (i = 0; i < pattern_count; ++i) {
    const char *pattern = patterns[i];
    int rc;
    int found = 0;

    memset(&g->find, 0, sizeof(g->find));
    if (set_find_dta(cpu, command_psp) < 0)
      return;

    rc = dos_find_first_attr(cpu, command_psp, g, pattern, 0x37);
    while (rc == 0) {
      UWORD attributes;
      const char *name;

      if (strcmp(g->find.dm_name, ".") != 0 &&
          strcmp(g->find.dm_name, "..") != 0 &&
          fcom_build_matched_path(g, pattern, g->find.dm_name)) {
        name = g->path2;
        attributes = g->find.dm_attr_fnd;
        found = 1;

        if (set_mask != 0 || clear_mask != 0) {
          UWORD new_attributes =
              (UWORD)((attributes | set_mask) & ~clear_mask);

          if (fcom_set_file_attr(cpu, command_psp, g,
                                 name, new_attributes) < 0) {
            dos_puts(cpu, command_psp, g,
                     "Unable to change attribute - ");
            dos_puts(cpu, command_psp, g, name);
            dos_puts(cpu, command_psp, g, "\r\n");
          }
        } else {
          fcom_print_attr_line(cpu, command_psp, g, attributes, name);
        }
      }

      rc = dos_find_next(cpu, command_psp);
    }

    restore_default_dta(cpu, command_psp);

    if (!found) {
      dos_puts(cpu, command_psp, g, "File not found - ");
      dos_puts(cpu, command_psp, g, pattern);
      dos_puts(cpu, command_psp, g, "\r\n");
    }
  }
}


static int run_builtin(CPU *cpu, UWORD command_psp,
                       struct fcom_guest *g, char *args)
{
  if (is_native_command_name(g->filename)) {
    /*
     * Each nested COMMAND has its own MCB/PSP.  /C returns after one
     * command; an interactive child returns to this shell on EXIT.
     */
    fcom_run(cpu, args, 0);
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
    if (*args == '\0') {
      unsigned drive = dos_get_drive(cpu, command_psp);
      if (dos_get_cwd(cpu, command_psp, g, 0) == 0) {
        g->text[0] = (char)('A' + drive);
        g->text[1] = ':';
        g->text[2] = '\\';
        strncpy(g->text + 3, g->path, sizeof(g->text) - 6);
        g->text[sizeof(g->text) - 3] = '\0';
        strcat(g->text, "\r\n");
        dos_puts(cpu, command_psp, g, g->text);
      }
    } else if (dos_change_dir(cpu, command_psp, g, args) < 0) {
      dos_puts(cpu, command_psp, g, "Invalid directory\r\n");
    }
    return 1;
  }

  if (command_is(g->filename, "CLS")) {
    clear_screen(cpu, command_psp);
    return 1;
  }

  if (command_is(g->filename, "ECHO")) {
    if (*args == '\0') {
      dos_puts(cpu, command_psp, g,
               g->echo_enabled ? "ECHO is on.\r\n"
                               : "ECHO is off.\r\n");
    } else if (strcasecmp(args, "ON") == 0) {
      g->echo_enabled = 1;
    } else if (strcasecmp(args, "OFF") == 0) {
      g->echo_enabled = 0;
    } else {
      dos_puts(cpu, command_psp, g, args);
      dos_puts(cpu, command_psp, g, "\r\n");
    }
    return 1;
  }

  if (command_is(g->filename, "PAUSE")) {
    pause_command(cpu, command_psp, g);
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

  if (command_is(g->filename, "SET")) {
    builtin_set(cpu, command_psp, g, args);
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

  if (command_is(g->filename, "ATTRIB")) {
    builtin_attrib(cpu, command_psp, g, args);
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
    dos_puts(cpu, command_psp, g, "\r\nFreeDOS native command processor 0.86 port\r\n");
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

static int exec_once(CPU *cpu, UWORD command_psp, struct fcom_guest *g)
{
  memset(&g->exec_block, 0, sizeof(g->exec_block));
  g->exec_block.exec.env_seg = 0;                 /* inherit process-0 environment */
  g->exec_block.exec.cmd_line = MK_FP(command_psp, FCOM_WORK_OFFSET +
      (UWORD)offsetof(struct fcom_guest, tail));
  g->exec_block.exec.fcb_1 = MK_FP(0xffff, 0xffff);
  g->exec_block.exec.fcb_2 = MK_FP(0xffff, 0xffff);

  SET_DS(command_psp);
  CPU_DX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, filename);
  SET_ES(command_psp);
  CPU_BX = FCOM_WORK_OFFSET + (UWORD)offsetof(struct fcom_guest, exec_block);
  CPU_AX = 0x4b00;
  fcom_intcall(cpu, command_psp, 0x21, "FCOM EXEC");
  return int21_failed(cpu) ? -(int)CPU_AX : 0;
}

static int path_is_explicit(const char *name)
{
  return strchr(name, '\\') != NULL ||
         strchr(name, '/') != NULL ||
         strchr(name, ':') != NULL;
}

static int exec_program(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                        const char *name)
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

    rc = exec_once(cpu, command_psp, g);
    if (rc == 0)
      return 0;

    /* Only "file/path not found" permits trying another candidate. */
    if (rc != -2 && rc != -3)
      return rc;
  }
  return rc;
}

static const char *find_path_value(UWORD command_psp)
{
  const psp *p = (const psp *)ARM_PTR(MK_FP(command_psp, 0));
  const char *env;
  unsigned left = 0x8000u;

  if (p->ps_environ == 0)
    return NULL;

  env = (const char *)ARM_PTR(MK_FP(p->ps_environ, 0));
  while (left && *env) {
    size_t n = strnlen(env, left);
    if (n == left)
      break;
    if (n >= 5 && strncasecmp(env, "PATH=", 5) == 0)
      return env + 5;
    env += n + 1;
    left -= (unsigned)n + 1u;
  }
  return NULL;
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

static int exec_external(CPU *cpu, UWORD command_psp, struct fcom_guest *g,
                         const char *args)
{
  const char *path;
  int rc;

  build_tail(g, args);
  strcpy(g->program, g->filename);

  /* Explicit drive/path names are never searched through PATH. */
  if (path_is_explicit(g->program))
    return exec_program(cpu, command_psp, g, g->program);

  /* DOS shells search the current directory before PATH. */
  rc = exec_program(cpu, command_psp, g, g->program);
  if (rc == 0 || (rc != -2 && rc != -3))
    return rc;

  path = find_path_value(command_psp);
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

    /* Empty PATH components denote the current directory, already tried. */
    if (len && make_path_candidate(g->text, sizeof(g->text),
                                   dir, len, g->program)) {
      rc = exec_program(cpu, command_psp, g, g->text);
      if (rc == 0 || (rc != -2 && rc != -3))
        return rc;
    }

    if (!end)
      break;
    path = end + 1;
  }
  return rc;
}



static int execute_command_line(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, char *line);

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
  const char *path;
  int handle;

  if (path_is_explicit(name))
    return open_batch_candidate(cpu, command_psp, g, name);

  handle = open_batch_candidate(cpu, command_psp, g, name);
  if (handle >= 0 || (handle != -2 && handle != -3))
    return handle;

  path = find_path_value(command_psp);
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
      if (handle >= 0 || (handle != -2 && handle != -3))
        return handle;
    }

    if (!end)
      break;
    path = end + 1;
  }

  return -2;
}

static int read_batch_byte(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, UWORD handle)
{
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
  return g->io[0];
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

static int batch_label_matches(const char *line, const char *wanted)
{
  const char *p = line;
  const char *end;
  size_t len;

  while (*p == ' ' || *p == '\t')
    ++p;
  if (*p != ':')
    return 0;

  ++p;
  while (*p == ' ' || *p == '\t')
    ++p;

  end = p;
  while (*end != '\0' && *end != ' ' && *end != '\t')
    ++end;

  len = (size_t)(end - p);
  return len == strlen(wanted) &&
         strncasecmp(p, wanted, len) == 0;
}

static void batch_shift_args(struct fcom_guest *g)
{
  char *p = skip_space(g->batch_args);
  char *rest;

  if (*p == '\0') {
    g->batch_args[0] = '\0';
    return;
  }

  if (*p == '"') {
    ++p;
    while (*p != '\0' && *p != '"')
      ++p;
    if (*p == '"')
      ++p;
  } else {
    while (*p != '\0' && *p != ' ' && *p != '\t')
      ++p;
  }

  rest = skip_space(p);
  memmove(g->batch_args, rest, strlen(rest) + 1);
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

static void expand_batch_parameters(struct fcom_guest *g, char *line)
{
  char expanded[FCOM_LINE_MAX + 1];
  const char *srcp = line;
  char *dst = expanded;
  char *end = expanded + FCOM_LINE_MAX;

  while (*srcp != '\0' && dst < end) {
    if (*srcp == '%' && srcp[1] != '\0') {
      const char *value = NULL;
      char digit = srcp[1];

      if (digit == '0')
        value = g->batch_name;
      else if (digit >= '1' && digit <= '9') {
        const char *p = g->batch_args;
        unsigned wanted = (unsigned)(digit - '1');

        while (*p != '\0') {
          p = skip_space((char *)p);
          if (*p == '\0')
            break;

          if (wanted == 0) {
            char *a = g->batch_arg;
            char *aend = g->batch_arg + sizeof(g->batch_arg) - 1;
            int quoted = 0;

            if (*p == '"') {
              quoted = 1;
              ++p;
            }

            while (*p != '\0' && a < aend) {
              if (quoted) {
                if (*p == '"')
                  break;
              } else if (*p == ' ' || *p == '\t') {
                break;
              }
              *a++ = *p++;
            }
            *a = '\0';
            value = g->batch_arg;
            break;
          }

          if (*p == '"') {
            ++p;
            while (*p != '\0' && *p != '"')
              ++p;
            if (*p == '"')
              ++p;
          } else {
            while (*p != '\0' && *p != ' ' && *p != '\t')
              ++p;
          }
          --wanted;
        }
      } else if (digit == '%') {
        *dst++ = '%';
        srcp += 2;
        continue;
      }

      if (value != NULL) {
        while (*value != '\0' && dst < end)
          *dst++ = *value++;
        srcp += 2;
        continue;
      }
    }

    *dst++ = *srcp++;
  }

  *dst = '\0';
  strcpy(line, expanded);
}

static int execute_batch_handle(CPU *cpu, UWORD command_psp,
                                struct fcom_guest *g, UWORD handle)
{
  size_t used = 0;
  int skip_lf = 0;
  int searching = 0;

  for (;;) {
    int ch = read_batch_byte(cpu, command_psp, g, handle);

    if (ch < 0) {
      if (used != 0) {
        g->batch_line[used] = '\0';

        if (searching) {
          if (batch_label_matches(g->batch_line, g->batch_goto)) {
            g->batch_goto[0] = '\0';
            return 0;
          }
        } else {
          expand_batch_parameters(g, g->batch_line);
          if (execute_command_line(cpu, command_psp, g,
                                   g->batch_line) < 0)
            return -1;
        }
      }

      if (searching) {
        g->batch_goto[0] = '\0';
        dos_puts(cpu, command_psp, g, "Label not found\r\n");
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
      g->batch_line[used] = '\0';
      if (ch == '\r')
        skip_lf = 1;

      if (searching) {
        if (batch_label_matches(g->batch_line, g->batch_goto)) {
          searching = 0;
          g->batch_goto[0] = '\0';
        }
      } else {
        char *line = skip_space(g->batch_line);

        /* Labels are declarations, not executable command lines. */
        if (*line != ':') {
          expand_batch_parameters(g, g->batch_line);
          if (execute_command_line(cpu, command_psp, g,
                                   g->batch_line) < 0)
            return -1;
        }

        if (g->batch_goto[0] != '\0') {
          int rc = seek_batch_start(cpu, command_psp, handle);

          if (rc < 0) {
            g->batch_goto[0] = '\0';
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
      g->batch_line[used++] = (char)ch;
  }
}

static int execute_batch_file(CPU *cpu, UWORD command_psp,
                              struct fcom_guest *g, const char *name,
                              const char *args)
{
  int handle = find_batch_file(cpu, command_psp, g, name);
  int rc;
  char saved_name[sizeof(g->batch_name)];
  char saved_args[sizeof(g->batch_args)];
  char saved_goto[sizeof(g->batch_goto)];
  UBYTE saved_active;
  UBYTE saved_exit_batch_only;

  if (handle < 0)
    return handle;

  memcpy(saved_name, g->batch_name, sizeof(saved_name));
  memcpy(saved_args, g->batch_args, sizeof(saved_args));
  memcpy(saved_goto, g->batch_goto, sizeof(saved_goto));
  saved_active = g->batch_active;
  saved_exit_batch_only = g->exit_batch_only;

  strncpy(g->batch_name, name, sizeof(g->batch_name) - 1);
  g->batch_name[sizeof(g->batch_name) - 1] = '\0';
  strncpy(g->batch_args, args ? args : "", sizeof(g->batch_args) - 1);
  g->batch_args[sizeof(g->batch_args) - 1] = '\0';
  g->batch_goto[0] = '\0';
  g->batch_active = 1;
  g->exit_batch_only = 0;

  rc = execute_batch_handle(cpu, command_psp, g, (UWORD)handle);
  fcom_close(cpu, command_psp, (UWORD)handle);

  if (g->exit_batch_only)
    rc = 0;

  memcpy(g->batch_name, saved_name, sizeof(g->batch_name));
  memcpy(g->batch_args, saved_args, sizeof(g->batch_args));
  memcpy(g->batch_goto, saved_goto, sizeof(g->batch_goto));
  g->batch_active = saved_active;
  g->exit_batch_only = saved_exit_batch_only;
  return rc;
}

static int execute_default_autoexec(CPU *cpu, UWORD command_psp,
                                    struct fcom_guest *g)
{
  unsigned drive = dos_get_drive(cpu, command_psp);
  int rc;

  g->autoexec_path[0] = (char)('A' + drive);
  g->autoexec_path[1] = ':';
  g->autoexec_path[2] = '\\';

  memcpy(g->autoexec_path + 3, "FDAUTO.BAT", 11);
  rc = execute_batch_file(cpu, command_psp, g,
                          g->autoexec_path, "");
  if (rc != -2 && rc != -3)
    return rc;

  memcpy(g->autoexec_path + 3, "AUTOEXEC.BAT", 13);
  return execute_batch_file(cpu, command_psp, g,
                            g->autoexec_path, "");
}


enum fcom_start_action {
  FCOM_START_NONE = 0,
  FCOM_START_KEEP,
  FCOM_START_EXIT
};


static char *parse_if_operand(char *p, char *dst, size_t dst_size)
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
    } else if (*p == '=' || *p == ' ' || *p == '\t') {
      break;
    }
    *d++ = *p++;
  }

  *d = '\0';
  return p;
}

static int dos_file_exists(CPU *cpu, UWORD command_psp,
                           struct fcom_guest *g, const char *name)
{
  int handle = dos_open_read(cpu, command_psp, g, name);

  if (handle < 0)
    return 0;
  fcom_close(cpu, command_psp, (UWORD)handle);
  return 1;
}

static int execute_if(CPU *cpu, UWORD command_psp,
                      struct fcom_guest *g, char *args)
{
  char *p = skip_space(args);
  char *command;
  int negate = 0;
  int condition = 0;

  if (strncasecmp(p, "NOT", 3) == 0 &&
      (p[3] == '\0' || p[3] == ' ' || p[3] == '\t')) {
    negate = 1;
    p = skip_space(p + 3);
  }

  if (strncasecmp(p, "ERRORLEVEL", 10) == 0 &&
      (p[10] == '\0' || p[10] == ' ' || p[10] == '\t')) {
    unsigned wanted = 0;

    p = skip_space(p + 10);
    if (!isdigit((unsigned char)*p)) {
      dos_puts(cpu, command_psp, g, "Invalid IF syntax\r\n");
      return 0;
    }

    while (isdigit((unsigned char)*p)) {
      wanted = wanted * 10u + (unsigned)(*p - '0');
      ++p;
    }

    condition = (DosGetRetCode() & 0xffu) >= wanted;
    command = skip_space(p);
  } else if (strncasecmp(p, "EXIST", 5) == 0 &&
             (p[5] == '\0' || p[5] == ' ' || p[5] == '\t')) {
    p = skip_space(p + 5);
    p = parse_if_operand(p, g->if_left, sizeof(g->if_left));
    condition = dos_file_exists(cpu, command_psp, g, g->if_left);
    command = skip_space(p);
  } else {
    p = parse_if_operand(p, g->if_left, sizeof(g->if_left));
    p = skip_space(p);

    if (*p != '=') {
      dos_puts(cpu, command_psp, g, "Invalid IF syntax\r\n");
      return 0;
    }

    while (*p == '=')
      ++p;

    p = parse_if_operand(p, g->if_right, sizeof(g->if_right));
    condition = strcmp(g->if_left, g->if_right) == 0;
    command = skip_space(p);
  }

  if (negate)
    condition = !condition;

  if (condition && *command != '\0')
    return execute_command_line(cpu, command_psp, g, command);

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
    if (execute_for_value(cpu, command_psp, g, variable,
                          g->find.dm_name, command) < 0) {
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

  if (*p != '%' || p[1] == '\0') {
    dos_puts(cpu, command_psp, g, "Invalid FOR syntax\r\n");
    return 0;
  }

  variable = p[1];
  p = skip_space(p + 2);

  if (strncasecmp(p, "IN", 2) != 0 ||
      (p[2] != ' ' && p[2] != '\t' && p[2] != '(')) {
    dos_puts(cpu, command_psp, g, "Invalid FOR syntax\r\n");
    return 0;
  }

  p = skip_space(p + 2);
  if (*p != '(') {
    dos_puts(cpu, command_psp, g, "Invalid FOR syntax\r\n");
    return 0;
  }

  list_start = ++p;
  list_end = strchr(list_start, ')');
  if (list_end == NULL) {
    dos_puts(cpu, command_psp, g, "Invalid FOR syntax\r\n");
    return 0;
  }

  *list_end = '\0';
  p = skip_space(list_end + 1);

  if (strncasecmp(p, "DO", 2) != 0 ||
      (p[2] != '\0' && p[2] != ' ' && p[2] != '\t')) {
    dos_puts(cpu, command_psp, g, "Invalid FOR syntax\r\n");
    return 0;
  }

  command = skip_space(p + 2);
  if (*command == '\0') {
    dos_puts(cpu, command_psp, g, "Invalid FOR syntax\r\n");
    return 0;
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

static int execute_command_core(CPU *cpu, UWORD command_psp,
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
   * FreeCOM's INCLUDE_CMD_FAKELOADHIGH maps LH/LOADHIGH to cmd_call:
   * consume the prefix and execute the remaining command normally.
   */
  if (command_is(g->filename, "LH") ||
      command_is(g->filename, "LOADHIGH")) {
    args = skip_space(args);
    if (*args == '\0') {
      error_bad_command(cpu, command_psp, g, g->filename);
      return 0;
    }
    return execute_command_line(cpu, command_psp, g, args);
  }

  if (g->batch_active && command_is(g->filename, "GOTO")) {
    args = skip_space(args);

    if (strcasecmp(args, ":EOF") == 0 ||
        strcasecmp(args, "EOF") == 0) {
      g->exit_batch_only = 1;
      return -1;
    }

    if (*args == ':')
      ++args;
    args = skip_space(args);

    if (*args == '\0') {
      dos_puts(cpu, command_psp, g, "Required parameter missing\r\n");
    } else {
      strncpy(g->batch_goto, args, sizeof(g->batch_goto) - 1);
      g->batch_goto[sizeof(g->batch_goto) - 1] = '\0';
    }
    return 0;
  }

  if (g->batch_active && command_is(g->filename, "SHIFT")) {
    batch_shift_args(g);
    return 0;
  }

  if (g->batch_active && command_is(g->filename, "CALL")) {
    char *target;
    char *call_args;
    int call_rc;

    split_call_target(args, &target, &call_args);
    if (*target == '\0') {
      dos_puts(cpu, command_psp, g, "Required parameter missing\r\n");
      return 0;
    }

    call_rc = execute_batch_file(cpu, command_psp, g, target, call_args);
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

  rc = exec_external(cpu, command_psp, g, args);
  if (rc == -2 || rc == -3) {
    int batch_rc = execute_batch_file(cpu, command_psp, g, g->filename, args);

    if (batch_rc == 0)
      return 0;
    if (batch_rc == -1)
      return -1;
  }

  if (rc < 0)
    error_bad_command(cpu, command_psp, g, g->program);
  return 0;
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
  int append_output;
  int saved_stdin;
  int saved_stdout;
  int pipe_state;
  int rc;

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

  rc = apply_redirections(cpu, command_psp, g, append_output,
                          &saved_stdin, &saved_stdout);
  if (rc < 0) {
    restore_redirections(cpu, command_psp,
                         saved_stdin, saved_stdout);
    dos_puts(cpu, command_psp, g, "Redirection failed\r\n");
    return 0;
  }

  rc = execute_command_core(cpu, command_psp, g,
                            g->redirect_command);
  restore_redirections(cpu, command_psp,
                       saved_stdin, saved_stdout);
  return rc;
}


static char *skip_option_argument(char *p)
{
  if (*p == ':' || *p == '=')
    ++p;
  while (*p != '\0' && *p != ' ' && *p != '\t')
    ++p;
  return p;
}

static enum fcom_start_action parse_init_tail(struct fcom_guest *g,
                                               char **command)
{
  char *p = skip_space(g->init_tail);
  enum fcom_start_action action = FCOM_START_NONE;

  *command = NULL;

  while (*p != '\0') {
    char option;

    if (*p != '/' && *p != '-')
      break;

    ++p;
    option = (char)toupper((unsigned char)*p);
    if (*p != '\0')
      ++p;

    switch (option) {
    case 'P':
      g->persistent = 1;
      if (*p == ':' || *p == '=') {
        char *start;
        size_t n;

        ++p;
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
          ++p;
        n = (size_t)(p - start);
        if (n >= sizeof(g->autoexec_path))
          n = sizeof(g->autoexec_path) - 1;
        memcpy(g->autoexec_path, start, n);
        g->autoexec_path[n] = '\0';
      } else {
        p = skip_option_argument(p);
      }
      break;

    case 'D':
      g->skip_autoexec = 1;
      p = skip_option_argument(p);
      break;

    case 'C':
    case 'K':
      action = option == 'C' ? FCOM_START_EXIT : FCOM_START_KEEP;
      if (*p == '=' || *p == ':')
        ++p;
      p = skip_space(p);
      *command = p;
      return action;

    /*
     * These switches are recognized by FreeCOM but their storage-related
     * effects are unnecessary for the native shell. Consume their value
     * so they cannot be mistaken for a command.
     */
    case 'E':
    case 'L':
    case 'U':
      p = skip_option_argument(p);
      break;

    /*
     * /Y, /F and /! are boolean FreeCOM startup switches.  Their effects
     * are not implemented yet, but the switch itself is consumed.
     */
    case 'Y':
    case 'F':
    case '!':
      p = skip_option_argument(p);
      break;

    default:
      p = skip_option_argument(p);
      break;
    }

    p = skip_space(p);
  }

  return action;
}


static UWORD create_command_process(const char *init_tail, UBYTE start_mode,
                                    UWORD parent_psp)
{
  seg mcb_seg;
  UWORD largest = 0;
  UWORD command_psp;
  mcb *block;
  psp *process;
  size_t n = init_tail ? strlen(init_tail) : 0;

  {
    UBYTE old_umb_link = LoL->uppermem_link;
    COUNT alloc_mode = (start_mode & FIRST_FIT_U) ? FIRST_FIT_U : FIRST_FIT;
    COUNT rc;

    if (alloc_mode == FIRST_FIT_U)
      DosUmbLink(1);
    rc = DosMemAlloc(FCOM_PROCESS_PARAS, alloc_mode, &mcb_seg, &largest);
    if (alloc_mode == FIRST_FIT_U)
      DosUmbLink(old_umb_link);
    if (rc < SUCCESS)
      return 0;
  }

  command_psp = mcb_seg + 1;
  block = (mcb *)ARM_PTR(MK_FP(mcb_seg, 0));
  block->m_psp = command_psp;
  memcpy(block->m_name, "COMMAND ", sizeof(block->m_name));

  child_psp(command_psp, parent_psp, command_psp + FCOM_PROCESS_PARAS);
  process = (psp *)ARM_PTR(MK_FP(command_psp, 0));
  process->ps_parent = parent_psp;
  process->ps_prevpsp = MK_FP(parent_psp, 0);
  process->ps_environ =
      ((psp *)ARM_PTR(MK_FP(parent_psp, 0)))->ps_environ;

  if (n > sizeof(process->ps_cmd.ctBuffer) - 1)
    n = sizeof(process->ps_cmd.ctBuffer) - 1;
  process->ps_cmd.ctCount = (UBYTE)n;
  if (n)
    memcpy(process->ps_cmd.ctBuffer, init_tail, n);
  process->ps_cmd.ctBuffer[n] = '\r';

  internal_data->cu_psp = command_psp;
  internal_data->dta = MK_FP(command_psp, offsetof(psp, ps_cmd));
  return command_psp;
}

void fcom_run(CPU *cpu, const char *init_tail, UBYTE start_mode)
{
  UWORD parent_psp = internal_data->cu_psp;
  dos_far_ptr parent_dta = internal_data->dta;
  UWORD command_psp =
      create_command_process(init_tail, start_mode, parent_psp);
  struct fcom_guest *g;
  enum fcom_start_action start_action;
  char *start_command = NULL;

  if (command_psp == 0) {
    dos_printf("FCOM: cannot allocate COMMAND process\n");
    return;
  }

  g = (struct fcom_guest *)ARM_PTR(MK_FP(command_psp, FCOM_WORK_OFFSET));
  memset(g, 0, sizeof(*g));
  g->echo_enabled = 1;
  init_stack_guard(command_psp);

  dos_printf("FCOM: PSP=%04x parent=%04x data=%04x..%04x "
             "stack=%04x..ffff (%u bytes)\n",
             command_psp,
             parent_psp,
             FCOM_WORK_OFFSET,
             (unsigned)(FCOM_DATA_END - 1u),
             (unsigned)FCOM_STACK_BOTTOM,
             (unsigned)FCOM_STACK_BYTES);

  if (init_tail) {
    strncpy(g->init_tail, init_tail, sizeof(g->init_tail) - 1);
    g->init_tail[sizeof(g->init_tail) - 1] = '\0';
  }

  start_action = parse_init_tail(g, &start_command);

  if (g->persistent && !g->skip_autoexec &&
      start_action == FCOM_START_NONE) {
    if (g->autoexec_path[0] != '\0')
      (void)execute_batch_file(cpu, command_psp, g,
                               g->autoexec_path, "");
    else
      (void)execute_default_autoexec(cpu, command_psp, g);
  }

  if (start_command && *start_command) {
    int rc = execute_command_line(cpu, command_psp, g, start_command);

    if (rc < 0 && !g->persistent)
      goto done;

    if (start_action == FCOM_START_EXIT && !g->persistent)
      goto done;
  }

  for (;;) {
    int rc;

    ensure_prompt_column_zero(cpu, command_psp, g);
    show_prompt(cpu, command_psp, g);
    if (fcom_readline(cpu, command_psp, g) == 0)
      continue;

    rc = execute_command_line(cpu, command_psp, g, g->input.kb_buf);
    if (rc < 0)
      break;
  }

done:
  /*
   * TODO(kernel): publish g->exit_code through the same DOS return-code
   * path used when an ordinary guest process terminates.
   */

  /*
   * Restore the parent before releasing the child process block.  A nested
   * COMMAND therefore behaves like a normal synchronous DOS child.
   */
  internal_data->cu_psp = parent_psp;
  internal_data->dta = parent_dta;

  if (g->owned_env_seg != 0)
    DosMemFree(g->owned_env_seg - 1);

  DosMemFree(command_psp - 1);
}
