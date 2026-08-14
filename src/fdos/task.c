/****************************************************************/
/*                                                              */
/*                           task.c                             */
/*                                                              */
/*                 Task Manager for DOS Processes               */
/*                                                              */
/*  Ported from upstream FreeDOS kernel/task.c. Algorithms      */
/*  (ChildEnv/child_psp/patchPSP/DosComLoader/DosExeLoader/      */
/*  ExecMemAlloc/ExecMemLargest) are largely unchanged; what's   */
/*  entirely new is exec_run_child() below, replacing upstream's */
/*  exec_user()/return_user()/user_r real-hardware task-switch   */
/*  primitives (a fixed register-frame on DOS's own internal     */
/*  stack, restored via a manual IRETD-equivalent) with this     */
/*  port's own mechanism: a nested C call plus a blocking        */
/*  pc_step() loop, matching the pattern already used by         */
/*  cpu_far_call() (kernel.c) for calling into loaded device      */
/*  drivers - see exec_run_child()'s own comment for the full    */
/*  rationale.                                                  */
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include <ctype.h>
#include "fcom/fcom.h"
#include "../diag.h"

#if DIAG
extern volatile uint32_t dos_diag_kernel_code;
#endif
/*
 * fdos/hdr/portab.h already publishes the guest PSRAM base as
 * PSRAM_BASE_ADDR.  board_config.h publishes the same address through the
 * board-level PSRAM_BASE macro, but does so with another unconditional
 * definition.  Drop the API-side spelling before importing board_config.h;
 * this TU needs its PSRAM_SIZE_BYTES configuration as well.
 */
#ifdef PSRAM_BASE_ADDR
#undef PSRAM_BASE_ADDR
#endif
#include "../board_config.h"

/*
 * Keep this dependency narrow.  Including ../pc.h here pulls host stdio
 * declarations into the FreeDOS kernel translation unit, where hdrs.h maps
 * printf() to dos_printf(); that changes the visible prototype and conflicts
 * with the kernel's own void dos_printf(...).
 *
 * These are the same small cross-module declarations already used by other
 * emulator modules which need the current PC/device service or microsecond
 * clock without importing the full pc.h include graph.
 */
extern struct PC *pc;
void pc_step(struct PC *pc, int stepcount);
void pc_service(struct PC *pc);
uint32_t get_uticks(void);

/* Native-yield IRQ trampoline uses the same callback trap mechanism as
   bios_intcall(), but deliberately does not execute the suspended parent
   CS:IP. */
bool set_bios_callback(CPU *cpu, bios_callback_params_t *params, bool reenter);
bool drop_bios_callback(CPU *cpu, bios_callback_params_t *params);
bool cpu_pending_trap(void);
void cpu_pending_trap_set(bool v);

#define ExeHeader (*(exe_header *)(SecPathName + 0))
#define TempExeBlock (*(exec_blk *)(SecPathName + sizeof(exe_header)))
#define Shell (SecPathName + sizeof(exe_header) + sizeof(exec_blk))

/* Scratch pair of UWORDs used only while reading EXE relocation table
   entries (DosExeLoader()). Reuses the same SecPathName bytes as
   Shell: safe, because by the time DosExeLoader() runs, "lp" (which
   may itself have been Shell, e.g. for the running shell re-EXEC'ing
   itself) has already been fully consumed by DosOpenSft() at the very
   start of DosExec(), and Shell isn't touched again until well after
   DosExec() returns (see P_0() below) - same "recycle SecPathBuffer"
   approach already used for ExeHeader/TempExeBlock. */
#define RelocBuf ((UWORD *)(SecPathName + sizeof(exe_header) + sizeof(exec_blk)))

#define DEVLOAD_CHUNK_PARAS (32256 / 16)       /* also used by EXEC_OVERLAY loads */
#define CHUNK           32256                  /* bytes per DosExeLoader() read */
#define MAXENV          32768u
#define ENV_KEEPFREE    0x83   /* sizeof(PriPathBuffer)+3: 2 bytes "extra
                                   strings" count, 0x80 bytes max absolute
                                   filename, 1 byte '\0' - see ChildEnv() */

_Static_assert(sizeof(((struct dos_data *) 0)->PriPathBuffer) + 3 == ENV_KEEPFREE,
               "ENV_KEEPFREE must track sizeof(PriPathBuffer)+3, see ChildEnv()");

#define LOAD_HIGH 0x80          /* mode bit: try UMB first (see DosUmbLink()) */


/* Minimal ARM ELF32 loader support.  ET_REL sections are loaded on demand,
   starting from the Murmulator application entry symbols.  Section placement
   and load state are kept in the program's own DOS memory block rather than
   consuming persistent native SRAM. */
#define ELF32_MAGIC             0x464c457fu
#define ELFCLASS32              1u
#define ELFDATA2LSB             1u
#define EV_CURRENT              1u
#define ET_REL                  1u
#define EM_ARM                  40u
#define EF_ARM_ABI_FLOAT_HARD   0x00000400u
#define SHN_UNDEF               0u
#define SHN_ABS                 0xfff1u
#define SHT_PROGBITS            1u
#define SHT_SYMTAB              2u
#define SHT_STRTAB              3u
#define SHT_NOBITS              8u
#define SHT_REL                 9u
#define SHT_INIT_ARRAY          14u
#define SHT_FINI_ARRAY          15u
#define SHT_PREINIT_ARRAY       16u
#define SHF_ALLOC               0x2u
#define STB_GLOBAL              1u
#define STB_WEAK                2u
#define STT_FUNC                2u
#define R_ARM_ABS32             2u
#define R_ARM_REL32             3u
#define R_ARM_THM_PC22          10u
#define R_ARM_THM_JUMP24        30u
#define R_ARM_THM_ALU_ABS_G0_NC 102u
#define M_API_VERSION              9
#define ARM_ELF_DEFAULT_NATIVE_STACK_SIZE 4096u
#define ARM_ELF_DEFAULT_DOS_STACK_SIZE    256u
#define ARM_ELF_ARGV_SLOTS        66u
#define ARM_ELF_ARG_TEXT_SIZE     (NAMEMAX + sizeof(((CommandTail *)0)->ctBuffer) + 2u)
#define ARM_ELF_ARG_AREA_SIZE     (ARM_ELF_ARGV_SLOTS * sizeof(ULONG) + ARM_ELF_ARG_TEXT_SIZE)
#define ARM_ELF_SEC_UNSEEN      0u
#define ARM_ELF_SEC_LOADING     1u
#define ARM_ELF_SEC_LOADED      2u
#define ARM_ELF_NO_SYMBOL       0xfffffffful

#define ARM_ELF_STARTUP_NONE     0u
#define ARM_ELF_STARTUP_PREINIT  1u
#define ARM_ELF_STARTUP_INIT     2u
#define ARM_ELF_STARTUP_FINI     3u

/* Sparse progress: enough activity to see that a large ET_REL still moves. */
#define ARM_ELF_PROGRESS_RELOCS  32u
#define ARM_ELF_LONG_JOB_US        2000000ul

#pragma pack(push, 1)
typedef struct arm_elf32_ehdr {
  ULONG magic;
  UBYTE elf_class;
  UBYTE data;
  UBYTE ident_version;
  UBYTE abi;
  UBYTE abi_version;
  UBYTE pad[7];
  UWORD type;
  UWORD machine;
  ULONG version;
  ULONG entry;
  ULONG phoff;
  ULONG shoff;
  ULONG flags;
  UWORD ehsize;
  UWORD phentsize;
  UWORD phnum;
  UWORD shentsize;
  UWORD shnum;
  UWORD shstrndx;
} arm_elf32_ehdr;

typedef struct arm_elf32_shdr {
  ULONG name;
  ULONG type;
  ULONG flags;
  ULONG addr;
  ULONG offset;
  ULONG size;
  ULONG link;
  ULONG info;
  ULONG addralign;
  ULONG entsize;
} arm_elf32_shdr;

typedef struct arm_elf32_sym {
  ULONG name;
  ULONG value;
  ULONG size;
  UBYTE info;
  UBYTE other;
  UWORD shndx;
} arm_elf32_sym;

typedef struct arm_elf32_rel {
  ULONG offset;
  ULONG info;
} arm_elf32_rel;

typedef struct arm_elf_sec_state {
  ULONG offset;
  ULONG size;
  UBYTE state;
  UBYTE startup_kind;
  UBYTE startup_plain;
  UBYTE reserved;
} arm_elf_sec_state;

typedef struct arm_elf_load_meta {
  arm_elf32_ehdr eh;
  arm_elf32_shdr symtab;
  arm_elf32_shdr strtab;
  arm_elf32_shdr shstrtab;
  ULONG cursor;
  ULONG allocation_end;
  ULONG required_api_addr;
  ULONG requirements_addr;
  ULONG init_addr;
  ULONG main_addr;
  ULONG fini_addr;
  ULONG signal_addr;
  ULONG preinit_array_addr;
  ULONG preinit_array_count;
  ULONG init_array_addr;
  ULONG init_array_count;
  ULONG fini_array_addr;
  ULONG fini_array_count;
  ULONG relocation_progress;
  ULONG loader_started_us;
  UBYTE is_long_running_job;
  UBYTE loader_progress_reserved[3];
  ULONG argv_addr;
  ULONG native_stack_addr;
  ULONG native_main_sp;
  ULONG native_stack_size;
  ULONG dos_stack_size;
  UWORD dos_stack_mcb;
  UWORD dos_stack_seg;
  LONG required_api_version;
  UWORD argc;
  UWORD shnum;
  UWORD symtab_index;
} arm_elf_load_meta;
#pragma pack(pop)

/* Metadata reads must still pass through DosRWSft(), whose destination is a
   guest far pointer.  RelocBuf is existing synchronous task.c scratch space
   and is large enough for the largest ELF record above (52-byte Ehdr). */
#define ElfScratch ((BYTE *)RelocBuf)

static int arm_elf_read_meta(COUNT fd, ULONG file_off, void *dst, UWORD len)
{
  LONG pos = SftSeek(fd, (LONG)file_off, SEEK_SET);
  LONG got;

  if (pos < 0)
    return DE_INVLDFMT;
  got = DosRWSft(fd, len,
                 x86_FAR_PTR(DOS_PSP, ElfScratch), XFR_READ);
  if (got != len)
    return DE_INVLDFMT;
  memcpy(dst, ElfScratch, len);
  return SUCCESS;
}

static dos_far_ptr arm_elf_guest_ptr(UWORD base_seg, ULONG off)
{
  return MK_FP((UWORD)(base_seg + (UWORD)(off >> 4)), (UWORD)(off & 0x0fu));
}

static ULONG arm_elf_align_up(ULONG v, ULONG a)
{
  ULONG rem, add;

  if (a <= 1)
    return v;

  rem = v % a;
  if (rem == 0)
    return v;
  add = a - rem;
  if (v > 0xfffffffful - add)
    return 0xfffffffful;
  return v + add;
}

static arm_elf_sec_state *arm_elf_states(arm_elf_load_meta *meta)
{
  return (arm_elf_sec_state *)(meta + 1);
}

static ULONG arm_elf_metadata_size(UWORD shnum)
{
  ULONG table_size = (ULONG)shnum * (ULONG)sizeof(arm_elf_sec_state);
  ULONG total = (ULONG)sizeof(arm_elf_load_meta);

  if (shnum != 0 && table_size / sizeof(arm_elf_sec_state) != shnum)
    return 0xfffffffful;
  if (total > 0xfffffffful - table_size)
    return 0xfffffffful;
  return arm_elf_align_up(total + table_size, 4u);
}

static int arm_elf_read_shdr(COUNT fd, const arm_elf32_ehdr *eh,
                             UWORD sec_num, arm_elf32_shdr *sh)
{
  if (sec_num >= eh->shnum)
    return DE_INVLDFMT;
  return arm_elf_read_meta(fd,
      eh->shoff + (ULONG)sec_num * eh->shentsize, sh, sizeof(*sh));
}

/*
 * The ELF loader owns the largest available DOS block for the duration of
 * loading and shrinks it to the final image size only after relocation.
 * Therefore section placement never needs to modify the MCB chain.  This is
 * both simpler and avoids repeated DosMemChange() calls while recursively
 * discovering sections.
 */
static int arm_elf_ensure_capacity(const arm_elf_load_meta *meta, ULONG end_off)
{
  if (end_off == 0)
    return SUCCESS;
  if (end_off > 0xffff0ul || end_off > meta->allocation_end)
    return DE_NOMEM;
  return SUCCESS;
}

static int arm_elf_read_section(COUNT fd, UWORD base_seg, ULONG dst_off,
                                ULONG file_off, ULONG size)
{
  ULONG done = 0;
  LONG pos = SftSeek(fd, (LONG)file_off, SEEK_SET);

  if (pos < 0)
    return DE_INVLDFMT;

  while (done < size) {
    UWORD chunk = (UWORD)min((ULONG)CHUNK, size - done);
    LONG got = DosRWSft(fd, chunk,
                        arm_elf_guest_ptr(base_seg, dst_off + done), XFR_READ);
    if (got != chunk)
      return DE_INVLDFMT;
    done += chunk;
  }
  return SUCCESS;
}

static void arm_elf_resolve_thm_pc22(UWORD *addr, UWORD *addr_ref,
                                     ULONG sym_val)
{
  UWORD instr0 = addr[0], instr1 = addr[1];
  ULONG s = (instr0 >> 10) & 1u;
  ULONG j1 = (instr1 >> 13) & 1u;
  ULONG j2 = (instr1 >> 11) & 1u;
  ULONG imm10 = instr0 & 0x03ffu;
  ULONG imm11 = instr1 & 0x07ffu;
  ULONG i1 = (~(j1 ^ s)) & 1u;
  ULONG i2 = (~(j2 ^ s)) & 1u;
  LONG offset = (LONG)((i1 << 23) | (i2 << 22) |
                       (imm10 << 12) | (imm11 << 1));
  ULONG new_offset;

  if (s)
    offset |= (LONG)0xff800000ul;
  new_offset = (ULONG)(offset + (LONG)sym_val - (LONG)(uintptr_t)addr_ref);
  s = new_offset >> 31;
  i1 = (new_offset >> 23) & 1u;
  i2 = (new_offset >> 22) & 1u;
  imm10 = (new_offset >> 12) & 0x03ffu;
  imm11 = (new_offset >> 1) & 0x07ffu;
  j1 = (~(i1 ^ s)) & 1u;
  j2 = (~(i2 ^ s)) & 1u;
  addr[0] = (UWORD)(0xf000u | (s << 10) | imm10);
  addr[1] = (UWORD)((0x1au << 11) | (j1 << 13) | (j2 << 11) | imm11);
}

static int arm_elf_resolve_thm_jump24(UWORD *addr, UWORD *addr_ref,
                                      ULONG sym_val)
{
  UWORD instr0 = addr[0], instr1 = addr[1];
  ULONG s = (instr0 >> 10) & 1u;
  ULONG j1 = (instr1 >> 13) & 1u;
  ULONG j2 = (instr1 >> 11) & 1u;
  ULONG imm10 = instr0 & 0x03ffu;
  ULONG imm11 = instr1 & 0x07ffu;
  ULONG i1 = (~(j1 ^ s)) & 1u;
  ULONG i2 = (~(j2 ^ s)) & 1u;
  LONG addend = (LONG)((s << 24) | (i1 << 23) | (i2 << 22) |
                       (imm10 << 12) | (imm11 << 1));
  LONG rel;
  ULONG urel;

  if (s)
    addend |= (LONG)0xff000000ul;
  rel = addend + (LONG)(sym_val & ~1ul) -
        ((LONG)(uintptr_t)addr_ref + 4);
  if (rel < -(1L << 24) || rel > ((1L << 24) - 2))
    return DE_INVLDFMT;

  urel = (ULONG)rel;
  s = (urel >> 24) & 1u;
  i1 = (urel >> 23) & 1u;
  i2 = (urel >> 22) & 1u;
  imm10 = (urel >> 12) & 0x03ffu;
  imm11 = (urel >> 1) & 0x07ffu;
  j1 = (~(i1 ^ s)) & 1u;
  j2 = (~(i2 ^ s)) & 1u;
  addr[0] = (UWORD)(0xf000u | (s << 10) | imm10);
  addr[1] = (UWORD)((0x1cu << 11) | (j1 << 13) | (j2 << 11) | imm11);
  return SUCCESS;
}

static void arm_elf_resolve_thm_alu_abs_g0_nc(UWORD *addr, ULONG value)
{
  UWORD instr0 = addr[0], instr1 = addr[1];
  ULONG imm4 = instr0 & 0x000fu;
  ULONG ibit = (instr0 >> 10) & 1u;
  ULONG imm3 = (instr1 >> 12) & 7u;
  ULONG imm8 = instr1 & 0x00ffu;
  ULONG addend = (imm4 << 12) | (ibit << 11) | (imm3 << 8) | imm8;
  ULONG imm16 = (value + addend) & 0xffffu;

  instr0 = (UWORD)((instr0 & ~0x040fu) |
                   ((imm16 >> 12) & 0x0fu) |
                   (((imm16 >> 11) & 1u) << 10));
  instr1 = (UWORD)((instr1 & ~0x70ffu) |
                   (((imm16 >> 8) & 7u) << 12) |
                   (imm16 & 0xffu));
  addr[0] = instr0;
  addr[1] = instr1;
}

static COUNT arm_elf_reject(COUNT rc, const char *reason)
{
  dos_printf("ARM ELF: %s\r\n", reason);
  return rc;
}

static int arm_elf_find_tables(COUNT fd, arm_elf_load_meta *meta)
{
  UWORD i;
  int rc;

  if (meta->eh.shstrndx == SHN_UNDEF || meta->eh.shstrndx >= meta->eh.shnum)
    return DE_INVLDFMT;
  rc = arm_elf_read_shdr(fd, &meta->eh, meta->eh.shstrndx, &meta->shstrtab);
  if (rc != SUCCESS || meta->shstrtab.type != SHT_STRTAB)
    return DE_INVLDFMT;

  for (i = 0; i < meta->eh.shnum; ++i) {
    arm_elf32_shdr sh;
    rc = arm_elf_read_shdr(fd, &meta->eh, i, &sh);
    if (rc != SUCCESS)
      return rc;
    if (sh.type != SHT_SYMTAB)
      continue;
    if (sh.link >= meta->eh.shnum)
      return DE_INVLDFMT;
    rc = arm_elf_read_shdr(fd, &meta->eh, (UWORD)sh.link, &meta->strtab);
    if (rc != SUCCESS || meta->strtab.type != SHT_STRTAB)
      return DE_INVLDFMT;
    meta->symtab = sh;
    meta->symtab_index = i;
    return SUCCESS;
  }

  return DE_INVLDFMT;
}

static int arm_elf_read_symbol(COUNT fd, const arm_elf_load_meta *meta,
                               ULONG sym_index, arm_elf32_sym *sym)
{
  ULONG entsize = meta->symtab.entsize ? meta->symtab.entsize : sizeof(*sym);

  if (entsize < sizeof(*sym) || sym_index >= meta->symtab.size / entsize)
    return DE_INVLDFMT;
  return arm_elf_read_meta(fd, meta->symtab.offset + sym_index * entsize,
                           sym, sizeof(*sym));
}

/* Bounded .strtab lookup used only by diagnostics. */
static int arm_elf_symbol_name(COUNT fd, const arm_elf_load_meta *meta,
                               const arm_elf32_sym *sym,
                               BYTE *buf, UWORD buf_size)
{
  ULONG remain;
  UWORD read_len;
  BYTE *nul;

  if (!buf || buf_size < 2 || sym->name >= meta->strtab.size)
    return FALSE;

  remain = meta->strtab.size - sym->name;
  read_len = remain < (ULONG)(buf_size - 1)
             ? (UWORD)remain : (UWORD)(buf_size - 1);
  if (read_len == 0)
    return FALSE;
  if (arm_elf_read_meta(fd, meta->strtab.offset + sym->name,
                        buf, read_len) != SUCCESS)
    return FALSE;

  nul = memchr(buf, '\0', read_len);
  if (nul) {
    if (nul == buf)
      return FALSE;
  } else {
    buf[read_len] = '\0';
  }
  return TRUE;
}

static int arm_elf_section_name(COUNT fd, const arm_elf_load_meta *meta,
                                const arm_elf32_shdr *sh,
                                BYTE *buf, UWORD buf_size)
{
  ULONG remain;
  UWORD read_len;
  BYTE *nul;

  if (buf == NULL || buf_size < 2 || sh->name >= meta->shstrtab.size)
    return FALSE;

  remain = meta->shstrtab.size - sh->name;
  read_len = remain < (ULONG)(buf_size - 1)
             ? (UWORD)remain : (UWORD)(buf_size - 1);
  if (read_len == 0)
    return FALSE;

  if (arm_elf_read_meta(fd, meta->shstrtab.offset + sh->name,
                        buf, read_len) != SUCCESS)
    return FALSE;

  nul = memchr(buf, '\0', read_len);
  if (nul == NULL)
    buf[read_len] = '\0';
  else if (nul == buf)
    return FALSE;

  return TRUE;
}

static UBYTE arm_elf_startup_kind(const BYTE *name, ULONG type,
                                  UBYTE *is_plain)
{
  const char *base;
  UBYTE kind;
  size_t n;

  if (type == SHT_PREINIT_ARRAY) {
    base = ".preinit_array";
    kind = ARM_ELF_STARTUP_PREINIT;
  } else if (type == SHT_INIT_ARRAY) {
    base = ".init_array";
    kind = ARM_ELF_STARTUP_INIT;
  } else if (type == SHT_FINI_ARRAY) {
    base = ".fini_array";
    kind = ARM_ELF_STARTUP_FINI;
  } else {
    return ARM_ELF_STARTUP_NONE;
  }

  n = strlen(base);
  if (strcmp((const char *)name, base) == 0) {
    *is_plain = TRUE;
    return kind;
  }
  if (strncmp((const char *)name, base, n) == 0 && name[n] == '.') {
    *is_plain = FALSE;
    return kind;
  }
  return ARM_ELF_STARTUP_NONE;
}

static int arm_elf_load_section(COUNT fd, UWORD base_seg,
                                arm_elf_load_meta *meta, UWORD sec_num,
                                ULONG *sec_addr);

/*
 * The final Pico/GNU linker makes preinit/init/fini arrays roots even when no
 * ordinary code references them.  ET_REL has the input sections separately,
 * so discover and relocate those sections explicitly.
 */
static int arm_elf_discover_startup_sections(COUNT fd, UWORD base_seg,
                                             arm_elf_load_meta *meta)
{
  arm_elf_sec_state *states = arm_elf_states(meta);
  UWORD i;

  for (i = 0; i < meta->eh.shnum; ++i) {
    arm_elf32_shdr sh;
    BYTE name[64];
    UBYTE plain = FALSE;
    UBYTE kind;
    ULONG sec_addr;
    int rc = arm_elf_read_shdr(fd, &meta->eh, i, &sh);

    if (rc != SUCCESS)
      return rc;
    if ((sh.flags & SHF_ALLOC) == 0)
      continue;
    if (sh.type != SHT_PREINIT_ARRAY &&
        sh.type != SHT_INIT_ARRAY &&
        sh.type != SHT_FINI_ARRAY)
      continue;
    if (!arm_elf_section_name(fd, meta, &sh, name, sizeof(name)))
      return DE_INVLDFMT;

    kind = arm_elf_startup_kind(name, sh.type, &plain);
    if (kind == ARM_ELF_STARTUP_NONE)
      continue;
    if ((sh.size & 3u) != 0)
      return DE_INVLDFMT;

    states[i].startup_kind = kind;
    states[i].startup_plain = plain;
    states[i].size = sh.size;

    rc = arm_elf_load_section(fd, base_seg, meta, i, &sec_addr);
    if (rc != SUCCESS)
      return rc;
  }

  return SUCCESS;
}

/*
 * Compare two startup-array sections using the same lexical ordering as
 * SORT(.xxx_array.*).  Section number is the stable tie-breaker.
 */
static int arm_elf_startup_section_less(COUNT fd,
                                        const arm_elf_load_meta *meta,
                                        UWORD lhs, UWORD rhs)
{
  arm_elf32_shdr lsh, rsh;
  BYTE lname[64], rname[64];
  int cmp;

  if (rhs == 0xffffu)
    return TRUE;
  if (arm_elf_read_shdr(fd, &meta->eh, lhs, &lsh) != SUCCESS ||
      arm_elf_read_shdr(fd, &meta->eh, rhs, &rsh) != SUCCESS)
    return lhs < rhs;
  if (!arm_elf_section_name(fd, meta, &lsh, lname, sizeof(lname)) ||
      !arm_elf_section_name(fd, meta, &rsh, rname, sizeof(rname)))
    return lhs < rhs;

  cmp = strcmp((const char *)lname, (const char *)rname);
  return cmp < 0 || (cmp == 0 && lhs < rhs);
}

static int arm_elf_startup_section_after(COUNT fd,
                                         const arm_elf_load_meta *meta,
                                         UWORD candidate, UWORD previous)
{
  if (previous == 0xffffu)
    return TRUE;
  return arm_elf_startup_section_less(fd, meta, previous, candidate);
}

static int arm_elf_copy_startup_kind(COUNT fd, UWORD base_seg,
                                     arm_elf_load_meta *meta, UBYTE kind,
                                     ULONG dst_off, ULONG *entry_count)
{
  arm_elf_sec_state *states = arm_elf_states(meta);
  ULONG count = 0;
  UBYTE plain_pass;

  /*
   * GNU/Pico scripts use SORT(.array.*), followed by the plain .array.
   * Reproduce that exact two-phase ordering without keeping native-SRAM lists.
   */
  for (plain_pass = FALSE; plain_pass <= TRUE; ++plain_pass) {
    UWORD previous = 0xffffu;

    for (;;) {
      UWORD best = 0xffffu;
      UWORD i;

      for (i = 0; i < meta->eh.shnum; ++i) {
        if (states[i].startup_kind != kind ||
            states[i].startup_plain != plain_pass ||
            !arm_elf_startup_section_after(fd, meta, i, previous))
          continue;
        if (arm_elf_startup_section_less(fd, meta, i, best))
          best = i;
      }

      if (best == 0xffffu)
        break;

      if (states[best].size != 0) {
        BYTE *src = (BYTE *)ARM_PTR(
            arm_elf_guest_ptr(base_seg, states[best].offset));
        BYTE *dst = (BYTE *)ARM_PTR(
            arm_elf_guest_ptr(base_seg, dst_off + count * sizeof(ULONG)));
        memcpy(dst, src, states[best].size);
        count += states[best].size / sizeof(ULONG);
      }
      previous = best;
    }
  }

  *entry_count = count;
  return SUCCESS;
}

/*
 * Synthesize the contiguous __preinit_array/__init_array/__fini_array ranges
 * that a normal final linker script would create.
 */
static int arm_elf_build_startup_arrays(COUNT fd, UWORD base_seg,
                                        arm_elf_load_meta *meta)
{
  arm_elf_sec_state *states = arm_elf_states(meta);
  ULONG pre_count = 0, init_count = 0, fini_count = 0;
  ULONG total_entries, bytes, off;
  UWORD i;
  int rc;

  rc = arm_elf_discover_startup_sections(fd, base_seg, meta);
  if (rc != SUCCESS)
    return rc;

  for (i = 0; i < meta->eh.shnum; ++i) {
    ULONG n = states[i].size / sizeof(ULONG);
    if (states[i].startup_kind == ARM_ELF_STARTUP_PREINIT)
      pre_count += n;
    else if (states[i].startup_kind == ARM_ELF_STARTUP_INIT)
      init_count += n;
    else if (states[i].startup_kind == ARM_ELF_STARTUP_FINI)
      fini_count += n;
  }

  total_entries = pre_count + init_count + fini_count;
  if (total_entries == 0)
    return SUCCESS;
  if (total_entries > 0xfffffffful / sizeof(ULONG))
    return DE_NOMEM;

  bytes = total_entries * sizeof(ULONG);
  off = arm_elf_align_up(meta->cursor, sizeof(ULONG));
  if (off == 0xfffffffful || off > 0xfffffffful - bytes)
    return DE_NOMEM;
  rc = arm_elf_ensure_capacity(meta, off + bytes);
  if (rc != SUCCESS)
    return rc;

  meta->preinit_array_addr = (ULONG)(uintptr_t)ARM_PTR(
      arm_elf_guest_ptr(base_seg, off));
  rc = arm_elf_copy_startup_kind(fd, base_seg, meta,
                                 ARM_ELF_STARTUP_PREINIT, off,
                                 &meta->preinit_array_count);
  if (rc != SUCCESS)
    return rc;
  off += meta->preinit_array_count * sizeof(ULONG);

  meta->init_array_addr = (ULONG)(uintptr_t)ARM_PTR(
      arm_elf_guest_ptr(base_seg, off));
  rc = arm_elf_copy_startup_kind(fd, base_seg, meta,
                                 ARM_ELF_STARTUP_INIT, off,
                                 &meta->init_array_count);
  if (rc != SUCCESS)
    return rc;
  off += meta->init_array_count * sizeof(ULONG);

  meta->fini_array_addr = (ULONG)(uintptr_t)ARM_PTR(
      arm_elf_guest_ptr(base_seg, off));
  rc = arm_elf_copy_startup_kind(fd, base_seg, meta,
                                 ARM_ELF_STARTUP_FINI, off,
                                 &meta->fini_array_count);
  if (rc != SUCCESS)
    return rc;
  off += meta->fini_array_count * sizeof(ULONG);

  meta->cursor = off;
  return SUCCESS;
}


static int arm_elf_symbol_addr(COUNT fd, UWORD base_seg,
                               arm_elf_load_meta *meta, ULONG sym_index,
                               ULONG *sym_addr)
{
  arm_elf32_sym sym;
  arm_elf32_shdr sec;
  ULONG sec_addr;
  int rc = arm_elf_read_symbol(fd, meta, sym_index, &sym);

  if (rc != SUCCESS)
    return rc;
  if (sym.shndx == SHN_UNDEF) {
    BYTE name[64];
    if (arm_elf_symbol_name(fd, meta, &sym, name, sizeof(name)))
      dos_printf("ARM ELF: undefined symbol: %s\r\n", name);
    else
      dos_printf("ARM ELF: undefined symbol #%lu\r\n", sym_index);
    return DE_INVLDFMT;
  }
  if (sym.shndx == SHN_ABS) {
    *sym_addr = sym.value;
    return SUCCESS;
  }
  if (sym.shndx >= meta->eh.shnum) {
    BYTE name[64];
    if (arm_elf_symbol_name(fd, meta, &sym, name, sizeof(name)))
      dos_printf("ARM ELF: unsupported section index %u for symbol %s\r\n",
                 (unsigned)sym.shndx, name);
    else
      dos_printf("ARM ELF: unsupported section index %u for symbol #%lu\r\n",
                 (unsigned)sym.shndx, sym_index);
    return DE_INVLDFMT;
  }

  rc = arm_elf_read_shdr(fd, &meta->eh, sym.shndx, &sec);
  if (rc != SUCCESS || sym.value > sec.size)
    return DE_INVLDFMT;
  rc = arm_elf_load_section(fd, base_seg, meta, sym.shndx, &sec_addr);
  if (rc != SUCCESS)
    return rc;
  *sym_addr = sec_addr + sym.value;
  return SUCCESS;
}

/*
 * Compact loader progress is also a cooperative cancellation/service point.
 *
 * pc_service() lets host keyboard/device state advance while the native ELF
 * loader owns core0.  ndread() peeks the DOS console without blocking; when
 * Ctrl+C is pending, consume exactly that character and abort the EXEC load.
 *
 * Do not route this through handle_break()/INT 23h: the child process has not
 * started yet, so invoking the current process's INT 23h would incorrectly
 * deliver the break to the parent which is synchronously waiting in EXEC.
 */
static int arm_elf_loader_progress(arm_elf_load_meta *meta,
                                   const char *marker)
{
  int c;

  /*
   * Service devices and poll Ctrl+C from the first loader iteration.  Only
   * visual progress is delayed, so fast native programs do not print a line
   * of dots at all.
   */
  pc_service(pc);

  if (!meta->is_long_running_job) {
    ULONG elapsed = get_uticks() - meta->loader_started_us;
    if (elapsed >= ARM_ELF_LONG_JOB_US)
      meta->is_long_running_job = TRUE;
  }

  if (meta->is_long_running_job)
    dos_printf("%s", marker);

  c = ndread(&LoL->syscon);
  if (c != CTL_C)
    return FALSE;

  (void)read_char_stdin(FALSE);
  dos_printf("^C\r\n");
  return TRUE;
}

static int arm_elf_apply_section_relocations(COUNT fd, UWORD base_seg,
                                             arm_elf_load_meta *meta,
                                             UWORD sec_num,
                                             const arm_elf32_shdr *target,
                                             ULONG target_off)
{
  UWORD i;

  for (i = 0; i < meta->eh.shnum; ++i) {
    arm_elf32_shdr relsec, symtab;
    ULONG j, rel_entsize;
    int rc = arm_elf_read_shdr(fd, &meta->eh, i, &relsec);
    if (rc != SUCCESS)
      return rc;
    if (relsec.type != SHT_REL || relsec.info != sec_num)
      continue;
    if (relsec.link >= meta->eh.shnum ||
        relsec.link != meta->symtab_index)
      return DE_INVLDFMT;
    rc = arm_elf_read_shdr(fd, &meta->eh, (UWORD)relsec.link, &symtab);
    if (rc != SUCCESS || symtab.type != SHT_SYMTAB)
      return DE_INVLDFMT;

    rel_entsize = relsec.entsize ? relsec.entsize : sizeof(arm_elf32_rel);
    if (rel_entsize < sizeof(arm_elf32_rel))
      return DE_INVLDFMT;

    for (j = 0; j < relsec.size / rel_entsize; ++j) {
      arm_elf32_rel rel;
      ULONG sym_addr;
      ULONG sym_index;
      UBYTE type;
      BYTE *place;
      ULONG paddr;

      rc = arm_elf_read_meta(fd, relsec.offset + j * rel_entsize,
                             &rel, sizeof(rel));
      if (rc != SUCCESS)
        return rc;
      if (rel.offset > target->size || target->size - rel.offset < 4)
        return DE_INVLDFMT;

      sym_index = rel.info >> 8;
      type = (UBYTE)(rel.info & 0xffu);
      rc = arm_elf_symbol_addr(fd, base_seg, meta, sym_index, &sym_addr);
      if (rc != SUCCESS)
        return rc;

      place = (BYTE *)ARM_PTR(arm_elf_guest_ptr(base_seg,
                                                target_off + rel.offset));
      paddr = (ULONG)(uintptr_t)place;
      /*
       * Relocations are the useful fine-grained loader progress signal.
       * Do not print every record: thousands of DOS console writes would make
       * loading slower than the work being measured.
       */
      if (++meta->relocation_progress >= ARM_ELF_PROGRESS_RELOCS) {
        meta->relocation_progress = 0;
        if (arm_elf_loader_progress(meta, "."))
          return DE_ACCESS;
      }

      switch (type) {
        case R_ARM_ABS32:
          *(ULONG *)place += sym_addr;
          break;
        case R_ARM_REL32:
          *(ULONG *)place = sym_addr + *(ULONG *)place - paddr;
          break;
        case R_ARM_THM_PC22:
          arm_elf_resolve_thm_pc22((UWORD *)place, (UWORD *)place, sym_addr);
          break;
        case R_ARM_THM_JUMP24:
          rc = arm_elf_resolve_thm_jump24((UWORD *)place,
                                          (UWORD *)place, sym_addr);
          if (rc != SUCCESS) {
            dos_printf("ARM ELF: Thumb JUMP24 relocation out of range "
                       "(section %u, offset %lu)\r\n",
                       (unsigned)sec_num, rel.offset);
            return rc;
          }
          break;
        case R_ARM_THM_ALU_ABS_G0_NC:
          if ((uintptr_t)place & 1u)
            return DE_INVLDFMT;
          arm_elf_resolve_thm_alu_abs_g0_nc((UWORD *)place, sym_addr);
          break;
        default:
        {
          arm_elf32_sym diag_sym;
          BYTE name[64];

          if (arm_elf_read_symbol(fd, meta, sym_index, &diag_sym) == SUCCESS &&
              arm_elf_symbol_name(fd, meta, &diag_sym, name, sizeof(name)))
            dos_printf("ARM ELF: unsupported relocation type %u "
                       "(section %u, offset %lu, symbol %s)\r\n",
                       (unsigned)type, (unsigned)sec_num,
                       rel.offset, name);
          else
            dos_printf("ARM ELF: unsupported relocation type %u "
                       "(section %u, offset %lu, symbol #%lu)\r\n",
                       (unsigned)type, (unsigned)sec_num,
                       rel.offset, sym_index);
          return DE_INVLDFMT;
        }
      }
    }
  }
  return SUCCESS;
}

static int arm_elf_load_section(COUNT fd, UWORD base_seg,
                                arm_elf_load_meta *meta, UWORD sec_num,
                                ULONG *sec_addr)
{
  arm_elf_sec_state *states = arm_elf_states(meta);
  arm_elf_sec_state *state;
  arm_elf32_shdr sh;
  ULONG off;
  int rc;

  if (sec_num >= meta->shnum)
    return DE_INVLDFMT;
  state = &states[sec_num];
  if (state->state == ARM_ELF_SEC_LOADING ||
      state->state == ARM_ELF_SEC_LOADED) {
    *sec_addr = (ULONG)(uintptr_t)ARM_PTR(
        arm_elf_guest_ptr(base_seg, state->offset));
    return SUCCESS;
  }

  rc = arm_elf_read_shdr(fd, &meta->eh, sec_num, &sh);
  if (rc != SUCCESS)
    return rc;

  off = arm_elf_align_up(meta->cursor, sh.addralign);
  if (off == 0xfffffffful || off > 0xfffffffful - sh.size)
    return DE_INVLDFMT;

  rc = arm_elf_ensure_capacity(meta, off + sh.size);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot grow guest block for section %u to %lu bytes\r\n",
               (unsigned)sec_num, off + sh.size);
    return rc;
  }

  state->offset = off;
  state->size = sh.size;
  state->state = ARM_ELF_SEC_LOADING;
  meta->cursor = off + sh.size;

  if (sh.size != 0) {
    if (sh.type == SHT_NOBITS) {
      memset(ARM_PTR(arm_elf_guest_ptr(base_seg, off)), 0, sh.size);
    } else {
      rc = arm_elf_read_section(fd, base_seg, off, sh.offset, sh.size);
      if (rc != SUCCESS) {
        dos_printf("ARM ELF: cannot read section %u (%lu bytes at file offset %lu)\r\n",
                   (unsigned)sec_num, sh.size, sh.offset);
        return rc;
      }
    }
  }

  *sec_addr = (ULONG)(uintptr_t)ARM_PTR(arm_elf_guest_ptr(base_seg, off));
  rc = arm_elf_apply_section_relocations(fd, base_seg, meta,
                                         sec_num, &sh, off);
  if (rc != SUCCESS)
    return rc;
  state->state = ARM_ELF_SEC_LOADED;

  /* 's' marks one completed newly loaded/relocated section. */
  if (arm_elf_loader_progress(meta, "s"))
    return DE_ACCESS;
  return SUCCESS;
}

static int arm_elf_find_roots(COUNT fd, arm_elf_load_meta *meta,
                              ULONG *req_idx, ULONG *requirements_idx,
                              ULONG *init_idx, ULONG *main_idx,
                              ULONG *fini_idx, ULONG *sig_idx)
{
  ULONG count;
  ULONG i;
  ULONG weak_init = ARM_ELF_NO_SYMBOL;
  ULONG weak_fini = ARM_ELF_NO_SYMBOL;
  ULONG entsize = meta->symtab.entsize ? meta->symtab.entsize
                                        : sizeof(arm_elf32_sym);

  if (entsize < sizeof(arm_elf32_sym))
    return DE_INVLDFMT;
  count = meta->symtab.size / entsize;
  *req_idx = *requirements_idx = *init_idx = *main_idx = *fini_idx =
      *sig_idx = ARM_ELF_NO_SYMBOL;

  for (i = 0; i < count; ++i) {
    arm_elf32_sym sym;
    BYTE name[64];
    UBYTE bind, type;
    int rc = arm_elf_read_symbol(fd, meta, i, &sym);

    if (rc != SUCCESS)
      return rc;

    /*
     * Root discovery used to be pathologically I/O-heavy: after reading one
     * symbol entry it performed up to six independent seeks into .strtab,
     * one for every candidate root name.  Large GCC ET_REL files contain
     * thousands of symbols, so startup spent most of its time seeking rather
     * than relocating.
     *
     * Read a candidate's name once and compare it locally against every root.
     */
    if ((i & 63u) == 63u && arm_elf_loader_progress(meta, "."))
      return DE_ACCESS;

    bind = sym.info >> 4;
    type = sym.info & 0x0fu;
    if (type != STT_FUNC || (bind != STB_GLOBAL && bind != STB_WEAK))
      continue;
    if (!arm_elf_symbol_name(fd, meta, &sym, name, sizeof(name)))
      continue;

    if (strcmp((const char *)name, "_init") == 0) {
      if (bind == STB_GLOBAL)
        *init_idx = i;
      else
        weak_init = i;
    } else if (strcmp((const char *)name,
                      "__required_dos_api_verion") == 0) {
      if (bind == STB_GLOBAL)
        *req_idx = i;
    } else if (strcmp((const char *)name,
                      "__native_dos_process_requirements") == 0) {
      if (bind == STB_GLOBAL)
        *requirements_idx = i;
    } else if (strcmp((const char *)name, "_fini") == 0) {
      if (bind == STB_GLOBAL)
        *fini_idx = i;
      else
        weak_fini = i;
    } else if (strcmp((const char *)name, "main") == 0) {
      if (bind == STB_GLOBAL)
        *main_idx = i;
    } else if (strcmp((const char *)name, "signal") == 0) {
      if (bind == STB_GLOBAL)
        *sig_idx = i;
    }
  }

  if (*init_idx == ARM_ELF_NO_SYMBOL)
    *init_idx = weak_init;
  if (*fini_idx == ARM_ELF_NO_SYMBOL)
    *fini_idx = weak_fini;
  return SUCCESS;
}

static int arm_elf_load_root(COUNT fd, UWORD base_seg,
                             arm_elf_load_meta *meta, ULONG sym_index,
                             ULONG *address)
{
  if (sym_index == ARM_ELF_NO_SYMBOL) {
    *address = 0;
    return SUCCESS;
  }
  return arm_elf_symbol_addr(fd, base_seg, meta, sym_index, address);
}

STATIC int ExecMemLargest(UWORD *asize, UWORD threshold);
STATIC int ExecMemAlloc(UWORD size, seg * para, UWORD * asize);
STATIC COUNT ChildEnv(exec_blk *exp, UWORD *pChildEnvSeg, char *pathname);
STATIC UWORD patchPSP(UWORD pspseg, UWORD envseg, exec_blk *exb, BYTE *fnam);
static dos_far_ptr exec_caller_return_addr(void);
static COUNT exec_run_arm_elf(UWORD child_psp_seg,
                              arm_elf_load_meta *meta);

typedef int (*arm_elf_req_ver_fn)(void);

#define ARM_ELF_PROCESS_REQUIREMENTS_V1_SIZE 12u
#define ARM_ELF_PROCESS_REQUIREMENTS_V2_SIZE 20u
#define ARM_ELF_PROCESS_REQUIREMENTS_V3_SIZE 28u

/*
 * Keep native application stacks out of DOS memory without making the rest of
 * PSRAM a kernel heap.  The fixed arena is excluded from every application's
 * published PSRAM range, so nested native EXEC can safely consume it LIFO
 * without colliding with a parent's application allocations.
 */
#define ARM_ELF_NATIVE_STACK_ARENA_SIZE (256u * 1024u)
#define ARM_ELF_APP_PSRAM_BEGIN_OFFSET  0x00110000ul

typedef struct __attribute__((aligned(4))) arm_elf_process_requirements {
  ULONG struct_size __attribute__((aligned(4)));
  ULONG native_stack_size __attribute__((aligned(4)));
  ULONG dos_stack_size __attribute__((aligned(4)));
  ULONG assigned_native_stack_size __attribute__((aligned(4)));
  ULONG assigned_dos_stack_size __attribute__((aligned(4)));
  ULONG app_psram_begin __attribute__((aligned(4)));
  ULONG app_psram_end __attribute__((aligned(4)));
} arm_elf_process_requirements;

_Static_assert(sizeof(ULONG) == 4, "ARM ELF process ABI requires 32-bit ULONG");
_Static_assert(__alignof__(arm_elf_process_requirements) == 4,
               "ARM ELF process requirements alignment");
_Static_assert(sizeof(arm_elf_process_requirements) ==
               ARM_ELF_PROCESS_REQUIREMENTS_V3_SIZE,
               "ARM ELF process requirements layout");

typedef arm_elf_process_requirements *
    (*arm_elf_requirements_fn)(void);

/*
 * Native stack arena.
 *
 * PSRAM_SIZE_BYTES already excludes the EMS backing store when LTEMS is
 * enabled, so this arena is carved from the top of the application-visible
 * PSRAM portion, not from EMS.  Every application receives an app_psram_end
 * below the entire arena.  Stack lifetimes follow synchronous nested EXEC, so
 * a LIFO allocator is sufficient and cannot fragment.
 */
static uintptr_t arm_elf_native_stack_cursor;


/*
 * Runtime stack bounds for the currently executing native process.
 * Nested native EXEC saves/restores these around the child.
 */
static uintptr_t doom_diag_native_stack_bottom;
static uintptr_t doom_diag_native_stack_top;
static UWORD doom_diag_dos_stack_seg;
static UWORD doom_diag_dos_stack_size;

#define DOOM_NATIVE_STACK_GUARD 32u
#define DOOM_DOS_STACK_GUARD    64u
#define DOOM_STACK_CANARY       0xa5u

/*
 * Cheap stack-boundary check used by the existing diagnostic checkpoints.
 * No DOS/BIOS calls are made here.
 *
 * KRN codes on failure:
 *   70SSFFFF  native stack free bytes FFFF too small/out of range
 *   71SSOOVV  native bottom canary offset OO changed to VV
 *   72SSFFFF  DOS stack SP FFFF too small/out of range
 *   73SSOOVV  DOS bottom canary offset OO changed to VV
 */
void doom_stack_guard_check(unsigned stage)
{
  extern volatile uint32_t dos_diag_kernel_code;
  uintptr_t sp;
  unsigned i;

  if (doom_diag_native_stack_bottom == 0 || doom_diag_native_stack_top == 0)
    return;

  __asm volatile ("mov %0, sp" : "=r" (sp));

  if (sp < doom_diag_native_stack_bottom ||
      sp > doom_diag_native_stack_top ||
      sp - doom_diag_native_stack_bottom < 256u)
  {
    unsigned free_bytes =
        sp >= doom_diag_native_stack_bottom
        ? (unsigned)(sp - doom_diag_native_stack_bottom) : 0u;
    dos_diag_kernel_code =
        0x70000000u | ((stage & 0xffu) << 16) | (free_bytes & 0xffffu);
    for (;;)
      __asm volatile ("nop");
  }

  for (i = 0; i < DOOM_NATIVE_STACK_GUARD; ++i)
  {
    uint8_t v = ((volatile uint8_t *)doom_diag_native_stack_bottom)[i];
    if (v != DOOM_STACK_CANARY)
    {
      dos_diag_kernel_code =
          0x71000000u | ((stage & 0xffu) << 16)
          | ((i & 0xffu) << 8) | v;
      for (;;)
        __asm volatile ("nop");
    }
  }

  if (doom_diag_dos_stack_seg != 0)
  {
    volatile uint8_t *base =
        (volatile uint8_t *)ARM_PTR(MK_FP(doom_diag_dos_stack_seg, 0));

    for (i = 0; i < DOOM_DOS_STACK_GUARD; ++i)
    {
      uint8_t v = base[i];
      if (v != DOOM_STACK_CANARY)
      {
        dos_diag_kernel_code =
            0x73000000u | ((stage & 0xffu) << 16)
            | ((i & 0xffu) << 8) | v;
        for (;;)
          __asm volatile ("nop");
      }
    }

    if (CPU_SS == doom_diag_dos_stack_seg &&
        (CPU_SP > doom_diag_dos_stack_size || CPU_SP < 256u))
    {
      dos_diag_kernel_code =
          0x72000000u | ((stage & 0xffu) << 16) | ((unsigned)CPU_SP & 0xffffu);
      for (;;)
        __asm volatile ("nop");
    }
  }
}

static uintptr_t arm_elf_native_stack_arena_end(void)
{
  return (uintptr_t)PSRAM_BASE_ADDR + (uintptr_t)PSRAM_SIZE_BYTES;
}

static uintptr_t arm_elf_native_stack_arena_begin(void)
{
  return arm_elf_native_stack_arena_end() - ARM_ELF_NATIVE_STACK_ARENA_SIZE;
}

static int arm_elf_native_stack_acquire(ULONG size, uintptr_t *bottom,
                                        uintptr_t *previous_cursor)
{
  uintptr_t arena_begin = arm_elf_native_stack_arena_begin();
  uintptr_t cursor = arm_elf_native_stack_cursor;
  uintptr_t next;

  if (cursor == 0)
    cursor = arm_elf_native_stack_arena_end();
  if (size == 0 || size > cursor - arena_begin)
    return DE_NOMEM;

  next = (cursor - size) & ~(uintptr_t)7u;
  if (next < arena_begin)
    return DE_NOMEM;

  *previous_cursor = cursor;
  *bottom = next;
  arm_elf_native_stack_cursor = next;
  memset((void *)next, 0, size);
  if (size >= DOOM_NATIVE_STACK_GUARD)
    memset((void *)next, DOOM_STACK_CANARY, DOOM_NATIVE_STACK_GUARD);
  return SUCCESS;
}

static void arm_elf_native_stack_release(uintptr_t bottom,
                                         uintptr_t previous_cursor)
{
  /* Native EXEC is synchronous; stack reservations must unwind in LIFO order. */
  if (arm_elf_native_stack_cursor == bottom)
    arm_elf_native_stack_cursor = previous_cursor;
}

/*
 * Allocate the guest DOS stack as its own DOS-owned block.  Prefer UMB when
 * available, then fall back to conventional memory.  FreeProcessMem() will
 * later release it because the MCB owner is changed to the child PSP.
 */
static int arm_elf_alloc_dos_stack(arm_elf_load_meta *meta, UWORD child_psp)
{
  UWORD paras = (UWORD)((meta->dos_stack_size + 15u) >> 4);
  UWORD mcb_seg = 0;
  UWORD largest = 0;
  UBYTE old_umb_link = LoL->uppermem_link;
  int rc;

  if (paras == 0)
    return DE_NOMEM;

  DosUmbLink(1);
  rc = DosMemAlloc(paras, FIRST_FIT_U, &mcb_seg, &largest);
  DosUmbLink(old_umb_link);

  if (rc != SUCCESS)
    rc = DosMemAlloc(paras, FIRST_FIT, &mcb_seg, &largest);
  if (rc != SUCCESS)
    return rc;

  meta->dos_stack_mcb = mcb_seg;
  meta->dos_stack_seg = mcb_seg + 1;
  memset(ARM_PTR(MK_FP(meta->dos_stack_seg, 0)), 0, meta->dos_stack_size);
  if (meta->dos_stack_size >= DOOM_DOS_STACK_GUARD)
    memset(ARM_PTR(MK_FP(meta->dos_stack_seg, 0)),
           DOOM_STACK_CANARY, DOOM_DOS_STACK_GUARD);

  {
    mcb *block = (mcb *)ARM_PTR(MK_FP(mcb_seg, 0));
    block->m_psp = child_psp;
    memcpy(block->m_name, "ARMSTK  ", sizeof(block->m_name));
  }

  return SUCCESS;
}

/*
 * Run the startup ABI preflight on the kernel stack, before either application
 * stack exists.  Version negotiation remains the first application call.  An
 * optional requirements hook then selects the native ARM and guest DOS stack
 * independently; absent/zero fields retain the historical defaults.
 */
static int arm_elf_preflight(arm_elf_load_meta *meta)
{
  arm_elf_process_requirements *requirements = NULL;
  ULONG native_size = ARM_ELF_DEFAULT_NATIVE_STACK_SIZE;
  ULONG dos_size = ARM_ELF_DEFAULT_DOS_STACK_SIZE;

  meta->required_api_version = M_API_VERSION;
  if (meta->required_api_addr != 0)
    meta->required_api_version =
        ((arm_elf_req_ver_fn)(uintptr_t)meta->required_api_addr)();
  if (meta->required_api_version > M_API_VERSION) {
    dos_printf("ARM ELF: application requires DOS-API version %ld; provided %u\r\n",
               meta->required_api_version, (unsigned)M_API_VERSION);
    return DE_INVLDFMT;
  }

  if (meta->requirements_addr != 0) {
    requirements = ((arm_elf_requirements_fn)(uintptr_t)
        meta->requirements_addr)();
    if (requirements != NULL) {
      if (requirements->struct_size < ARM_ELF_PROCESS_REQUIREMENTS_V1_SIZE) {
        dos_printf("ARM ELF: process requirements structure is too small (%lu)\r\n",
                   requirements->struct_size);
        return DE_INVLDFMT;
      }
      if (requirements->native_stack_size != 0)
        native_size = requirements->native_stack_size;
      if (requirements->dos_stack_size != 0)
        dos_size = requirements->dos_stack_size;
    }
  }

  native_size = arm_elf_align_up(native_size, 8u);
  dos_size = arm_elf_align_up(dos_size, 16u);
  if (native_size == 0xfffffffful || dos_size == 0xfffffffful ||
      native_size == 0 || dos_size == 0) {
    dos_printf("ARM ELF: invalid process stack requirements\r\n");
    return DE_INVLDFMT;
  }

  meta->native_stack_size = native_size;
  meta->dos_stack_size = dos_size;
  return SUCCESS;
}

/*
 * Publish loader-selected values only after _init().
 *
 * Some native runtimes restore the application's statically initialized
 * .data image in _init().  Writing these output fields during preflight would
 * therefore be lost before main().  Re-enter the already loaded requirements
 * hook after _init and write only ABI-v2 fields.
 */
static void arm_elf_publish_process_requirements(arm_elf_load_meta *meta)
{
  arm_elf_process_requirements *requirements;

  if (meta->requirements_addr == 0)
    return;

  requirements = ((arm_elf_requirements_fn)(uintptr_t)
      meta->requirements_addr)();
  if (requirements == NULL ||
      requirements->struct_size < ARM_ELF_PROCESS_REQUIREMENTS_V2_SIZE)
    return;

  requirements->assigned_native_stack_size = meta->native_stack_size;
  requirements->assigned_dos_stack_size = meta->dos_stack_size;

  if (requirements->struct_size >= ARM_ELF_PROCESS_REQUIREMENTS_V3_SIZE) {
    requirements->app_psram_begin =
        (ULONG)((uintptr_t)PSRAM_BASE_ADDR + ARM_ELF_APP_PSRAM_BEGIN_OFFSET);
    requirements->app_psram_end = (ULONG)arm_elf_native_stack_arena_begin();
  }

  /*
   * Temporary visible cross-check: DOOM prints the same object later.  If the
   * values differ, the problem is no longer stack selection but object/image
   * identity or a later overwrite.
   */
}

static int arm_elf_build_argv(UWORD base_seg, arm_elf_load_meta *meta,
                              exec_blk *exp, const BYTE *namep)
{
  ULONG argv_off = arm_elf_align_up(meta->cursor, 4u);
  ULONG text_off;
  ULONG argv_end;
  ULONG *argv;
  BYTE *text;
  ULONG text_used = 0;
  ULONG i;
  UWORD argc = 0;
  const CommandTail *tail = NULL;
  UWORD tail_len = 0;

  if (argv_off == 0xfffffffful ||
      argv_off > 0xfffffffful - ARM_ELF_ARG_AREA_SIZE)
    return DE_NOMEM;
  argv_end = argv_off + ARM_ELF_ARG_AREA_SIZE;
  if (arm_elf_ensure_capacity(meta, argv_end) != SUCCESS)
    return DE_NOMEM;
  text_off = argv_off + ARM_ELF_ARGV_SLOTS * sizeof(ULONG);
  argv = (ULONG *)ARM_PTR(arm_elf_guest_ptr(base_seg, argv_off));
  text = (BYTE *)ARM_PTR(arm_elf_guest_ptr(base_seg, text_off));
  memset(argv, 0, ARM_ELF_ARGV_SLOTS * sizeof(ULONG));
  memset(text, 0, ARM_ELF_ARG_TEXT_SIZE);

  if (namep != NULL && *namep != '\0') {
    ULONG start = text_used;
    while (namep[text_used] != '\0' && text_used + 1 < NAMEMAX) {
      text[text_used] = namep[text_used];
      ++text_used;
    }
    text[text_used++] = '\0';
    argv[argc++] = (ULONG)(uintptr_t)(text + start);
  }

  if (!far_is_null(exp->exec.cmd_line) && !far_is_end(exp->exec.cmd_line)) {
    tail = (const CommandTail *)ARM_PTR(exp->exec.cmd_line);
    tail_len = tail->ctCount;
    if (tail_len > sizeof(tail->ctBuffer))
      tail_len = sizeof(tail->ctBuffer);
  }

  i = 0;
  while (i < tail_len) {
    ULONG start;
    bool quoted = false;

    while (i < tail_len && (tail->ctBuffer[i] == ' ' ||
                            tail->ctBuffer[i] == '\t'))
      ++i;
    if (i >= tail_len)
      break;
    if (argc + 1 >= ARM_ELF_ARGV_SLOTS)
      return DE_INVLDFMT;
    if (text_used >= ARM_ELF_ARG_TEXT_SIZE)
      return DE_NOMEM;

    start = text_used;
    while (i < tail_len) {
      BYTE ch = tail->ctBuffer[i++];
      if (ch == '"') {
        quoted = !quoted;
        continue;
      }
      if (!quoted && (ch == ' ' || ch == '\t'))
        break;
      if (text_used + 1 >= ARM_ELF_ARG_TEXT_SIZE)
        return DE_NOMEM;
      text[text_used++] = ch;
    }
    text[text_used++] = '\0';
    argv[argc++] = (ULONG)(uintptr_t)(text + start);
  }
  argv[argc] = 0;
  meta->argc = argc;
  meta->argv_addr = (ULONG)(uintptr_t)argv;
  meta->cursor = text_off + ARM_ELF_ARG_TEXT_SIZE;
  return SUCCESS;
}

static COUNT DosArmElfLoader(exec_blk *exp, COUNT mode, COUNT fd, BYTE *namep)
{
  arm_elf32_ehdr eh;
  arm_elf_load_meta *meta;
  ULONG metadata_size;
  ULONG metadata_off;
  ULONG initial_cursor;
  ULONG final_end;
  ULONG paras_long;
  ULONG req_idx, requirements_idx, init_idx, main_idx, fini_idx, sig_idx;
  UWORD alloc_mcb, asize = 0, load_seg;
  UWORD env_mcb = 0;
  UWORD fcbcode;
  UWORD final_paras;
  int rc;

  if ((mode & 0x7f) == EXEC_OVERLAY)
    return arm_elf_reject(DE_INVLDFMT, "EXEC overlay mode is not supported");
  if ((mode & 0x7f) == EXEC_LOAD)
    return arm_elf_reject(DE_INVLDFMT, "EXEC load-only mode cannot expose a native ARM entry point");

  rc = arm_elf_read_meta(fd, 0, &eh, sizeof(eh));
  if (rc != SUCCESS)
    return arm_elf_reject((COUNT)rc, "cannot read ELF header");

  if (eh.magic != ELF32_MAGIC)
    return arm_elf_reject(DE_INVLDFMT, "bad ELF magic");
  if (eh.elf_class != ELFCLASS32)
    return arm_elf_reject(DE_INVLDFMT, "not ELF32");
  if (eh.data != ELFDATA2LSB)
    return arm_elf_reject(DE_INVLDFMT, "ELF is not little-endian");
  if (eh.ident_version != EV_CURRENT || eh.version != EV_CURRENT)
    return arm_elf_reject(DE_INVLDFMT, "unsupported ELF version");
  if (eh.abi != 0)
    return arm_elf_reject(DE_INVLDFMT, "unsupported ELF OS ABI");
  if (eh.type != ET_REL) {
    dos_printf("ARM ELF: unsupported ELF type %u; ET_REL required\r\n",
               (unsigned)eh.type);
    return DE_INVLDFMT;
  }
  if (eh.machine != EM_ARM) {
    dos_printf("ARM ELF: unsupported machine %u; EM_ARM required\r\n",
               (unsigned)eh.machine);
    return DE_INVLDFMT;
  }
  if (eh.flags & EF_ARM_ABI_FLOAT_HARD)
    return arm_elf_reject(DE_INVLDFMT, "hard-float ABI is not supported");
  if (eh.ehsize < sizeof(eh))
    return arm_elf_reject(DE_INVLDFMT, "ELF header is too small");
  if (eh.shentsize != sizeof(arm_elf32_shdr))
    return arm_elf_reject(DE_INVLDFMT, "unexpected section-header size");
  if (eh.shnum == 0)
    return arm_elf_reject(DE_INVLDFMT, "ELF has no section table");

  metadata_size = arm_elf_metadata_size(eh.shnum);
  if (metadata_size == 0xfffffffful)
    return arm_elf_reject(DE_NOMEM, "section metadata does not fit guest memory");
  metadata_off = arm_elf_align_up((ULONG)sizeof(psp), 4u);
  if (metadata_off == 0xfffffffful ||
      metadata_off > 0xfffffffful - metadata_size)
    return arm_elf_reject(DE_NOMEM, "PSP/metadata layout overflow");
  initial_cursor = metadata_off + metadata_size;
  paras_long = (initial_cursor + 15u) >> 4;
  if (paras_long == 0 || paras_long > 0xffffu)
    return arm_elf_reject(DE_NOMEM, "ELF metadata does not fit DOS guest memory");

  rc = ChildEnv(exp, &env_mcb, (char *)namep);
  if (rc != SUCCESS)
    return (COUNT)rc;

  /*
   * Reserve the largest available DOS block while loading.  ET_REL discovery
   * is recursive, so the final image size is not known until all reachable
   * sections have been relocated.  Owning the whole block avoids growing the
   * MCB once per newly reached section; the unused tail is returned below by
   * the existing final DosMemChange().
   */
  rc = ExecMemLargest(&asize, (UWORD)paras_long);
  if (rc == SUCCESS)
    rc = ExecMemAlloc(asize, &alloc_mcb, &asize);
  if (rc != SUCCESS) {
    DosMemFree(env_mcb);
    dos_printf("ARM ELF: cannot reserve largest guest block (minimum %lu bytes)\r\n",
               paras_long << 4);
    return (COUNT)rc;
  }
  load_seg = alloc_mcb + 1;

  /* Persistent loader metadata belongs to the child allocation. */
  meta = (arm_elf_load_meta *)ARM_PTR(arm_elf_guest_ptr(load_seg, metadata_off));
  memset(meta, 0, metadata_size);
  meta->loader_started_us = get_uticks();
  meta->eh = eh;
  meta->shnum = eh.shnum;
  meta->cursor = initial_cursor;
  meta->allocation_end = (ULONG)asize << 4;

  rc = arm_elf_find_tables(fd, meta);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot find a valid SHT_SYMTAB/SHT_STRTAB pair\r\n");
    goto fail;
  }
  rc = arm_elf_find_roots(fd, meta, &req_idx, &requirements_idx,
                          &init_idx, &main_idx, &fini_idx, &sig_idx);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot scan application entry symbols\r\n");
    goto fail;
  }
  if (main_idx == ARM_ELF_NO_SYMBOL) {
    rc = DE_INVLDFMT;
    dos_printf("ARM ELF: global main() entry symbol not found\r\n");
    goto fail;
  }

  /* These are the same roots used by murmulator-os2.  load_root() recursively
     follows relocation references and assigns each reached section its final
     runtime address before descending further, so cycles are harmless. */
  rc = arm_elf_load_root(fd, load_seg, meta, req_idx,
                         &meta->required_api_addr);
  if (rc != SUCCESS)
    goto reloc_fail;
  rc = arm_elf_load_root(fd, load_seg, meta, requirements_idx,
                         &meta->requirements_addr);
  if (rc != SUCCESS)
    goto reloc_fail;
  rc = arm_elf_load_root(fd, load_seg, meta, init_idx, &meta->init_addr);
  if (rc != SUCCESS)
    goto reloc_fail;
  rc = arm_elf_load_root(fd, load_seg, meta, main_idx, &meta->main_addr);
  if (rc != SUCCESS)
    goto reloc_fail;
  rc = arm_elf_load_root(fd, load_seg, meta, fini_idx, &meta->fini_addr);
  if (rc != SUCCESS)
    goto reloc_fail;
  rc = arm_elf_load_root(fd, load_seg, meta, sig_idx, &meta->signal_addr);
  if (rc != SUCCESS)
    goto reloc_fail;

  /*
   * ET_REL has not passed through the normal final linker script.  Make the
   * linker-owned startup array sections explicit roots and synthesize the
   * contiguous arrays which crt startup code normally receives.
   */
  rc = arm_elf_build_startup_arrays(fd, load_seg, meta);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot build startup init arrays\r\n");
    goto reloc_fail;
  }

  rc = arm_elf_preflight(meta);
  if (rc != SUCCESS)
    goto fail;

  /* Do not emit a blank line for loads completed before the threshold. */
  if (meta->is_long_running_job)
    dos_printf("\r\n");
  rc = arm_elf_build_argv(load_seg, meta, exp, namep);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot build argv in child memory\r\n");
    goto fail;
  }

  /*
   * Stacks no longer consume the main ELF MCB:
   *   - native ARM stack comes from the fixed PSRAM runtime arena;
   *   - guest DOS stack is a separate DOS allocation (UMB first).
   * The child image can therefore be shrunk immediately after argv.
   */
  final_end = arm_elf_align_up(meta->cursor, 16u);
  if (final_end == 0xfffffffful) {
    rc = DE_NOMEM;
    goto fail;
  }

  rc = arm_elf_ensure_capacity(meta, final_end);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: final image exceeds reserved guest block (%lu bytes)\r\n",
               final_end);
    goto fail;
  }
  final_paras = (UWORD)((final_end + 15u) >> 4);
  /* Trim any paragraph tail left by incremental growth.  No loaded section
     moves, so all absolute and PC-relative relocations retain their addresses. */
  rc = DosMemChange(load_seg, final_paras, NULL);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot shrink guest block to %u paragraphs\r\n",
               (unsigned)final_paras);
    goto fail;
  }

  DosCloseSft(fd, FALSE);

  /* Turn the allocation into an ordinary DOS child before native code runs.
     The first 256 bytes are its PSP; ELF metadata/sections/argv/stacks live
     after it in the same MCB and are therefore released by FreeProcessMem(). */
  setvec(0x22, exec_caller_return_addr());
  child_psp(load_seg, internal_data->cu_psp, load_seg + final_paras);
  fcbcode = patchPSP(alloc_mcb, env_mcb, exp, namep);
  (void)fcbcode;

  rc = arm_elf_alloc_dos_stack(meta, load_seg);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot allocate %lu-byte DOS stack\r\n",
               meta->dos_stack_size);
    goto fail;
  }

  CfgDbgPrintf(("ARM ELF loaded: psp=%04x size=%lu argc=%u "
                "DOS-stack=%04x:0000 req=%08lx init=%08lx main=%08lx "
                "fini=%08lx sig=%08lx\n",
                load_seg, final_end, (unsigned)meta->argc,
                meta->dos_stack_seg, meta->required_api_addr,
                meta->init_addr, meta->main_addr, meta->fini_addr,
                meta->signal_addr));
  return exec_run_arm_elf(load_seg, meta);

reloc_fail:
  dos_printf("ARM ELF: dependency loading/relocation failed, DOS error %d\r\n",
             (int)rc);
fail:
  if (meta != NULL && meta->dos_stack_mcb != 0)
    DosMemFree(meta->dos_stack_mcb);
  DosMemFree(alloc_mcb);
  DosMemFree(env_mcb);
  DosCloseSft(fd, FALSE);
  return (COUNT)rc;
}

ULONG SftGetFsize(int sft_idx)
{
  dos_far_ptr s = idx_to_sft(sft_idx);
  if (far_is_end(s))
    return DE_INVLDHNDL;
  return ((sft*)ARM_PTR(s))->sft_size;
}

/* dsk: 0 = current default drive, 1 = A:, 2 = B:, ... (FCB drive-byte
   convention). Returns NULL if invalid, exactly like get_cds(). */
struct cds *get_cds1(unsigned dsk)
{
  dos_far_ptr p;

  if (dsk == 0)
    dsk = internal_data->default_drive + 1;
  if (dsk == 0)
    return NULL;

  p = get_cds(dsk - 1);
  if (far_is_null(p))
    return NULL;
  return (struct cds *) ARM_PTR(p);
}

/*
 * Compare two SETVER filename fields case-insensitively.
 *
 * This is the original FreeDOS helper adapted only for native pointers:
 * both strings have already been mapped from guest memory by the caller.
 */
STATIC WORD SetverCompareFilename(const BYTE *m1, const BYTE *m2, COUNT count)
{
  while (count--)
  {
    if (toupper((unsigned char)*m1) != toupper((unsigned char)*m2))
      return (WORD)((unsigned char)*m1 - (unsigned char)*m2);

    ++m1;
    ++m2;
  }

  return 0;
}

/*
 * Look up a program basename in the guest SETVER table.
 *
 * Table records are encoded exactly as in FreeDOS:
 *   length byte, filename bytes, minor byte, major byte
 * and the list ends with a zero length byte.
 */
STATIC UWORD SetverGetVersion(dos_far_ptr table_ptr, const BYTE *name)
{
  BYTE *table;
  COUNT name_len;

  if (far_is_null(table_ptr) || name == NULL)
    return 0;

  table = (BYTE *)ARM_PTR(table_ptr);
  name_len = (COUNT)strlen((const char *)name);

  while (*table != 0)
  {
    BYTE len = *table;

    if (len == name_len &&
        SetverCompareFilename(name, table + 1, len) == 0)
      return *(UWORD *)(table + len + 1);

    table += len + 3;
  }

  return 0;
}

/*
   allocate memory for and copy the current process's env to a new
   child environment. Returns the segment of the env's *MCB* (not the
   env block itself) in *pChildEnvSeg.
*/
STATIC COUNT ChildEnv(exec_blk * exp, UWORD * pChildEnvSeg, char *pathname)
{
  BYTE *pSrc;
  BYTE *pDest;
  UWORD nEnvSize;
  COUNT RetCode;
  psp *ppsp = (psp *) ARM_PTR(MK_FP(internal_data->cu_psp, 0));

  *pChildEnvSeg = 0;             /* prevent freeing a random address on
                                     errors by callers of ChildEnv() */

  /* copy parent's environment if exec.env_seg == 0 */
  pSrc = exp->exec.env_seg ?
    (BYTE *) ARM_PTR(MK_FP(exp->exec.env_seg, 0)) :
    (ppsp->ps_environ ? (BYTE *) ARM_PTR(MK_FP(ppsp->ps_environ, 0)) : NULL);
  
  ///printf("ChildEnv\n");
  nEnvSize = 1;
  if (pSrc)
  {                              /* if no environment is available, one
                                     byte is required */
    for (nEnvSize = 0;; nEnvSize++)
    {
      if (nEnvSize >= MAXENV - ENV_KEEPFREE)
        return DE_INVLDENV;

      /* loop until first double terminator '\0\0' found */
      if (*(UWORD *) (pSrc + nEnvSize) == 0)
        break;
    }
    nEnvSize += 2;                /* account for trailing \0\0 */
  }

  /* allocate enough space for env + path (rounding up to nearest
     paragraph); at least 1 paragraph for an empty environment, plus
     ENV_KEEPFREE for argv[0] (the fully-qualified program name) */
  if ((RetCode = DosMemAlloc((nEnvSize + ENV_KEEPFREE + 15) / 16,
                             internal_data->mem_access_mode,
                             pChildEnvSeg, NULL)) < SUCCESS)
    return RetCode;

  pDest = (BYTE *) ARM_PTR(MK_FP(*pChildEnvSeg + 1, 0));      /* skip past MCB */

  if (pSrc)
  {
    memcpy(pDest, pSrc, nEnvSize);
    pDest += nEnvSize;
  }
  else
    *pDest++ = '\0';             /* empty environment */

  /* "extra strings" count (DOS 3.0+: argv[0] follows the env block) */
  *((UWORD *) pDest) = 1;
  pDest += sizeof(UWORD);

  /* copy the fully-qualified program name */
  /* pathname is DosExec()'s "lp": a NATIVE pointer that INT 21h/AH=4Bh built
     as ARM_PTR(guest DS:DX). That guest pointer belongs to the CALLER's
     segment (e.g. FreeCOM's PSP), NOT DOS_PSP - so it must be turned back
     into a far pointer by its true linear address, not re-anchored on
     DOS_PSP. Using x86_FAR_PTR(DOS_PSP, ...) here computes a wrong offset,
     truename() then fails to find the file, and every external command dies
     with "Bad command or filename". This is the one native->far conversion in
     the EXEC path that genuinely needs linear_to_far() until DosExec()/
     ChildEnv() are changed to carry a dos_far_ptr end to end. */
  if ((RetCode = truename(linear_to_far((const BYTE *) pathname),
                          PriPathName, CDS_MODE_SKIP_PHYSICAL)) < SUCCESS) {
    dpb_watch_check_chain("ChildEnv 1");
    return RetCode;
  }
  dpb_watch_check_chain("ChildEnv 2");
  strcpy(pDest, PriPathName);
  dpb_watch_check_chain("ChildEnv 3");

  return SUCCESS;
}

/*
 * Base PSP setup shared by every child: copy the parent PSP wholesale,
 * then replace the fields that must not be inherited.
 *
 * In particular, ps_retdosver starts from the current global DOS version.
 * A SETVER match for the child may override it later in patchPSP(); the
 * parent's own per-program fake version must not leak into the child.
 */
void new_psp(seg para, seg cur_psp)   /* exported: INT 21h AH=26h */
{
  psp *p = (psp *) ARM_PTR(MK_FP(para, 0));

  memcpy(p, ARM_PTR(MK_FP(cur_psp, 0)), sizeof(psp));

  p->ps_isv22 = getvec(0x22);
  p->ps_isv23 = getvec(0x23);
  p->ps_isv24 = getvec(0x24);
  p->ps_retdosver =
      ((UWORD)LoL->os_setver_minor << 8) | LoL->os_setver_major;
}

void child_psp(seg para, seg cur_psp, int psize)   /* exported: INT 21h AH=55h */
{
  psp *p = (psp *) ARM_PTR(MK_FP(para, 0));
  psp *q = (psp *) ARM_PTR(MK_FP(cur_psp, 0));
  /* Parent's JFT. NULL if the parent corrupted its own ps_filetab: the
     child then simply inherits no handles (its own table is already
     filled with 0xff below) instead of us reading 20 bytes out of the
     guest IVT and treating them as SFT indices. */
  UBYTE *q_filetab = jft_of(q);
  int i;

  new_psp(para, cur_psp);

  p->ps_parent = cur_psp;
  p->ps_prevpsp = MK_FP(cur_psp, 0);

  p->ps_size = psize;

  p->ps_maxfiles = 20;
  memset(p->ps_files, 0xff, 20);
  /* Canonical far pair <psp_seg>:0018h. NOT linear_to_far(p->ps_files):
     that normalises to (lin>>4):(lin&0xF), i.e. (psp_seg+1):0008h - the same
     LINEAR address, but a different seg:off pair. Programs and TSRs test the
     pair itself to decide whether the JFT is still the default one inside the
     PSP (that is what SetJFTSize() moves), so the normalised form reads to
     them as "JFT already relocated". Build it from the segment we know. */
  p->ps_filetab = MK_FP(para, offsetof(psp, ps_files));

  /*
   * Inherit the parent's first 20 handles, matching upstream
   * CloneHandle(): handles whose SFT has O_NOINHERIT are deliberately
   * omitted from the child JFT and their SFT reference count is not
   * incremented.
   */
  for (i = 0; q_filetab != NULL && i < 20; i++)
  {
    if (q_filetab[i] != 0xff)
    {
      dos_far_ptr sft_ptr = idx_to_sft(q_filetab[i]);
      if (!far_is_end(sft_ptr))
      {
        sft *entry = (sft *)ARM_PTR(sft_ptr);
        if (!(entry->sft_mode & O_NOINHERIT))
        {
          p->ps_files[i] = q_filetab[i];
          entry->sft_count++;
        }
      }
    }
  }

  p->ps_fcb1.fcb_drive = 0;
  memset(p->ps_fcb1.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);
  p->ps_fcb2.fcb_drive = 0;
  memset(p->ps_fcb2.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);

  p->ps_cmd.ctCount = 0;
  p->ps_cmd.ctBuffer[0] = 0xd;
}

/*
    exec_caller_return_addr() - the address the current INT 21h call will
    return to, i.e. upstream's user_r->CS:IP.

    DosComLoader()/DosExeLoader() publish this as the child's terminate
    vector (INT 22h, mirrored into the child PSP at +0Ah). Upstream reads it
    from the saved INT 21h register frame; the port was reading the LIVE
    CPU_CS:CPU_IP instead, which is not the same thing - by the time a loader
    runs, CS:IP no longer point at the caller's return site.

    internal_data->user_r already holds exactly the frame we need: fdos_21h()
    publishes the guest-visible iregs at PSP:2Eh on every INT 21h and points
    user_r at it (see fdos_21h.c). Read cs/ip back out of it.

    Falls back to the live CS:IP if there is no frame (a loader invoked from
    kernel init rather than from a guest INT 21h).
*/
static dos_far_ptr /* -> caller's return address */ exec_caller_return_addr(void)
{
  dos_far_ptr /* -> struct int21_guest_iregs */ fr = internal_data->user_r;

  if (!far_is_null(fr) && !far_is_end(fr))
  {
    const struct int21_guest_iregs *r =
        (const struct int21_guest_iregs *)ARM_PTR(fr);
    return MK_FP(r->cs, r->ip);
  }
  return MK_FP(CPU_CS, CPU_IP);
}

STATIC UWORD patchPSP(UWORD pspseg, UWORD envseg, exec_blk * exb, BYTE * fnam)
{
  psp *p;
  mcb *pspmcb;
  int i;
  BYTE *np;

  pspmcb = (mcb *) ARM_PTR(MK_FP(pspseg, 0));
  ++pspseg;
  p = (psp *) ARM_PTR(MK_FP(pspseg, 0));

  /* cmd_line/fcb_1/fcb_2 are guest far pointers out of the exec block, so a
     128-byte command tail placed near a segment end must wrap rather than
     read on past it. */
  guest_read(&p->ps_cmd, exb->exec.cmd_line, sizeof(CommandTail));
  /* "No FCBs" is signalled by an OFFSET of FFFFh - the segment is not part
     of the sentinel. Upstream tests exactly that (task.c: "if
     (FP_OFF(exb->exec.fcb_1) != 0xffff)"), and a guest is entitled to pass
     e.g. DS:FFFF. Testing the full FFFF:FFFF pair instead (far_is_end())
     would miss those and memcpy() 32 bytes of whatever ARM_PTR(DS:FFFF)
     lands on straight into the child's PSP FCBs. */
  if (FP_OFF(exb->exec.fcb_1) != 0xFFFF)
  {
    guest_read(&p->ps_fcb1, exb->exec.fcb_1, 16);
    guest_read(&p->ps_fcb2, exb->exec.fcb_2, 16);
  }

  pspmcb->m_psp = pspseg;
  if (envseg)
  {
    ((mcb *) ARM_PTR(MK_FP(envseg, 0)))->m_psp = pspseg;
    envseg++;
  }
  p->ps_environ = envseg;

  /* use the file name less extension, path, and drive */
  np = fnam;
  for (;;)
  {
    switch (*fnam++)
    {
      case '\0':
        goto set_name;
      case ':':
      case '/':
      case '\\':
        np = fnam;
    }
  }
set_name:
  for (i = 0; i < 8 && np[i] != '.' && np[i] != '\0'; i++)
    pspmcb->m_name[i] = toupper((unsigned char) np[i]);
  if (i < 8)
    pspmcb->m_name[i] = '\0';

  /* Per-program DOS version faking (SETVER). Upstream does this here and
     new_psp()/DosExec() both already claim we do too - but the block was
     dropped in the port, leaving SetverGetVersion() with no caller at all
     (-Wunused-function flags it). Restore it.

     LoL->setverPtr is still 0000:0000 until something publishes a SETVER
     table, and SetverGetVersion() returns 0 for a null table, so this is a
     no-op today - but the code path is live again and the comments are no
     longer lying. */
  {
    UWORD fakever = SetverGetVersion(LoL->setverPtr, np);
    if (fakever != 0)
      p->ps_retdosver = fakever;
  }

  /* AX value to be passed to the child, based on FCB drive validity -
     matches upstream's INT21/4B return convention (some old programs
     check this instead of parsing their own command line). */
  return (get_cds1(p->ps_fcb1.fcb_drive) ? 0 : 0xff) |
    (get_cds1(p->ps_fcb2.fcb_drive) ? 0 : 0xff00);
}

/*
   exec_run_child() - block the caller and run the freshly-built child
   process until it terminates, then resume the caller exactly where
   it left off. This is the architectural replacement for upstream's
   exec_user()/return_user()/user_r: those rely on a single, fixed,
   real-hardware "current user register frame" plus a manual IRETD to
   jump between processes, because upstream itself has no other way to
   suspend and resume execution contexts.

   This port still uses a synchronous, nested C call to suspend the
   caller and run the child, but the suspended DOS process state must
   not live for the whole child lifetime on the tiny native ARM stack.
   Real DOS keeps that state in guest memory, on the current process's
   DOS/user stack.  The port therefore does the same:
     1. reserve an exec_child_context below the parent's guest SS:SP;
     2. save the parent CPU/DOS state there, then switch SS:SP and the
        visible process state to the child;
     3. run the child (pc_step() for guest code, a native function for
        FCOM), release it, restore the parent from the guest frame and
        restore the parent's original SP, which releases the frame.

   Only the small native ABI frame, scalar temporaries and return
   addresses remain on the ARM stack.  Consequently nested EXECs use
   the per-process guest stacks for their long-lived saved contexts
   instead of accumulating those contexts in SCRATCH_Y.

   terminate_flag still only needs to be checked by the innermost
   active guest pc_step() loop.  Its outer value, together with
   native_done, is part of each guest-resident exec_child_context.
*/
static volatile bool terminate_flag;
static UBYTE term_exit_code, term_exit_type;

struct saved_cpu_ctx
{
  UWORD ax, bx, cx, dx, si, di, bp, sp;
  UWORD cs, ds, es, ss, ip;
  UWORD flags;
};

static void save_ctx(CPU * cpu, struct saved_cpu_ctx *s)
{
  s->ax = CPU_AX; s->bx = CPU_BX; s->cx = CPU_CX; s->dx = CPU_DX;
  s->si = CPU_SI; s->di = CPU_DI; s->bp = CPU_BP; s->sp = CPU_SP;
  s->cs = CPU_CS; s->ds = CPU_DS; s->es = CPU_ES; s->ss = CPU_SS;
  s->ip = CPU_IP; s->flags = cpu_getflags(cpu);
}

static void restore_ctx(CPU * cpu, struct saved_cpu_ctx *s)
{
  SET_SS(s->ss); CPU_SP = s->sp;
  SET_CS(s->cs); SET_IP(s->ip);
  SET_DS(s->ds); SET_ES(s->es);
  CPU_AX = s->ax; CPU_BX = s->bx; CPU_CX = s->cx; CPU_DX = s->dx;
  CPU_SI = s->si; CPU_DI = s->di; CPU_BP = s->bp;
  /* Restore FLAGS exactly. cpu_setflags() is (set_mask, clear_mask)
     applied in that order in both cores, so (s->flags, 0xFFFF) would
     zero everything: bits are set first, then the full clear wipes
     them. Masked in practice only because the final IRET of the
     parent's INT 21h re-pops the real flags - fix it anyway. */
  cpu_setflags(cpu, s->flags, (uword)~s->flags);
}

/* Called synchronously from INT 20h and INT 21h AH=00h/4Ch (see
   fdos_21h.c/fdos_20h() below). exit_type: 0=normal, 1=Ctrl-Break,
   2=critical error abort, 3=TSR (INT 21h AH=31h: the resident block
   was already resized by DosMemChange() in the 31h handler;
   exec_run_child() below keeps the process's memory and open
   handles). */
void request_terminate(UBYTE exit_code, UBYTE exit_type)
{
  term_exit_code = exit_code;
  term_exit_type = exit_type;
  terminate_flag = true;
  /* CRITICAL: stop the innermost pc_step() batch *immediately*, the
     same way intcall_waiter()/cpu_far_call_waiter() do. Without this,
     the CPU core IRETs back into the just-terminated program (to the
     byte right after its INT 20h / INT 21h AH=00h/4Ch) and keeps
     executing whatever garbage follows - for up to the remaining
     ~4095 instructions of the current pc_step(pc, 4096) batch -
     because exec_run_child() only checks terminate_flag *between*
     batches. Programs place the terminate call at the very end of
     their code, so those bytes are data/nothing, and execution
     deterministically walks off into the weeds. Both CPU cores
     (286/cpu.c i286_step() and i386.c) test native_done at the top of
     every instruction iteration and break out at once. */
  cpu->native_done = true;
}

/*
 * Наблюдатель terminate_flag для путей, где upstream-код стоит ПОСЛЕ
 * noreturn-вызова (spawn_int23() в chario.c): порт не может развернуть
 * нативный C-стек прыжком в int21_handler, поэтому вызывающие циклы
 * обязаны сами прекратить I/O, как только терминация запрошена.
 */
bool terminate_requested(void)
{
  return terminate_flag;
}

/*
 * Return AX-packed AL=last exit code, AH=exit type for INT 21h/AH=4Dh.
 *
 * The status is consumed by the read, matching upstream FreeDOS:
 * a second AH=4Dh call returns 0000h until another child terminates.
 */
UWORD DosGetRetCode(void)
{
  UWORD result = term_exit_code | ((UWORD)term_exit_type << 8);
  term_exit_code = 0;
  term_exit_type = 0;
  return result;
}

struct exec_child_context
{
  struct saved_cpu_ctx cpu;
  UWORD cu_psp;
  dos_far_ptr dta;
  UBYTE indos;
  UBYTE error_mode;
  bool terminate;
  bool native_done;
};

static void exec_enter_child(struct exec_child_context *saved,
                             UWORD child_psp_seg, dos_far_ptr stack,
                             UWORD dses)
{
  save_ctx(cpu,&saved->cpu);
  saved->cu_psp=internal_data->cu_psp; saved->dta=internal_data->dta;
  saved->indos=internal_data->InDOS;
  saved->error_mode=internal_data->ErrorMode;
  saved->terminate=terminate_flag;
  /* native_done is a shared signalling channel between three users:
     request_terminate(), bios_intcall()'s waiter and cpu_far_call()'s
     waiter. When this EXEC is entered from inside an OUTER pc_step()
     loop (a guest parent - e.g. a file manager - spawning a native
     COMMAND), the outer level may have its own pending state; it must
     be part of this stack frame, not destroyed by the blanket clears
     that used to live on both the enter and leave paths. */
  saved->native_done=cpu->native_done;

  internal_data->cu_psp=child_psp_seg;
  internal_data->dta=MK_FP(child_psp_seg,offsetof(psp,ps_cmd));
  SET_SS(FP_SEG(stack)); CPU_SP=FP_OFF(stack);
  SET_DS(dses); SET_ES(dses);
  terminate_flag=false;
  /* term_exit_code/term_exit_type are intentionally NOT cleared and NOT
     part of this per-level context: upstream FreeDOS keeps the AH=4Dh
     return status in a single kernel global (the SDA), so starting a
     new child must not erase the status a previous sibling left for our
     caller, and the status the child leaves at its termination must
     survive exec_leave_child() for the parent to read via AH=4Dh. */
  cpu->native_done=false;
  if (internal_data->InDOS != 0) --internal_data->InDOS;
}

static void exec_release_child(UWORD child_psp_seg)
{
  psp *p=(psp *)ARM_PTR(MK_FP(child_psp_seg,0));
  setvec(0x22,p->ps_isv22); setvec(0x23,p->ps_isv23); setvec(0x24,p->ps_isv24);
  if (term_exit_type != 3) {
    int i; for(i=0;i<p->ps_maxfiles;i++) DosClose(i);
    FcbCloseAll(); FreeProcessMem(child_psp_seg);
  }
}

static void exec_leave_child(struct exec_child_context *saved,
                             UWORD child_psp_seg)
{
  bool outer_terminate = saved->terminate;
  bool outer_native_done = saved->native_done;

  /* Cleanup runs with LOCAL, quiescent signals: the child has already
     terminated (its request_terminate() left terminate_flag=true /
     native_done=true on this level), and the OUTER level's pending
     state must not be visible either - DosClose()/FcbCloseAll()/
     FreeProcessMem() below may perform nested DOS/device calls (device
     driver close goes through cpu_far_call() and its pc_step() loop),
     and those must not be aborted by a signal that belongs to a
     different nesting level. The outer values are re-armed LAST, when
     this level is fully dismantled.

     term_exit_code/term_exit_type stay untouched throughout: they are
     the single DOS-global AH=4Dh status the child just left for the
     parent, and exec_release_child() reads term_exit_type for the
     keep-resident (TSR) decision. cu_psp is still the child's until
     exec_release_child() finishes - DosClose() locates the handle
     table through it. */
  cpu->native_done = false;
  terminate_flag = false;
  internal_data->InDOS = saved->indos;
  /* Match return_user(): suppress recursive critical-error aborts
     while vectors, handles, FCBs and process memory are released. */
  internal_data->abort_progress = (UBYTE)-1;
  exec_release_child(child_psp_seg);

  internal_data->cu_psp = saved->cu_psp;
  internal_data->dta = saved->dta;
  internal_data->abort_progress = 0;
  internal_data->ErrorMode = saved->error_mode;
  restore_ctx(cpu, &saved->cpu);
  /* restore_ctx() reinstates the parent's original SP and thereby
     releases the guest-resident exec_child_context.  Do not dereference
     saved beyond this point; the two outer signals were copied above. */
  terminate_flag = outer_terminate;
  cpu->native_done = outer_native_done;
}

enum exec_process_kind
{
  EXEC_PROCESS_GUEST,
  EXEC_PROCESS_NATIVE_COMMAND,
  EXEC_PROCESS_ARM_ELF
};

struct exec_process_start
{
  dos_far_ptr entry;
  dos_far_ptr stack;
  UWORD dses;
  UWORD ax_bx;
  UWORD child_psp;
  enum exec_process_kind kind;
  arm_elf_load_meta *arm_elf;
};

static void exec_set_initial_registers(const struct exec_process_start *start)
{
  SET_CS(FP_SEG(start->entry));
  SET_IP(FP_OFF(start->entry));
  CPU_AX = CPU_BX = start->ax_bx;
  CPU_CX = 0x00ff;
  CPU_DX = start->dses;
  CPU_SI = FP_OFF(start->entry);
  CPU_DI = FP_OFF(start->stack);
  CPU_BP = 0x091e;
  cpu_setflags(cpu, 0x0200, (uword)~0x0200u);
}

typedef void *(*arm_elf_init_fn)(void);
typedef int (*arm_elf_main_fn)(int, char **);
typedef void (*arm_elf_fini_fn)(void *);

/*
 * The currently active native main() recovery slot.
 *
 * The slot itself lives in arm_elf_load_meta, i.e. in metadata belonging to
 * the particular EXEC child.  Keeping only a pointer here is important for
 * nested EXEC: arm_elf_run_body() saves/restores the previous pointer while a
 * nested native child is running, so every process retains its own recovery
 * SP in its own metadata.
 */
static volatile ULONG *arm_elf_active_main_sp;

/*
 * Synthetic guest return point used while a native ELF application yields.
 *
 * The CPU image normally parked while native main() is running contains the
 * suspended parent process CS:IP.  It must never be resumed from yield().
 * Instead, pending IRQs run with CS:IP pointing at this callback trap.  A
 * hardware IRQ pushes that synthetic return address, executes through the
 * normal PIC/IVT path, and its final IRET lands here.
 *
 * Setting native_done gives cpu_step()/pc_step() an immediate stop condition:
 * the interpreter exits at the top of its next iteration instead of executing
 * even one instruction from the suspended parent context.
 */
static bool arm_elf_irq_return(CPU *cpu, bios_callback_params_t *params)
{
  if (!params->done) {
    params->done = true;
    cpu->native_done = true;
  }
  return false; /* callback address itself has no guest IRET */
}

/*
 * Run pending guest hardware IRQs without resuming the process which EXEC'ed
 * the native ELF child.
 *
 * exec_enter_child() already switched SS:SP to the native child's dedicated
 * DOS stack.  We preserve the complete parked CPU register image, replace only
 * CS:IP with a synthetic BIOS callback return point and force IF=1/TF=0.  If
 * an IRQ is pending, i286/i386 step takes it before fetching the synthetic
 * address, so PIC arbitration, IVT dispatch, guest hooks, STI/nested IRQs and
 * EOI semantics remain entirely in the existing guest machinery.
 *
 * If no IRQ is pending this helper is not entered.  During the handler we use
 * ten-instruction pc_step() slices: that keeps the AdLib pc_step path to one
 * CPU slice, and when the final IRET reaches arm_elf_irq_return(),
 * native_done causes the current slice to stop immediately.
 */
static void arm_elf_service_guest_irq(void)
{
  struct saved_cpu_ctx saved;
  bios_callback_params_t params;
  bool old_native_done;
  bool old_pending_trap;

  if (pc == NULL || cpu == NULL || !cpu->intr)
    return;

  save_ctx(cpu, &saved);
  old_native_done = cpu->native_done;
  old_pending_trap = cpu_pending_trap();

  memset(&params, 0, sizeof(params));
  params.callback = arm_elf_irq_return;
  params.expected_cs = 0xFFEF;
  params.expected_ip = 0x000F;
  params.owner = "NATIVE ELF IRQ";

  cpu_pending_trap_set(false);
  set_bios_callback(cpu, &params, true);

  /*
   * This is the only guest continuation visible to the IRQ.  The interrupt is
   * accepted before opcode fetch, so its IRET returns here, never to saved.cs:
   * saved.ip (the suspended parent EXEC context).
   */
  SET_CS(params.expected_cs);
  SET_IP(params.expected_ip);
  cpu_setflags(cpu, 0x0200u, 0x0100u); /* IF=1, TF=0 */
  cpu->native_done = false;

  while (!params.done) {
    pc_step(pc, 10);

    /* A DOS termination request aborts the active guest burst.  Do not spin on
       native_done waiting for an IRET which will never arrive. */
    if (terminate_requested())
      break;
  }

  drop_bios_callback(cpu, &params);
  cpu_pending_trap_set(old_pending_trap);
  restore_ctx(cpu, &saved);
  cpu->native_done = old_native_done;
}

/*
 * Cooperative service point for a running native ELF application.
 *
 * Device polling first makes fresh IRQ state visible (keyboard, PIT, etc.).
 * Then any pending IRQ is allowed to execute through the ordinary guest
 * PIC/IVT machinery on the child's DOS stack, but only between the synthetic
 * callback boundaries above.  The suspended parent CS:IP remains frozen for
 * the entire native-child lifetime.
 */
uint32_t arm_elf_yield(void)
{
  pc_service(pc);
  arm_elf_service_guest_irq();
  return get_uticks();
}

/*
 * Call the application main() through a kernel-owned fixed frame.
 *
 * There must be no matching assembler wrapper in the application: the whole
 * purpose of this trampoline is to keep process-unwind mechanics in the
 * loader/runtime.  exit(status) can then restore the SP saved here and enter
 * arm_elf_main_return exactly as if main() had returned status normally.
 *
 * AAPCS on entry:
 *   r0 = main_fn
 *   r1 = argc
 *   r2 = argv
 *
 * Cortex-M0+/Thumb-1 cannot push/pop r8-r11 directly, hence the moves through
 * r4-r7.  The extra 4-byte slot restores the required 8-byte SP alignment
 * before BLX into ordinary C code.
 */
static int __attribute__((naked, noinline))
arm_elf_call_main(arm_elf_main_fn main_fn, int argc, char **argv)
{
  __asm volatile (
      /* Save caller state and the LR back into arm_elf_run_body(). */
      "push {r4-r7, lr}\n"
      "mov  r4, r8\n"
      "mov  r5, r9\n"
      "mov  r6, r10\n"
      "mov  r7, r11\n"
      "push {r4-r7}\n"
      "sub  sp, #4\n"

      /* Store this exact recovery SP into current process metadata. */
      "ldr  r4, =arm_elf_active_main_sp\n"
      "ldr  r4, [r4]\n"
      "mov  r5, sp\n"
      "str  r5, [r4]\n"

      /* Rearrange wrapper arguments into main(argc, argv). */
      "mov  r3, r0\n"  /* r3 = main_fn */
      "mov  r0, r1\n"  /* r0 = argc */
      "mov  r1, r2\n"  /* r1 = argv */
      "blx  r3\n"

      /* Ordinary main() return: restore the wrapper's saved state. */
      "add  sp, #4\n"
      "pop  {r4-r7}\n"
      "mov  r8, r4\n"
      "mov  r9, r5\n"
      "mov  r10, r6\n"
      "mov  r11, r7\n"
      "pop  {r4-r7, pc}\n");
}

/*
 * Public native-process termination backend exported through DOS_API.
 *
 * AAPCS supplies status in r0.  This naked function deliberately never
 * modifies r0: after unwinding to arm_elf_main_return it therefore becomes
 * the return value of main(), and arm_elf_run_body() resumes its normal path
 * (including _fini()).
 *
 * The wrapper's saved registers/LR are restored here after switching back to
 * its recovery SP.  MSPLIM/PRIMASK still belong to the native stack context
 * and are restored later by arm_elf_call_on_stack(), exactly as on a normal
 * main() return.
 */
void __attribute__((naked, noreturn)) arm_elf_process_exit(int status)
{
  __asm volatile (
      /*
       * r0 == status and must survive unchanged: it becomes main()'s return
       * value.  Restore the exact recovery SP recorded by arm_elf_call_main()
       * and execute that wrapper's epilogue directly.
       *
       * Keeping the epilogue here avoids a cross-function branch to a global
       * assembler label and therefore removes any dependence on the linker
       * preserving the Thumb-function bit for that synthetic symbol.
       */
      "ldr  r1, =arm_elf_active_main_sp\n"
      "ldr  r1, [r1]\n" /* r1 = &meta->native_main_sp */
      "ldr  r1, [r1]\n" /* r1 = saved recovery SP */
      "mov  sp, r1\n"   /* discard all application frames below main */

      "add  sp, #4\n"
      "pop  {r4-r7}\n"
      "mov  r8, r4\n"
      "mov  r9, r5\n"
      "mov  r10, r6\n"
      "mov  r11, r7\n"
      "pop  {r4-r7, pc}\n");
}

typedef void (*arm_elf_array_fn)(void);

static void arm_elf_run_init_array(ULONG addr, ULONG count)
{
  ULONG i;
  arm_elf_array_fn *array = (arm_elf_array_fn *)(uintptr_t)addr;

  for (i = 0; i < count; ++i) {
    arm_elf_array_fn fn = array[i];
    if (fn != NULL && (uintptr_t)fn != ~(uintptr_t)0)
      fn();
  }
}

static void arm_elf_run_fini_array_reverse(ULONG addr, ULONG count)
{
  arm_elf_array_fn *array = (arm_elf_array_fn *)(uintptr_t)addr;

  while (count != 0) {
    arm_elf_array_fn fn = array[--count];
    if (fn != NULL && (uintptr_t)fn != ~(uintptr_t)0)
      fn();
  }
}

static int __attribute__((noinline)) arm_elf_run_body(arm_elf_load_meta *meta)
{
  void *fini_ctx = NULL;
  int result;

  /*
   * Standard final-link/crt startup semantics:
   *   preinit_array -> init_array -> optional application _init -> main.
   * Pico SDK 2.x runtime initializers and C++ constructors are registered via
   * these arrays.  _init remains a weak/optional user hook, not a substitute
   * for the standard linker-created mechanism.
   */
  arm_elf_run_init_array(meta->preinit_array_addr, meta->preinit_array_count);
  arm_elf_run_init_array(meta->init_array_addr, meta->init_array_count);

  if (meta->init_addr != 0)
    fini_ctx = ((arm_elf_init_fn)(uintptr_t)meta->init_addr)();

  arm_elf_publish_process_requirements(meta);

  /*
   * Keep the recovery SP in this process's metadata, not in the application.
   * Save/restore the active slot pointer so nested native EXEC has an
   * independent unwind target and the parent resumes with its own target.
   */
  volatile ULONG *saved_main_sp_slot = arm_elf_active_main_sp;
  arm_elf_active_main_sp = &meta->native_main_sp;
  result = arm_elf_call_main((arm_elf_main_fn)(uintptr_t)meta->main_addr,
                             meta->argc,
                             (char **)(uintptr_t)meta->argv_addr);
  arm_elf_active_main_sp = saved_main_sp_slot;

  /* A DOS terminate request made from native code through bios_intcall()
     is semantically noreturn even though the native bridge itself must
     unwind back to this frame.  In particular AH=31h has already turned
     the process into a TSR: running _fini() here would tear down state
     that is explicitly meant to remain resident.  The same rule also
     preserves normal INT 21h/4Ch semantics for native applications that
     choose to terminate through DOS rather than by returning from main(). */
  if (!terminate_requested()) {
    if (meta->fini_addr != 0)
      ((arm_elf_fini_fn)(uintptr_t)meta->fini_addr)(fini_ctx);

    /* Standard fini_array execution is reverse registration order. */
    arm_elf_run_fini_array_reverse(meta->fini_array_addr,
                                   meta->fini_array_count);
  }

  return result;
}

/* RP2350 core0 normally runs with MSPLIM guarding its dedicated SRAM stack.
   Native DOS applications run on their process stack in the reserved PSRAM
   stack arena, so both SP and MSPLIM must move as one atomic transition.
   IRQs are masked
   only across the two transitions; the application itself runs with the
   caller's original PRIMASK. */
static int __attribute__((naked, noinline))
arm_elf_call_on_stack(arm_elf_load_meta *meta, uintptr_t stack_top,
                      uintptr_t stack_bottom,
                      int (*body)(arm_elf_load_meta *))
{
  __asm volatile (
      "push {r4-r6, lr}\n"
      "mov  r4, sp\n"
      "mrs  r5, primask\n"
      "mrs  r6, msplim\n"
      "cpsid i\n"
      "bic  r1, r1, #7\n"
      "mov  sp, r1\n"
      "adds r2, #32\n"
      "msr  msplim, r2\n"
      "msr  primask, r5\n"
      "blx  r3\n"
      "cpsid i\n"
      "msr  msplim, r6\n"
      "mov  sp, r4\n"
      "msr  primask, r5\n"
      "pop  {r4-r6, pc}\n");
}

static COUNT exec_run_process(const struct exec_process_start *start)
{
  UWORD parent_sp = CPU_SP;
  UWORD frame_sp;
  struct exec_child_context *saved;

  /*
   * Keep the long-lived suspended-parent context in guest RAM, exactly
   * where a real DOS task switch keeps it: below the current process's
   * SS:SP.  Align the native view to a UWORD boundary; the extra byte,
   * when the guest supplied an odd SP, is released together with the
   * frame when restore_ctx() reinstates parent_sp.
   *
   * This object belongs to the suspended DOS process, remains live for
   * the complete child lifetime, and nested EXEC levels must therefore
   * consume their respective parent stacks rather than any shared
   * arena.
   */
  frame_sp = (UWORD)((parent_sp - sizeof(*saved)) & (UWORD)~1u);
  CPU_SP = frame_sp;
  saved = (struct exec_child_context *)ARM_PTR(MK_FP(CPU_SS, frame_sp));

  exec_enter_child(saved, start->child_psp,
                   start->stack, start->dses);
  /* save_ctx() saw the reserved frame at SS:frame_sp.  The process state
     to restore is the pre-reservation stack pointer. */
  saved->cpu.sp = parent_sp;
  if (start->kind != EXEC_PROCESS_ARM_ELF)
    exec_set_initial_registers(start);

  if (start->kind == EXEC_PROCESS_ARM_ELF)
  {
    arm_elf_load_meta *meta = start->arm_elf;
    uintptr_t stack_bottom;
    uintptr_t stack_top;
    uintptr_t previous_stack_cursor;
    int exit_code;

    if (arm_elf_native_stack_acquire(meta->native_stack_size, &stack_bottom,
                                     &previous_stack_cursor) != SUCCESS) {
      dos_printf("ARM ELF: native PSRAM stack arena exhausted (%lu bytes)\r\n",
                 meta->native_stack_size);
      exec_leave_child(saved, start->child_psp);
      return DE_NOMEM;
    }
    meta->native_stack_addr = (ULONG)stack_bottom;
    stack_top = stack_bottom + meta->native_stack_size;

    {
      uintptr_t saved_diag_native_bottom = doom_diag_native_stack_bottom;
      uintptr_t saved_diag_native_top = doom_diag_native_stack_top;
      UWORD saved_diag_dos_seg = doom_diag_dos_stack_seg;
      UWORD saved_diag_dos_size = doom_diag_dos_stack_size;

      doom_diag_native_stack_bottom = stack_bottom;
      doom_diag_native_stack_top = stack_top;
      doom_diag_dos_stack_seg = meta->dos_stack_seg;
      doom_diag_dos_stack_size = (UWORD)meta->dos_stack_size;

      diag_native_code_enter();
      exit_code = arm_elf_call_on_stack(meta, stack_top, stack_bottom,
                                        arm_elf_run_body);
      diag_native_code_leave();

      doom_diag_native_stack_bottom = saved_diag_native_bottom;
      doom_diag_native_stack_top = saved_diag_native_top;
      doom_diag_dos_stack_seg = saved_diag_dos_seg;
      doom_diag_dos_stack_size = saved_diag_dos_size;
    }

    arm_elf_native_stack_release(stack_bottom, previous_stack_cursor);
    meta->native_stack_addr = 0;

    /*
     * A TSR keeps its resident ELF image, but neither startup stack is part of
     * resident state.  The native stack above has already returned to the
     * LIFO PSRAM arena.  The guest DOS stack is a separate child-owned MCB;
     * free it explicitly because exec_release_child() intentionally skips
     * FreeProcessMem() for exit type 3.
     *
     * Resident callbacks execute in the context/stack of the process they
     * intercept, so retaining either startup stack would only leak memory.
     */
    if (terminate_requested() && term_exit_type == 3 &&
        meta->dos_stack_mcb != 0) {
      DosMemFree(meta->dos_stack_mcb);
      meta->dos_stack_mcb = 0;
      meta->dos_stack_seg = 0;
    }

    /* Returning from main() is the native equivalent of INT 21h/4Ch,
       but only if the application has not already requested termination
       through DOS.  AH=31h, for example, has set exit type 3 and resized
       the MCB; overwriting that status here would make exec_release_child()
       free a process which DOS has just made resident. */
    if (!terminate_requested()) {
      term_exit_code = (UBYTE)exit_code;
      term_exit_type = 0;
    }
  }
  else if (start->kind == EXEC_PROCESS_NATIVE_COMMAND)
  {
    UBYTE exit_code = fcom_process_main(cpu, start->child_psp);

    /* The native COMMAND has no pc_step() loop of its own, so
       request_terminate() would be a category error here: its
       cpu->native_done = true targets "the innermost ACTIVE pc_step()
       batch" - which at this point is the OUTER loop of a guest
       parent (if any), producing a spurious stop/clear pulse inside
       someone else's CPU loop. All the native process needs is what
       return_user() records for the parent's AH=4Dh: the exit status.
       exec_release_child() below reads term_exit_type for the
       keep-resident decision, so it must be set on this path too. */
    term_exit_code = exit_code;
    term_exit_type = 0;
  }
  else
  {
    while (!terminate_flag)
      pc_step(pc, 4096);
  }

  exec_leave_child(saved, start->child_psp);
  return SUCCESS;
}

COUNT exec_run_native_command(UWORD child_psp_seg, UWORD fcbcode)
{
  struct exec_process_start start;

  start.entry = MK_FP(child_psp_seg, fcom_process_entry_offset());
  start.stack = MK_FP(child_psp_seg, fcom_process_stack_top());
  start.dses = child_psp_seg;
  start.ax_bx = fcbcode;
  start.child_psp = child_psp_seg;
  start.kind = EXEC_PROCESS_NATIVE_COMMAND;
  start.arm_elf = NULL;

  return exec_run_process(&start);
}

static COUNT exec_run_arm_elf(UWORD child_psp_seg,
                              arm_elf_load_meta *meta)
{
  struct exec_process_start start;

  start.entry = MK_FP(child_psp_seg, 0);
  start.stack = MK_FP(meta->dos_stack_seg,
                      (UWORD)meta->dos_stack_size);
  start.dses = child_psp_seg;
  start.ax_bx = 0;
  start.child_psp = child_psp_seg;
  start.kind = EXEC_PROCESS_ARM_ELF;
  start.arm_elf = meta;
  return exec_run_process(&start);
}

static COUNT exec_run_child(dos_far_ptr entry, dos_far_ptr stack,
                            UWORD dses, UWORD ax_bx, UWORD child_psp_seg)
{
  struct exec_process_start start;

  start.entry = entry;
  start.stack = stack;
  start.dses = dses;
  start.ax_bx = ax_bx;
  start.child_psp = child_psp_seg;
  start.kind = EXEC_PROCESS_GUEST;
  start.arm_elf = NULL;

  return exec_run_process(&start);
}

STATIC int load_transfer(UWORD ds, exec_blk * exp, UWORD fcbcode, COUNT mode)
{
  psp *p = (psp *) ARM_PTR(MK_FP(ds, 0));

  p->ps_parent = internal_data->cu_psp;
  p->ps_prevpsp = MK_FP(internal_data->cu_psp, 0);

  if (mode == EXEC_LOADNGO) {
    CfgDbgPrintf(("LOAD psp=%04x entry=%04x:%04x stack=%04x:%04x ds=%04x ax=%04x\n",
                  ds,
                  FP_SEG(exp->exec.start_addr), FP_OFF(exp->exec.start_addr),
                  FP_SEG(exp->exec.stack), FP_OFF(exp->exec.stack),
                  ds, fcbcode));
    return exec_run_child(exp->exec.start_addr, exp->exec.stack, ds, fcbcode, ds);
  }

  /* mode == EXEC_LOAD: don't run it, just hand the caller back the
     entry point/stack we computed (exp->exec.start_addr/stack) plus
     fcbcode pushed onto that stack, matching INT21/4B AL=1. */
  exp->exec.stack.offset -= 2;
  *((UWORD *) ARM_PTR(exp->exec.stack)) = fcbcode;
  return SUCCESS;
}

/* Find out how many paragraphs are available, considering a
   threshold, trying HIGH then LOW memory. */
STATIC int ExecMemLargest(UWORD * asize, UWORD threshold)
{
  int rc;

  if (internal_data->mem_access_mode & 0x80)
  {
    internal_data->mem_access_mode &= ~0x80;
    internal_data->mem_access_mode |= 0x40;
    rc = DosMemLargest(asize);
    internal_data->mem_access_mode &= ~0x40;
    if (rc != SUCCESS || *asize < threshold)
      rc = DosMemLargest(asize);
    internal_data->mem_access_mode |= 0x80;
  }
  else
    rc = DosMemLargest(asize);

  return (*asize < threshold ? DE_NOMEM : rc);
}

STATIC int ExecMemAlloc(UWORD size, seg * para, UWORD * asize)
{
  int rc = DosMemAlloc(size, internal_data->mem_access_mode, para, asize);

  if (rc != SUCCESS)
  {
    if (rc == DE_NOMEM)
    {
      rc = DosMemAlloc(0, LARGEST, para, asize);
      if ((internal_data->mem_access_mode & 0x80) && (rc != SUCCESS))
      {
        internal_data->mem_access_mode &= ~0x80;
        rc = DosMemAlloc(0, LARGEST, para, asize);
        internal_data->mem_access_mode |= 0x80;
      }
    }
  }
  else
    *asize = size;

  if (rc == SUCCESS && *asize < size)
  {
    DosMemFree(*para);
    return DE_NOMEM;
  }
  return rc;
}

COUNT DosComLoader(BYTE * namep, exec_blk * exp, COUNT mode, COUNT fd)
{
  UWORD mem;
  UWORD env = 0, asize = 0;

  {
    UWORD com_size;
    ULONG com_size_long = SftGetFsize(fd);

    /* max 64K - 256 bytes stack - 256 bytes PSP */
    com_size = ((UWORD) min(com_size_long, 0xfe00u) >> 4) + 0x10;

    if ((mode & 0x7f) != EXEC_OVERLAY)
    {
      COUNT rc;
      UBYTE UMBstate = LoL->uppermem_link;
      UBYTE orig_mem_access = internal_data->mem_access_mode;

      if (mode & LOAD_HIGH)
      {
        internal_data->mem_access_mode |= 0x80;
        DosUmbLink(1);
      }

      rc = ChildEnv(exp, &env, namep);

      /* COM files always load into the largest available block */
      if (rc == SUCCESS)
        rc = ExecMemLargest(&asize, com_size);
      if (rc == SUCCESS)
        rc = ExecMemAlloc(asize, &mem, &asize);
      if (rc != SUCCESS)
        DosMemFree(env);

      if (mode & LOAD_HIGH)
      {
        DosUmbLink(UMBstate);
        internal_data->mem_access_mode = orig_mem_access;
        mode &= 0x7f;
      }

      if (rc != SUCCESS)
        return rc;

      ++mem;
    }
    else
      mem = exp->load.load_seg;
  }

  {
    dos_far_ptr sp;

    if (mode == EXEC_OVERLAY)
      sp = MK_FP(mem, 0);
    else
      sp = MK_FP(mem, sizeof(psp));

    /* DOS always loads only the first 64K - sizeof(psp) bytes */
    SftSeek(fd, 0, SEEK_SET);
    DosRWSft(fd, (mode == EXEC_OVERLAY) ? 0xfffeU : 0xff00U, sp, XFR_READ);
    DosCloseSft(fd, FALSE);
  }

  if (mode == EXEC_OVERLAY)
    return SUCCESS;

  {
    UWORD fcbcode;
    psp *p;
    // termination vector (not used by the kernel, but may be used by gues process)
    setvec(0x22, exec_caller_return_addr());
    child_psp(mem, internal_data->cu_psp, mem + asize);
    fcbcode = patchPSP(mem - 1, env, exp, namep);

    if (asize > 0x1000)
      asize = 0x1000;
    if (asize < 0x11)
      return DE_NOMEM;
    asize -= 0x11;

    /* CP/M compatibility: far-call-to-0:00C0h stub encoding the
       segment size, at PSP+5 */
    p = (psp *) ARM_PTR(MK_FP(mem, 0));
    p->ps_reentry = MK_FP(0xc - asize, asize << 4);
    asize <<= 4;
    asize += 0x10e;
    exp->exec.stack = MK_FP(mem, asize);
    exp->exec.start_addr = MK_FP(mem, 0x100);
    *((UWORD *) ARM_PTR(MK_FP(mem, asize))) = 0;
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

COUNT DosExeLoader(BYTE * namep, exec_blk * exp, COUNT mode, COUNT fd)
{
  UWORD mem, env = 0, start_seg, asize = 0;
  UWORD exe_size;
  UWORD image_size;

  image_size = (ExeHeader.exPages << 5) - ExeHeader.exHeaderSize;

  if ((mode & 0x7f) != EXEC_OVERLAY)
  {
    UBYTE UMBstate = LoL->uppermem_link;
    UBYTE orig_mem_access = internal_data->mem_access_mode;
    COUNT rc;

    image_size += sizeof(psp) / 16;
    exe_size = image_size + ExeHeader.exMinAlloc;

    if (exe_size < image_size)   /* overflow: exMinAlloc==0xffff etc. */
      return DE_NOMEM;

    if (mode & LOAD_HIGH)
    {
      DosUmbLink(1);
      internal_data->mem_access_mode |= 0x80;
    }

    rc = ChildEnv(exp, &env, namep);

    if (rc == SUCCESS)
      rc = ExecMemLargest(&asize, exe_size);

    exe_size = image_size + ExeHeader.exMaxAlloc;
    if (exe_size > asize || exe_size < image_size)
      exe_size = asize;

    /* exMinAlloc==exMaxAlloc==0: allocate the largest possible block
       and load the image as high in it as possible */
    if ((ExeHeader.exMinAlloc | ExeHeader.exMaxAlloc) == 0)
      exe_size = asize;

    if (rc == SUCCESS)
      rc = ExecMemAlloc(exe_size, &mem, &asize);
    if (rc != SUCCESS)
      DosMemFree(env);

    if (mode & LOAD_HIGH)
    {
      internal_data->mem_access_mode = orig_mem_access;
      DosUmbLink(UMBstate);
    }
    if (rc != SUCCESS)
      return rc;

    mode &= 0x7f;
    ++mem;
  }
  else
    mem = exp->load.load_seg;

  if (SftSeek(fd, (LONG) ExeHeader.exHeaderSize * 16UL, SEEK_SET) < SUCCESS)
  {
    if (mode != EXEC_OVERLAY)
    {
      DosMemFree(--mem);
      DosMemFree(env);
    }
    return DE_INVLDDATA;
  }

  start_seg = mem;
  exe_size = image_size;
  if (mode != EXEC_OVERLAY)
  {
    exe_size -= sizeof(psp) / 16;
    start_seg += sizeof(psp) / 16;
    if (exe_size > 0 && (ExeHeader.exMinAlloc | ExeHeader.exMaxAlloc) == 0)
    {
      mcb *mp = (mcb *) ARM_PTR(MK_FP(mem - 1, 0));
      start_seg += mp->m_size - image_size;
    }
  }

  /* read the image in CHUNK-sized (paragraph-aligned) pieces,
     advancing the *segment* between reads - see DEVLOAD_CHUNK_PARAS's
     comment in DosExec()'s file-level note for why */
  {
    int nBytesRead, toRead = CHUNK;
    seg sp = start_seg;

    for (;;)
    {
      if (exe_size < CHUNK / 16)
        toRead = exe_size * 16;
      nBytesRead = (int) DosRWSft(fd, toRead, MK_FP(sp, 0), XFR_READ);
      if (nBytesRead < toRead || exe_size <= CHUNK / 16)
        break;
      sp += CHUNK / 16;
      exe_size -= CHUNK / 16;
    }
  }

  {
    COUNT i;
    UWORD *reloc = RelocBuf;

    SftSeek(fd, (LONG) ExeHeader.exRelocTable, SEEK_SET);
    for (i = 0; i < ExeHeader.exRelocItems; i++)
    {
      UWORD *spot;

      if (DosRWSft(fd, sizeof(UWORD) * 2, x86_FAR_PTR(DOS_PSP, reloc) /* -> UWORD[] */,
                   XFR_READ) != sizeof(UWORD) * 2)
      {
        if (mode != EXEC_OVERLAY)
        {
          DosMemFree(--mem);
          DosMemFree(env);
        }
        return DE_INVLDDATA;
      }
      if (mode == EXEC_OVERLAY)
      {
        spot = (UWORD *) ARM_PTR(MK_FP(reloc[1] + mem, reloc[0]));
        *spot += exp->load.reloc;
      }
      else
      {
        spot = (UWORD *) ARM_PTR(MK_FP(reloc[1] + start_seg, reloc[0]));
        *spot += start_seg;
      }
    }
  }

  DosCloseSft(fd, FALSE);

  if (mode == EXEC_OVERLAY)
    return SUCCESS;

  {
    UWORD fcbcode;

    setvec(0x22, exec_caller_return_addr());
    // termination vector (not used by the kernel, but may be used by gues process)
    child_psp(mem, internal_data->cu_psp, mem + asize);
    fcbcode = patchPSP(mem - 1, env, exp, namep);
    exp->exec.stack = MK_FP(ExeHeader.exInitSS + start_seg, ExeHeader.exInitSP);
    exp->exec.start_addr = MK_FP(ExeHeader.exInitCS + start_seg, ExeHeader.exInitIP);
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

static void fcom_copy_exec_tail(char *dst, size_t dst_size, const CommandTail *tail)
{
  size_t count;

  if (dst_size == 0)
    return;

  dst[0] = '\0';
  if (tail == NULL)
    return;

  count = tail->ctCount;
  if (count >= dst_size)
    count = dst_size - 1;

  memcpy(dst, tail->ctBuffer, count);
  dst[count] = '\0';
}

/*
    DosExec() - COUNT DosExec(COUNT mode, exec_blk FAR *ep, BYTE FAR *lp)

    mode: EXEC_LOADNGO (0) - load, build a PSP+environment, and run
    the program, blocking until it terminates (see exec_run_child()).
    EXEC_LOAD (1) - load and build a PSP+environment, but don't run it
    (the caller gets exp->exec.stack/start_addr back and is expected
    to transfer control itself - no caller in this port actually uses
    this mode yet, but it's implemented for completeness/parity with
    the DOS API). EXEC_OVERLAY (3) - load a raw image at a
    caller-supplied segment, no PSP, no execution (this is what
    CONFIG.SYS's DEVICE=/DEVICEHIGH= uses - see LoadDevice() in
    config.c).
*/
#if CONTROL_STACK
/* Свободный запас нативного стека, без которого новый уровень EXEC не
   стартует. Лучше честный DE_NOMEM, чем сползание SP через данные
   SCRATCH_Y в SCRATCH_X - в стек core1.

   Калибровка: пока 512 с тенденцией к снижению */
#define DOSEXEC_NATIVE_STACK_HEADROOM 512u

static uint32_t native_stack_free(void)
{
#if defined(__arm__) || defined(__thumb__)
  extern uint32_t __StackBottom;
  uint32_t sp;
  __asm volatile ("mov %0, sp" : "=r" (sp));
  return sp - (uint32_t)(uintptr_t)&__StackBottom;
#else
  return 0xffffffffu;   /* host-сборки для статического анализа */
#endif
}
#endif

COUNT DosExec(COUNT mode, exec_blk * ep, BYTE * lp)
{
  COUNT rc;
  COUNT fd;
  long openresult;
  dos_far_ptr x86_lp;
#if CONTROL_STACK
  {
    uint32_t free_bytes = native_stack_free();

    if (free_bytes < DOSEXEC_NATIVE_STACK_HEADROOM)
    {
      /* Событие редкое и важное: молчаливый DE_NOMEM fcom показывает
         как "Bad command or filename", маскируя причину. */
      dos_printf("DOSEXEC: native stack low (%u bytes free), "
                 "EXEC refused\n", (unsigned)free_bytes);
      return DE_NOMEM;
    }
  }
#endif
  if ((mode & 0x7f) == EXEC_LOADNGO &&
      fcom_is_command_com((const char *)lp))
  {
    /*
     * Хвост командной строки ребёнка собирается в фиксированном слоте
     * SDA_EXEC_TAIL_OFF на disk API-стеке SDA (init-mod.h) - там, где
     * у оригинала живут автоматики kernel-кода. Живёт до копирования в
     * PSP:80h ребёнка функцией fcom_create_process(); patchPSP()
     * отдельно переносит FCB1/FCB2 из EXEC parameter block. Слот
     * one-shot: к моменту запуска ребёнка (и любых вложенных EXEC)
     * содержимое уже скопировано. Стек вызывающего не трогается.
     */
    _Static_assert(sizeof(((CommandTail *)0)->ctBuffer) + 1 <=
                   SDA_EXEC_TAIL_LEN, "EXEC tail slot too small");
    char *tail = (char *)ARM_PTR(MK_FP(DOS_PSP, SDA_EXEC_TAIL_OFF));
    const CommandTail *command_tail = NULL;
    UWORD child_env_mcb = 0;
    COUNT env_rc;

    if (!far_is_null(ep->exec.cmd_line) &&
        !far_is_end(ep->exec.cmd_line))
      command_tail =
          (const CommandTail *)ARM_PTR(ep->exec.cmd_line);

    /*
     * Match ordinary EXEC semantics: ChildEnv() copies either the explicit
     * EPB environment or, for env_seg == 0, the current process environment,
     * and appends argv[0].  FCOM owns that copy for its whole lifetime.
     */
    env_rc = ChildEnv(ep, &child_env_mcb, (char *)lp);
    if (env_rc < SUCCESS)
      return env_rc;

    {
      UWORD command_psp;
      fcom_copy_exec_tail(tail,
                          sizeof(((CommandTail *)0)->ctBuffer) + 1,
                          command_tail);
      UWORD fcbcode;

      command_psp=fcom_create_process(tail,mode & LOAD_HIGH,
                                      internal_data->cu_psp,
                                      child_env_mcb + 1);
      if (command_psp == 0) {
        DosMemFree(child_env_mcb);
        return DE_NOMEM;
      }

      /*
       * A native COMMAND is still an ordinary EXEC child.  patchPSP()
       * installs the exact caller-supplied command tail and FCBs, transfers
       * environment ownership, applies SETVER, and sets the canonical MCB
       * process name.  The first process-model pass skipped this entire
       * loader stage.
       */
      fcbcode=patchPSP(command_psp - 1,child_env_mcb,ep,lp);

      /* tail уже скопирован в PSP:80h ребёнка; patchPSP() отдельно
         установил caller-supplied FCB1/FCB2 из EXEC parameter block.
         Слот SDA_EXEC_TAIL_OFF с этого момента свободен для любых
         вложенных EXEC. */
      return exec_run_native_command(command_psp,fcbcode);
    }
  }
  
  if ((mode & 0x7f) > EXEC_OVERLAY || (mode & 0x7f) == 2)
    return DE_INVLDFMT;

  memcpy(&TempExeBlock, ep, sizeof(exec_blk));

  /* Same as ChildEnv()/truename() above: "lp" is ARM_PTR(guest DS:DX) from
     INT 21h/AH=4Bh, so it lives in the CALLER's segment (FreeCOM's PSP for an
     external command), not DOS_PSP. Re-anchoring it on DOS_PSP produces a
     bogus offset and DosOpenSft() below then fails to open the executable. */
  x86_lp = linear_to_far((const BYTE *) lp);
  dos_far_ptr x86_dhp = IsDevice(lp);
  if (EFFECTIVE(x86_dhp) ||           /* don't try to "execute" e.g. C:\NUL */
      (openresult = DosOpenSft(x86_lp, O_LEGACY | O_OPEN | O_RDONLY, 0)) < SUCCESS) {
    dpb_watch_check_chain("DosExec err");
    return DE_FILENOTFND;
  }
  dpb_watch_check_chain("DosExec");
  fd = (COUNT) (openresult & 0xffff);

  rc = (int) DosRWSft(fd, sizeof(exe_header),
                      x86_FAR_PTR(DOS_PSP, &ExeHeader) /* -> exe_header */,
                      XFR_READ);

  if (rc == sizeof(exe_header) &&
      (ExeHeader.exSignature == MAGIC || ExeHeader.exSignature == OLD_MAGIC))
    rc = DosExeLoader(lp, &TempExeBlock, mode, fd);
  else if (rc >= 4 &&
           ((const UBYTE *)&ExeHeader)[0] == 0x7f &&
           ((const UBYTE *)&ExeHeader)[1] == 'E' &&
           ((const UBYTE *)&ExeHeader)[2] == 'L' &&
           ((const UBYTE *)&ExeHeader)[3] == 'F')
    rc = DosArmElfLoader(&TempExeBlock, mode, fd, lp);
  else if (rc != 0)
    rc = DosComLoader(lp, &TempExeBlock, mode, fd);
  else
  {
    DosCloseSft(fd, FALSE);
    return DE_INVLDFMT;
  }

  if (mode == EXEC_LOAD && rc == SUCCESS)
    memcpy(ep, &TempExeBlock, sizeof(exec_blk));

  return rc;
}

/* res_DosExec() - what a running process calls to EXEC a child
   (P_0() below, for the shell). Upstream's version is a tiny asm
   shim that sets AH=4Bh and self-issues "int 21h" (DOS recursively
   calling its own handler - the same trick init_DosExec() used to
   rely on for CONFIG.SYS's DEVICE= loading, before DosExec() became
   directly callable - see config.c). There's no need for that
   indirection here: DosExec() is an ordinary C function. */
COUNT res_DosExec(COUNT mode, exec_blk * ep, BYTE * lp)
{
  COUNT res = DosExec(mode, ep, lp);
  dpb_watch_check_chain("res_DosExec");
  return res;
}

/* start process 0 (the shell) */
VOID P_0(CPU * cpu_, struct config FAR *Config)
{
  for ( ; ; )   /* endless shell load loop - reboot or shut down to exit it! */
  {
#if GUEST_SHELL
  BYTE *tailp, *endp;
  exec_blk exb;
  UBYTE mode = Config->cfgP_0_startmode;

  /* build exec block and save all parameters here as init part will vanish! */
  exb.exec.fcb_1 = exb.exec.fcb_2 = MK_FP(0xffff, 0xffff);  /* "no FCBs" - see
                                                                far_is_end()/
                                                                patchPSP() */
  exb.exec.env_seg = DOS_PSP + 8;
  fstrcpy(Shell, Config->cfgInit);
  /* join name and tail */
  fstrcpy(Shell + strlen(Shell), Config->cfgInitTail);
  endp =  Shell + strlen(Shell);

    BYTE *p;
    /* if there are no parameters, point to end without "\r\n" */
    if((tailp = strchr(Shell,'\t')) == NULL &&
       (tailp = strchr(Shell, ' ')) == NULL)
        tailp = endp - 2;
    /* shift tail to right by 2 to make room for '\0', ctCount */
    for (p = endp - 1; p >= tailp; p--)
      *(p + 2) = *p;
    /* terminate name and tail */
    *tailp =  *(endp + 2) = '\0';
    /* ctCount: just past '\0' do not count the "\r\n" */
    {
      CommandTail *ct = (CommandTail *)(tailp + 1);
      ct->ctCount = endp - tailp - 2;
      exb.exec.cmd_line = x86_FAR_PTR(DOS_PSP, ct) /* -> CommandTail */;
    }
    CfgDbgPrintf(("EXEC file='%s' tail='%s'\n", Shell, tailp + 2));
    res_DosExec(mode, &exb, Shell);
    /* only reached once the shell terminates (or couldn't be
       started at all) - matches upstream: P_0's loop always falls
       through here and reprompts, exactly like real DOS does if
       COMMAND.COM itself exits. */
    put_string("Bad or missing Command Interpreter: "); /* failure _or_ exit */
    put_string(Shell);
    put_string(tailp + 2);
    put_string(" Enter the full shell command line: ");
    endp = Shell + res_read(cpu_, STDIN, x86_FAR_PTR(DOS_PSP, Shell) /* -> char[] */, NAMEMAX);
    *endp = '\0';                             /* terminate string for strchr */
#else
    /*
     * One fcom_run() call represents one COMMAND process lifetime.
     * If it exits, recreate process 0 just as the original P_0 loop
     * recreated COMMAND.COM.
     * Config->cfgP_0_startmode - ignored, try HMA/UMB anytime
     */
    fcom_run(cpu_, (const char *)Config->cfgInitTail, 0x80, DOS_PSP + 8, 0);
    put_string("Native COMMAND.COM restarting as process #0 with parameters: ");
    put_string((BYTE *)Config->cfgInitTail);
#endif
  }
  __unreachable();
}
