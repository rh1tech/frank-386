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
#include "../core0_stack.h"
#include "../../apps/api/ez.h"
#include "../mem.h"
#include "mcb_proxy.h"
#include "kernel_guest_proxy.h"

#if DIAG
extern volatile uint32_t dos_diag_kernel_code;
#endif
/* board_config.h also publishes the PSRAM base.  Its compatibility alias is
 * guarded, so it can coexist with the FreeDOS-side definition from portab.h. */
#include "../board_config.h"
#include "sdcard.h"
#include "psram_layout.h"

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
size_t psram_usable_size(void);

static BOOL arm_native_runtime_available(void)
{
  /* Native ARM code/data addresses must remain stable for the lifetime of
     the process. Only physically detected QSPI PSRAM provides that model;
     SPI/SWAP guest paging deliberately has no stable native aliases. */
  return psram_usable_size() >= (1u << 20);
}

/* The ARM native loader is the sole remaining consumer of a stable host
   alias for DOS guest memory.  Callers must already have passed
   arm_native_runtime_available(); SPI/SWAP paging never reaches this helper.
   Keep the conversion centralized so generic FDOS code cannot accidentally
   reintroduce persistent ARM_PTR() aliases. */
#define arm_native_guest_ptr(p) ((void*)ARM_PTR(p))

/* Native-yield IRQ trampoline uses the same callback trap mechanism as
   bios_intcall(), but deliberately does not execute the suspended parent
   CS:IP. */
bool set_bios_callback(CPU *cpu, bios_callback_params_t *params, bool reenter);
bool drop_bios_callback(CPU *cpu, bios_callback_params_t *params);
bool cpu_pending_trap(void);
void cpu_pending_trap_set(bool v);

/* SecPathBuffer remains a DOS guest scratch area.  Never expose it as a
   native pointer: in SPI/SWAP paging an ARM_PTR alias is only valid for the
   currently mapped cache span. */
static const uint32_t task_sec_path_linear =
    ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF +
    offsetof(struct dos_data, SecPathBuffer);

static inline dos_far_ptr task_sec_path_far(size_t off)
{
  return MK_FP(DOS_PSP,
               (UWORD)(X86_INTERNAL_DATA_OFF +
                       offsetof(struct dos_data, SecPathBuffer) + off));
}

static inline UBYTE task_sec_read8(size_t off)
{
  return pload8(task_sec_path_linear + (uint32_t)off);
}

static inline UWORD task_sec_read16(size_t off)
{
  return pload16(task_sec_path_linear + (uint32_t)off);
}

#define EXE_U16(member) task_sec_read16(offsetof(exe_header, member))

/* The EXEC parameter block is native working state passed between native C
   routines.  It is deliberately not stored in pageable guest memory. */
static exec_blk TempExeBlock;

/* Two guest UWORDs immediately after the EXE header are used as relocation
   input scratch. */
#define TASK_RELOC_OFF (sizeof(exe_header))

#define DEVLOAD_CHUNK_PARAS (32256 / 16)       /* also used by EXEC_OVERLAY loads */
#define CHUNK           32256                  /* bytes per DosExeLoader() read */
#define MAXENV          32768u
#define ENV_KEEPFREE    0x83   /* sizeof(PriPathBuffer)+3: 2 bytes "extra
                                   strings" count, 0x80 bytes max absolute
                                   filename, 1 byte '\0' - see ChildEnv() */

_Static_assert(sizeof(((struct dos_data *) 0)->PriPathBuffer) + 3 == ENV_KEEPFREE,
               "ENV_KEEPFREE must track sizeof(PriPathBuffer)+3, see ChildEnv()");

#define LOAD_HIGH 0x80          /* mode bit: try UMB first (see DosUmbLink()) */

static inline uint32_t task_guest_linear(dos_far_ptr p)
{
  return ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
}

static inline uint32_t task_guest_seg_linear(UWORD seg)
{
  return (uint32_t)seg << 4;
}

static inline void task_guest_read(uint32_t addr, void *dst, size_t len)
{
  guest_read_block(addr, dst, len);
}

static inline void task_guest_write(uint32_t addr, const void *src, size_t len)
{
  guest_write_block(addr, src, len);
}

static inline dos_far_ptr task_guest_read_far(uint32_t addr)
{
  dos_far_ptr v;
  v.offset = pload16(addr);
  v.segment = pload16(addr + 2u);
  return v;
}

static inline void task_guest_write_far(uint32_t addr, dos_far_ptr v)
{
  pstore16(addr, v.offset);
  pstore16(addr + 2u, v.segment);
}

static const uint32_t task_idata_linear =
    ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF;
static const uint32_t task_lol_linear =
    ((uint32_t)DOS_PSP << 4) + 0x08f0u;

static inline UBYTE task_idata_read8(size_t off)
{
  return pload8(task_idata_linear + (uint32_t)off);
}

static inline void task_idata_write8(size_t off, UBYTE v)
{
  pstore8(task_idata_linear + (uint32_t)off, v);
}

static inline UWORD task_idata_read16(size_t off)
{
  return pload16(task_idata_linear + (uint32_t)off);
}

static inline void task_idata_write16(size_t off, UWORD v)
{
  pstore16(task_idata_linear + (uint32_t)off, v);
}

static inline dos_far_ptr task_idata_read_far(size_t off)
{
  return task_guest_read_far(task_idata_linear + (uint32_t)off);
}

static inline void task_idata_write_far(size_t off, dos_far_ptr v)
{
  task_guest_write_far(task_idata_linear + (uint32_t)off, v);
}

static inline UBYTE task_far_peek8(dos_far_ptr p, UWORD add)
{
  UWORD off = (UWORD)(FP_OFF(p) + add);
  return pload8(((uint32_t)FP_SEG(p) << 4) + off);
}

static int task_guest_is_command_com(dos_far_ptr name)
{
  UWORD pos = 0, base = 0;
  static const char command[] = "COMMAND.COM";
  unsigned i;

  for (;;) {
    UBYTE c = task_far_peek8(name, pos);
    if (c == 0)
      break;
    if (c == ':' || c == '/' || c == '\\')
      base = (UWORD)(pos + 1u);
    ++pos;
  }

  if ((UWORD)(pos - base) != (UWORD)(sizeof(command) - 1u))
    return 0;

  for (i = 0; i < sizeof(command) - 1u; ++i) {
    UBYTE c = task_far_peek8(name, (UWORD)(base + i));
    if (c >= 'a' && c <= 'z')
      c = (UBYTE)(c - ('a' - 'A'));
    if (c != (UBYTE)command[i])
      return 0;
  }
  return 1;
}

static dos_far_ptr task_guest_is_device(dos_far_ptr name)
{
  UWORD pos = 0, root = 0;
  dos_far_ptr dh;

  for (;;) {
    UBYTE c = task_far_peek8(name, pos);
    if (c == 0)
      break;
    if (c == ':' || c == '/' || c == '\\')
      root = (UWORD)(pos + 1u);
    ++pos;
  }

  {
    UBYTE c0 = task_far_peek8(name, root);
    if (c0 == 0)
      return MK_FP(0, 0);
    if (c0 == '.') {
      UBYTE c1 = task_far_peek8(name, (UWORD)(root + 1u));
      UBYTE c2 = task_far_peek8(name, (UWORD)(root + 2u));
      if (c1 == 0 || (c1 == '.' && c2 == 0))
        return MK_FP(0, 0);
    }
  }

  dh = MK_FP(DOS_PSP,
             (UWORD)(0x08f0u + offsetof(struct lol, nul_dev)));

  while (!far_is_end(dh))
  {
    struct dhdr h;
    int i;
    task_guest_read(task_guest_linear(dh), &h, sizeof(h));

    if (h.dh_attr & ATTR_CHAR)
    {
      for (i = 0; i < FNAME_SIZE; ++i)
      {
        UBYTE c1 = task_far_peek8(name, (UWORD)(root + i));
        if (c1 == '.' || c1 == 0)
        {
          for (; i < FNAME_SIZE; ++i)
          {
            UBYTE c2 = h.dh_name[i];
            if (c2 != ' ' && c2 != 0)
              break;
          }
          break;
        }
        if (DosUpFChar(c1) != DosUpFChar(h.dh_name[i]))
          break;
      }
      if (i == FNAME_SIZE)
        return dh;
    }

    dh = h.dh_next;
  }

  return MK_FP(0, 0);
}


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
#define STT_OBJECT              1u
#define STT_FUNC                2u
#define STT_SECTION             3u
#define R_ARM_ABS32             2u
#define R_ARM_REL32             3u
#define R_ARM_THM_PC22          10u
#define R_ARM_THM_JUMP24        30u
#define R_ARM_THM_ALU_ABS_G0_NC 102u
#define DOS_API_VERSION         21
#define ARM_ELF_DEFAULT_NATIVE_STACK_SIZE 4096u
#define ARM_ELF_MIN_DOS_STACK_SIZE        4096u
#define ARM_ELF_DEFAULT_DOS_STACK_SIZE    ARM_ELF_MIN_DOS_STACK_SIZE
#define ARM_ELF_ARGV_SLOTS      66u
#define ARM_ELF_ARG_TEXT_SIZE   (NAMEMAX + sizeof(((CommandTail *)0)->ctBuffer) + 2u)
#define ARM_ELF_ARG_AREA_SIZE   (ARM_ELF_ARGV_SLOTS * sizeof(ULONG) + ARM_ELF_ARG_TEXT_SIZE)
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

typedef struct arm_elf_sec_chunk {
  ULONG next_addr;          /* native address of the next descriptor, or 0 */
  ULONG logical_start;      /* section-relative first byte in this block */
  ULONG logical_end;        /* section-relative byte just past this block */
  ULONG data_addr;          /* native base of descriptor/allocation */
  ULONG data_off;           /* byte offset from data_addr to payload */
  UWORD mcb_seg;            /* DOS MCB segment, 0 for application heap */
  UWORD data_seg;           /* DOS data segment, 0 for application heap */
} arm_elf_sec_chunk;

typedef struct arm_app_heap_context {
  ULONG begin;
  ULONG end;
  UBYTE ready;
  UBYTE reserved[3];
} arm_app_heap_context;

typedef struct arm_elf_sec_state {
  ULONG offset;             /* primary-block offset for logical section 0 */
  ULONG size;
  ULONG primary_size;       /* bytes kept in the primary process block */
  ULONG extra_chunks;       /* native arm_elf_sec_chunk* linked list */
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
  ULONG entry_addr;
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
  UWORD extra_block_count;
  ULONG extra_block_bytes;
  UBYTE allocation_high;
  UBYTE allocation_reserved[3];
  native_ez_process_info process_info;
  arm_app_heap_context app_heap;
} arm_elf_load_meta;
#pragma pack(pop)

typedef struct arm_ez_load_meta {
  ULONG entry_addr;
  ULONG argv_addr;
  ULONG native_stack_addr;
  ULONG native_stack_size;
  ULONG dos_stack_size;
  UWORD dos_stack_mcb;
  UWORD dos_stack_seg;
  UWORD argc;
  native_ez_process_info process_info;
  arm_app_heap_context app_heap;
} arm_ez_load_meta;

static const native_ez_process_info *arm_ez_active_process_info;
static arm_app_heap_context *arm_app_active_heap;
static int arm_native_process_is_active(void);

const native_ez_process_info *arm_ez_get_process_info(void)
{
  return arm_ez_active_process_info;
}

/*
 * Process-local native application heap.  The heap context belongs to the
 * EXEC child metadata, so nested native EXEC saves/restores allocator state
 * together with the active process.
 *
 * Allocation policy is unchanged from the former apps/api implementation:
 * application PSRAM first, ordinary DOS allocation second.
 */
#define ARM_APP_HEAP_ALIGN       16u
#define ARM_APP_BLOCK_FREE       1u
#define ARM_APP_BLOCK_MAGIC      0x5053524du /* "PSRM" */
#define ARM_APP_MIN_PAYLOAD      16u

typedef struct arm_app_heap_block {
  ULONG size_flags;
  ULONG prev_size;
  ULONG magic;
  ULONG reserved;
} arm_app_heap_block;

_Static_assert(sizeof(arm_app_heap_block) == ARM_APP_HEAP_ALIGN,
               "native application heap header alignment");

/* QSPI L2 is a discardable owner of the free PSRAM tail.  Native application
 * memory uses in-band heap headers, so the first implementation only raises
 * this high-water mark.  It never reclaims native heap space on free; that
 * conservative policy avoids exposing stale cache lines over live allocator
 * metadata and can be relaxed later when the heap moves to out-of-band extents. */
static uintptr_t arm_ff_qspi_high_water;

static void arm_ff_qspi_claim_until(uintptr_t end)
{
  if (end <= arm_ff_qspi_high_water)
    return;
  arm_ff_qspi_high_water = end;
  sdcard_ff_qspi_cache_set_floor(SDCARD_FF_QSPI_OWNER_NATIVE,
                                  (void *)end);
}

static uintptr_t arm_app_align_up(uintptr_t value)
{
  return (value + (ARM_APP_HEAP_ALIGN - 1u)) &
         ~(uintptr_t)(ARM_APP_HEAP_ALIGN - 1u);
}

static ULONG arm_app_block_size(const arm_app_heap_block *block)
{
  return block->size_flags & ~ARM_APP_BLOCK_FREE;
}

static int arm_app_block_is_free(const arm_app_heap_block *block)
{
  return (block->size_flags & ARM_APP_BLOCK_FREE) != 0;
}

static uintptr_t arm_app_heap_begin(void)
{
  return arm_app_active_heap != NULL ? (uintptr_t)arm_app_active_heap->begin : 0;
}

static uintptr_t arm_app_heap_end(void)
{
  return arm_app_active_heap != NULL ? (uintptr_t)arm_app_active_heap->end : 0;
}

static int arm_app_heap_init(void)
{
  uintptr_t begin;
  uintptr_t end;
  arm_app_heap_block *first;

  if (arm_app_active_heap == NULL)
    return FALSE;
  if (arm_app_active_heap->ready)
    return arm_app_active_heap->begin != 0;

  arm_app_active_heap->ready = TRUE;
  begin = arm_app_align_up((uintptr_t)arm_app_active_heap->begin);
  end = (uintptr_t)arm_app_active_heap->end &
        ~(uintptr_t)(ARM_APP_HEAP_ALIGN - 1u);

  if (begin == 0 || end <= begin ||
      end - begin < sizeof(arm_app_heap_block) + ARM_APP_MIN_PAYLOAD)
    return FALSE;

  arm_app_active_heap->begin = (ULONG)begin;
  arm_app_active_heap->end = (ULONG)end;
  first = (arm_app_heap_block *)begin;
  arm_ff_qspi_claim_until(begin + sizeof(*first));
  first->size_flags = (ULONG)(end - begin) | ARM_APP_BLOCK_FREE;
  first->prev_size = 0;
  first->magic = ARM_APP_BLOCK_MAGIC;
  first->reserved = 0;
  return TRUE;
}

static arm_app_heap_block *arm_app_next(arm_app_heap_block *block)
{
  uintptr_t next = (uintptr_t)block + arm_app_block_size(block);
  return next < arm_app_heap_end() ? (arm_app_heap_block *)next : NULL;
}

static arm_app_heap_block *arm_app_prev(arm_app_heap_block *block)
{
  uintptr_t begin = arm_app_heap_begin();

  if (block->prev_size == 0 ||
      (uintptr_t)block < begin + block->prev_size)
    return NULL;
  return (arm_app_heap_block *)((uintptr_t)block - block->prev_size);
}

static int arm_app_block_valid(const arm_app_heap_block *block)
{
  uintptr_t begin = arm_app_heap_begin();
  uintptr_t end = arm_app_heap_end();
  uintptr_t p = (uintptr_t)block;
  ULONG size;

  if (begin == 0 || p < begin || p >= end ||
      block->magic != ARM_APP_BLOCK_MAGIC)
    return FALSE;

  size = arm_app_block_size(block);
  return size >= sizeof(arm_app_heap_block) + ARM_APP_MIN_PAYLOAD &&
         (size & (ARM_APP_HEAP_ALIGN - 1u)) == 0 &&
         size <= end - p;
}

static void arm_app_fix_next_prev(arm_app_heap_block *block)
{
  arm_app_heap_block *next = arm_app_next(block);
  if (next != NULL)
    next->prev_size = arm_app_block_size(block);
}

static void arm_app_split_allocated(arm_app_heap_block *block, ULONG wanted)
{
  ULONG old_size = arm_app_block_size(block);
  ULONG remainder = old_size - wanted;

  /* The final free block's payload may currently be occupied by discardable
   * QSPI cache lines.  Shrink the cache before writing a new tail header or
   * returning any part of that payload to the native application. */
  if (arm_app_next(block) == NULL)
    arm_ff_qspi_claim_until((uintptr_t)block + wanted + sizeof(*block));

  if (remainder >= sizeof(arm_app_heap_block) + ARM_APP_MIN_PAYLOAD) {
    arm_app_heap_block *tail =
        (arm_app_heap_block *)((uintptr_t)block + wanted);

    block->size_flags = wanted;
    tail->size_flags = remainder | ARM_APP_BLOCK_FREE;
    tail->prev_size = wanted;
    tail->magic = ARM_APP_BLOCK_MAGIC;
    tail->reserved = 0;
    arm_app_fix_next_prev(tail);
  } else {
    block->size_flags = old_size;
    arm_app_fix_next_prev(block);
  }
}

static void *arm_app_psram_malloc(size_t size)
{
  uintptr_t cursor;
  ULONG wanted;

  if (size == 0)
    size = 1;
  if (!arm_app_heap_init() ||
      size > 0xfffffffful - sizeof(arm_app_heap_block) -
             (ARM_APP_HEAP_ALIGN - 1u))
    return NULL;

  wanted = (ULONG)(size + sizeof(arm_app_heap_block) +
                   (ARM_APP_HEAP_ALIGN - 1u)) &
           ~(ARM_APP_HEAP_ALIGN - 1u);

  for (cursor = arm_app_heap_begin(); cursor < arm_app_heap_end(); ) {
    arm_app_heap_block *block = (arm_app_heap_block *)cursor;

    if (!arm_app_block_valid(block))
      return NULL;
    if (arm_app_block_is_free(block) &&
        arm_app_block_size(block) >= wanted) {
      arm_app_split_allocated(block, wanted);
      return (void *)(block + 1);
    }
    cursor += arm_app_block_size(block);
  }
  return NULL;
}

static size_t arm_app_psram_largest(void)
{
  uintptr_t cursor;
  size_t largest = 0;

  if (!arm_app_heap_init())
    return 0;

  for (cursor = arm_app_heap_begin(); cursor < arm_app_heap_end(); ) {
    arm_app_heap_block *block = (arm_app_heap_block *)cursor;
    ULONG total;

    if (!arm_app_block_valid(block))
      return 0;
    total = arm_app_block_size(block);
    if (arm_app_block_is_free(block) && total > sizeof(*block)) {
      size_t payload = total - sizeof(*block);
      if (payload > largest)
        largest = payload;
    }
    cursor += total;
  }
  return largest;
}

static int arm_app_psram_owns(const void *ptr)
{
  uintptr_t p;

  if (ptr == NULL || !arm_app_heap_init())
    return FALSE;
  p = (uintptr_t)ptr;
  return p >= arm_app_heap_begin() + sizeof(arm_app_heap_block) &&
         p < arm_app_heap_end();
}

static void arm_app_psram_free(void *ptr)
{
  arm_app_heap_block *block = ((arm_app_heap_block *)ptr) - 1;
  arm_app_heap_block *next;
  arm_app_heap_block *prev;
  ULONG size;

  if (!arm_app_block_valid(block) || arm_app_block_is_free(block))
    return;

  size = arm_app_block_size(block);
  block->size_flags = size | ARM_APP_BLOCK_FREE;

  next = arm_app_next(block);
  if (next != NULL && arm_app_block_valid(next) &&
      arm_app_block_is_free(next)) {
    block->size_flags =
        (size + arm_app_block_size(next)) | ARM_APP_BLOCK_FREE;
    size = arm_app_block_size(block);
    arm_app_fix_next_prev(block);
  }

  prev = arm_app_prev(block);
  if (prev != NULL && arm_app_block_valid(prev) &&
      arm_app_block_is_free(prev)) {
    prev->size_flags =
        (arm_app_block_size(prev) + size) | ARM_APP_BLOCK_FREE;
    arm_app_fix_next_prev(prev);
  }
}

static void *arm_app_psram_realloc(void *ptr, size_t size)
{
  arm_app_heap_block *block = ((arm_app_heap_block *)ptr) - 1;
  arm_app_heap_block *next;
  ULONG old_total;
  ULONG wanted;

  if (!arm_app_block_valid(block) || arm_app_block_is_free(block) ||
      size > 0xfffffffful - sizeof(*block) - (ARM_APP_HEAP_ALIGN - 1u))
    return NULL;

  old_total = arm_app_block_size(block);
  wanted = (ULONG)(size + sizeof(*block) + (ARM_APP_HEAP_ALIGN - 1u)) &
           ~(ARM_APP_HEAP_ALIGN - 1u);

  if (wanted <= old_total) {
    arm_app_split_allocated(block, wanted);
    return ptr;
  }

  next = arm_app_next(block);
  if (next != NULL && arm_app_block_valid(next) &&
      arm_app_block_is_free(next) &&
      old_total + arm_app_block_size(next) >= wanted) {
    block->size_flags = old_total + arm_app_block_size(next);
    arm_app_split_allocated(block, wanted);
    return ptr;
  }
  return NULL;
}

static UWORD arm_app_dos_segment(const void *ptr)
{
  uintptr_t guest_base = (uintptr_t)X86_RAM_BASE;
  uintptr_t address = (uintptr_t)ptr;
  uintptr_t linear;

  if (address < guest_base)
    return 0;
  linear = address - guest_base;
  if ((linear & 15u) != 0 || linear > 0x000ffff0ul)
    return 0;
  return (UWORD)(linear >> 4);
}

static ULONG arm_app_dos_block_size(UWORD segment)
{
  seg mseg;
  BYTE type;

  if (segment == 0)
    return 0;
  mseg = (seg)(segment - 1);
  type = fdos_mcb_type(mseg);
  if ((type != MCB_NORMAL && type != MCB_LAST) ||
      fdos_mcb_owner(mseg) != fdos_dos_cu_psp())
    return 0;
  return (ULONG)fdos_mcb_size(mseg) << 4;
}

static void *arm_app_dos_malloc(size_t size)
{
  ULONG paras_long;
  UWORD mcb_seg = 0;
  UWORD largest = 0;

  if (size == 0)
    size = 1;
  if (size > 0x000ffff0ul)
    return NULL;

  paras_long = ((ULONG)size + 15u) >> 4;
  if (paras_long == 0 || paras_long > 0xffffu)
    return NULL;

  if (DosMemAlloc((UWORD)paras_long, fdos_dos_mem_access_mode(),
                  &mcb_seg, &largest) != SUCCESS)
    return NULL;
  return arm_native_guest_ptr(MK_FP((UWORD)(mcb_seg + 1), 0));
}

void *arm_native_app_malloc(size_t size)
{
  void *ptr;
  if (!arm_native_runtime_available())
    return NULL;
  ptr = arm_app_psram_malloc(size);
  return ptr != NULL ? ptr : arm_app_dos_malloc(size);
}

void arm_native_app_free(void *ptr)
{
  UWORD segment;

  if (!arm_native_runtime_available())
    return;
  if (ptr == NULL)
    return;
  if (arm_app_psram_owns(ptr)) {
    arm_app_psram_free(ptr);
    return;
  }

  segment = arm_app_dos_segment(ptr);
  if (arm_app_dos_block_size(segment) != 0)
    (void)DosMemFree((UWORD)(segment - 1));
}

void *arm_native_app_calloc(size_t count, size_t size)
{
  size_t total;
  void *ptr;

  if (count != 0 && size > (size_t)-1 / count)
    return NULL;
  total = count * size;
  ptr = arm_native_app_malloc(total);
  if (ptr != NULL)
    nf_memset(ptr, 0, total);
  return ptr;
}

void *arm_native_app_realloc(void *ptr, size_t size)
{
  UWORD segment;
  if (!arm_native_runtime_available())
    return NULL;
  ULONG old_size;
  void *new_ptr;

  if (ptr == NULL)
    return arm_native_app_malloc(size);
  if (size == 0) {
    arm_native_app_free(ptr);
    return NULL;
  }

  if (arm_app_psram_owns(ptr)) {
    arm_app_heap_block *block = ((arm_app_heap_block *)ptr) - 1;
    size_t old_payload;

    if (!arm_app_block_valid(block) || arm_app_block_is_free(block))
      return NULL;
    old_payload = arm_app_block_size(block) - sizeof(*block);

    new_ptr = arm_app_psram_realloc(ptr, size);
    if (new_ptr != NULL)
      return new_ptr;

    new_ptr = arm_app_dos_malloc(size);
    if (new_ptr == NULL)
      return NULL;
    dos_api_memcpy(new_ptr, ptr, old_payload < size ? old_payload : size);
    arm_app_psram_free(ptr);
    return new_ptr;
  }

  segment = arm_app_dos_segment(ptr);
  old_size = arm_app_dos_block_size(segment);
  if (old_size == 0)
    return NULL;

  if (size <= 0x000ffff0ul) {
    ULONG paras_long = ((ULONG)size + 15u) >> 4;

    if (paras_long != 0 && paras_long <= 0xffffu) {
      UWORD old_paras = (UWORD)(old_size >> 4);

      if (DosMemChange(segment, (UWORD)paras_long, NULL) == SUCCESS)
        return ptr;

      /* DosMemChange may join following free MCBs before reporting failure. */
      if (DosMemChange(segment, old_paras, NULL) != SUCCESS)
        return NULL;
    }
  }

  new_ptr = arm_native_app_malloc(size);
  if (new_ptr == NULL)
    return NULL;
  dos_api_memcpy(new_ptr, ptr, old_size < size ? old_size : size);
  arm_native_app_free(ptr);
  return new_ptr;
}

size_t arm_native_app_malloc_largest(void)
{
  UWORD paragraphs = 0;
  if (!arm_native_runtime_available())
    return 0;
  size_t psram_largest = arm_app_psram_largest();
  size_t dos_largest = 0;

  if (DosMemLargest(&paragraphs) == SUCCESS)
    dos_largest = (size_t)paragraphs << 4;
  return psram_largest > dos_largest ? psram_largest : dos_largest;
}

/* Native ARM loaders are runtime-guarded by arm_native_runtime_available().
   Their metadata scratch remains in SecPathBuffer; only after the QSPI guard
   may it be exposed as a stable native alias. */
#define ELF_SCRATCH_FAR MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, PriPathBuffer)))
#define ElfScratch ((BYTE *)arm_native_guest_ptr(ELF_SCRATCH_FAR))

static int arm_elf_read_meta(COUNT fd, ULONG file_off, void *dst, UWORD len)
{
  LONG pos = SftSeek(fd, (LONG)file_off, SEEK_SET);
  LONG got;

  if (pos < 0)
    return DE_INVLDFMT;
  got = DosRWSft(fd, len,
                 ELF_SCRATCH_FAR, XFR_READ);
  if (got != len)
    return DE_INVLDFMT;
  dos_api_memcpy(dst, ElfScratch, len);
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

static void arm_elf_loader_progress_end(arm_elf_load_meta *meta);
STATIC int ExecMemLargest(UWORD *asize, UWORD threshold);
STATIC int ExecMemAlloc(UWORD size, seg *para, UWORD *asize);

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

/* DosRWSft() accepts a guest far pointer, while application-heap storage is a
   native PSRAM address.  Stage small pieces through the existing synchronous
   ELF scratch buffer and copy them into the native destination. */
#define ARM_ELF_NATIVE_READ_CHUNK 64u

static int arm_elf_read_native_section(COUNT fd, void *dst,
                                       ULONG file_off, ULONG size)
{
  BYTE *out = (BYTE *)dst;
  ULONG done = 0;
  LONG pos = SftSeek(fd, (LONG)file_off, SEEK_SET);

  if (pos < 0)
    return DE_INVLDFMT;

  while (done < size) {
    UWORD chunk = (UWORD)min((ULONG)ARM_ELF_NATIVE_READ_CHUNK, size - done);
    LONG got = DosRWSft(fd, chunk,
                        ELF_SCRATCH_FAR, XFR_READ);
    if (got != chunk)
      return DE_INVLDFMT;
    dos_api_memcpy(out + done, ElfScratch, chunk);
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

static COUNT arm_ez_reject(COUNT rc, const char *reason)
{
  dos_printf("ARM EZ: %s\r\n", reason);
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

/* Allocate from one DOS arena only.  Normal ARM ELF loading consumes LOW
   first and switches to UMB only when the next whole symbol no longer fits;
   LOADHIGH keeps the inverse preference (UMB first, then LOW). */
static int arm_elf_alloc_largest_pool(UBYTE use_umb,
                                      UWORD *mcb_seg, UWORD *paras)
{
  UBYTE umb_state = fdos_lol_uppermem_link();
  UBYTE mem_access = fdos_dos_mem_access_mode();
  int rc;

  if (use_umb) {
    DosUmbLink(1);
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0x80u));
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() | 0x40u)); /* UMB-only, as in ExecMemLargest */
  } else {
    DosUmbLink(0);
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0xc0u)); /* conventional memory only */
  }

  rc = DosMemLargest(paras);
  if (rc == SUCCESS && *paras != 0)
    rc = DosMemAlloc(*paras, fdos_dos_mem_access_mode(), mcb_seg, paras);
  else if (rc == SUCCESS)
    rc = DE_NOMEM;

  DosUmbLink(umb_state);
  fdos_dos_set_mem_access_mode(mem_access);
  return rc;
}

static void arm_elf_pool_free_stats(UBYTE use_umb,
                                    ULONG *total_bytes, ULONG *largest_bytes)
{
  UBYTE umb_state = fdos_lol_uppermem_link();
  seg pseg;
  ULONG total = 0;
  ULONG largest = 0;
  ULONG guard = 0;

  if (use_umb) {
    if (fdos_lol_uppermem_root() == 0xffffu) {
      *total_bytes = 0;
      *largest_bytes = 0;
      return;
    }
    DosUmbLink(1);
    pseg = fdos_lol_uppermem_root();
  } else {
    DosUmbLink(0);
    pseg = fdos_lol_first_mcb();
  }

  for (;;) {
    const UWORD size = fdos_mcb_size(pseg);
    const BYTE type = fdos_mcb_type(pseg);
    if (size == 0xffffu ||
        (type != MCB_NORMAL && type != MCB_LAST))
      break;
    if (fdos_mcb_owner(pseg) == FREE_PSP) {
      ULONG bytes = (ULONG)size << 4;
      if (bytes > largest)
        largest = bytes;
      if (total <= 0xfffffffful - bytes)
        total += bytes;
      else
        total = 0xfffffffful;
    }
    if (type == MCB_LAST)
      break;
    pseg = (seg)(pseg + size + 1u);
    if (++guard > 0xfffful)
      break;
  }

  DosUmbLink(umb_state);
  *total_bytes = total;
  *largest_bytes = largest;
}

static ULONG arm_elf_chunk_payload_off_native(uintptr_t base,
                                              ULONG logical_start,
                                              ULONG align)
{
  ULONG off = (ULONG)sizeof(arm_elf_sec_chunk);

  if (align > 1) {
    ULONG rem = (ULONG)((base + off - logical_start) % align);
    if (rem != 0)
      off += align - rem;
  }
  return off;
}

static ULONG arm_elf_chunk_payload_off(UWORD data_seg, ULONG logical_start,
                                       ULONG align)
{
  return arm_elf_chunk_payload_off_native(
      (uintptr_t)arm_native_guest_ptr(MK_FP(data_seg, 0)), logical_start, align);
}

static int arm_elf_section_runtime_addr(UWORD base_seg,
                                        arm_elf_load_meta *meta,
                                        UWORD sec_num, ULONG logical_off,
                                        ULONG width, ULONG *addr)
{
  arm_elf_sec_state *states = arm_elf_states(meta);
  arm_elf_sec_state *state;
  ULONG chunk_addr;

  if (sec_num >= meta->shnum || addr == NULL)
    return DE_INVLDFMT;
  state = &states[sec_num];
  if (logical_off > state->size || width > state->size - logical_off)
    return DE_INVLDFMT;

  if (logical_off < state->primary_size &&
      width <= state->primary_size - logical_off) {
    *addr = (ULONG)(uintptr_t)arm_native_guest_ptr(
        arm_elf_guest_ptr(base_seg, state->offset + logical_off));
    return SUCCESS;
  }
  if (state->size == 0 && logical_off == 0 && width == 0) {
    *addr = (ULONG)(uintptr_t)arm_native_guest_ptr(
        arm_elf_guest_ptr(base_seg, state->offset));
    return SUCCESS;
  }

  chunk_addr = state->extra_chunks;
  while (chunk_addr != 0) {
    arm_elf_sec_chunk *chunk = (arm_elf_sec_chunk *)(uintptr_t)chunk_addr;
    if (logical_off >= chunk->logical_start &&
        logical_off < chunk->logical_end &&
        width <= chunk->logical_end - logical_off) {
      *addr = chunk->data_addr + chunk->data_off +
              logical_off - chunk->logical_start;
      return SUCCESS;
    }
    if (width == 0 && logical_off == state->size &&
        logical_off == chunk->logical_end && chunk->next_addr == 0) {
      *addr = chunk->data_addr + chunk->data_off +
              chunk->logical_end - chunk->logical_start;
      return SUCCESS;
    }
    chunk_addr = chunk->next_addr;
  }
  if (width == 0 && logical_off == state->size &&
      state->primary_size == state->size) {
    *addr = (ULONG)(uintptr_t)arm_native_guest_ptr(
        arm_elf_guest_ptr(base_seg, state->offset + state->size));
    return SUCCESS;
  }
  return DE_INVLDFMT;
}

static ULONG arm_elf_symbol_section_offset(const arm_elf32_sym *sym)
{
  /*
   * ELF32/ARM marks a Thumb function by setting bit 0 of st_value.  That bit
   * belongs to the callable function address, not to the byte offset/range
   * occupied by the symbol inside its section.
   */
  if ((sym->info & 0x0fu) == STT_FUNC)
    return sym->value & ~1ul;
  return sym->value;
}

static int arm_elf_safe_piece_end(COUNT fd, const arm_elf_load_meta *meta,
                                  UWORD sec_num, ULONG start, ULONG capacity,
                                  ULONG section_size, ULONG *piece_end)
{
  ULONG entsize = meta->symtab.entsize ? meta->symtab.entsize
                                        : sizeof(arm_elf32_sym);
  ULONG count;
  ULONG cut;
  int changed;

  if (entsize < sizeof(arm_elf32_sym) || piece_end == NULL)
    return DE_INVLDFMT;
  count = meta->symtab.size / entsize;
  cut = section_size - start < capacity ? section_size : start + capacity;

  do {
    ULONG i;
    changed = FALSE;
    for (i = 0; i < count; ++i) {
      arm_elf32_sym sym;
      ULONG sym_off;
      ULONG sym_end;
      int rc = arm_elf_read_symbol(fd, meta, i, &sym);
      if (rc != SUCCESS)
        return rc;
      if (sym.shndx != sec_num || sym.size == 0)
        continue;
      sym_off = arm_elf_symbol_section_offset(&sym);
      if (sym_off > section_size || sym.size > section_size - sym_off)
        return DE_INVLDFMT;
      sym_end = sym_off + sym.size;
      if (sym_off < start && sym_end > start)
        return DE_INVLDFMT;
      if (sym_off < cut && sym_end > cut) {
        if (sym_off < start)
          return DE_INVLDFMT;
        cut = sym_off;
        changed = TRUE;
      }
    }
  } while (changed);

  *piece_end = cut;
  return SUCCESS;
}

static int arm_elf_find_blocking_symbol(COUNT fd,
                                        const arm_elf_load_meta *meta,
                                        UWORD sec_num, ULONG logical_off,
                                        ULONG *sym_index,
                                        arm_elf32_sym *blocking)
{
  ULONG entsize = meta->symtab.entsize ? meta->symtab.entsize
                                        : sizeof(arm_elf32_sym);
  ULONG count;
  ULONG i;
  ULONG best_index = ARM_ELF_NO_SYMBOL;
  ULONG best_value = 0xfffffffful;
  arm_elf32_sym best;

  if (entsize < sizeof(arm_elf32_sym))
    return DE_INVLDFMT;
  count = meta->symtab.size / entsize;
  nf_memset(&best, 0, sizeof(best));

  for (i = 0; i < count; ++i) {
    arm_elf32_sym sym;
    ULONG sym_off;
    ULONG sym_end;
    int rc = arm_elf_read_symbol(fd, meta, i, &sym);
    if (rc != SUCCESS)
      return rc;
    if (sym.shndx != sec_num || sym.size == 0)
      continue;
    sym_off = arm_elf_symbol_section_offset(&sym);
    if (sym_off > 0xfffffffful - sym.size)
      return DE_INVLDFMT;
    sym_end = sym_off + sym.size;
    if (sym_off <= logical_off && sym_end > logical_off) {
      best = sym;
      best_index = i;
      break;
    }
    if (sym_off >= logical_off && sym_off < best_value) {
      best = sym;
      best_index = i;
      best_value = sym_off;
    }
  }

  if (best_index == ARM_ELF_NO_SYMBOL)
    return DE_NOMEM;
  if (sym_index != NULL)
    *sym_index = best_index;
  if (blocking != NULL)
    *blocking = best;
  return SUCCESS;
}

static void arm_elf_report_symbol_oom(COUNT fd, arm_elf_load_meta *meta,
                                      UWORD sec_num, const arm_elf32_shdr *sh,
                                      ULONG logical_off, ULONG largest_payload)
{
  ULONG sym_index = ARM_ELF_NO_SYMBOL;
  arm_elf32_sym sym;
  BYTE sec_name[64];
  BYTE sym_name[64];
  ULONG low_free, low_largest;
  ULONG umb_free, umb_largest;
  size_t app_largest;
  arm_app_heap_context *saved_app_heap;
  int have_sym;

  arm_elf_loader_progress_end(meta);
  if (!arm_elf_section_name(fd, meta, sh, sec_name, sizeof(sec_name)))
    strcpy((char *)sec_name, "?");
  have_sym = arm_elf_find_blocking_symbol(fd, meta, sec_num, logical_off,
                                           &sym_index, &sym) == SUCCESS;
  if (!have_sym ||
      !arm_elf_symbol_name(fd, meta, &sym, sym_name, sizeof(sym_name)))
    strcpy((char *)sym_name, "?");
  arm_elf_pool_free_stats(FALSE, &low_free, &low_largest);
  arm_elf_pool_free_stats(TRUE, &umb_free, &umb_largest);
  saved_app_heap = arm_app_active_heap;
  arm_app_active_heap = &meta->app_heap;
  app_largest = arm_app_psram_largest();
  arm_app_active_heap = saved_app_heap;

  if (have_sym)
    dos_printf("ARM ELF: symbol %s in section %u %s does not fit DOS memory\r\n",
               sym_name, (unsigned)sec_num, sec_name);
  else
    dos_printf("ARM ELF: section %u %s cannot continue at offset %lu\r\n",
               (unsigned)sec_num, sec_name, logical_off);
  if (have_sym)
    dos_printf("  symbol-offset=%lu symbol-size=%lu largest-next-payload=%lu\r\n",
               sym.value, sym.size, largest_payload);
  dos_printf("  occupied-blocks=%u allocated=%lu\r\n",
             (unsigned)(meta->extra_block_count + 1u),
             meta->allocation_end + meta->extra_block_bytes);
  dos_printf("  LOW free=%lu largest=%lu; UMB free=%lu largest=%lu\r\n",
             low_free, low_largest, umb_free, umb_largest);
  dos_printf("  APP largest=%lu\r\n", (ULONG)app_largest);
}

static void arm_elf_append_chunk(arm_elf_sec_state *state,
                                 arm_elf_sec_chunk *chunk)
{
  ULONG *link = &state->extra_chunks;

  while (*link != 0) {
    arm_elf_sec_chunk *cur = (arm_elf_sec_chunk *)(uintptr_t)*link;
    link = &cur->next_addr;
  }
  *link = (ULONG)(uintptr_t)chunk;
}

static void arm_elf_free_extra_blocks(arm_elf_load_meta *meta)
{
  arm_elf_sec_state *states;
  arm_app_heap_context *saved_app_heap;
  UWORD i;

  if (meta == NULL)
    return;
  saved_app_heap = arm_app_active_heap;
  arm_app_active_heap = &meta->app_heap;
  states = arm_elf_states(meta);
  for (i = 0; i < meta->shnum; ++i) {
    ULONG chunk_addr = states[i].extra_chunks;
    while (chunk_addr != 0) {
      arm_elf_sec_chunk *chunk = (arm_elf_sec_chunk *)(uintptr_t)chunk_addr;
      ULONG next_addr = chunk->next_addr;
      UWORD mcb_seg = chunk->mcb_seg;
      if (mcb_seg != 0)
        DosMemFree(mcb_seg);
      else
        arm_native_app_free(chunk);
      chunk_addr = next_addr;
    }
    states[i].extra_chunks = 0;
  }
  meta->extra_block_count = 0;
  meta->extra_block_bytes = 0;
  arm_app_active_heap = saved_app_heap;
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
        guest_move_block(
            task_guest_linear(arm_elf_guest_ptr(
                base_seg, dst_off + count * sizeof(ULONG))),
            task_guest_linear(arm_elf_guest_ptr(
                base_seg, states[best].offset)),
            states[best].size);
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
  if (rc != SUCCESS) {
    arm_elf_loader_progress_end(meta);
    dos_printf("ARM ELF: startup arrays do not fit guest image\r\n");
    dos_printf("  entries=%lu bytes=%lu cursor=%lu aligned=%lu end=%lu reserved=%lu\r\n",
               total_entries, bytes, meta->cursor, off, off + bytes,
               meta->allocation_end);
    return rc;
  }

  meta->preinit_array_addr = (ULONG)(uintptr_t)arm_native_guest_ptr(
      arm_elf_guest_ptr(base_seg, off));
  rc = arm_elf_copy_startup_kind(fd, base_seg, meta,
                                 ARM_ELF_STARTUP_PREINIT, off,
                                 &meta->preinit_array_count);
  if (rc != SUCCESS)
    return rc;
  off += meta->preinit_array_count * sizeof(ULONG);

  meta->init_array_addr = (ULONG)(uintptr_t)arm_native_guest_ptr(
      arm_elf_guest_ptr(base_seg, off));
  rc = arm_elf_copy_startup_kind(fd, base_seg, meta,
                                 ARM_ELF_STARTUP_INIT, off,
                                 &meta->init_array_count);
  if (rc != SUCCESS)
    return rc;
  off += meta->init_array_count * sizeof(ULONG);

  meta->fini_array_addr = (ULONG)(uintptr_t)arm_native_guest_ptr(
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


static int arm_elf_crt_boundary_addr(const arm_elf_load_meta *meta,
                                     const BYTE *name, ULONG *sym_addr)
{
  if (strcmp((const char *)name, "__ez_preinit_array_start") == 0)
    *sym_addr = meta->preinit_array_addr;
  else if (strcmp((const char *)name, "__ez_preinit_array_end") == 0)
    *sym_addr = meta->preinit_array_addr +
        meta->preinit_array_count * sizeof(ULONG);
  else if (strcmp((const char *)name, "__ez_init_array_start") == 0)
    *sym_addr = meta->init_array_addr;
  else if (strcmp((const char *)name, "__ez_init_array_end") == 0)
    *sym_addr = meta->init_array_addr +
        meta->init_array_count * sizeof(ULONG);
  else if (strcmp((const char *)name, "__ez_fini_array_start") == 0)
    *sym_addr = meta->fini_array_addr;
  else if (strcmp((const char *)name, "__ez_fini_array_end") == 0)
    *sym_addr = meta->fini_array_addr +
        meta->fini_array_count * sizeof(ULONG);
  else
    return FALSE;
  return TRUE;
}

static int arm_elf_symbol_addr(COUNT fd, UWORD base_seg,
                               arm_elf_load_meta *meta, ULONG sym_index,
                               ULONG *sym_addr)
{
  arm_elf32_sym sym;
  arm_elf32_shdr sec;
  ULONG sec_addr;
  ULONG sym_off;
  ULONG thumb_bit;
  int rc = arm_elf_read_symbol(fd, meta, sym_index, &sym);

  if (rc != SUCCESS)
    return rc;
  if (sym.shndx == SHN_UNDEF) {
    BYTE name[64];
    if (arm_elf_symbol_name(fd, meta, &sym, name, sizeof(name))) {
      if (arm_elf_crt_boundary_addr(meta, name, sym_addr))
        return SUCCESS;
      dos_printf("ARM ELF: undefined symbol: %s\r\n", name);
    } else {
      dos_printf("ARM ELF: undefined symbol #%lu\r\n", sym_index);
    }
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

  sym_off = arm_elf_symbol_section_offset(&sym);
  thumb_bit = ((sym.info & 0x0fu) == STT_FUNC) ? (sym.value & 1u) : 0u;

  rc = arm_elf_read_shdr(fd, &meta->eh, sym.shndx, &sec);
  if (rc != SUCCESS || sym_off > sec.size)
    return DE_INVLDFMT;
  rc = arm_elf_load_section(fd, base_seg, meta, sym.shndx, &sec_addr);
  if (rc != SUCCESS)
    return rc;
  rc = arm_elf_section_runtime_addr(base_seg, meta, sym.shndx,
                                     sym_off, 0, sym_addr);
  if (rc == SUCCESS)
    *sym_addr |= thumb_bit;
  return rc;
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

  { dos_far_ptr syscon = fdos_lol_syscon(); c = ndread(&syscon); }
  if (c != CTL_C)
    return FALSE;

  (void)read_char_stdin(FALSE);
  dos_printf("^C\r\n");
  return TRUE;
}

static void arm_elf_loader_progress_end(arm_elf_load_meta *meta)
{
  if (meta != NULL && meta->is_long_running_job) {
    dos_printf("\r\n");
    /* Prevent a second diagnostic on the same failure path from adding an
       extra blank line.  A later progress point will re-enable output because
       loader_started_us is unchanged. */
    meta->is_long_running_job = FALSE;
  }
}

static void arm_elf_report_relocation_failure(COUNT fd,
                                              arm_elf_load_meta *meta,
                                              int error,
                                              UWORD target_section,
                                              UWORD rel_section,
                                              ULONG record,
                                              ULONG offset,
                                              UBYTE type,
                                              ULONG symbol)
{
  arm_elf32_sym sym;
  arm_elf32_shdr sh;
  BYTE target_name[64];
  BYTE symbol_name[64];
  BYTE symbol_section_name[64];
  int have_sym = FALSE;
  int have_target_name = FALSE;
  int have_symbol_name = FALSE;
  int have_symbol_section_name = FALSE;

  arm_elf_loader_progress_end(meta);

  if (target_section < meta->eh.shnum &&
      arm_elf_read_shdr(fd, &meta->eh, target_section, &sh) == SUCCESS)
    have_target_name = arm_elf_section_name(fd, meta, &sh, target_name,
                                            sizeof(target_name));

  if (arm_elf_read_symbol(fd, meta, symbol, &sym) == SUCCESS) {
    have_sym = TRUE;
    have_symbol_name = arm_elf_symbol_name(fd, meta, &sym, symbol_name,
                                           sizeof(symbol_name));
    if (sym.shndx < meta->eh.shnum &&
        arm_elf_read_shdr(fd, &meta->eh, sym.shndx, &sh) == SUCCESS)
      have_symbol_section_name = arm_elf_section_name(
          fd, meta, &sh, symbol_section_name, sizeof(symbol_section_name));
  }

  dos_printf("ARM ELF: relocation failed: error=%d target=%u%s%s "
             "relsec=%u record=%lu offset=%lu type=%u\r\n",
             error, (unsigned)target_section,
             have_target_name ? " " : "",
             have_target_name ? (const char *)target_name : "",
             (unsigned)rel_section, record, offset, (unsigned)type);
  if (have_sym) {
    dos_printf("  symbol=%s%s#%lu type=%u shndx=%u%s%s value=%lu size=%lu\r\n",
               have_symbol_name ? (const char *)symbol_name : "",
               have_symbol_name ? " " : "", symbol,
               (unsigned)(sym.info & 0x0fu), (unsigned)sym.shndx,
               have_symbol_section_name ? " " : "",
               have_symbol_section_name ? (const char *)symbol_section_name : "",
               sym.value, sym.size);
  } else {
    dos_printf("  symbol=#%lu (symbol record cannot be read)\r\n", symbol);
  }
}

static int arm_elf_apply_section_relocations(COUNT fd, UWORD base_seg,
                                             arm_elf_load_meta *meta,
                                             UWORD sec_num,
                                             const arm_elf32_shdr *target)
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
      arm_elf32_sym rel_sym;
      ULONG sym_addr;
      ULONG sym_index;
      ULONG place_addr;
      UBYTE type;
      UBYTE sym_type;
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
      rc = arm_elf_read_symbol(fd, meta, sym_index, &rel_sym);
      if (rc != SUCCESS) {
        arm_elf_report_relocation_failure(fd, meta, rc, sec_num, i, j,
                                          rel.offset, type, sym_index);
        return rc;
      }
      sym_type = rel_sym.info & 0x0fu;
      rc = arm_elf_symbol_addr(fd, base_seg, meta, sym_index, &sym_addr);
      if (rc != SUCCESS) {
        arm_elf_report_relocation_failure(fd, meta, rc, sec_num, i, j,
                                          rel.offset, type, sym_index);
        return rc;
      }

      rc = arm_elf_section_runtime_addr(base_seg, meta, sec_num,
                                        rel.offset, 4, &place_addr);
      if (rc != SUCCESS) {
        arm_elf_report_relocation_failure(fd, meta, rc, sec_num, i, j,
                                          rel.offset, type, sym_index);
        dos_printf("ARM ELF: relocation field crosses section block boundary "
                   "(section %u, offset %lu)\r\n",
                   (unsigned)sec_num, rel.offset);
        return rc;
      }
      place = (BYTE *)(uintptr_t)place_addr;
      paddr = place_addr;
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
          if (sym_type == STT_SECTION) {
            ULONG target_addr;
            ULONG addend = *(ULONG *)place;
            rc = arm_elf_section_runtime_addr(base_seg, meta, rel_sym.shndx,
                                              addend, 0, &target_addr);
            if (rc != SUCCESS) {
              arm_elf_report_relocation_failure(fd, meta, rc, sec_num, i, j,
                                                rel.offset, type, sym_index);
              return rc;
            }
            *(ULONG *)place = target_addr;
          } else {
            *(ULONG *)place += sym_addr;
          }
          break;
        case R_ARM_REL32:
          if (sym_type == STT_SECTION) {
            ULONG target_addr;
            ULONG addend = *(ULONG *)place;
            rc = arm_elf_section_runtime_addr(base_seg, meta, rel_sym.shndx,
                                              addend, 0, &target_addr);
            if (rc != SUCCESS) {
              arm_elf_report_relocation_failure(fd, meta, rc, sec_num, i, j,
                                                rel.offset, type, sym_index);
              return rc;
            }
            *(ULONG *)place = target_addr - paddr;
          } else {
            *(ULONG *)place = sym_addr + *(ULONG *)place - paddr;
          }
          break;
        case R_ARM_THM_PC22:
          if (sym_type == STT_SECTION && rel_sym.shndx < meta->shnum &&
              arm_elf_states(meta)[rel_sym.shndx].extra_chunks != 0) {
            arm_elf_report_relocation_failure(fd, meta, DE_INVLDFMT,
                                                sec_num, i, j, rel.offset,
                                                type, sym_index);
            dos_printf("ARM ELF: section-symbol Thumb relocation into split "
                       "section %u is not supported\r\n",
                       (unsigned)rel_sym.shndx);
            return DE_INVLDFMT;
          }
          arm_elf_resolve_thm_pc22((UWORD *)place, (UWORD *)place, sym_addr);
          break;
        case R_ARM_THM_JUMP24:
          if (sym_type == STT_SECTION && rel_sym.shndx < meta->shnum &&
              arm_elf_states(meta)[rel_sym.shndx].extra_chunks != 0) {
            arm_elf_report_relocation_failure(fd, meta, DE_INVLDFMT,
                                                sec_num, i, j, rel.offset,
                                                type, sym_index);
            dos_printf("ARM ELF: section-symbol Thumb relocation into split "
                       "section %u is not supported\r\n",
                       (unsigned)rel_sym.shndx);
            return DE_INVLDFMT;
          }
          rc = arm_elf_resolve_thm_jump24((UWORD *)place,
                                          (UWORD *)place, sym_addr);
          if (rc != SUCCESS) {
            arm_elf_report_relocation_failure(fd, meta, rc,
                                                sec_num, i, j, rel.offset,
                                                type, sym_index);
            dos_printf("ARM ELF: Thumb JUMP24 relocation out of range "
                       "(section %u, offset %lu)\r\n",
                       (unsigned)sec_num, rel.offset);
            return rc;
          }
          break;
        case R_ARM_THM_ALU_ABS_G0_NC:
          if (sym_type == STT_SECTION && rel_sym.shndx < meta->shnum &&
              arm_elf_states(meta)[rel_sym.shndx].extra_chunks != 0) {
            arm_elf_report_relocation_failure(fd, meta, DE_INVLDFMT,
                                                sec_num, i, j, rel.offset,
                                                type, sym_index);
            dos_printf("ARM ELF: section-symbol MOV relocation into split "
                       "section %u is not supported\r\n",
                       (unsigned)rel_sym.shndx);
            return DE_INVLDFMT;
          }
          if ((uintptr_t)place & 1u) {
            arm_elf_report_relocation_failure(fd, meta, DE_INVLDFMT,
                                                sec_num, i, j, rel.offset,
                                                type, sym_index);
            dos_printf("ARM ELF: MOV relocation place is not halfword aligned "
                       "(section %u, offset %lu, address=%08lx)\r\n",
                       (unsigned)sec_num, rel.offset, place_addr);
            return DE_INVLDFMT;
          }
          arm_elf_resolve_thm_alu_abs_g0_nc((UWORD *)place, sym_addr);
          break;
        default:
        {
          arm_elf32_sym diag_sym;
          BYTE name[64];

          arm_elf_report_relocation_failure(fd, meta, DE_INVLDFMT,
                                              sec_num, i, j, rel.offset,
                                              type, sym_index);
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
  ULONG logical = 0;
  ULONG piece_end;
  UBYTE use_umb;
  UBYTE second_pool;
  int rc;

  if (sec_num >= meta->shnum)
    return DE_INVLDFMT;
  state = &states[sec_num];
  if (state->state == ARM_ELF_SEC_LOADING ||
      state->state == ARM_ELF_SEC_LOADED)
    return arm_elf_section_runtime_addr(base_seg, meta, sec_num, 0, 0,
                                        sec_addr);

  rc = arm_elf_read_shdr(fd, &meta->eh, sec_num, &sh);
  if (rc != SUCCESS)
    return rc;

  off = arm_elf_align_up(meta->cursor, sh.addralign);
  if (off == 0xfffffffful)
    return DE_INVLDFMT;

  state->offset = off;
  state->size = sh.size;
  state->primary_size = 0;
  state->extra_chunks = 0;
  state->state = ARM_ELF_SEC_LOADING;
  use_umb = meta->allocation_high ? TRUE : FALSE;
  second_pool = use_umb ? FALSE : TRUE;

  /* Use the remainder of the primary process block first, but never cut an
     ELF symbol.  Once a symbol no longer fits, the section continues in the
     next largest DOS block instead of requiring the whole section to be
     contiguous. */
  if (logical < sh.size && off < meta->allocation_end) {
    ULONG capacity = meta->allocation_end - off;
    if (capacity >= sh.size - logical) {
      piece_end = sh.size;
    } else {
      rc = arm_elf_safe_piece_end(fd, meta, sec_num, logical, capacity,
                                  sh.size, &piece_end);
      if (rc != SUCCESS)
        return rc;
    }
    if (piece_end > logical) {
      ULONG piece_size = piece_end - logical;
      if (sh.type == SHT_NOBITS) {
        guest_fill_block(task_guest_linear(arm_elf_guest_ptr(base_seg, off)),
                         0, piece_size);
      } else {
        rc = arm_elf_read_section(fd, base_seg, off, sh.offset, piece_size);
        if (rc != SUCCESS) {
          dos_printf("ARM ELF: cannot read section %u bytes 0..%lu\r\n",
                     (unsigned)sec_num, piece_end);
          return rc;
        }
      }
      state->primary_size = piece_end;
      meta->cursor = off + piece_size;
      logical = piece_end;
    }
  }

  while (logical < sh.size) {
    UWORD mcb_seg = 0;
    UWORD paras = 0;
    UWORD data_seg;
    ULONG block_bytes;
    ULONG data_off;
    ULONG capacity;
    ULONG piece_size;
    ULONG used_bytes;
    UWORD used_paras;
    arm_elf_sec_chunk *chunk;

retry_pool:
    rc = arm_elf_alloc_largest_pool(use_umb, &mcb_seg, &paras);
    if (rc != SUCCESS || paras == 0) {
      if (use_umb != second_pool) {
        use_umb = second_pool;
        mcb_seg = 0;
        paras = 0;
        goto retry_pool;
      }
      goto app_heap_fallback;
    }

    data_seg = (UWORD)(mcb_seg + 1u);
    block_bytes = (ULONG)paras << 4;
    data_off = arm_elf_chunk_payload_off(data_seg, logical, sh.addralign);
    capacity = data_off < block_bytes ? block_bytes - data_off : 0;

    if (capacity >= sh.size - logical) {
      piece_end = sh.size;
    } else {
      rc = arm_elf_safe_piece_end(fd, meta, sec_num, logical, capacity,
                                  sh.size, &piece_end);
      if (rc != SUCCESS) {
        DosMemFree(mcb_seg);
        return rc;
      }
    }
    if (piece_end <= logical) {
      DosMemFree(mcb_seg);
      if (use_umb != second_pool) {
        use_umb = second_pool;
        mcb_seg = 0;
        paras = 0;
        goto retry_pool;
      }
      goto app_heap_fallback;
    }

    piece_size = piece_end - logical;
    if (sh.type == SHT_NOBITS) {
      guest_fill_block(task_guest_linear(arm_elf_guest_ptr(data_seg, data_off)),
                       0, piece_size);
    } else {
      rc = arm_elf_read_section(fd, data_seg, data_off,
                                sh.offset + logical, piece_size);
      if (rc != SUCCESS) {
        DosMemFree(mcb_seg);
        dos_printf("ARM ELF: cannot read section %u bytes %lu..%lu\r\n",
                   (unsigned)sec_num, logical, piece_end);
        return rc;
      }
    }

    used_bytes = data_off + piece_size;
    used_paras = (UWORD)((used_bytes + 15u) >> 4);
    if (used_paras == 0 || used_paras > paras) {
      DosMemFree(mcb_seg);
      return DE_NOMEM;
    }
    if (used_paras < paras) {
      rc = DosMemChange(data_seg, used_paras, NULL);
      if (rc != SUCCESS) {
        DosMemFree(mcb_seg);
        return rc;
      }
    }

    /* DosMemChange() assigns the caller as owner.  These blocks belong to the
       child PSP, exactly like its separate DOS-stack MCB, so normal process
       teardown/TSR ownership sees them as part of the child. */
    fdos_mcb_set_owner(mcb_seg, base_seg);

    chunk = (arm_elf_sec_chunk *)arm_native_guest_ptr(MK_FP(data_seg, 0));
    chunk->next_addr = 0;
    chunk->logical_start = logical;
    chunk->logical_end = piece_end;
    chunk->data_addr = (ULONG)(uintptr_t)arm_native_guest_ptr(MK_FP(data_seg, 0));
    chunk->data_off = data_off;
    chunk->mcb_seg = mcb_seg;
    chunk->data_seg = data_seg;
    arm_elf_append_chunk(state, chunk);

    ++meta->extra_block_count;
    meta->extra_block_bytes += (ULONG)used_paras << 4;
    logical = piece_end;
    continue;

app_heap_fallback:
    {
      arm_app_heap_context *saved_app_heap = arm_app_active_heap;
      size_t largest;
      size_t overhead;
      size_t alloc_size;
      void *allocation;
      uintptr_t native_base;

      arm_app_active_heap = &meta->app_heap;
      largest = arm_app_psram_largest();
      overhead = sizeof(arm_elf_sec_chunk) +
                 (sh.addralign > 1 ? (size_t)sh.addralign - 1u : 0u);
      if (largest <= overhead) {
        arm_app_active_heap = saved_app_heap;
        arm_elf_report_symbol_oom(fd, meta, sec_num, &sh, logical, 0);
        return DE_NOMEM;
      }

      capacity = (ULONG)(largest - overhead);
      if (capacity >= sh.size - logical) {
        piece_end = sh.size;
      } else {
        rc = arm_elf_safe_piece_end(fd, meta, sec_num, logical, capacity,
                                    sh.size, &piece_end);
        if (rc != SUCCESS) {
          arm_app_active_heap = saved_app_heap;
          return rc;
        }
      }
      if (piece_end <= logical) {
        arm_app_active_heap = saved_app_heap;
        arm_elf_report_symbol_oom(fd, meta, sec_num, &sh, logical, capacity);
        return DE_NOMEM;
      }

      piece_size = piece_end - logical;
      alloc_size = overhead + piece_size;
      allocation = arm_native_app_malloc(alloc_size);
      if (allocation == NULL || !arm_app_psram_owns(allocation)) {
        if (allocation != NULL)
          arm_native_app_free(allocation);
        arm_app_active_heap = saved_app_heap;
        arm_elf_report_symbol_oom(fd, meta, sec_num, &sh, logical, capacity);
        return DE_NOMEM;
      }

      chunk = (arm_elf_sec_chunk *)allocation;
      native_base = (uintptr_t)allocation;
      data_off = arm_elf_chunk_payload_off_native(native_base, logical,
                                                  sh.addralign);
      if ((size_t)data_off + piece_size > alloc_size) {
        arm_native_app_free(allocation);
        arm_app_active_heap = saved_app_heap;
        return DE_NOMEM;
      }

      if (sh.type == SHT_NOBITS) {
        nf_memset((BYTE *)allocation + data_off, 0, piece_size);
      } else {
        rc = arm_elf_read_native_section(fd, (BYTE *)allocation + data_off,
                                         sh.offset + logical, piece_size);
        if (rc != SUCCESS) {
          arm_native_app_free(allocation);
          arm_app_active_heap = saved_app_heap;
          dos_printf("ARM ELF: cannot read section %u bytes %lu..%lu "
                     "into application heap\r\n",
                     (unsigned)sec_num, logical, piece_end);
          return rc;
        }
      }

      chunk->next_addr = 0;
      chunk->logical_start = logical;
      chunk->logical_end = piece_end;
      chunk->data_addr = (ULONG)native_base;
      chunk->data_off = data_off;
      chunk->mcb_seg = 0;
      chunk->data_seg = 0;
      arm_elf_append_chunk(state, chunk);

      ++meta->extra_block_count;
      meta->extra_block_bytes += (ULONG)alloc_size;
      logical = piece_end;
      arm_app_active_heap = saved_app_heap;
    }
  }

  if (sh.size == 0)
    meta->cursor = off;

  rc = arm_elf_section_runtime_addr(base_seg, meta, sec_num, 0, 0, sec_addr);
  if (rc != SUCCESS)
    return rc;

  rc = arm_elf_apply_section_relocations(fd, base_seg, meta, sec_num, &sh);
  if (rc != SUCCESS)
    return rc;
  state->state = ARM_ELF_SEC_LOADED;

  /* 's' marks one completed newly loaded/relocated section. */
  if (arm_elf_loader_progress(meta, "s"))
    return DE_ACCESS;
  return SUCCESS;
}

static int arm_elf_read_crt_requirements(
  COUNT fd,
  arm_elf_load_meta *meta,
  native_ez_process_requirements *requirements
) {
  arm_elf32_shdr fallback;
  UBYTE have_fallback = FALSE;
  UWORD i;

  nf_memset(requirements, 0, sizeof(*requirements));
  nf_memset(&fallback, 0, sizeof(fallback));

  for (i = 0; i < meta->eh.shnum; ++i) {
    arm_elf32_shdr sh;
    BYTE name[64];
    int rc = arm_elf_read_shdr(fd, &meta->eh, i, &sh);

    if (rc != SUCCESS)
      return rc;
    if (sh.type == SHT_NOBITS || sh.size != sizeof(*requirements))
      continue;
    if (!arm_elf_section_name(fd, meta, &sh, name, sizeof(name)))
      continue;

    if (strcmp((const char *)name,
               NATIVE_EZ_PROCESS_REQUIREMENTS_SECTION) == 0)
      return arm_elf_read_meta(fd, sh.offset, requirements,
                               sizeof(*requirements));

    if (!have_fallback &&
        strcmp((const char *)name, NATIVE_EZ_PROCESS_DEFAULT_SECTION) == 0) {
      fallback = sh;
      have_fallback = TRUE;
    }
  }

  if (have_fallback)
    return arm_elf_read_meta(fd, fallback.offset, requirements,
                             sizeof(*requirements));
  return SUCCESS;
}

static int arm_elf_find_roots(COUNT fd, arm_elf_load_meta *meta,
                              ULONG *req_idx, ULONG *requirements_idx,
                              ULONG *entry_idx, ULONG *init_idx,
                              ULONG *main_idx, ULONG *fini_idx,
                              ULONG *sig_idx)
{
  ULONG count;
  ULONG i;
  ULONG weak_entry = ARM_ELF_NO_SYMBOL;
  ULONG weak_init = ARM_ELF_NO_SYMBOL;
  ULONG weak_fini = ARM_ELF_NO_SYMBOL;
  ULONG entsize = meta->symtab.entsize ? meta->symtab.entsize
                                        : sizeof(arm_elf32_sym);

  if (entsize < sizeof(arm_elf32_sym))
    return DE_INVLDFMT;
  count = meta->symtab.size / entsize;
  *req_idx = *requirements_idx = *entry_idx = *init_idx =
      *main_idx = *fini_idx = *sig_idx = ARM_ELF_NO_SYMBOL;

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
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      continue;

    if (type != STT_FUNC)
      continue;
    if (!arm_elf_symbol_name(fd, meta, &sym, name, sizeof(name)))
      continue;

    if (strcmp((const char *)name, "__ez_start") == 0) {
      if (bind == STB_GLOBAL)
        *entry_idx = i;
      else
        weak_entry = i;
    } else if (strcmp((const char *)name, "_init") == 0) {
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

  if (*entry_idx == ARM_ELF_NO_SYMBOL)
    *entry_idx = weak_entry;
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
ULONG SftGetFsize(int sft_idx);
STATIC COUNT ChildEnvGuest(exec_blk *exp, UWORD *pChildEnvSeg, dos_far_ptr pathname);
STATIC UWORD patchPSPGuest(UWORD pspseg, UWORD envseg, exec_blk *exb, dos_far_ptr fnam);
static dos_far_ptr exec_caller_return_addr(void);
static COUNT exec_run_arm_elf(UWORD child_psp_seg,
                              arm_elf_load_meta *meta);
static COUNT exec_run_arm_ez(UWORD child_psp_seg, arm_ez_load_meta *meta);

typedef int (*arm_elf_req_ver_fn)(void);

#define ARM_ELF_PROCESS_REQUIREMENTS_V1_SIZE 12u
#define ARM_ELF_PROCESS_REQUIREMENTS_V2_SIZE 20u
/*
 * Keep native application stacks out of DOS memory without making the rest of
 * PSRAM a kernel heap.  The fixed arena is excluded from the application heap,
 * so nested native EXEC can safely consume it LIFO without colliding with a
 * parent's application allocations.
 */


typedef struct __attribute__((aligned(4))) arm_elf_process_requirements {
  ULONG struct_size __attribute__((aligned(4)));
  ULONG native_stack_size __attribute__((aligned(4)));
  ULONG dos_stack_size __attribute__((aligned(4)));
  ULONG assigned_native_stack_size __attribute__((aligned(4)));
  ULONG assigned_dos_stack_size __attribute__((aligned(4)));
} arm_elf_process_requirements;

_Static_assert(sizeof(ULONG) == 4, "ARM ELF process ABI requires 32-bit ULONG");
_Static_assert(__alignof__(arm_elf_process_requirements) == 4,
               "ARM ELF process requirements alignment");
_Static_assert(sizeof(arm_elf_process_requirements) ==
               ARM_ELF_PROCESS_REQUIREMENTS_V2_SIZE,
               "ARM ELF process requirements layout");

typedef arm_elf_process_requirements *
    (*arm_elf_requirements_fn)(void);

/*
 * Native stack arena.
 *
 * The stack arena is carved from the top of physically detected QSPI PSRAM.
 * The FatFs reclaimable L2 cache independently caps its ceiling at
 * config.mem_size, so LTEMS backing is never borrowed by the cache.  Stack
 * lifetimes follow synchronous nested EXEC, so a LIFO allocator is sufficient
 * and cannot fragment.
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
  if (!arm_native_runtime_available())
    return;
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
        (volatile uint8_t *)arm_native_guest_ptr(
            MK_FP(doom_diag_dos_stack_seg, 0));

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
  return (uintptr_t)PSRAM_BASE_ADDR + (uintptr_t)psram_usable_size();
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
  nf_memset((void *)next, 0, size);
  if (size >= DOOM_NATIVE_STACK_GUARD)
    nf_memset((void *)next, DOOM_STACK_CANARY, DOOM_NATIVE_STACK_GUARD);
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
static int arm_native_alloc_low_exact(UWORD paras,
                                      UWORD *mcb_seg, UWORD *actual)
{
  UBYTE saved_umb_link = fdos_lol_uppermem_link();
  UBYTE saved_mode = fdos_dos_mem_access_mode();
  int rc;

  DosUmbLink(0);
  fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0xc0u));
  rc = DosMemAlloc(paras, fdos_dos_mem_access_mode(), mcb_seg, actual);
  fdos_dos_set_mem_access_mode(saved_mode);
  DosUmbLink(saved_umb_link);

  if (rc == SUCCESS)
    *actual = paras;
  return rc;
}

static int arm_native_reserve_dos_stack_low(ULONG stack_size,
                                            UWORD *out_mcb, UWORD *out_seg)
{
  UWORD paras;
  UWORD mcb_seg = 0;
  UWORD actual = 0;
  int rc;

  if (stack_size == 0 || stack_size > 0x10000ul)
    return DE_INVLDFMT;

  paras = (UWORD)((stack_size + 15u) >> 4);
  if (paras == 0)
    return DE_NOMEM;

  rc = arm_native_alloc_low_exact(paras, &mcb_seg, &actual);
  if (rc != SUCCESS)
    return rc;

  *out_mcb = mcb_seg;
  *out_seg = mcb_seg + 1;
  guest_fill_block(task_guest_seg_linear(*out_seg), 0, stack_size);
  if (stack_size >= DOOM_DOS_STACK_GUARD)
    guest_fill_block(task_guest_seg_linear(*out_seg),
                     DOOM_STACK_CANARY, DOOM_DOS_STACK_GUARD);

  fdos_mcb_set_owner(mcb_seg, fdos_dos_cu_psp());
  fdos_mcb_set_name8(mcb_seg, "ARMSTK  ");

  return SUCCESS;
}

static int arm_native_reserve_dos_stack(ULONG stack_size,
                                        UWORD *out_mcb, UWORD *out_seg)
{
  UWORD paras;

  /* Guest SS:SP is 16-bit.  64 KiB is representable with SP=0000h
     (the first push wraps to FFFEh), but anything larger would make the
     paragraph count truncate below while only a 64 KiB stack window is
     addressable by the child. */
  if (stack_size == 0 || stack_size > 0x10000ul)
    return DE_INVLDFMT;

  paras = (UWORD)((stack_size + 15u) >> 4);
  UWORD mcb_seg = 0;
  UWORD largest = 0;
  UBYTE old_umb_link = fdos_lol_uppermem_link();
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

  *out_mcb = mcb_seg;
  *out_seg = mcb_seg + 1;
  guest_fill_block(task_guest_seg_linear(*out_seg), 0, stack_size);
  if (stack_size >= DOOM_DOS_STACK_GUARD)
    guest_fill_block(task_guest_seg_linear(*out_seg),
                     DOOM_STACK_CANARY, DOOM_DOS_STACK_GUARD);

  /* Temporary owner until the child PSP exists. */
  fdos_mcb_set_owner(mcb_seg, fdos_dos_cu_psp());
  fdos_mcb_set_name8(mcb_seg, "ARMSTK  ");

  return SUCCESS;
}

static void arm_native_assign_dos_stack_owner(UWORD stack_mcb, UWORD child_psp)
{
  fdos_mcb_set_owner(stack_mcb, child_psp);
}

static int arm_native_alloc_dos_stack(ULONG stack_size, UWORD child_psp,
                                      UWORD *out_mcb, UWORD *out_seg)
{
  int rc = arm_native_reserve_dos_stack(stack_size, out_mcb, out_seg);
  if (rc == SUCCESS)
    arm_native_assign_dos_stack_owner(*out_mcb, child_psp);
  return rc;
}

static int arm_elf_alloc_dos_stack(arm_elf_load_meta *meta, UWORD child_psp)
{
  return arm_native_alloc_dos_stack(meta->dos_stack_size, child_psp,
                                    &meta->dos_stack_mcb,
                                    &meta->dos_stack_seg);
}

/*
 * Run the startup ABI preflight on the kernel stack, before either application
 * stack exists.  Version negotiation remains the first application call.  An
 * optional requirements hook then selects the native ARM and guest DOS stack
 * independently; absent/zero fields retain the historical defaults.
 */
static int arm_elf_set_stack_sizes(arm_elf_load_meta *meta,
                                   ULONG native_size, ULONG dos_size)
{
  native_size = arm_elf_align_up(native_size, 8u);
  dos_size = arm_elf_align_up(dos_size, 16u);
  if (dos_size != 0xfffffffful && dos_size < ARM_ELF_MIN_DOS_STACK_SIZE)
    dos_size = ARM_ELF_MIN_DOS_STACK_SIZE;
  if (native_size == 0xfffffffful || dos_size == 0xfffffffful ||
      native_size == 0 || dos_size == 0 || dos_size > 0x10000ul) {
    dos_printf("ARM ELF: invalid process stack requirements\r\n");
    return DE_INVLDFMT;
  }

  meta->native_stack_size = native_size;
  meta->dos_stack_size = dos_size;
  return SUCCESS;
}

static int arm_elf_preflight(arm_elf_load_meta *meta)
{
  arm_elf_process_requirements *requirements = NULL;
  ULONG native_size = ARM_ELF_DEFAULT_NATIVE_STACK_SIZE;
  ULONG dos_size = ARM_ELF_DEFAULT_DOS_STACK_SIZE;

  if (meta->entry_addr != 0) {
    if (meta->required_api_version == 0)
      meta->required_api_version = DOS_API_VERSION;
    if (meta->native_stack_size != 0)
      native_size = meta->native_stack_size;
    if (meta->dos_stack_size != 0)
      dos_size = meta->dos_stack_size;
  } else {
    meta->required_api_version = DOS_API_VERSION;
    if (meta->required_api_addr != 0) {
      meta->required_api_version = ((arm_elf_req_ver_fn)(uintptr_t)meta->required_api_addr)();
    }
    if (meta->requirements_addr != 0) {
      requirements = ((arm_elf_requirements_fn)(uintptr_t)meta->requirements_addr)();
      if (requirements != NULL) {
        if (requirements->struct_size < ARM_ELF_PROCESS_REQUIREMENTS_V1_SIZE) {
          dos_printf("ARM ELF: process requirements structure is too small (%lu)\r\n",
                    requirements->struct_size);
          return DE_INVLDFMT;
        }
        if (requirements->native_stack_size != 0) {
          native_size = requirements->native_stack_size;
        }
        if (requirements->dos_stack_size != 0) {
          dos_size = requirements->dos_stack_size;
        }
      }
    }
  }
  if (meta->required_api_version > DOS_API_VERSION) {
    dos_printf("ARM ELF: application requires DOS-API version %ld; provided %u\r\n",
               meta->required_api_version, (unsigned)DOS_API_VERSION);
    return DE_INVLDFMT;
  }

  return arm_elf_set_stack_sizes(meta, native_size, dos_size);
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
  if (arm_elf_ensure_capacity(meta, argv_end) != SUCCESS) {
    arm_elf_loader_progress_end(meta);
    dos_printf("ARM ELF: argv area does not fit guest image\r\n");
    dos_printf("  cursor=%lu aligned=%lu argv-bytes=%lu end=%lu reserved=%lu\r\n",
               meta->cursor, argv_off, (ULONG)ARM_ELF_ARG_AREA_SIZE,
               argv_end, meta->allocation_end);
    return DE_NOMEM;
  }
  text_off = argv_off + ARM_ELF_ARGV_SLOTS * sizeof(ULONG);
  argv = (ULONG *)arm_native_guest_ptr(arm_elf_guest_ptr(base_seg, argv_off));
  text = (BYTE *)arm_native_guest_ptr(arm_elf_guest_ptr(base_seg, text_off));
  nf_memset(argv, 0, ARM_ELF_ARGV_SLOTS * sizeof(ULONG));
  nf_memset(text, 0, ARM_ELF_ARG_TEXT_SIZE);

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
    tail = (const CommandTail *)arm_native_guest_ptr(exp->exec.cmd_line);
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

static int arm_ez_build_argv(UWORD base_seg, arm_ez_load_meta *meta,
                             ULONG *cursor, ULONG allocation_end,
                             exec_blk *exp, const BYTE *namep)
{
  ULONG argv_off = arm_elf_align_up(*cursor, 4u);
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
  if (argv_end > allocation_end)
    return DE_NOMEM;

  text_off = argv_off + ARM_ELF_ARGV_SLOTS * sizeof(ULONG);
  argv = (ULONG *)arm_native_guest_ptr(arm_elf_guest_ptr(base_seg, argv_off));
  text = (BYTE *)arm_native_guest_ptr(arm_elf_guest_ptr(base_seg, text_off));
  nf_memset(argv, 0, ARM_ELF_ARGV_SLOTS * sizeof(ULONG));
  nf_memset(text, 0, ARM_ELF_ARG_TEXT_SIZE);

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
    tail = (const CommandTail *)arm_native_guest_ptr(exp->exec.cmd_line);
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
  *cursor = text_off + ARM_ELF_ARG_TEXT_SIZE;
  return SUCCESS;
}

static int arm_ez_apply_reloc(BYTE *base,
                              const struct ez_file_header *header,
                              const struct ez_reloc *rel)
{
  ULONG image_end = EZ_IMAGE_RVA + header->image_mem_size;
  BYTE *place;

  if (rel->reserved[0] != 0 || rel->reserved[1] != 0 || rel->reserved[2] != 0)
    return DE_INVLDFMT;
  if (rel->rva < EZ_IMAGE_RVA || rel->rva > image_end ||
      image_end - rel->rva < 4u)
    return DE_INVLDFMT;

  place = base + rel->rva;
  switch (rel->type) {
    case EZ_RELOC_ABS32:
      *(ULONG *)place += (ULONG)(uintptr_t)base;
      return SUCCESS;

    case EZ_RELOC_THM_ALU_ABS_G0_NC:
      if ((uintptr_t)place & 1u)
        return DE_INVLDFMT;
      arm_elf_resolve_thm_alu_abs_g0_nc((UWORD *)place,
                                        (ULONG)(uintptr_t)base);
      return SUCCESS;

    default:
      dos_printf("ARM EZ: unsupported relocation type %u at RVA %08lx\r\n",
                 (unsigned)rel->type, rel->rva);
      return DE_INVLDFMT;
  }
}


/* Allocate an exact low-memory DOS block without changing the caller's mode. */
static int arm_ez_alloc_low(UWORD paras, UWORD *mcb_seg, UWORD *actual)
{
  UBYTE saved_mode = fdos_dos_mem_access_mode();
  int rc;

  fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0xc0u));
  rc = DosMemAlloc(paras, fdos_dos_mem_access_mode(), mcb_seg, actual);
  fdos_dos_set_mem_access_mode(saved_mode);
  if (rc == SUCCESS)
    *actual = paras;
  return rc;
}

/*
 * DosRWSft() transfers only to guest far memory.  A PSRAM-resident EZ image is
 * therefore read through one temporary guest-DOS chunk below 1 MiB and copied
 * to its final native address.
 */
static int arm_ez_read_native_image(COUNT fd, BYTE *dst,
                                    ULONG file_off, ULONG size)
{
  UWORD paras = (UWORD)((CHUNK + 15u) >> 4);
  UWORD mcb_seg = 0;
  UWORD largest = 0;
  ULONG done = 0;
  LONG pos;
  int rc;

  rc = arm_ez_alloc_low(paras, &mcb_seg, &largest);
  if (rc != SUCCESS)
    return rc;

  pos = SftSeek(fd, (LONG)file_off, SEEK_SET);
  if (pos < 0) {
    DosMemFree(mcb_seg);
    return DE_INVLDFMT;
  }

  while (done < size) {
    UWORD chunk = (UWORD)min((ULONG)CHUNK, size - done);
    dos_far_ptr stage = MK_FP(mcb_seg + 1, 0);
    LONG got = DosRWSft(fd, chunk, stage, XFR_READ);

    if (got != chunk) {
      DosMemFree(mcb_seg);
      return DE_INVLDFMT;
    }

    guest_read_block(task_guest_linear(stage), dst + done, chunk);
    done += chunk;
  }

  DosMemFree(mcb_seg);
  return SUCCESS;
}

static COUNT DosArmEzLoader(exec_blk *exp, COUNT mode, COUNT fd, BYTE *namep)
{
  struct ez_file_header header;
  arm_ez_load_meta *meta = NULL;
  ULONG image_end;
  ULONG meta_off;
  ULONG cursor;
  ULONG final_end;
  ULONG file_size;
  ULONG image_file_end;
  ULONG reloc_bytes;
  ULONG reloc_end;
  ULONG native_stack_size;
  ULONG dos_stack_size;
  ULONG low_meta_off;
  ULONG low_final_end;
  UWORD low_required_paras = 0;
  UWORD low_largest = 0;
  int image_in_low = FALSE;
  BYTE *image_base;
  BYTE *image_data;
  uintptr_t psram_image_addr = 0;
  uintptr_t app_psram_begin;
  UWORD final_paras;
  UWORD alloc_mcb = 0, asize = 0, load_seg;
  UWORD env_mcb = 0;
  UWORD reserved_stack_mcb = 0, reserved_stack_seg = 0;
  const char *fail_stage = "preflight";
  UWORD fcbcode;
  UBYTE umb_state;
  UBYTE orig_mem_access;
  ULONG i;
  int rc;

  if ((mode & 0x7f) == EXEC_OVERLAY)
    return arm_ez_reject(DE_INVLDFMT, "EZ overlay mode is not supported");
  if ((mode & 0x7f) == EXEC_LOAD)
    return arm_ez_reject(DE_INVLDFMT, "EZ load-only mode is not supported");

  rc = arm_elf_read_meta(fd, 0, &header, sizeof(header));
  if (rc != SUCCESS)
    return arm_ez_reject((COUNT)rc, "cannot read EZ header");

  if (header.magic != EZ_MAGIC || header.version != EZ_FORMAT_VERSION)
    return arm_ez_reject(DE_INVLDFMT, "unsupported EZ header");
  if (header.header_size < sizeof(header))
    return arm_ez_reject(DE_INVLDFMT, "EZ header is too small");
  if ((header.flags & ~EZ_FLAG_KNOWN_MASK) != 0 ||
      (header.flags & EZ_FLAG_THUMB) == 0 ||
      (header.flags & EZ_FLAG_SOFT_FLOAT) == 0 ||
      ((header.flags & EZ_FLAG_ARMV6M) && (header.flags & EZ_FLAG_THUMB2)))
    return arm_ez_reject(DE_INVLDFMT, "unsupported EZ CPU/ABI flags");
#if defined(PICO_RP2040) && PICO_RP2040
  if (header.flags & EZ_FLAG_THUMB2)
    return arm_ez_reject(DE_INVLDFMT, "EZ image requires Thumb-2");
#endif
  if (header.required_dos_api_version > DOS_API_VERSION) {
    dos_printf("ARM EZ: application requires DOS-API version %lu; provided %u\r\n",
               header.required_dos_api_version, (unsigned)DOS_API_VERSION);
    return DE_INVLDFMT;
  }
  if (header.image_mem_size < header.image_file_size)
    return arm_ez_reject(DE_INVLDFMT, "invalid EZ image size");

  image_end = EZ_IMAGE_RVA + header.image_mem_size;
  if (image_end < EZ_IMAGE_RVA)
    return arm_ez_reject(DE_INVLDFMT, "EZ image size overflows RVA space");
  if ((header.entry_rva & 1u) == 0 ||
      (header.entry_rva & ~1ul) < EZ_IMAGE_RVA ||
      (header.entry_rva & ~1ul) >= image_end)
    return arm_ez_reject(DE_INVLDFMT, "invalid EZ entry RVA");

  if (header.reloc_entry_size < sizeof(struct ez_reloc) ||
      (header.reloc_count != 0 &&
       header.reloc_count > 0xfffffffful / header.reloc_entry_size))
    return arm_ez_reject(DE_INVLDFMT, "invalid EZ relocation table");
  reloc_bytes = header.reloc_count * header.reloc_entry_size;
  if (header.image_file_size > 0xfffffffful - (ULONG)header.header_size)
    return arm_ez_reject(DE_INVLDFMT, "EZ image file range overflows");
  image_file_end = (ULONG)header.header_size + header.image_file_size;
  if (header.reloc_offset < image_file_end ||
      header.reloc_offset > 0xfffffffful - reloc_bytes)
    return arm_ez_reject(DE_INVLDFMT, "invalid EZ relocation offset");
  reloc_end = header.reloc_offset + reloc_bytes;

  file_size = SftGetFsize(fd);
  if ((LONG)file_size < 0) {
    dos_printf("ARM EZ: cannot determine file size, DOS error %ld\r\n",
               (LONG)file_size);
    return (COUNT)file_size;
  }
  if (image_file_end > file_size) {
    dos_printf("ARM EZ: truncated image: file=%lu bytes, need=%lu "
               "(header=%u image=%lu)\r\n",
               file_size, image_file_end, (unsigned)header.header_size,
               header.image_file_size);
    return DE_INVLDFMT;
  }
  if (reloc_end > file_size) {
    dos_printf("ARM EZ: truncated relocation table: file=%lu bytes, need=%lu "
               "(offset=%lu count=%lu entry=%u)\r\n",
               file_size, reloc_end, header.reloc_offset, header.reloc_count,
               (unsigned)header.reloc_entry_size);
    return DE_INVLDFMT;
  }

  native_stack_size = header.native_stack_size != 0
                    ? header.native_stack_size
                    : ARM_ELF_DEFAULT_NATIVE_STACK_SIZE;
  dos_stack_size = header.dos_stack_size != 0
                 ? header.dos_stack_size
                 : ARM_ELF_DEFAULT_DOS_STACK_SIZE;
  native_stack_size = arm_elf_align_up(native_stack_size, 8u);
  dos_stack_size = arm_elf_align_up(dos_stack_size, 16u);
  if (dos_stack_size != 0xfffffffful &&
      dos_stack_size < ARM_ELF_MIN_DOS_STACK_SIZE)
    dos_stack_size = ARM_ELF_MIN_DOS_STACK_SIZE;
  if (native_stack_size == 0xfffffffful || dos_stack_size == 0xfffffffful ||
      native_stack_size == 0 || dos_stack_size == 0 ||
      dos_stack_size > 0x10000ul)
    return arm_ez_reject(DE_INVLDFMT, "invalid EZ stack requirements");

  /* First calculate the historical all-in-DOS layout as a candidate. */
  low_meta_off = arm_elf_align_up(image_end, 4u);
  if (low_meta_off != 0xfffffffful &&
      low_meta_off <= 0xfffffffful - sizeof(*meta)) {
    ULONG low_cursor = arm_elf_align_up(low_meta_off + sizeof(*meta), 4u);
    if (low_cursor != 0xfffffffful &&
        low_cursor <= 0xfffffffful - ARM_ELF_ARG_AREA_SIZE) {
      low_final_end = arm_elf_align_up(low_cursor + ARM_ELF_ARG_AREA_SIZE, 16u);
      if (low_final_end != 0xfffffffful && low_final_end <= 0xffff0ul)
        low_required_paras = (UWORD)((low_final_end + 15u) >> 4);
    }
  }

  /*
   * LOAD_HIGH is part of the EXEC request itself.  Do not depend on the
   * caller having preconfigured the global UMB link/allocation strategy:
   * handle AX=4B80h here exactly like the ordinary COM/MZ loaders.
   */
  umb_state = fdos_lol_uppermem_link();
  orig_mem_access = fdos_dos_mem_access_mode();
  if (mode & LOAD_HIGH) {
    DosUmbLink(1);
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() | 0x80u));
  }

  /*
   * Allocate the environment first, then inspect the largest block that is
   * actually still available below 1 MiB.  Keep the complete EZ image there
   * whenever it fits.  PSRAM is a fallback, never the preferred placement.
   */
  fail_stage = "environment allocation";
  rc = ChildEnvGuest(exp, &env_mcb, linear_to_far((const BYTE *)namep));
  if (rc == SUCCESS) {
    int low_rc;
    fail_stage = "guest largest-block query";
    low_rc = ExecMemLargest(&low_largest, low_required_paras);
    if (low_rc != SUCCESS)
      rc = low_rc;
    else if (low_required_paras != 0 && low_largest >= low_required_paras)
      image_in_low = TRUE;
  }
#if DIAG
  dos_printf("ARM EZ: file=%lu mem=%lu stack=%lu LOW need=%u largest=%u -> %s\r\n",
             header.image_file_size, header.image_mem_size, dos_stack_size,
             (unsigned)low_required_paras, (unsigned)low_largest,
             image_in_low ? "LOW" : "PSRAM");
#endif
  if (rc == SUCCESS && image_in_low) {
    meta_off = low_meta_off;
    cursor = arm_elf_align_up(meta_off + sizeof(*meta), 4u);
    final_end = low_final_end;
    final_paras = low_required_paras;
    fail_stage = "guest process-block allocation";
    rc = ExecMemAlloc(final_paras, &alloc_mcb, &asize);
  } else if (rc == SUCCESS) {
    /* Only PSP/metadata/argv consume DOS memory when the image is in PSRAM. */
    meta_off = arm_elf_align_up(EZ_IMAGE_RVA, 4u);
    if (meta_off == 0xfffffffful || meta_off > 0xfffffffful - sizeof(*meta))
      rc = DE_NOMEM;
    if (rc == SUCCESS) {
      cursor = arm_elf_align_up(meta_off + sizeof(*meta), 4u);
      if (cursor == 0xfffffffful ||
          cursor > 0xfffffffful - ARM_ELF_ARG_AREA_SIZE)
        rc = DE_NOMEM;
    }
    if (rc == SUCCESS) {
      final_end = arm_elf_align_up(cursor + ARM_ELF_ARG_AREA_SIZE, 16u);
      if (final_end == 0xfffffffful || final_end > 0xffff0ul)
        rc = DE_NOMEM;
    }
    if (rc == SUCCESS) {
      final_paras = (UWORD)((final_end + 15u) >> 4);
      fail_stage = "PSRAM-fallback process-block allocation";
      rc = ExecMemAlloc(final_paras, &alloc_mcb, &asize);
    }
  }

  if (mode & LOAD_HIGH) {
    DosUmbLink(umb_state);
    fdos_dos_set_mem_access_mode(orig_mem_access);
    mode &= 0x7f;
  }

  if (rc == SUCCESS) {
    fail_stage = "DOS stack reservation";
    rc = arm_native_reserve_dos_stack(dos_stack_size,
                                      &reserved_stack_mcb,
                                      &reserved_stack_seg);
  }

  if (rc != SUCCESS) {
    dos_printf("ARM EZ: load failed at %s, rc=%d\r\n", fail_stage, rc);
    if (reserved_stack_mcb != 0)
      DosMemFree(reserved_stack_mcb);
    if (env_mcb != 0)
      DosMemFree(env_mcb);
    return (COUNT)rc;
  }
  load_seg = alloc_mcb + 1;

  if (image_in_low) {
    image_base = (BYTE *)arm_native_guest_ptr(MK_FP(load_seg, 0));
    image_data = image_base + EZ_IMAGE_RVA;
    if (arm_elf_read_section(fd, load_seg, EZ_IMAGE_RVA,
                             header.header_size,
                             header.image_file_size) != SUCCESS) {
      rc = DE_INVLDFMT;
      goto fail;
    }
    app_psram_begin =
        (uintptr_t)PSRAM_BASE_ADDR + ARM_ELF_APP_PSRAM_BEGIN_OFFSET;
  } else {
    uintptr_t psram_end = arm_elf_native_stack_arena_begin();

    if (arm_native_process_is_active()) {
      rc = arm_ez_reject(DE_NOMEM,
          "PSRAM fallback is unsafe during nested native EXEC");
      goto fail;
    }
    psram_image_addr =
        (uintptr_t)PSRAM_BASE_ADDR + ARM_ELF_APP_PSRAM_BEGIN_OFFSET;
#if DIAG
    dos_printf("ARM EZ: PSRAM image=%08lx end=%08lx available=%lu need=%lu\r\n",
               (ULONG)psram_image_addr, (ULONG)psram_end,
               (ULONG)(psram_end - psram_image_addr),
               header.image_mem_size);
#endif
    if (psram_image_addr < EZ_IMAGE_RVA ||
        header.image_mem_size > (ULONG)(psram_end - psram_image_addr)) {
      rc = arm_ez_reject(DE_NOMEM, "EZ image does not fit application PSRAM");
      goto fail;
    }
    arm_ff_qspi_claim_until(
        (psram_image_addr + header.image_mem_size + 15u) & ~(uintptr_t)15u);
    image_data = (BYTE *)psram_image_addr;
    image_base = image_data - EZ_IMAGE_RVA;
    fail_stage = "PSRAM image read/staging";
    rc = arm_ez_read_native_image(fd, image_data, header.header_size,
                                  header.image_file_size);
    if (rc != SUCCESS)
      goto fail;
    app_psram_begin =
        (psram_image_addr + header.image_mem_size + 15u) & ~(uintptr_t)15u;
    if (app_psram_begin < psram_image_addr || app_psram_begin > psram_end) {
      rc = arm_ez_reject(DE_NOMEM, "EZ image leaves no valid application PSRAM");
      goto fail;
    }
  }

  if (header.image_mem_size > header.image_file_size)
    nf_memset(image_data + header.image_file_size, 0,
           header.image_mem_size - header.image_file_size);

  for (i = 0; i < header.reloc_count; ++i) {
    struct ez_reloc rel;
    fail_stage = "relocation-table read";
    rc = arm_elf_read_meta(fd,
        header.reloc_offset + i * header.reloc_entry_size,
        &rel, sizeof(rel));
    if (rc != SUCCESS) {
      dos_printf("ARM EZ: relocation %lu read failed, rc=%d\r\n", i, rc);
      goto fail;
    }
    fail_stage = "relocation apply";
    rc = arm_ez_apply_reloc(image_base, &header, &rel);
    if (rc != SUCCESS) {
      dos_printf("ARM EZ: relocation %lu apply failed, rc=%d\r\n", i, rc);
      goto fail;
    }
  }

  meta = (arm_ez_load_meta *)arm_native_guest_ptr(arm_elf_guest_ptr(load_seg, meta_off));
  nf_memset(meta, 0, sizeof(*meta));
  meta->entry_addr = (ULONG)(uintptr_t)(image_base + header.entry_rva);
  meta->native_stack_size = native_stack_size;
  meta->dos_stack_size = dos_stack_size;
  meta->dos_stack_mcb = reserved_stack_mcb;
  meta->dos_stack_seg = reserved_stack_seg;
  reserved_stack_mcb = 0;
  reserved_stack_seg = 0;
  meta->process_info.native_stack_size = native_stack_size;
  meta->process_info.dos_stack_size = dos_stack_size;
  meta->app_heap.begin = (ULONG)app_psram_begin;
  meta->app_heap.end = (ULONG)arm_elf_native_stack_arena_begin();

  fail_stage = "argv construction";
  rc = arm_ez_build_argv(load_seg, meta, &cursor, (ULONG)asize << 4,
                         exp, namep);
  if (rc != SUCCESS)
    goto fail;

  fail_stage = "guest process-block shrink";
  rc = DosMemChange(load_seg, final_paras, NULL);
  if (rc != SUCCESS) {
    dos_printf("ARM EZ: cannot shrink guest block to %u paragraphs\r\n",
               (unsigned)final_paras);
    goto fail;
  }

  DosCloseSft(fd, FALSE);
  setvec(0x22, exec_caller_return_addr());
  child_psp(load_seg, fdos_dos_cu_psp(), load_seg + final_paras);
  fcbcode = patchPSPGuest(alloc_mcb, env_mcb, exp, linear_to_far(namep));
  (void)fcbcode;

  arm_native_assign_dos_stack_owner(meta->dos_stack_mcb, load_seg);

  CfgDbgPrintf(("ARM EZ loaded: psp=%04x image=%s:%08lx+%lu entry=%08lx argc=%u "
                "DOS-stack=%04x:0000 reloc=%lu\n",
                load_seg, image_in_low ? "LOW" : "PSRAM",
                (ULONG)(uintptr_t)image_data, header.image_mem_size,
                meta->entry_addr,
                (unsigned)meta->argc, meta->dos_stack_seg,
                header.reloc_count));
  return exec_run_arm_ez(load_seg, meta);

fail:
  dos_printf("ARM EZ: load failed at %s, rc=%d\r\n", fail_stage, rc);
  DosCloseSft(fd, FALSE);

//fail_closed:
  if (meta != NULL && meta->dos_stack_mcb != 0)
    DosMemFree(meta->dos_stack_mcb);
  else if (reserved_stack_mcb != 0)
    DosMemFree(reserved_stack_mcb);
  DosMemFree(alloc_mcb);
  DosMemFree(env_mcb);
  return (COUNT)rc;
}

static COUNT DosArmElfLoader(exec_blk *exp, COUNT mode, COUNT fd, BYTE *namep)
{
  arm_elf32_ehdr eh;
  arm_elf_load_meta *meta = NULL;
  ULONG metadata_size;
  ULONG metadata_off;
  ULONG initial_cursor;
  ULONG final_end;
  ULONG paras_long;
  ULONG req_idx, requirements_idx;
  ULONG entry_idx, init_idx, main_idx, fini_idx, sig_idx;
  native_ez_process_requirements crt_requirements;
  UWORD alloc_mcb, asize = 0, load_seg;
  UWORD env_mcb = 0;
  UWORD fcbcode;
  UWORD final_paras;
  UBYTE umb_state;
  UBYTE orig_mem_access;
  UBYTE load_high_requested;
  int rc;

  load_high_requested = (mode & LOAD_HIGH) != 0;

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

  umb_state = fdos_lol_uppermem_link();
  orig_mem_access = fdos_dos_mem_access_mode();
  if (mode & LOAD_HIGH) {
    DosUmbLink(1);
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() | 0x80u));
  }

  rc = ChildEnvGuest(exp, &env_mcb, linear_to_far((const BYTE *)namep));
  if (rc == SUCCESS) {
    /*
     * Reserve the largest available DOS block while loading.  ET_REL discovery
     * is recursive, so the final image size is not known until all reachable
     * sections have been relocated.  Owning the whole block avoids growing the
     * MCB once per newly reached section; the unused tail is returned below by
     * the existing final DosMemChange().
     */
    rc = ExecMemLargest(&asize, (UWORD)paras_long);
    if (rc == SUCCESS) {
      /*
       * Leave 64 KiB outside the primary ELF MCB until CRT requirements are
       * known.  Unlike allocating a provisional stack MCB first, this preserves
       * the primary image base and therefore does not force earlier section
       * splitting merely because stack space was reserved.
       */
      if (asize <= 0x1000u ||
          (UWORD)(asize - 0x1000u) < (UWORD)paras_long) {
        rc = DE_NOMEM;
      } else {
        asize = (UWORD)(asize - 0x1000u);
        rc = ExecMemAlloc(asize, &alloc_mcb, &asize);
      }
    }
  }

  if (mode & LOAD_HIGH) {
    DosUmbLink(umb_state);
    fdos_dos_set_mem_access_mode(orig_mem_access);
    mode &= 0x7f;
  }

  if (rc != SUCCESS) {
    if (env_mcb != 0)
      DosMemFree(env_mcb);
    dos_printf("ARM ELF: cannot reserve largest guest block (minimum %lu bytes)\r\n",
               paras_long << 4);
    return (COUNT)rc;
  }
  load_seg = alloc_mcb + 1;

  /* Persistent loader metadata belongs to the child allocation. */
  meta = (arm_elf_load_meta *)arm_native_guest_ptr(
      arm_elf_guest_ptr(load_seg, metadata_off));
  nf_memset(meta, 0, metadata_size);
  meta->loader_started_us = get_uticks();
  meta->eh = eh;
  meta->shnum = eh.shnum;
  meta->cursor = initial_cursor;
  meta->allocation_end = (ULONG)asize << 4;
  meta->allocation_high = load_high_requested;
  meta->app_heap.begin =
      (ULONG)((uintptr_t)PSRAM_BASE_ADDR + ARM_ELF_APP_PSRAM_BEGIN_OFFSET);
  meta->app_heap.end = (ULONG)arm_elf_native_stack_arena_begin();

  /*
   * argv is permanent child runtime state, not part of the relocatable ELF
   * image.  Reserve it before recursive section placement so LOW/UMB chunks
   * cannot consume the tail of the primary guest block first.
   */
  rc = arm_elf_build_argv(load_seg, meta, exp, namep);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot reserve argv area in child memory\r\n");
    goto fail;
  }

  rc = arm_elf_find_tables(fd, meta);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot find a valid SHT_SYMTAB/SHT_STRTAB pair\r\n");
    goto fail;
  }
  rc = arm_elf_find_roots(fd, meta, &req_idx, &requirements_idx, &entry_idx,
                          &init_idx, &main_idx, &fini_idx, &sig_idx);
  if (rc != SUCCESS) {
    dos_printf("ARM ELF: cannot scan application entry symbols\r\n");
    goto fail;
  }
  if (entry_idx != ARM_ELF_NO_SYMBOL) {
    rc = arm_elf_read_crt_requirements(fd, meta, &crt_requirements);
    if (rc != SUCCESS) {
      dos_printf("ARM ELF: invalid __native_ez_process_requirements\r\n");
      goto fail;
    }
    meta->required_api_version = crt_requirements.required_dos_api_version;

    rc = arm_elf_set_stack_sizes(
        meta,
        crt_requirements.native_stack_size != 0
            ? crt_requirements.native_stack_size
            : ARM_ELF_DEFAULT_NATIVE_STACK_SIZE,
        crt_requirements.dos_stack_size != 0
            ? crt_requirements.dos_stack_size
            : ARM_ELF_DEFAULT_DOS_STACK_SIZE);
    if (rc != SUCCESS)
      goto fail;

    if (meta->required_api_version == 0)
      meta->required_api_version = DOS_API_VERSION;
    if (meta->required_api_version > DOS_API_VERSION) {
      dos_printf("ARM ELF: application requires DOS-API version %ld; provided %u\r\n",
                 meta->required_api_version, (unsigned)DOS_API_VERSION);
      rc = DE_INVLDFMT;
      goto fail;
    }

    /*
     * A 64 KiB tail was left outside the primary ELF MCB before loading began.
     * Requirements are known now, so allocate only the exact low-memory stack
     * size.  The unused part of that reservation remains available for later
     * LOW section chunks.
     */
    rc = arm_native_reserve_dos_stack_low(meta->dos_stack_size,
                                          &meta->dos_stack_mcb,
                                          &meta->dos_stack_seg);
    if (rc != SUCCESS) {
      arm_elf_loader_progress_end(meta);
      dos_printf("ARM ELF: cannot reserve %lu-byte low DOS stack\r\n",
                 meta->dos_stack_size);
      goto fail;
    }

    /*
     * New CRT ABI: __ez_start is the only application root.  It owns the
     * complete startup/shutdown sequence, so the kernel must not separately
     * root or invoke _init/main/_fini/signal.  Build the ET_REL startup arrays
     * first because __ez_start refers to their linker boundary symbols.
     */
    rc = arm_elf_build_startup_arrays(fd, load_seg, meta);
    if (rc != SUCCESS) {
      dos_printf("ARM ELF: cannot build startup init arrays\r\n");
      goto reloc_fail;
    }
    rc = arm_elf_load_root(fd, load_seg, meta, entry_idx, &meta->entry_addr);
    if (rc != SUCCESS)
      goto reloc_fail;
  } else {
    if (main_idx == ARM_ELF_NO_SYMBOL) {
      rc = DE_INVLDFMT;
      dos_printf("ARM ELF: global main() entry symbol not found\r\n");
      goto fail;
    }

    /* Legacy native ELF ABI.  Keep its independent kernel-owned roots. */
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

    rc = arm_elf_build_startup_arrays(fd, load_seg, meta);
    if (rc != SUCCESS) {
      dos_printf("ARM ELF: cannot build startup init arrays\r\n");
      goto reloc_fail;
    }
  }

  /* Do not let a diagnostic continue on the sparse progress line. */
  arm_elf_loader_progress_end(meta);

  rc = arm_elf_preflight(meta);
  if (rc != SUCCESS)
    goto fail;

  if (meta->entry_addr != 0) {
    meta->process_info.native_stack_size = meta->native_stack_size;
    meta->process_info.dos_stack_size = meta->dos_stack_size;
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
    ULONG hard_limit = 0xffff0ul;
    ULONG usable_limit = meta->allocation_end < hard_limit
                         ? meta->allocation_end : hard_limit;
    arm_elf_loader_progress_end(meta);
    dos_printf("ARM ELF: final image does not fit guest block\r\n");
    dos_printf("  cursor=%lu final=%lu reserved=%lu usable-limit=%lu shortfall=%lu\r\n",
               meta->cursor, final_end, meta->allocation_end, usable_limit,
               final_end > usable_limit ? final_end - usable_limit : 0);
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
  child_psp(load_seg, fdos_dos_cu_psp(), load_seg + final_paras);
  fcbcode = patchPSPGuest(alloc_mcb, env_mcb, exp, linear_to_far(namep));
  (void)fcbcode;

  if (meta->dos_stack_mcb != 0) {
    arm_native_assign_dos_stack_owner(meta->dos_stack_mcb, load_seg);
  } else {
    rc = arm_elf_alloc_dos_stack(meta, load_seg);
    if (rc != SUCCESS) {
      dos_printf("ARM ELF: cannot allocate %lu-byte DOS stack\r\n",
                 meta->dos_stack_size);
      goto fail;
    }
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
  arm_elf_loader_progress_end(meta);
  dos_printf("ARM ELF: dependency loading/relocation failed, DOS error %d\r\n",
             (int)rc);
fail:
  if (meta != NULL && meta->dos_stack_mcb != 0)
    DosMemFree(meta->dos_stack_mcb);
  if (meta != NULL)
    arm_elf_free_extra_blocks(meta);
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
  return fdos_sft_size(s);
}

/* dsk: 0 = current default drive, 1 = A:, 2 = B:, ... */
static int cds_drive_valid(unsigned dsk)
{
  if (dsk == 0)
    dsk = fdos_dos_default_drive() + 1;
  return dsk != 0 && !far_is_null(get_cds(dsk - 1));
}

/*
 * Compare two SETVER filename fields case-insensitively.
 *
 * Compare a native program basename with a filename stored in the guest
 * SETVER table without creating a native alias for the guest bytes.
 */
STATIC WORD SetverCompareFilenameGuest(const BYTE *native_name,
                                       uint32_t guest_name,
                                       COUNT count)
{
  while (count--)
  {
    BYTE guest_ch = pload8(guest_name++);
    BYTE native_ch = *native_name++;

    if (toupper((unsigned char)native_ch) !=
        toupper((unsigned char)guest_ch))
      return (WORD)((unsigned char)native_ch - (unsigned char)guest_ch);
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
  uint32_t table;
  COUNT name_len;

  if (far_is_null(table_ptr) || name == NULL)
    return 0;

  table = task_guest_linear(table_ptr);
  name_len = (COUNT)strlen((const char *)name);

  for (;;)
  {
    BYTE len = pload8(table);

    if (len == 0)
      break;

    if (len == name_len &&
        SetverCompareFilenameGuest(name, table + 1u, len) == 0)
      return pload16(table + (uint32_t)len + 1u);

    table += (uint32_t)len + 3u;
  }

  return 0;
}

/*
   allocate memory for and copy the current process's env to a new
   child environment. Returns the segment of the env's *MCB* (not the
   env block itself) in *pChildEnvSeg.
*/
STATIC COUNT ChildEnvGuest(exec_blk *exp, UWORD *pChildEnvSeg,
                           dos_far_ptr pathname)
{
  uint32_t src_linear = 0;
  uint32_t dst_linear;
  UWORD nEnvSize;
  COUNT ret;
  UWORD parent_psp;
  UWORD parent_env = 0;

  *pChildEnvSeg = 0;

  parent_psp = task_idata_read16(offsetof(struct dos_data, cu_psp));
  if (parent_psp != 0)
    parent_env = pload16(task_guest_seg_linear(parent_psp) +
                         offsetof(psp, ps_environ));

  if (exp->exec.env_seg)
    src_linear = task_guest_seg_linear(exp->exec.env_seg);
  else if (parent_env)
    src_linear = task_guest_seg_linear(parent_env);

  nEnvSize = 1;
  if (src_linear != 0)
  {
    for (nEnvSize = 0;; ++nEnvSize)
    {
      if (nEnvSize >= MAXENV - ENV_KEEPFREE)
        return DE_INVLDENV;
      if (pload16(src_linear + nEnvSize) == 0)
        break;
    }
    nEnvSize += 2;
  }

  if ((ret = DosMemAlloc((nEnvSize + ENV_KEEPFREE + 15) / 16,
                         task_idata_read8(offsetof(struct dos_data,
                                                   mem_access_mode)),
                         pChildEnvSeg, NULL)) < SUCCESS)
    return ret;

  dst_linear = task_guest_seg_linear((UWORD)(*pChildEnvSeg + 1u));

  if (src_linear != 0)
  {
    UWORD i;
    for (i = 0; i < nEnvSize; ++i)
      pstore8(dst_linear + i, pload8(src_linear + i));
    dst_linear += nEnvSize;
  }
  else
  {
    pstore8(dst_linear++, 0);
  }

  pstore16(dst_linear, 1);
  dst_linear += sizeof(UWORD);

  if ((ret = truename_guest(
           pathname,
           MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, PriPathBuffer))),
           CDS_MODE_SKIP_PHYSICAL)) < SUCCESS)
  {
    dpb_watch_check_chain("ChildEnv 1");
    return ret;
  }

  dpb_watch_check_chain("ChildEnv 2");
  {
    uint32_t pri =
        task_idata_linear + offsetof(struct dos_data, PriPathBuffer);
    size_t i;
    for (i = 0; i < sizeof(((struct dos_data *)0)->PriPathBuffer); ++i)
    {
      UBYTE c = pload8(pri + (uint32_t)i);
      pstore8(dst_linear + (uint32_t)i, c);
      if (c == 0)
        break;
    }
  }
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
  guest_move_block(task_guest_seg_linear(para),
                   task_guest_seg_linear(cur_psp), sizeof(psp));
  fdos_psp_set_vector(para, 0x22, getvec(0x22));
  fdos_psp_set_vector(para, 0x23, getvec(0x23));
  fdos_psp_set_vector(para, 0x24, getvec(0x24));
  fdos_psp_set_return_version(
      para,
      ((UWORD)pload8(task_lol_linear + offsetof(struct lol, os_setver_minor)) << 8) |
      pload8(task_lol_linear + offsetof(struct lol, os_setver_major)));
}

void child_psp(seg para, seg cur_psp, int psize)   /* exported: INT 21h AH=55h */
{
  dos_far_ptr parent_jft;
  int i;

  new_psp(para, cur_psp);
  fdos_psp_set_parent(para, cur_psp);
  fdos_psp_set_prev(para, MK_FP(cur_psp, 0));
  fdos_psp_set_size(para, (UWORD)psize);
  fdos_psp_set_max_files(para, 20);
  guest_fill_block(task_guest_seg_linear(para) + offsetof(psp, ps_files),
                   0xff, 20);
  fdos_psp_set_file_table(para, MK_FP(para, offsetof(psp, ps_files)));

  parent_jft = fdos_psp_file_table(cur_psp);
  if (!far_is_null(parent_jft) && !far_is_end(parent_jft)) {
    uint32_t q_jft = task_guest_linear(parent_jft);
    for (i = 0; i < 20; i++) {
      UBYTE h = pload8(q_jft + (uint32_t)i);
      if (h != 0xff) {
        dos_far_ptr sft_ptr = idx_to_sft(h);
        if (!far_is_end(sft_ptr) && !(fdos_sft_mode_raw(sft_ptr) & O_NOINHERIT)) {
          fdos_psp_set_file_handle(para, (UWORD)i, h);
          fdos_sft_inc_ref_raw(sft_ptr);
        }
      }
    }
  }

  fdos_psp_set_fcb_drive(para, 1, 0);
  fdos_psp_clear_fcb_name(para, 1);
  fdos_psp_set_fcb_drive(para, 2, 0);
  fdos_psp_clear_fcb_name(para, 2);
  fdos_psp_set_command_empty(para);
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
  dos_far_ptr fr =
      task_idata_read_far(offsetof(struct dos_data, user_r));

  if (!far_is_null(fr) && !far_is_end(fr))
  {
    uint32_t linear = task_guest_linear(fr);
    UWORD ip = pload16(linear + offsetof(struct int21_guest_iregs, ip));
    UWORD cs = pload16(linear + offsetof(struct int21_guest_iregs, cs));
    return MK_FP(cs, ip);
  }
  return MK_FP(CPU_CS, CPU_IP);
}

STATIC UWORD patchPSPGuest(UWORD pspseg, UWORD envseg,
                           exec_blk *exb, dos_far_ptr fnam)
{
  UWORD psp_mcb_seg;
  UWORD pos = 0, base = 0;
  int i;
  BYTE shortname[13];

  psp_mcb_seg = pspseg;
  ++pspseg;

  guest_move_block(task_guest_seg_linear(pspseg) + offsetof(psp, ps_cmd),
                   task_guest_linear(exb->exec.cmd_line), sizeof(CommandTail));
  if (FP_OFF(exb->exec.fcb_1) != 0xFFFF)
  {
    guest_move_block(task_guest_seg_linear(pspseg) + offsetof(psp, ps_fcb1),
                     task_guest_linear(exb->exec.fcb_1), 16);
    guest_move_block(task_guest_seg_linear(pspseg) + offsetof(psp, ps_fcb2),
                     task_guest_linear(exb->exec.fcb_2), 16);
  }

  fdos_mcb_set_owner(psp_mcb_seg, pspseg);
  if (envseg)
  {
    fdos_mcb_set_owner(envseg, pspseg);
    envseg++;
  }
  fdos_psp_set_environment(pspseg, envseg);

  for (;;)
  {
    UBYTE c = task_far_peek8(fnam, pos);
    if (c == 0)
      break;
    if (c == ':' || c == '/' || c == '\\')
      base = (UWORD)(pos + 1u);
    ++pos;
  }

  nf_memset(shortname, 0, sizeof(shortname));
  for (i = 0; i < 12; ++i)
  {
    UBYTE c = task_far_peek8(fnam, (UWORD)(base + i));
    if (c == 0)
      break;
    shortname[i] = c;
  }

  for (i = 0; i < 8 && shortname[i] != '.' && shortname[i] != '\0'; ++i)
    fdos_mcb_set_name_byte(psp_mcb_seg, (unsigned)i,
                           (BYTE)toupper((unsigned char)shortname[i]));
  if (i < 8)
    fdos_mcb_set_name_byte(psp_mcb_seg, (unsigned)i, 0);

  {
    dos_far_ptr setver_ptr =
        task_guest_read_far(task_lol_linear + offsetof(struct lol, setverPtr));
    UWORD fakever = SetverGetVersion(setver_ptr, shortname);
    if (fakever != 0)
      fdos_psp_set_return_version(pspseg, fakever);
  }

  return (cds_drive_valid(fdos_psp_fcb_drive(pspseg, 1)) ? 0 : 0xff) |
         (cds_drive_valid(fdos_psp_fcb_drive(pspseg, 2)) ? 0 : 0xff00);
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

static void exec_enter_child(uint32_t saved_linear,
                             UWORD child_psp_seg, dos_far_ptr stack,
                             UWORD dses)
{
  struct saved_cpu_ctx saved_cpu;
  dos_far_ptr dta;
  UWORD cu_psp;
  UBYTE byte;

  save_ctx(cpu, &saved_cpu);
  task_guest_write(saved_linear + offsetof(struct exec_child_context, cpu),
                   &saved_cpu, sizeof(saved_cpu));

  cu_psp = task_idata_read16(offsetof(struct dos_data, cu_psp));
  pstore16(saved_linear + offsetof(struct exec_child_context, cu_psp), cu_psp);

  dta = task_idata_read_far(offsetof(struct dos_data, dta));
  task_guest_write_far(saved_linear + offsetof(struct exec_child_context, dta),
                       dta);

  byte = task_idata_read8(offsetof(struct dos_data, InDOS));
  pstore8(saved_linear + offsetof(struct exec_child_context, indos), byte);
  byte = task_idata_read8(offsetof(struct dos_data, ErrorMode));
  pstore8(saved_linear + offsetof(struct exec_child_context, error_mode), byte);
  pstore8(saved_linear + offsetof(struct exec_child_context, terminate),
          terminate_flag ? 1u : 0u);
  pstore8(saved_linear + offsetof(struct exec_child_context, native_done),
          cpu->native_done ? 1u : 0u);

  task_idata_write16(offsetof(struct dos_data, cu_psp), child_psp_seg);
  task_idata_write_far(offsetof(struct dos_data, dta),
                       MK_FP(child_psp_seg,offsetof(psp,ps_cmd)));
  SET_SS(FP_SEG(stack)); CPU_SP=FP_OFF(stack);
  SET_DS(dses); SET_ES(dses);
  terminate_flag=false;
  cpu->native_done=false;
  {
    UBYTE indos = task_idata_read8(offsetof(struct dos_data, InDOS));
    if (indos != 0)
      task_idata_write8(offsetof(struct dos_data, InDOS), (UBYTE)(indos - 1));
  }
}

static void exec_release_child(UWORD child_psp_seg)
{
  setvec(0x22, fdos_psp_vector(child_psp_seg, 0x22));
  setvec(0x23, fdos_psp_vector(child_psp_seg, 0x23));
  setvec(0x24, fdos_psp_vector(child_psp_seg, 0x24));
  if (term_exit_type != 3) {
    int i;
    UWORD maxfiles = fdos_psp_max_files(child_psp_seg);
    for(i=0;i<maxfiles;i++) DosClose(i);
    FcbCloseAll();
    FreeProcessMem(child_psp_seg);
  }
}

static void exec_leave_child(uint32_t saved_linear,
                             UWORD child_psp_seg)
{
  struct saved_cpu_ctx saved_cpu;
  bool outer_terminate;
  bool outer_native_done;
  UWORD saved_cu_psp;
  dos_far_ptr saved_dta;
  UBYTE saved_indos;
  UBYTE saved_error_mode;
  UWORD cleanup_sp = SDA_CHAR_TOS_OFF;
  UWORD parent_frame_sp;

  /*
   * The long-lived frame remains in guest RAM. Only this short-lived CPU
   * snapshot is materialised on the native stack while leaving the child,
   * so nested EXEC levels do not accumulate native-stack usage.
   */
  task_guest_read(saved_linear + offsetof(struct exec_child_context, cpu),
                  &saved_cpu, sizeof(saved_cpu));
  saved_cu_psp =
      pload16(saved_linear + offsetof(struct exec_child_context, cu_psp));
  saved_dta =
      task_guest_read_far(saved_linear + offsetof(struct exec_child_context, dta));
  saved_indos =
      pload8(saved_linear + offsetof(struct exec_child_context, indos));
  saved_error_mode =
      pload8(saved_linear + offsetof(struct exec_child_context, error_mode));
  outer_terminate =
      pload8(saved_linear + offsetof(struct exec_child_context, terminate)) != 0;
  outer_native_done =
      pload8(saved_linear + offsetof(struct exec_child_context, native_done)) != 0;

  parent_frame_sp =
      (UWORD)((saved_cpu.sp - sizeof(struct exec_child_context)) &
              (UWORD)~1u);
  if (saved_cpu.ss == DOS_PSP &&
      parent_frame_sp >= SDA_DISK_TOS_OFF &&
      parent_frame_sp <= SDA_CHAR_TOS_OFF)
    cleanup_sp = parent_frame_sp;

  cpu->native_done = true;
  terminate_flag = false;
  SET_SS(DOS_PSP);
  CPU_SP = cleanup_sp;
  task_idata_write8(offsetof(struct dos_data, InDOS), saved_indos);
  task_idata_write8(offsetof(struct dos_data, abort_progress), (UBYTE)-1);
  exec_release_child(child_psp_seg);

  task_idata_write16(offsetof(struct dos_data, cu_psp), saved_cu_psp);
  task_idata_write_far(offsetof(struct dos_data, dta), saved_dta);
  task_idata_write8(offsetof(struct dos_data, abort_progress), 0);
  task_idata_write8(offsetof(struct dos_data, ErrorMode), saved_error_mode);
  restore_ctx(cpu, &saved_cpu);

  terminate_flag = outer_terminate;
  cpu->native_done = outer_native_done;
}

enum exec_process_kind
{
  EXEC_PROCESS_GUEST,
  EXEC_PROCESS_NATIVE_COMMAND,
  EXEC_PROCESS_ARM_ELF,
  EXEC_PROCESS_ARM_EZ
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
  arm_ez_load_meta *arm_ez;
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

static int arm_native_process_is_active(void)
{
  return arm_elf_active_main_sp != NULL || arm_ez_active_process_info != NULL;
}

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
    /*
     * cpu_step()/pc_step() checks a pending IRQ before native_done on the
     * next interpreter iteration.  Once the guest handler has IRET'ed to this
     * synthetic return point, prevent another IRQ from being accepted before
     * arm_elf_service_guest_irq() regains control.
     */
    ifl = 0;
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
  bool old_ifl;

  if (pc == NULL || cpu == NULL || !cpu->intr)
    return;

  save_ctx(cpu, &saved);
  old_native_done = cpu->native_done;
  old_pending_trap = cpu_pending_trap();
  old_ifl = ifl;

  nf_memset(&params, 0, sizeof(params));
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
  ifl = old_ifl;
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
  unsigned int n;

  pc_service(pc);

  /*
   * pc_service() can make several guest IRQs pending at once (for example
   * keyboard make/break bytes plus PIT or mouse).  Service a bounded batch
   * here so each IRQ still goes through the normal PIC/IVT/EOI path, instead
   * of trying to drain a device FIFO from inside one native IRQ handler.
   * The bound prevents a continuously active device from starving the native
   * application.
   */
  for (n = 0; n < 16u && cpu != NULL && cpu->intr; ++n) {
    arm_elf_service_guest_irq();
    if (terminate_requested())
      break;
  }

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

static int __attribute__((noinline)) arm_elf_run_body(void *opaque)
{
  arm_elf_load_meta *meta = (arm_elf_load_meta *)opaque;
  void *fini_ctx = NULL;
  int result;

  /*
   * New CRT ABI owns startup completely.  The kernel only supplies argc/argv
   * and the native stack; __ez_start performs init/main/fini itself.
   */
  if (meta->entry_addr != 0) {
    volatile ULONG *saved_main_sp_slot = arm_elf_active_main_sp;
    arm_elf_active_main_sp = &meta->native_main_sp;
    result = arm_elf_call_main((arm_elf_main_fn)(uintptr_t)meta->entry_addr,
                               meta->argc,
                               (char **)(uintptr_t)meta->argv_addr);
    arm_elf_active_main_sp = saved_main_sp_slot;
    return result;
  }

  /*
   * Legacy final-link/crt startup semantics:
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
arm_native_call_on_stack(void *context, uintptr_t stack_top,
                         uintptr_t stack_bottom,
                         int (*body)(void *))
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

static int arm_ez_run_body(void *opaque)
{
  arm_ez_load_meta *meta = (arm_ez_load_meta *)opaque;
  int (*entry)(int, char **) = (int (*)(int, char **))(uintptr_t)meta->entry_addr;

  return entry(meta->argc, (char **)(uintptr_t)meta->argv_addr);
}

static COUNT exec_run_process(const struct exec_process_start *start)
{
  UWORD parent_sp = CPU_SP;
  UWORD frame_sp;
  uint32_t saved_linear;

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
  frame_sp =
      (UWORD)((parent_sp - sizeof(struct exec_child_context)) & (UWORD)~1u);
  CPU_SP = frame_sp;
  saved_linear = ((uint32_t)CPU_SS << 4) + frame_sp;

  exec_enter_child(saved_linear, start->child_psp,
                   start->stack, start->dses);
  /*
   * save_ctx() saw the reserved frame at SS:frame_sp. Preserve the original
   * parent SP in the guest-resident context.
   */
  pstore16(saved_linear +
               offsetof(struct exec_child_context, cpu) +
               offsetof(struct saved_cpu_ctx, sp),
           parent_sp);
  if (start->kind != EXEC_PROCESS_ARM_ELF &&
      start->kind != EXEC_PROCESS_ARM_EZ)
    exec_set_initial_registers(start);

  if (start->kind == EXEC_PROCESS_ARM_ELF ||
      start->kind == EXEC_PROCESS_ARM_EZ)
  {
    arm_elf_load_meta *elf_meta = start->arm_elf;
    arm_ez_load_meta *ez_meta = start->arm_ez;
    ULONG native_stack_size = elf_meta ? elf_meta->native_stack_size
                                       : ez_meta->native_stack_size;
    ULONG dos_stack_size = elf_meta ? elf_meta->dos_stack_size
                                    : ez_meta->dos_stack_size;
    UWORD dos_stack_seg = elf_meta ? elf_meta->dos_stack_seg
                                   : ez_meta->dos_stack_seg;
    UWORD *dos_stack_mcb = elf_meta ? &elf_meta->dos_stack_mcb
                                    : &ez_meta->dos_stack_mcb;
    uintptr_t stack_bottom;
    uintptr_t stack_top;
    uintptr_t previous_stack_cursor;
    int exit_code;
    const native_ez_process_info *saved_ez_process_info =
        arm_ez_active_process_info;
    arm_app_heap_context *saved_app_heap = arm_app_active_heap;

    if (arm_elf_native_stack_acquire(native_stack_size, &stack_bottom,
                                     &previous_stack_cursor) != SUCCESS) {
      dos_printf("ARM native: PSRAM stack arena exhausted (%lu bytes)\r\n",
                 native_stack_size);
      exec_leave_child(saved_linear, start->child_psp);
      return DE_NOMEM;
    }
    if (elf_meta)
      elf_meta->native_stack_addr = (ULONG)stack_bottom;
    else
      ez_meta->native_stack_addr = (ULONG)stack_bottom;
    stack_top = stack_bottom + native_stack_size;

    if (ez_meta) {
      arm_ez_active_process_info = &ez_meta->process_info;
      arm_app_active_heap = &ez_meta->app_heap;
    } else if (elf_meta->entry_addr != 0) {
      arm_ez_active_process_info = &elf_meta->process_info;
      arm_app_active_heap = &elf_meta->app_heap;
    }

    {
      uintptr_t saved_diag_native_bottom = doom_diag_native_stack_bottom;
      uintptr_t saved_diag_native_top = doom_diag_native_stack_top;
      UWORD saved_diag_dos_seg = doom_diag_dos_stack_seg;
      UWORD saved_diag_dos_size = doom_diag_dos_stack_size;

      doom_diag_native_stack_bottom = stack_bottom;
      doom_diag_native_stack_top = stack_top;
      doom_diag_dos_stack_seg = dos_stack_seg;
      doom_diag_dos_stack_size = (UWORD)dos_stack_size;

      diag_native_code_enter();
      if (elf_meta)
        exit_code = arm_native_call_on_stack(elf_meta, stack_top, stack_bottom,
                                             arm_elf_run_body);
      else
        exit_code = arm_native_call_on_stack(ez_meta, stack_top, stack_bottom,
                                             arm_ez_run_body);
      diag_native_code_leave();

      doom_diag_native_stack_bottom = saved_diag_native_bottom;
      doom_diag_native_stack_top = saved_diag_native_top;
      doom_diag_dos_stack_seg = saved_diag_dos_seg;
      doom_diag_dos_stack_size = saved_diag_dos_size;
    }

    if (ez_meta || (elf_meta && elf_meta->entry_addr != 0)) {
      arm_ez_active_process_info = saved_ez_process_info;
      arm_app_active_heap = saved_app_heap;
    }

    arm_elf_native_stack_release(stack_bottom, previous_stack_cursor);
    if (elf_meta)
      elf_meta->native_stack_addr = 0;
    else
      ez_meta->native_stack_addr = 0;

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
        *dos_stack_mcb != 0) {
      DosMemFree(*dos_stack_mcb);
      *dos_stack_mcb = 0;
      if (elf_meta)
        elf_meta->dos_stack_seg = 0;
      else
        ez_meta->dos_stack_seg = 0;
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

  exec_leave_child(saved_linear, start->child_psp);
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
  start.arm_ez = NULL;

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
  start.arm_ez = NULL;
  return exec_run_process(&start);
}

static COUNT exec_run_arm_ez(UWORD child_psp_seg, arm_ez_load_meta *meta)
{
  struct exec_process_start start;

  start.entry = MK_FP(child_psp_seg, 0);
  start.stack = MK_FP(meta->dos_stack_seg,
                      (UWORD)meta->dos_stack_size);
  start.dses = child_psp_seg;
  start.ax_bx = 0;
  start.child_psp = child_psp_seg;
  start.kind = EXEC_PROCESS_ARM_EZ;
  start.arm_elf = NULL;
  start.arm_ez = meta;
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
  start.arm_ez = NULL;

  return exec_run_process(&start);
}

STATIC int load_transfer(UWORD ds, exec_blk * exp, UWORD fcbcode, COUNT mode)
{
  fdos_psp_set_parent(ds, fdos_dos_cu_psp());
  fdos_psp_set_prev(ds, MK_FP(fdos_dos_cu_psp(), 0));

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
  pstore16(task_guest_linear(exp->exec.stack), fcbcode);
  return SUCCESS;
}

/* Find out how many paragraphs are available, considering a
   threshold, trying HIGH then LOW memory. */
STATIC int ExecMemLargest(UWORD * asize, UWORD threshold)
{
  int rc;

  if (fdos_dos_mem_access_mode() & 0x80)
  {
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0x80u));
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() | 0x40u));
    rc = DosMemLargest(asize);
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0x40u));
    if (rc != SUCCESS || *asize < threshold)
      rc = DosMemLargest(asize);
    fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() | 0x80u));
  }
  else
    rc = DosMemLargest(asize);

  return (*asize < threshold ? DE_NOMEM : rc);
}

STATIC int ExecMemAlloc(UWORD size, seg * para, UWORD * asize)
{
  int rc = DosMemAlloc(size, fdos_dos_mem_access_mode(), para, asize);

  if (rc != SUCCESS)
  {
    if (rc == DE_NOMEM)
    {
      rc = DosMemAlloc(0, LARGEST, para, asize);
      if ((fdos_dos_mem_access_mode() & 0x80) && (rc != SUCCESS))
      {
        fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() & (UBYTE)~0x80u));
        rc = DosMemAlloc(0, LARGEST, para, asize);
        fdos_dos_set_mem_access_mode((UBYTE)(fdos_dos_mem_access_mode() | 0x80u));
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

COUNT DosComLoader(dos_far_ptr namep, exec_blk * exp, COUNT mode, COUNT fd)
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
      UBYTE UMBstate = pload8(task_lol_linear + offsetof(struct lol, uppermem_link));
      UBYTE orig_mem_access =
          task_idata_read8(offsetof(struct dos_data, mem_access_mode));

      if (mode & LOAD_HIGH)
      {
        task_idata_write8(offsetof(struct dos_data, mem_access_mode),
                         (UBYTE)(orig_mem_access | 0x80));
        DosUmbLink(1);
      }

      rc = ChildEnvGuest(exp, &env, namep);

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
        task_idata_write8(offsetof(struct dos_data, mem_access_mode),
                         orig_mem_access);
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
    // termination vector (not used by the kernel, but may be used by gues process)
    setvec(0x22, exec_caller_return_addr());
    child_psp(mem,
              task_idata_read16(offsetof(struct dos_data, cu_psp)),
              mem + asize);
    fcbcode = patchPSPGuest(mem - 1, env, exp, namep);

    if (asize > 0x1000)
      asize = 0x1000;
    if (asize < 0x11)
      return DE_NOMEM;
    asize -= 0x11;

    /* CP/M compatibility: far-call-to-0:00C0h stub encoding the
       segment size, at PSP+5 */
    task_guest_write_far(task_guest_seg_linear(mem) +
                             offsetof(psp, ps_reentry),
                         MK_FP((UWORD)(0xc - asize),
                               (UWORD)(asize << 4)));
    asize <<= 4;
    asize += 0x10e;
    exp->exec.stack = MK_FP(mem, asize);
    exp->exec.start_addr = MK_FP(mem, 0x100);
    pstore16(task_guest_seg_linear(mem) + asize, 0);
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

COUNT DosExeLoader(dos_far_ptr namep, exec_blk * exp, COUNT mode, COUNT fd)
{
  UWORD mem, env = 0, start_seg, asize = 0;
  UWORD exe_size;
  UWORD image_size;

  image_size = (EXE_U16(exPages) << 5) - EXE_U16(exHeaderSize);

  if ((mode & 0x7f) != EXEC_OVERLAY)
  {
    UBYTE UMBstate = pload8(task_lol_linear + offsetof(struct lol, uppermem_link));
    UBYTE orig_mem_access =
        task_idata_read8(offsetof(struct dos_data, mem_access_mode));
    COUNT rc;

    image_size += sizeof(psp) / 16;
    exe_size = image_size + EXE_U16(exMinAlloc);

    if (exe_size < image_size)   /* overflow: exMinAlloc==0xffff etc. */
      return DE_NOMEM;

    if (mode & LOAD_HIGH)
    {
      DosUmbLink(1);
      task_idata_write8(offsetof(struct dos_data, mem_access_mode),
                       (UBYTE)(orig_mem_access | 0x80));
    }

    rc = ChildEnvGuest(exp, &env, namep);

    if (rc == SUCCESS)
      rc = ExecMemLargest(&asize, exe_size);

    exe_size = image_size + EXE_U16(exMaxAlloc);
    if (exe_size > asize || exe_size < image_size)
      exe_size = asize;

    /* exMinAlloc==exMaxAlloc==0: allocate the largest possible block
       and load the image as high in it as possible */
    if ((EXE_U16(exMinAlloc) | EXE_U16(exMaxAlloc)) == 0)
      exe_size = asize;

    if (rc == SUCCESS)
      rc = ExecMemAlloc(exe_size, &mem, &asize);
    if (rc != SUCCESS)
      DosMemFree(env);

    if (mode & LOAD_HIGH)
    {
      task_idata_write8(offsetof(struct dos_data, mem_access_mode),
                       orig_mem_access);
      DosUmbLink(UMBstate);
    }
    if (rc != SUCCESS)
      return rc;

    mode &= 0x7f;
    ++mem;
  }
  else
    mem = exp->load.load_seg;

  if (SftSeek(fd, (LONG) EXE_U16(exHeaderSize) * 16UL, SEEK_SET) < SUCCESS)
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
    if (exe_size > 0 && (EXE_U16(exMinAlloc) | EXE_U16(exMaxAlloc)) == 0)
    {
      start_seg += fdos_mcb_size((seg)(mem - 1)) - image_size;
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

    SftSeek(fd, (LONG) EXE_U16(exRelocTable), SEEK_SET);
    for (i = 0; i < EXE_U16(exRelocItems); i++)
    {
      uint32_t spot;
      UWORD reloc_off;
      UWORD reloc_seg;

      if (DosRWSft(fd, sizeof(UWORD) * 2, task_sec_path_far(TASK_RELOC_OFF),
                   XFR_READ) != sizeof(UWORD) * 2)
      {
        if (mode != EXEC_OVERLAY)
        {
          DosMemFree(--mem);
          DosMemFree(env);
        }
        return DE_INVLDDATA;
      }
      reloc_off = task_sec_read16(TASK_RELOC_OFF);
      reloc_seg = task_sec_read16(TASK_RELOC_OFF + sizeof(UWORD));
      if (mode == EXEC_OVERLAY)
      {
        spot = task_guest_linear(
            MK_FP((UWORD)(reloc_seg + mem), reloc_off));
        pstore16(spot, (UWORD)(pload16(spot) + exp->load.reloc));
      }
      else
      {
        spot = task_guest_linear(
            MK_FP((UWORD)(reloc_seg + start_seg), reloc_off));
        pstore16(spot, (UWORD)(pload16(spot) + start_seg));
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
    child_psp(mem,
              task_idata_read16(offsetof(struct dos_data, cu_psp)),
              mem + asize);
    fcbcode = patchPSPGuest(mem - 1, env, exp, namep);
    exp->exec.stack = MK_FP(EXE_U16(exInitSS) + start_seg, EXE_U16(exInitSP));
    exp->exec.start_addr = MK_FP(EXE_U16(exInitCS) + start_seg, EXE_U16(exInitIP));
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
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
  /* Runtime floor follows the selected core0 stack: normally TEXT_BUFFER,
     or the unused tail of GFX_BUFFER when reduced VRAM uses direct QSPI RAM. */
  uint32_t sp;
  __asm volatile ("mov %0, sp" : "=r" (sp));
  uint32_t floor = (uint32_t)core0_stack_floor_runtime;
  return sp > floor ? sp - floor : 0;
#else
  return 0xffffffffu;   /* host-сборки для статического анализа */
#endif
}
#endif

static COUNT DosExecFar(COUNT mode, exec_blk *ep, dos_far_ptr x86_lp)
{
  COUNT rc;
  COUNT fd;
  long openresult;
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
      task_guest_is_command_com(x86_lp))
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
    UWORD child_env_mcb = 0;
    COUNT env_rc;

    /*
     * Match ordinary EXEC semantics: ChildEnv() copies either the explicit
     * EPB environment or, for env_seg == 0, the current process environment,
     * and appends argv[0].  FCOM owns that copy for its whole lifetime.
     */
    env_rc = ChildEnvGuest(ep, &child_env_mcb, x86_lp);
    if (env_rc < SUCCESS)
      return env_rc;

    {
      UWORD command_psp;
      UWORD fcbcode;

      command_psp=fcom_create_process_guest_tail(ep->exec.cmd_line,
                                      mode & LOAD_HIGH,
                                      task_idata_read16(offsetof(struct dos_data,
                                                                 cu_psp)),
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
      fcbcode=patchPSPGuest(command_psp - 1,child_env_mcb,ep,x86_lp);

      /* tail уже скопирован в PSP:80h ребёнка; patchPSP() отдельно
         установил caller-supplied FCB1/FCB2 из EXEC parameter block.
         Слот SDA_EXEC_TAIL_OFF с этого момента свободен для любых
         вложенных EXEC. */
      return exec_run_native_command(command_psp,fcbcode);
    }
  }
  
  if ((mode & 0x7f) > EXEC_OVERLAY || (mode & 0x7f) == 2)
    return DE_INVLDFMT;

  dos_api_memcpy(&TempExeBlock, ep, sizeof(exec_blk));

  dos_far_ptr x86_dhp = task_guest_is_device(x86_lp);
  if (EFFECTIVE(x86_dhp) ||           /* don't try to "execute" e.g. C:\NUL */
      (openresult = DosOpenSft(x86_lp, O_LEGACY | O_OPEN | O_RDONLY, 0)) < SUCCESS) {
    dpb_watch_check_chain("DosExec err");
    return DE_FILENOTFND;
  }
  dpb_watch_check_chain("DosExec");
  fd = (COUNT) (openresult & 0xffff);

  rc = (int) DosRWSft(fd, sizeof(exe_header),
                      task_sec_path_far(0),
                      XFR_READ);

  if (rc == sizeof(exe_header) &&
      (EXE_U16(exSignature) == MAGIC || EXE_U16(exSignature) == OLD_MAGIC))
    rc = DosExeLoader(x86_lp, &TempExeBlock, mode, fd);
  else if (rc >= 2 &&
           task_sec_read8(0) == 'E' &&
           task_sec_read8(1) == 'Z')
  {
    if (!arm_native_runtime_available())
    {
      DosCloseSft(fd, FALSE);
      rc = arm_ez_reject(DE_INVLDFMT,
                         "native ARM executables require QSPI PSRAM");
    }
    else
      rc = DosArmEzLoader(&TempExeBlock, mode, fd,
                          (BYTE *)arm_native_guest_ptr(x86_lp));
  }
  else if (rc >= 4 &&
           task_sec_read8(0) == 0x7f &&
           task_sec_read8(1) == 'E' &&
           task_sec_read8(2) == 'L' &&
           task_sec_read8(3) == 'F')
  {
    if (!arm_native_runtime_available())
    {
      DosCloseSft(fd, FALSE);
      rc = arm_elf_reject(DE_INVLDFMT,
                          "native ARM executables require QSPI PSRAM");
    }
    else
      rc = DosArmElfLoader(&TempExeBlock, mode, fd,
                           (BYTE *)arm_native_guest_ptr(x86_lp));
  }
  else if (rc != 0)
    rc = DosComLoader(x86_lp, &TempExeBlock, mode, fd);
  else
  {
    DosCloseSft(fd, FALSE);
    return DE_INVLDFMT;
  }

  if (mode == EXEC_LOAD && rc == SUCCESS)
    dos_api_memcpy(ep, &TempExeBlock, sizeof(exec_blk));

  return rc;
}


COUNT DosExec(COUNT mode, exec_blk *ep, BYTE *lp)
{
  return DosExecFar(mode, ep, linear_to_far((const BYTE *)lp));
}

COUNT DosExecGuest(COUNT mode, exec_blk *ep, dos_far_ptr x86_lp)
{
  return DosExecFar(mode, ep, x86_lp);
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
  size_t tail_off, end_off;
  exec_blk exb;
  UBYTE mode = Config->cfgP_0_startmode;
  const dos_far_ptr shell_far = task_sec_path_far(0);
  const uint32_t shell_linear = task_sec_path_linear;
  const size_t shell_capacity = sizeof(((struct dos_data *)0)->SecPathBuffer);

  /* build exec block and save all parameters here as init part will vanish! */
  exb.exec.fcb_1 = exb.exec.fcb_2 = MK_FP(0xffff, 0xffff);
  exb.exec.env_seg = DOS_PSP + 8;
  {
    size_t init_len = strnlen((const char *)Config->cfgInit, shell_capacity - 1u);
    size_t tail_len = strnlen((const char *)Config->cfgInitTail,
                              shell_capacity - init_len - 1u);
    guest_write_block(shell_linear, Config->cfgInit, init_len);
    guest_write_block(shell_linear + (uint32_t)init_len,
                      Config->cfgInitTail, tail_len);
    end_off = init_len + tail_len;
    pstore8(shell_linear + (uint32_t)end_off, 0);
  }

  /* Preserve the original P_0 byte layout exactly, but operate on the guest
     SecPathBuffer by offset rather than through a host pointer. */
  if (Config->cfgInitTail[0] == 0)
    tail_off = end_off >= 2u ? end_off - 2u : 0u;
  else
  {
    size_t tab_off = guest_find_byte(shell_linear, '\t', end_off);
    size_t space_off = guest_find_byte(shell_linear, ' ', end_off);
    size_t split_off;
    if (tab_off == SIZE_MAX)
      split_off = space_off;
    else if (space_off == SIZE_MAX)
      split_off = tab_off;
    else
      split_off = tab_off < space_off ? tab_off : space_off;

    if (split_off == SIZE_MAX)
      tail_off = end_off >= 2u ? end_off - 2u : 0u;
    else
    {
      pstore8(shell_linear + (uint32_t)split_off, 0);
      tail_off = split_off + 1u;
    }
  }

  if (tail_off != 0u)
    --tail_off;
  pstore8(shell_linear + (uint32_t)tail_off,
          (UBYTE)((end_off - tail_off) - 2u));
  exb.exec.cmd_line = task_sec_path_far(tail_off);

  DosExecGuest(mode, &exb, shell_far);
  put_string("Bad or missing Command Interpreter: ");
  {
    UBYTE c;
    size_t i = 0;
    while ((c = task_sec_read8(i++)) != 0)
      write_char_stdout(c);
    i = tail_off + 2u;
    while ((c = task_sec_read8(i++)) != 0)
      write_char_stdout(c);
  }
  put_string(" Enter the full shell command line: ");
  {
    COUNT n = res_read(cpu_, STDIN, shell_far, NAMEMAX);
    pstore8(shell_linear + (uint32_t)n, 0);
  }
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
