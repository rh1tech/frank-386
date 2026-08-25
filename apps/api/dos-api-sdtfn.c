#include "conio.h"
#include "string.h"
#include "stdlib.h"
#include "dos-api.h"
#include "dos.h"
#include "fcntl.h"
#include "io.h"
#include "direct.h"
#include "dos_mem.h"
#include "dos_process.h"
#include "ez.h"
#include "dos_vect.h"
#include "cpu.h"
#include "dos_diag.h"
#include "sys/stat.h"
#include "stdio.h"
#include "sound_hw.h"
#include "dos_yield.h"
#include "crt0.h"
#include "native_process.h"
#include <stdarg.h>


const native_ez_process_info *native_ez_get_process_info(void)
{
    typedef const native_ez_process_info *(*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[107])();
}

__attribute__((weak))
native_dos_process_requirements *__native_dos_process_requirements(void)
{
    return 0;
}

uint32_t sound_hw_mask(void)
{
    PC *pc = get_PC();
    uint32_t mask = 0;

    if (!pc)
        return 0;

    if (pc->pcspk_enabled)  mask |= SOUND_HW_PC_SPEAKER;
    if (pc->adlib_enabled)  mask |= SOUND_HW_ADLIB;
    if (pc->sb16_enabled)   mask |= SOUND_HW_SB16;
    if (pc->tandy_enabled)  mask |= SOUND_HW_TANDY;
    if (pc->covox_enabled)  mask |= SOUND_HW_COVOX;
    if (pc->mpu401_enabled) mask |= SOUND_HW_MPU401;
    if (pc->dss_enabled)    mask |= SOUND_HW_DSS;

    return mask;
}

uint8_t inp(uint16_t port)
{
    CPU *cpu = get_PC()->cpu;
    return cpu->cb.io_read8(cpu->cb.io, port);
}

uint16_t inpw(uint16_t port)
{
    CPU *cpu = get_PC()->cpu;
    return cpu->cb.io_read16(cpu->cb.io, port);
}

void outp(uint16_t port, uint8_t value)
{
    CPU *cpu = get_PC()->cpu;
    cpu->cb.io_write8(cpu->cb.io, port, value);
}

void outpw(uint16_t port, uint16_t value)
{
    CPU *cpu = get_PC()->cpu;
    cpu->cb.io_write16(cpu->cb.io, port, value);
}

int kbhit(void)
{
    CPU *cpu = get_PC()->cpu;
    gprx_t saved_gprx[8];
    x86_flags_t saved_flags = cpu->flags;
    int ready;

    (void)dos_yield();

    for (int i = 0; i < 8; ++i)
        saved_gprx[i] = cpu->gprx[i];

    cpu->gprx[regax].r16 = 0x0100;
    bios_intcall(cpu, 0x16, "native kbhit");
    ready = cpu->flags.bits.ZF ? 0 : 1;

    for (int i = 0; i < 8; ++i)
        cpu->gprx[i] = saved_gprx[i];
    cpu->flags = saved_flags;

    return ready;
}

int getch(void)
{
    static int pending_scan = -1;
    union REGS regs = {0};

    if (pending_scan >= 0) {
        int ch = pending_scan;
        pending_scan = -1;
        return ch;
    }

    while (!kbhit())
        (void)dos_yield();

    regs.h.ah = 0x00;
    int386(0x16, &regs, &regs);

    if (regs.h.al == 0) {
        pending_scan = regs.h.ah;
        return 0;
    }

    return regs.h.al;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n--)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
    return 0;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n && *src)
    {
        *dst++ = *src++;
        --n;
    }
    while (n--)
        *dst++ = '\0';
    return ret;
}

char *strcpy(char *dst, const char *src)
{
    char *ret = dst;
    while ((*dst++ = *src++) != '\0')
        ;
    return ret;
}

char *strcat(char *dst, const char *src)
{
    char *ret = dst;
    while (*dst)
        ++dst;
    while ((*dst++ = *src++) != '\0')
        ;
    return ret;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;;)
    {
        if (*s == ch)
            return (char *)s;
        if (*s == '\0')
            return NULL;
        ++s;
    }
}

int strcmp(const char *a, const char *b)
{
    for (;;)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

int strcmpi(const char *a, const char *b)
{
    for (;;)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

void strupr(char *s)
{
    while (*s)
    {
        if (*s >= 'a' && *s <= 'z')
            *s = (char)(*s - ('a' - 'A'));
        ++s;
    }
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char ch = (unsigned char)c;

    while (n--)
    {
        if (*p == ch)
            return (void *)p;
        ++p;
    }
    return NULL;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    typedef void *(*fn_ptr_t)(void *, const void *, size_t);
    return ((fn_ptr_t)_sys_table_ptrs[103])(dst, src, n);
}

void *memset(void *dst, int value, size_t n)
{
    typedef void *(*fn_ptr_t)(void *, int, size_t);
    return ((fn_ptr_t)_sys_table_ptrs[104])(dst, value, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    typedef void *(*fn_ptr_t)(void *, const void *, size_t);
    return ((fn_ptr_t)_sys_table_ptrs[109])(dst, src, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    typedef int (*fn_ptr_t)(const void *, const void *, size_t);
    return ((fn_ptr_t)_sys_table_ptrs[105])(a, b, n);
}


int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    for (;;)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));

        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

int atoi(const char *s)
{
    int sign = 1;
    int value = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        ++s;

    if (*s == '-' || *s == '+')
    {
        if (*s == '-')
            sign = -1;
        ++s;
    }

    while (*s >= '0' && *s <= '9')
    {
        value = value * 10 + (*s - '0');
        ++s;
    }

    return sign * value;
}

/* Segment-register indices used by CPU_ext_accessors. */
enum
{
    NATIVE_SEG_ES = 0,
    NATIVE_SEG_CS = 1,
    NATIVE_SEG_SS = 2,
    NATIVE_SEG_DS = 3,
    NATIVE_SEG_FS = 4,
    NATIVE_SEG_GS = 5
};

static int int386_core(int intnum, const union REGS *inregs, union REGS *outregs,
                       bool preserve_ss)
{
    PC *pc = get_PC();
    CPU *cpu = pc->cpu;

    gprx_t saved_gprx[8];
    x86_flags_t saved_flags = cpu->flags;
    uint16_t saved_ss = 0;

    for (int i = 0; i < 8; ++i)
        saved_gprx[i] = cpu->gprx[i];
    if (preserve_ss)
        saved_ss = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_SS);

    cpu->gprx[regax].r32 = inregs->x.eax;
    cpu->gprx[regbx].r32 = inregs->x.ebx;
    cpu->gprx[regcx].r32 = inregs->x.ecx;
    cpu->gprx[regdx].r32 = inregs->x.edx;
    cpu->gprx[regsi].r32 = inregs->x.esi;
    cpu->gprx[regdi].r32 = inregs->x.edi;

    bios_intcall(cpu, (uint8_t)intnum, "native int386");

    outregs->x.eax = cpu->gprx[regax].r32;
    outregs->x.ebx = cpu->gprx[regbx].r32;
    outregs->x.ecx = cpu->gprx[regcx].r32;
    outregs->x.edx = cpu->gprx[regdx].r32;
    outregs->x.esi = cpu->gprx[regsi].r32;
    outregs->x.edi = cpu->gprx[regdi].r32;
    outregs->x.cflag = cpu->flags.bits.CF ? 1u : 0u;

    for (int i = 0; i < 8; ++i)
        cpu->gprx[i] = saved_gprx[i];
    if (preserve_ss)
        cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_SS, saved_ss);
    cpu->flags = saved_flags;

    return (int)outregs->x.eax;
}

int int386(int intnum, const union REGS *inregs, union REGS *outregs)
{
    return int386_core(intnum, inregs, outregs, true);
}

void segread(struct SREGS *segregs)
{
    CPU *cpu = get_PC()->cpu;

    segregs->es = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_ES);
    segregs->cs = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_CS);
    segregs->ss = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_SS);
    segregs->ds = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_DS);
    segregs->fs = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_FS);
    segregs->gs = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_GS);
}

int int386x(int intnum, const union REGS *inregs, union REGS *outregs,
            struct SREGS *segregs)
{
    CPU *cpu = get_PC()->cpu;
    struct SREGS saved;
    int rc;

    /*
     * int386() already preserves the native process GPR/FLAGS context.
     * int386x() extends that contract to the six x86 segment registers.
     */
    segread(&saved);

    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_ES, segregs->es);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_CS, segregs->cs);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_SS, segregs->ss);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_DS, segregs->ds);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_FS, segregs->fs);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_GS, segregs->gs);

    rc = int386_core(intnum, inregs, outregs, false);

    /* Return the segment state produced by the interrupt to the caller. */
    segread(segregs);

    /* Restore the x86 context that belongs to the native application. */
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_ES, saved.es);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_CS, saved.cs);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_SS, saved.ss);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_DS, saved.ds);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_FS, saved.fs);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_GS, saved.gs);

    return rc;
}

uint16_t dos_ptr_segment(const void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    uint32_t linear;

    if (address < (uintptr_t)DOS_GUEST_RAM_BASE)
        return 0;

    linear = (uint32_t)(address - (uintptr_t)DOS_GUEST_RAM_BASE);

    /* A DOS segment identifies a paragraph and must fit into 16 bits. */
    if ((linear & 15u) != 0 || linear > 0x000ffff0u)
        return 0;

    return (uint16_t)(linear >> 4);
}

uint32_t dos_ptr_linear(const void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    uint32_t linear;

    if (address < (uintptr_t)DOS_GUEST_RAM_BASE)
        return UINT32_MAX;

    linear = (uint32_t)(address - (uintptr_t)DOS_GUEST_RAM_BASE);
    if (linear >= 0x00100000u)
        return UINT32_MAX;

    return linear;
}

void *dos_alloc_low(size_t size)
{
    union REGS regs = {0};
    uint32_t paragraphs;

    if (size == 0)
        size = 1;

    paragraphs = ((uint32_t)size + 15u) >> 4;
    if (paragraphs == 0 || paragraphs > 0xffffu)
        return NULL;

    regs.h.ah = 0x48;
    regs.w.bx = (uint16_t)paragraphs;
    int386(0x21, &regs, &regs);
    if (regs.x.cflag)
        return NULL;

    return dos_guest_far_ptr(regs.w.ax, 0);
}

void dos_free_low(void *ptr)
{
    union REGS regs = {0};
    struct SREGS sregs = {0};
    uint16_t segment;

    if (ptr == NULL)
        return;

    segment = dos_ptr_segment(ptr);
    if (segment == 0)
        return;

    segread(&sregs);
    regs.h.ah = 0x49;
    sregs.es = segment;
    int386x(0x21, &regs, &regs, &sregs);
}

static dos_malloc_policy_t native_dos_malloc_policy =
    DOS_MALLOC_POLICY_RETURN_NULL;

void dos_malloc_set_policy(dos_malloc_policy_t policy)
{
    switch (policy)
    {
    case DOS_MALLOC_POLICY_RETURN_NULL:
    case DOS_MALLOC_POLICY_EXIT:
    case DOS_MALLOC_POLICY_MESSAGE_EXIT:
        native_dos_malloc_policy = policy;
        break;
    default:
        native_dos_malloc_policy = DOS_MALLOC_POLICY_RETURN_NULL;
        break;
    }
}

dos_malloc_policy_t dos_malloc_get_policy(void)
{
    return native_dos_malloc_policy;
}

static void *native_dos_malloc_failed(size_t size)
{
    if (native_dos_malloc_policy == DOS_MALLOC_POLICY_MESSAGE_EXIT)
        printf("Out of memory: malloc(%lu) failed\r\n",
               (unsigned long)size);

    if (native_dos_malloc_policy == DOS_MALLOC_POLICY_EXIT ||
        native_dos_malloc_policy == DOS_MALLOC_POLICY_MESSAGE_EXIT)
        exit(1);

    return NULL;
}

static int native_int21_with_es(uint16_t es, union REGS *regs)
{
    CPU *cpu = get_PC()->cpu;
    uint16_t saved_es = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_ES);

    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_ES, es);
    int rc = int386(0x21, regs, regs);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_ES, saved_es);

    return rc;
}

#if 0 /* allocator backend moved to the kernel in DOS API v17 */
static uint32_t native_dos_block_size(uint16_t segment)
{
    typedef uint32_t (*fn_ptr_t)(uint16_t);
    return ((fn_ptr_t)_sys_table_ptrs[108])(segment);
}

static uint32_t native_dos_largest_free_block(void)
{
    typedef uint32_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[110])();
}

static int native_dos_size_to_paragraphs(size_t size, uint16_t *paragraphs)
{
    uint32_t value;

    if (size == 0)
        size = 1;
    if (size > 0x000ffff0u)
        return 0;

    value = ((uint32_t)size + 15u) >> 4;
    if (value == 0 || value > 0xffffu)
        return 0;

    *paragraphs = (uint16_t)value;
    return 1;
}

/*
 * Native process PSRAM heap.
 *
 * The loader publishes a private interval [app_psram_begin, app_psram_end).
 * Keep a conventional boundary-tag heap entirely inside that interval.  DOS
 * allocations remain headerless: PSRAM pointers are distinguished from DOS
 * pointers solely by their address range.
 */
#define NATIVE_PSRAM_HEAP_ALIGN       16u
#define NATIVE_PSRAM_BLOCK_FREE       1u
#define NATIVE_PSRAM_BLOCK_MAGIC      0x5053524du /* "PSRM" */
#define NATIVE_PSRAM_MIN_PAYLOAD      16u

typedef struct native_psram_block
{
    uint32_t size_flags; /* total block size including this header */
    uint32_t prev_size;  /* total size of the physically previous block */
    uint32_t magic;
    uint32_t reserved;
} native_psram_block;

_Static_assert(sizeof(native_psram_block) == NATIVE_PSRAM_HEAP_ALIGN,
               "PSRAM heap header must preserve 16-byte malloc alignment");

static uintptr_t native_psram_heap_begin;
static uintptr_t native_psram_heap_end;
static int native_psram_heap_ready;

static uintptr_t native_psram_align_up(uintptr_t value)
{
    return (value + (NATIVE_PSRAM_HEAP_ALIGN - 1u)) &
           ~(uintptr_t)(NATIVE_PSRAM_HEAP_ALIGN - 1u);
}

static uint32_t native_psram_block_size(const native_psram_block *block)
{
    return block->size_flags & ~NATIVE_PSRAM_BLOCK_FREE;
}

static int native_psram_block_is_free(const native_psram_block *block)
{
    return (block->size_flags & NATIVE_PSRAM_BLOCK_FREE) != 0;
}

static int native_psram_heap_init(void)
{
    uintptr_t begin;
    uintptr_t end;
    native_psram_block *first;

    if (native_psram_heap_ready)
        return native_psram_heap_begin != 0;

    native_psram_heap_ready = 1;
    begin = native_psram_align_up(native_dos_app_psram_begin());
    end = native_dos_app_psram_end() &
          ~(uintptr_t)(NATIVE_PSRAM_HEAP_ALIGN - 1u);

    if (begin == 0 || end <= begin ||
        end - begin < sizeof(native_psram_block) + NATIVE_PSRAM_MIN_PAYLOAD)
        return 0;

    native_psram_heap_begin = begin;
    native_psram_heap_end = end;

    first = (native_psram_block *)begin;
    first->size_flags = (uint32_t)(end - begin) | NATIVE_PSRAM_BLOCK_FREE;
    first->prev_size = 0;
    first->magic = NATIVE_PSRAM_BLOCK_MAGIC;
    first->reserved = 0;
    return 1;
}

static int native_psram_owns(const void *ptr)
{
    uintptr_t p;

    if (!ptr || !native_psram_heap_init())
        return 0;

    p = (uintptr_t)ptr;
    return p >= native_psram_heap_begin + sizeof(native_psram_block) &&
           p < native_psram_heap_end;
}

static native_psram_block *native_psram_next(native_psram_block *block)
{
    uintptr_t next = (uintptr_t)block + native_psram_block_size(block);
    return next < native_psram_heap_end ? (native_psram_block *)next : NULL;
}

static native_psram_block *native_psram_prev(native_psram_block *block)
{
    if (block->prev_size == 0)
        return NULL;
    if ((uintptr_t)block < native_psram_heap_begin + block->prev_size)
        return NULL;
    return (native_psram_block *)((uintptr_t)block - block->prev_size);
}

static int native_psram_block_valid(const native_psram_block *block)
{
    uintptr_t p = (uintptr_t)block;
    uint32_t size;

    if (p < native_psram_heap_begin || p >= native_psram_heap_end ||
        block->magic != NATIVE_PSRAM_BLOCK_MAGIC)
        return 0;

    size = native_psram_block_size(block);
    return size >= sizeof(native_psram_block) + NATIVE_PSRAM_MIN_PAYLOAD &&
           (size & (NATIVE_PSRAM_HEAP_ALIGN - 1u)) == 0 &&
           size <= native_psram_heap_end - p;
}

static void native_psram_fix_next_prev(native_psram_block *block)
{
    native_psram_block *next = native_psram_next(block);
    if (next)
        next->prev_size = native_psram_block_size(block);
}

static void native_psram_split_allocated(native_psram_block *block,
                                          uint32_t wanted)
{
    uint32_t old_size = native_psram_block_size(block);
    uint32_t remainder = old_size - wanted;

    if (remainder >= sizeof(native_psram_block) + NATIVE_PSRAM_MIN_PAYLOAD)
    {
        native_psram_block *tail =
            (native_psram_block *)((uintptr_t)block + wanted);

        block->size_flags = wanted;
        tail->size_flags = remainder | NATIVE_PSRAM_BLOCK_FREE;
        tail->prev_size = wanted;
        tail->magic = NATIVE_PSRAM_BLOCK_MAGIC;
        tail->reserved = 0;
        native_psram_fix_next_prev(tail);
    }
    else
    {
        block->size_flags = old_size;
        native_psram_fix_next_prev(block);
    }
}

static size_t native_psram_largest_free_payload(void)
{
    uintptr_t cursor;
    size_t largest = 0;

    if (!native_psram_heap_init())
        return 0;

    for (cursor = native_psram_heap_begin;
         cursor < native_psram_heap_end; )
    {
        native_psram_block *block = (native_psram_block *)cursor;
        uint32_t total;

        if (!native_psram_block_valid(block))
            return 0;

        total = native_psram_block_size(block);
        if (native_psram_block_is_free(block) &&
            total > sizeof(native_psram_block))
        {
            size_t payload = total - sizeof(native_psram_block);
            if (payload > largest)
                largest = payload;
        }

        cursor += total;
    }

    return largest;
}

static void *native_psram_malloc(size_t size)
{
    native_psram_block *block;
    uint32_t wanted;
    uintptr_t cursor;

    if (size == 0)
        size = 1;
    if (!native_psram_heap_init() ||
        size > UINT32_MAX - sizeof(native_psram_block) -
               (NATIVE_PSRAM_HEAP_ALIGN - 1u))
        return NULL;

    wanted = (uint32_t)(size + sizeof(native_psram_block) +
                        (NATIVE_PSRAM_HEAP_ALIGN - 1u)) &
             ~(NATIVE_PSRAM_HEAP_ALIGN - 1u);

    for (cursor = native_psram_heap_begin;
         cursor < native_psram_heap_end; )
    {
        block = (native_psram_block *)cursor;
        if (!native_psram_block_valid(block))
            return NULL;

        if (native_psram_block_is_free(block) &&
            native_psram_block_size(block) >= wanted)
        {
            native_psram_split_allocated(block, wanted);
            return (void *)(block + 1);
        }
        cursor += native_psram_block_size(block);
    }

    return NULL;
}

static void native_psram_free(void *ptr)
{
    native_psram_block *block;
    native_psram_block *next;
    native_psram_block *prev;
    uint32_t size;

    block = ((native_psram_block *)ptr) - 1;
    if (!native_psram_block_valid(block) || native_psram_block_is_free(block))
        return;

    size = native_psram_block_size(block);
    block->size_flags = size | NATIVE_PSRAM_BLOCK_FREE;

    next = native_psram_next(block);
    if (next && native_psram_block_valid(next) && native_psram_block_is_free(next))
    {
        block->size_flags = (size + native_psram_block_size(next)) |
                            NATIVE_PSRAM_BLOCK_FREE;
        size = native_psram_block_size(block);
        native_psram_fix_next_prev(block);
    }

    prev = native_psram_prev(block);
    if (prev && native_psram_block_valid(prev) && native_psram_block_is_free(prev))
    {
        prev->size_flags = (native_psram_block_size(prev) + size) |
                           NATIVE_PSRAM_BLOCK_FREE;
        native_psram_fix_next_prev(prev);
    }
}

static void *native_psram_realloc(void *ptr, size_t size)
{
    native_psram_block *block = ((native_psram_block *)ptr) - 1;
    native_psram_block *next;
    uint32_t old_total;
    uint32_t old_payload;
    uint32_t wanted;
    void *new_ptr;

    if (!native_psram_block_valid(block) || native_psram_block_is_free(block))
        return NULL;

    if (size > UINT32_MAX - sizeof(native_psram_block) -
               (NATIVE_PSRAM_HEAP_ALIGN - 1u))
        return NULL;

    old_total = native_psram_block_size(block);
    old_payload = old_total - sizeof(native_psram_block);
    wanted = (uint32_t)(size + sizeof(native_psram_block) +
                        (NATIVE_PSRAM_HEAP_ALIGN - 1u)) &
             ~(NATIVE_PSRAM_HEAP_ALIGN - 1u);

    if (wanted <= old_total)
    {
        native_psram_split_allocated(block, wanted);
        return ptr;
    }

    next = native_psram_next(block);
    if (next && native_psram_block_valid(next) && native_psram_block_is_free(next) &&
        old_total + native_psram_block_size(next) >= wanted)
    {
        block->size_flags = old_total + native_psram_block_size(next);
        native_psram_split_allocated(block, wanted);
        return ptr;
    }

    new_ptr = native_psram_malloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, old_payload < size ? old_payload : size);
    native_psram_free(ptr);
    return new_ptr;
}

static void *native_dos_malloc_raw(size_t size)
{
    union REGS regs = {0};
    uint16_t paragraphs;

    if (!native_dos_size_to_paragraphs(size, &paragraphs))
        return NULL;

    regs.h.ah = 0x48;
    regs.w.bx = paragraphs;
    int386(0x21, &regs, &regs);
    if (regs.x.cflag)
        return NULL;

    return dos_guest_far_ptr(regs.w.ax, 0);
}

size_t malloc_largest_block(void)
{
    size_t psram_largest = native_psram_largest_free_payload();
    size_t dos_largest = (size_t)native_dos_largest_free_block();

    return psram_largest > dos_largest ? psram_largest : dos_largest;
}

void *malloc(size_t size)
{
    void *ptr = native_psram_malloc(size);

    if (ptr)
        return ptr;

    ptr = native_dos_malloc_raw(size);
    if (ptr)
        return ptr;

    return native_dos_malloc_failed(size);
}

void free(void *ptr)
{
    union REGS regs = {0};
    uint16_t segment;

    if (!ptr)
        return;

    if (native_psram_owns(ptr))
    {
        native_psram_free(ptr);
        return;
    }

    segment = dos_ptr_segment(ptr);
    if (segment == 0)
        return;

    regs.h.ah = 0x49;
    native_int21_with_es(segment, &regs);
}

void *calloc(size_t count, size_t size)
{
    size_t total;
    void *ptr;

    if (count != 0 && size > (size_t)-1 / count)
        return native_dos_malloc_failed((size_t)-1);

    total = count * size;
    ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);

    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    union REGS regs = {0};
    uint16_t segment;
    uint16_t paragraphs;
    uint16_t old_paragraphs;
    uint32_t old_size;
    void *new_ptr;

    if (!ptr)
        return malloc(size);

    if (size == 0)
    {
        free(ptr);
        return NULL;
    }

    if (native_psram_owns(ptr))
    {
        native_psram_block *block = ((native_psram_block *)ptr) - 1;
        size_t old_payload;

        if (!native_psram_block_valid(block) || native_psram_block_is_free(block))
            return NULL;
        old_payload = native_psram_block_size(block) - sizeof(*block);

        new_ptr = native_psram_realloc(ptr, size);
        if (new_ptr)
            return new_ptr;

        /* No suitable PSRAM block remains: fall through to the common
           allocator, which tries PSRAM once more and then DOS AH=48. */
        new_ptr = native_dos_malloc_raw(size);
        if (!new_ptr)
            return native_dos_malloc_failed(size);
        memcpy(new_ptr, ptr, old_payload < size ? old_payload : size);
        native_psram_free(ptr);
        return new_ptr;
    }

    segment = dos_ptr_segment(ptr);
    if (segment == 0)
        return NULL;

    old_size = native_dos_block_size(segment);
    if (old_size == 0 || (old_size & 15u) != 0 || old_size > 0x000ffff0u)
        return NULL;
    old_paragraphs = (uint16_t)(old_size >> 4);

    if (native_dos_size_to_paragraphs(size, &paragraphs))
    {
        regs.h.ah = 0x4a;
        regs.w.bx = paragraphs;
        native_int21_with_es(segment, &regs);
        if (!regs.x.cflag)
            return ptr;

        /* DosMemChange() may join following free MCBs before reporting
           DE_NOMEM.  Restore the original size before moving the block. */
        regs = (union REGS){0};
        regs.h.ah = 0x4a;
        regs.w.bx = old_paragraphs;
        native_int21_with_es(segment, &regs);
        if (regs.x.cflag)
            return NULL;
    }

    new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    free(ptr);
    return new_ptr;
}

#endif

/*
 * The stdlib ABI remains local to the application, including its selectable
 * OOM policy.  Allocation itself is kernel-owned now.
 */
void *malloc(size_t size)
{
    typedef void *(*fn_ptr_t)(size_t);
    void *ptr = ((fn_ptr_t)_sys_table_ptrs[111])(size);

    return ptr ? ptr : native_dos_malloc_failed(size);
}

void *calloc(size_t count, size_t size)
{
    typedef void *(*fn_ptr_t)(size_t, size_t);
    void *ptr = ((fn_ptr_t)_sys_table_ptrs[112])(count, size);

    if (ptr)
        return ptr;
    if (count != 0 && size > (size_t)-1 / count)
        return native_dos_malloc_failed((size_t)-1);
    return native_dos_malloc_failed(count * size);
}

void *realloc(void *ptr, size_t size)
{
    typedef void *(*fn_ptr_t)(void *, size_t);
    void *result = ((fn_ptr_t)_sys_table_ptrs[113])(ptr, size);

    if (result || size == 0)
        return result;
    return native_dos_malloc_failed(size);
}

void free(void *ptr)
{
    typedef void (*fn_ptr_t)(void *);
    ((fn_ptr_t)_sys_table_ptrs[114])(ptr);
}

size_t malloc_largest_block(void)
{
    typedef size_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_sys_table_ptrs[115])();
}


enum
{
    NATIVE_IO_BUFFER_DEFAULT_SIZE = 512,
    NATIVE_IO_BUFFER_MAX_SIZE = 0xfff0
};

static uint16_t native_io_segment;
static unsigned char *native_io_buffer;
static unsigned int native_io_buffer_size = NATIVE_IO_BUFFER_DEFAULT_SIZE;

static int native_int21_with_ds(uint16_t ds, union REGS *regs)
{
    CPU *cpu = get_PC()->cpu;
    uint16_t saved_ds = cpu->ext_accessors->get_seg16(cpu, NATIVE_SEG_DS);

    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_DS, ds);
    int rc = int386(0x21, regs, regs);
    cpu->ext_accessors->set_seg16(cpu, NATIVE_SEG_DS, saved_ds);

    return rc;
}

static int native_io_ensure_buffer(void)
{
    union REGS regs = {0};

    if (native_io_buffer)
        return 0;

    regs.h.ah = 0x48;
    regs.w.bx = (uint16_t)(native_io_buffer_size / 16u);
    int386(0x21, &regs, &regs);
    if (regs.x.cflag)
        return -1;

    native_io_segment = regs.w.ax;
    native_io_buffer =
        (unsigned char *)dos_guest_far_ptr(native_io_segment, 0);
    return 0;
}

int dos_set_io_buffer_size(unsigned int size)
{
    union REGS regs = {0};
    unsigned int aligned_size;
    uint16_t new_segment;
    unsigned char *new_buffer;

    if (size == 0 || size > NATIVE_IO_BUFFER_MAX_SIZE)
        return -1;

    aligned_size = (size + 15u) & ~15u;
    if (aligned_size > NATIVE_IO_BUFFER_MAX_SIZE)
        return -1;

    if (aligned_size == native_io_buffer_size)
        return 0;

    if (!native_io_buffer)
    {
        native_io_buffer_size = aligned_size;
        return 0;
    }

    regs.h.ah = 0x48;
    regs.w.bx = (uint16_t)(aligned_size / 16u);
    int386(0x21, &regs, &regs);
    if (regs.x.cflag)
        return -1;

    new_segment = regs.w.ax;
    new_buffer = (unsigned char *)dos_guest_far_ptr(new_segment, 0);

    regs.x.eax = 0;
    regs.h.ah = 0x49;
    native_int21_with_es(native_io_segment, &regs);
    if (regs.x.cflag)
    {
        union REGS free_regs = {0};
        free_regs.h.ah = 0x49;
        native_int21_with_es(new_segment, &free_regs);
        return -1;
    }

    native_io_segment = new_segment;
    native_io_buffer = new_buffer;
    native_io_buffer_size = aligned_size;
    return 0;
}

static int native_io_copy_path(const char *path)
{
    size_t length;

    if (native_io_ensure_buffer() != 0)
        return -1;

    length = strlen(path) + 1;
    if (length > native_io_buffer_size)
        return -1;

    memcpy(native_io_buffer, path, length);
    return 0;
}

int open(const char *path, int flags, ...)
{
    union REGS regs = {0};

    if (native_io_copy_path(path) != 0)
        return -1;

    if ((flags & O_CREAT) && (flags & O_TRUNC))
    {
        regs.h.ah = 0x3c;
        regs.w.cx = 0;
        regs.w.dx = 0;
        native_int21_with_ds(native_io_segment, &regs);
    }
    else
    {
        regs.h.ah = 0x3d;
        regs.h.al = (uint8_t)(flags & 3);
        regs.w.dx = 0;
        native_int21_with_ds(native_io_segment, &regs);

        if (regs.x.cflag && (flags & O_CREAT))
        {
            regs.x.eax = 0;
            regs.x.ecx = 0;
            regs.x.edx = 0;
            regs.h.ah = 0x3c;
            native_int21_with_ds(native_io_segment, &regs);
        }
    }

    if (regs.x.cflag)
        return -1;

    return (int)regs.w.ax;
}

int close(int handle)
{
    union REGS regs = {0};


    regs.h.ah = 0x3e;
    regs.w.bx = (uint16_t)handle;
    int386(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

/*
 * Native file I/O is normally also a cooperative emulator service point.
 * Applications which already provide their own sufficiently frequent yields
 * may temporarily suppress that extra work around bulk I/O.
 */
static _Bool native_term_locked;

void dos_lock_term(_Bool lock)
{
    native_term_locked = lock;
}

static void dos_term_point(void)
{
    union REGS regs = {0};

    (void)dos_yield();
    if (dos_termination_requested())
        exit(0);

    /*
     * dos_yield() lets the guest BIOS service IRQ1 and place pending input
     * into the BIOS keyboard buffer. Peek at the next key without removing
     * it, so Ctrl+C remains queued for the command interpreter after this
     * native process exits.
     */
    regs.h.ah = 0x01;
    int386(0x16, &regs, &regs);
    if (regs.h.al == 0x03)
        exit(0);
}

int read(int handle, void *buffer, unsigned int count)
{
    unsigned char *dst = (unsigned char *)buffer;
    unsigned int total = 0;

    if (count == 0)
        return 0;

    if (native_io_ensure_buffer() != 0)
        return -1;

    while (total < count)
    {
        union REGS regs = {0};
        unsigned int remain = count - total;
        uint16_t chunk = (uint16_t)(
            remain > native_io_buffer_size
                ? native_io_buffer_size
                : remain);

        if (!native_term_locked)
            dos_term_point();

        regs.h.ah = 0x3f;
        regs.w.bx = (uint16_t)handle;
        regs.w.cx = chunk;
        regs.w.dx = 0;
        native_int21_with_ds(native_io_segment, &regs);

        if (regs.x.cflag)
        {
            unsigned dos_error = (unsigned)regs.w.ax & 0xffu;

            /*
             * Diagnose ERROR_INVALID_HANDLE only after the failing AH=3Fh.
             * This extra AH=51h call therefore cannot affect the failure.
             *
             * Border code:
             *   4M JJ HH EE
             *   M  = current PSP max-files (low 6 bits, added to 0x40)
             *   JJ = JFT entry for handle HH (FF=closed, FE=out of range)
             *   HH = handle passed to read()
             *   EE = DOS error (06 = invalid handle)
             */
            if (dos_error == 0x06u)
            {
                union REGS pspregs = {0};
                uint16_t psp_seg;
                uint16_t maxfiles;
                uint16_t jft_off;
                uint16_t jft_seg;
                unsigned jft_entry = 0xfeu;

                pspregs.h.ah = 0x51;
                int386(0x21, &pspregs, &pspregs);
                psp_seg = pspregs.w.bx;

                maxfiles = *(volatile uint16_t *)
                    dos_guest_far_ptr(psp_seg, 0x32);
                jft_off = *(volatile uint16_t *)
                    dos_guest_far_ptr(psp_seg, 0x34);
                jft_seg = *(volatile uint16_t *)
                    dos_guest_far_ptr(psp_seg, 0x36);

                if ((unsigned)handle < maxfiles)
                {
                    volatile uint8_t *jft =
                        (volatile uint8_t *)dos_guest_far_ptr(jft_seg, jft_off);
                    jft_entry = jft[(unsigned)handle];
                }

            }

            return total ? (int)total : -1;
        }

        if (regs.w.ax == 0)
        {
            break;
        }

        memcpy(dst + total, native_io_buffer, regs.w.ax);
        total += regs.w.ax;

        /*
         * A successful DOS read is allowed to return fewer bytes than CX.
         * Do not treat a non-zero short read as EOF here: this wrapper's
         * contract is already to satisfy the caller's whole count by issuing
         * repeated AH=3Fh requests through the low-memory bounce buffer.
         *
         * True EOF is AX==0 (handled above).  If a short read was caused by
         * an internal FAT/block-transfer boundary, the next iteration resumes
         * from the SFT's advanced file position and fills the remainder.
         */
    }


    return (int)total;
}

int write(int handle, const void *buffer, unsigned int count)
{
    const unsigned char *src = (const unsigned char *)buffer;
    unsigned int total = 0;

    if (count == 0)
        return 0;

    if (native_io_ensure_buffer() != 0)
        return -1;

    while (total < count)
    {
        union REGS regs = {0};
        unsigned int remain = count - total;
        uint16_t chunk = (uint16_t)(
            remain > native_io_buffer_size
                ? native_io_buffer_size
                : remain);

        memcpy(native_io_buffer, src + total, chunk);

        if (!native_term_locked)
            dos_term_point();

        regs.h.ah = 0x40;
        regs.w.bx = (uint16_t)handle;
        regs.w.cx = chunk;
        regs.w.dx = 0;
        native_int21_with_ds(native_io_segment, &regs);

        if (regs.x.cflag)
            return total ? (int)total : -1;

        total += regs.w.ax;
        if (regs.w.ax < chunk)
            break;
    }

    return (int)total;
}

int32_t lseek(int handle, int32_t offset, int origin)
{
    union REGS regs = {0};

    regs.h.ah = 0x42;
    regs.h.al = (uint8_t)origin;
    regs.w.bx = (uint16_t)handle;
    regs.w.cx = (uint16_t)((uint32_t)offset >> 16);
    regs.w.dx = (uint16_t)offset;
    int386(0x21, &regs, &regs);

    if (regs.x.cflag)
        return -1;

    return (int32_t)(((uint32_t)regs.w.dx << 16) | regs.w.ax);
}

int32_t filelength(int handle)
{
    int32_t current = lseek(handle, 0, 1);
    int32_t end;

    if (current < 0)
        return -1;

    end = lseek(handle, 0, 2);
    if (end < 0)
        return -1;

    if (lseek(handle, current, 0) < 0)
        return -1;

    return end;
}

int fstat(int handle, struct stat *info)
{
    int32_t size = filelength(handle);

    if (size < 0)
        return -1;

    info->st_size = size;
    return 0;
}

int access(const char *path, int mode)
{
    union REGS regs = {0};
    (void)mode;

    if (native_io_copy_path(path) != 0)
        return -1;

    regs.h.ah = 0x43;
    regs.h.al = 0x00;
    regs.w.dx = 0;
    native_int21_with_ds(native_io_segment, &regs);

    return regs.x.cflag ? -1 : 0;
}

int mkdir(const char *path, int mode)
{
    union REGS regs = {0};
    (void)mode;

    if (native_io_copy_path(path) != 0)
        return -1;

    regs.h.ah = 0x39;
    regs.w.dx = 0;
    native_int21_with_ds(native_io_segment, &regs);

    return regs.x.cflag ? -1 : 0;
}

int remove(const char *filename)
{
    union REGS regs = {0};

    if (native_io_copy_path(filename) != 0)
        return -1;

    regs.h.ah = 0x41;
    regs.w.dx = 0;
    native_int21_with_ds(native_io_segment, &regs);

    return regs.x.cflag ? -1 : 0;
}


struct native_dos_FILE
{
    int handle;
    int eof;
    int is_static;
    int text_mode;
};

static struct native_dos_FILE native_stdout_file = {1, 0, 1, 1};
FILE *stdout = &native_stdout_file;

/*
 * DOS console text output uses CRLF. Keep write() itself binary-transparent;
 * only stdio text sent to stdout/stderr is translated here.
 */
static int native_console_write(int handle, const char *data, size_t length)
{
    size_t start = 0;
    size_t i;
    int total = 0;

    for (i = 0; i < length; ++i)
    {
        if (data[i] != '\n')
            continue;

        if (i > start)
        {
            int rc = write(handle, data + start, (unsigned int)(i - start));
            if (rc < 0)
                return -1;
            total += rc;
        }

        if (i == 0 || data[i - 1] != '\r')
        {
            if (write(handle, "\r", 1) != 1)
                return -1;
            ++total;
        }

        if (write(handle, "\n", 1) != 1)
            return -1;
        ++total;
        start = i + 1;
    }

    if (start < length)
    {
        int rc = write(handle, data + start, (unsigned int)(length - start));
        if (rc < 0)
            return -1;
        total += rc;
    }

    return total;
}

/* DOS text streams translate LF to CRLF; low-level write() stays binary. */
static int native_stream_write(FILE *stream, const char *data, size_t length)
{
    size_t start = 0;
    size_t i;

    if (!stream || (!data && length != 0))
        return -1;

    if (!stream->text_mode)
        return write(stream->handle, data, (unsigned int)length) == (int)length
            ? (int)length : -1;

    /* Preserve the existing console policy; regular DOS text files use the
       CRT rule that every LF written to a text stream becomes CRLF. */
    if (stream->handle == 1 || stream->handle == 2)
        return native_console_write(stream->handle, data, length) < 0
            ? -1 : (int)length;

    for (i = 0; i < length; ++i)
    {
        if (data[i] != '\n')
            continue;

        if (i > start &&
            write(stream->handle, data + start, (unsigned int)(i - start)) !=
                (int)(i - start))
            return -1;

        if (write(stream->handle, "\r\n", 2) != 2)
            return -1;

        start = i + 1;
    }

    if (start < length &&
        write(stream->handle, data + start, (unsigned int)(length - start)) !=
            (int)(length - start))
        return -1;

    return (int)length;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    int rc;

    if (!stream)
        return -1;

    rc = native_stream_write(stream, (const char *)&ch, 1);

    return rc < 0 ? -1 : ch;
}

int fputs(const char *str, FILE *stream)
{
    size_t len;

    if (!str || !stream)
        return -1;

    len = strlen(str);
    if (len == 0)
        return 0;

    return native_stream_write(stream, str, len) >= 0 ? 0 : -1;
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int puts(const char *str)
{
    if (fputs(str, stdout) < 0)
        return -1;
    if (fputc('\n', stdout) < 0)
        return -1;
    return 0;
}

int vsnprintf(char *buffer, size_t size,
              const char *format, va_list args)
{
    typedef int (*fn_ptr_t)(char *, size_t, const char *, va_list);
    return ((fn_ptr_t)_sys_table_ptrs[10])(buffer, size, format, args);
}

static int native_vfprintf_stream(FILE *stream, const char *format, va_list args)
{
    char stackbuf[512];
    va_list copy;
    int length;

    va_copy(copy, args);
    length = vsnprintf(stackbuf, sizeof(stackbuf), format, copy);
    va_end(copy);
    if (length < 0)
        return length;

    if ((size_t)length < sizeof(stackbuf))
    {
        return native_stream_write(stream, stackbuf, (size_t)length) < 0
            ? -1 : length;
    }

    char *buffer = (char *)malloc((size_t)length + 1);
    if (!buffer)
        return -1;

    va_copy(copy, args);
    vsnprintf(buffer, (size_t)length + 1, format, copy);
    va_end(copy);

    int written = native_stream_write(stream, buffer, (size_t)length);
    free(buffer);
    return written < 0 ? -1 : length;
}

FILE *fopen(const char *filename, const char *mode)
{
    int flags;
    int handle;
    FILE *stream;

    if (!mode || mode[0] == '\0')
        return NULL;

    switch (mode[0])
    {
        case 'r':
            flags = O_RDONLY;
            break;
        case 'w':
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case 'a':
            flags = O_WRONLY | O_CREAT;
            break;
        default:
            return NULL;
    }

    for (const char *p = mode; *p; ++p)
        if (*p == 'b')
            flags |= O_BINARY;

    handle = open(filename, flags, 0666);
    if (handle < 0)
        return NULL;

    if (mode[0] == 'a' && lseek(handle, 0, SEEK_END) < 0)
    {
        close(handle);
        return NULL;
    }

    stream = (FILE *)malloc(sizeof(*stream));
    if (!stream)
    {
        close(handle);
        return NULL;
    }

    stream->handle = handle;
    stream->eof = 0;
    stream->is_static = 0;
    stream->text_mode = strchr(mode, 'b') == NULL;
    return stream;
}

int fclose(FILE *stream)
{
    int rc;

    if (!stream)
        return -1;

    if (stream->is_static)
        return 0;

    rc = close(stream->handle);
    free(stream);
    return rc;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    size_t total;
    int got;

    if (!stream || size == 0 || count == 0)
        return 0;

    if (count > (size_t)-1 / size)
        return 0;

    total = size * count;
    got = read(stream->handle, buffer, (unsigned int)total);
    if (got <= 0)
    {
        if (got == 0)
            stream->eof = 1;
        return 0;
    }

    if ((size_t)got < total)
        stream->eof = 1;

    return (size_t)got / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
    size_t total;
    int written;

    if (!stream || size == 0 || count == 0)
        return 0;

    if (count > (size_t)-1 / size)
        return 0;

    total = size * count;
    written = native_stream_write(stream, (const char *)buffer, total);
    if (written < 0)
        return 0;

    return count;
}

int fseek(FILE *stream, long offset, int origin)
{
    if (!stream)
        return -1;

    if (lseek(stream->handle, (int32_t)offset, origin) < 0)
        return -1;

    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream)
{
    if (!stream)
        return -1;

    return (long)lseek(stream->handle, 0, SEEK_CUR);
}

void rewind(FILE *stream)
{
    if (!stream)
        return;

    if (lseek(stream->handle, 0, SEEK_SET) >= 0)
        stream->eof = 0;
}

int feof(FILE *stream)
{
    return stream ? stream->eof : 1;
}

void setbuf(FILE *stream, char *buffer)
{
    (void)stream;
    (void)buffer;
}

int getchar(void)
{
    unsigned char ch;
    int rc = read(0, &ch, 1);
    return rc == 1 ? (int)ch : -1;
}

int vprintf(const char *format, va_list args)
{
    return native_vfprintf_stream(stdout, format, args);
}

int printf(const char *format, ...)
{
    va_list args;
    int rc;

    va_start(args, format);
    rc = native_vfprintf_stream(stdout, format, args);
    va_end(args);
    return rc;
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    int rc;

    if (!stream)
        return -1;

    va_start(args, format);
    rc = native_vfprintf_stream(stream, format, args);
    va_end(args);
    return rc;
}

int sprintf(char *buffer, const char *format, ...)
{
    va_list args;
    int rc;

    va_start(args, format);
    rc = vsnprintf(buffer, (size_t)-1, format, args);
    va_end(args);
    return rc;
}


static int native_vsscanf(const char *input, const char *format, va_list ap)
{
    typedef int (*fn_ptr_t)(const char *, const char *, va_list);
    return ((fn_ptr_t)_sys_table_ptrs[102])(input, format, ap);
}


int sscanf(const char *buffer, const char *format, ...)
{
    va_list ap;
    int result;

    va_start(ap, format);
    result = native_vsscanf(buffer, format, ap);
    va_end(ap);
    return result;
}

int fscanf(FILE *stream, const char *format, ...)
{
    char line[256];
    unsigned int n = 0;
    va_list ap;
    int result;

    if (!stream || stream->eof)
        return -1;

    while (n + 1 < sizeof(line))
    {
        char c;
        int rc = read(stream->handle, &c, 1);

        if (rc <= 0)
        {
            stream->eof = 1;
            break;
        }

        line[n++] = c;
        if (c == '\n')
            break;
    }

    if (!n)
        return -1;

    line[n] = '\0';

    va_start(ap, format);
    result = native_vsscanf(line, format, ap);
    va_end(ap);
    return result;
}


/* ------------------------------------------------------------------------- */
/* Native interrupt-vector ownership                                          */
/* ------------------------------------------------------------------------- */

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

static volatile uint16_t *dos_vector_ivt_word(unsigned intno)
{
    return (volatile uint16_t *)(uintptr_t)
        (0x11000000ul + ((uint32_t)(intno & 0xffu) << 2));
}

static dos_native_vector_handler_t *dos_vector_handler_table(void)
{
    const unsigned long *table =
        (const unsigned long *)(uintptr_t)DOS_OS_API_SYS_TABLE_BASE;
    return (dos_native_vector_handler_t *)(uintptr_t)table[1];
}

bool dos_native_setvect(dos_native_vector_t *state,
                        unsigned intno,
                        dos_native_vector_handler_t handler)
{
    volatile uint16_t *ivt;
    dos_native_vector_handler_t *handlers;

    if (!state || !handler || intno > 0xffu || state->installed)
        return false;

    ivt = dos_vector_ivt_word(intno);
    handlers = dos_vector_handler_table();
    if (!handlers)
        return false;

    state->intno = (uint16_t)intno;
    state->old_off = ivt[0];
    state->old_seg = ivt[1];
    state->old_handler = handlers[intno];

    handlers[intno] = handler;
    ivt[0] = (uint16_t)intno;
    ivt[1] = 0xFFE0u;
    state->installed = true;
    return true;
}

void dos_native_restorevect(dos_native_vector_t *state)
{
    volatile uint16_t *ivt;
    dos_native_vector_handler_t *handlers;
    unsigned intno;

    if (!state || !state->installed)
        return;

    intno = state->intno;
    ivt = dos_vector_ivt_word(intno);
    handlers = dos_vector_handler_table();

    ivt[0] = state->old_off;
    ivt[1] = state->old_seg;
    if (handlers)
        handlers[intno] = state->old_handler;

    state->installed = false;
}

/*
 * The common native-DOS libc is also linked into legacy ET_REL applications
 * which do not carry the EZ crt0 module.  Keep the EZ exit hooks optional:
 * crt0.c/crt0.S provide strong definitions when present, while legacy
 * applications fall back to the kernel-owned ELF exit path below.
 */
int __attribute__((weak)) __ez_crt_main_active(void)
{
    return 0;
}

void __attribute__((weak, noreturn)) __ez_crt_exit(int status)
{
    dos_process_exit(status);
    __builtin_unreachable();
}

/*
 * C library exit() for native DOS applications.
 *
 * Stack ownership and unwinding are kernel responsibilities.  The public
 * process API below returns to the kernel-owned main() trampoline; no client
 * translation unit needs to know the ELF stack address or contain assembler.
 */
void exit(int status)
{
    if (__ez_crt_main_active())
        __ez_crt_exit(status);
    dos_process_exit(status);
}

/* Open Watcom/DOS process-control and directory-search compatibility. */
void _disable(void)
{
    get_PC()->cpu->flags.bits.IF = 0;
}

void _enable(void)
{
    get_PC()->cpu->flags.bits.IF = 1;
}

static int dos_ptr_to_far(const void *ptr, uint16_t *seg, uint16_t *off)
{
    uintptr_t p = (uintptr_t)ptr;
    uint32_t linear;

    if (p < (uintptr_t)DOS_GUEST_RAM_BASE)
        return -1;
    linear = (uint32_t)(p - (uintptr_t)DOS_GUEST_RAM_BASE);
    if (linear > 0xfffffu)
        return -1;

    *seg = (uint16_t)(linear >> 4);
    *off = (uint16_t)(linear & 15u);
    return 0;
}

/* DOS 2+ findfirst/findnext DTA layout. */
static unsigned char dos_find_dta[64];

static int dos_set_find_dta(void)
{
    union REGS inregs;
    union REGS outregs;
    struct SREGS segregs;
    uint16_t seg;
    uint16_t off;

    if (dos_ptr_to_far(dos_find_dta, &seg, &off) != 0)
        return -1;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    segread(&segregs);
    segregs.ds = seg;
    inregs.h.ah = 0x1a;
    inregs.w.dx = off;
    int386x(0x21, &inregs, &outregs, &segregs);
    return outregs.x.cflag ? (int)outregs.w.ax : 0;
}

static void dos_copy_find_result(struct find_t *info)
{
    unsigned int i;

    info->attrib = dos_find_dta[21];
    for (i = 0; i < 13 && dos_find_dta[30 + i] != 0; ++i)
        info->name[i] = (char)dos_find_dta[30 + i];
    info->name[i] = '\0';
}

int _dos_findfirst(const char *pattern, unsigned attrib, struct find_t *info)
{
    union REGS inregs;
    union REGS outregs;
    struct SREGS segregs;
    uint16_t seg;
    uint16_t off;
    int rc;

    rc = dos_set_find_dta();
    if (rc != 0)
        return rc;
    if (dos_ptr_to_far(pattern, &seg, &off) != 0)
        return -1;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    segread(&segregs);
    segregs.ds = seg;
    inregs.h.ah = 0x4e;
    inregs.w.cx = (uint16_t)attrib;
    inregs.w.dx = off;
    int386x(0x21, &inregs, &outregs, &segregs);
    if (outregs.x.cflag)
        return (int)outregs.w.ax;

    dos_copy_find_result(info);
    return 0;
}

int _dos_findnext(struct find_t *info)
{
    union REGS inregs;
    union REGS outregs;
    int rc;

    rc = dos_set_find_dta();
    if (rc != 0)
        return rc;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x4f;
    int386(0x21, &inregs, &outregs);
    if (outregs.x.cflag)
        return (int)outregs.w.ax;

    dos_copy_find_result(info);
    return 0;
}

/* Standard C clock(), backed by the DOS/BIOS 18.2 Hz tick source. */
#include <time.h>
clock_t clock(void)
{
    union REGS inregs;
    union REGS outregs;
    uint32_t ticks;
    uint64_t scaled;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x00;
    int386(0x1a, &inregs, &outregs);

    ticks = ((uint32_t)outregs.w.cx << 16) | (uint32_t)outregs.w.dx;
    scaled = (uint64_t)ticks * (uint64_t)CLOCKS_PER_SEC * 65536ull;
    return (clock_t)(scaled / 1193182ull);
}
