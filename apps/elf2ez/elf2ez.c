#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ELF2EZ_HOST
#include "elf2ez_host.h"
#else
#include "dos.h"
#include "dos-api.h"
#include "fcntl.h"
#include "io.h"
#endif
#include "ez.h"

#define ELF2EZ_OUTPUT_NAME_MAX 260u
#define ELF2EZ_READ_CHUNK 32256u
#define ELF2EZ_PROGRESS_RELOCS 32u

#define ELF32_MAGIC             0x464c457fu
#define ELFCLASS32              1u
#define ELFDATA2LSB             1u
#define EV_CURRENT              1u
#define ET_REL                  1u
#define EM_ARM                  40u
#define EF_ARM_ABI_FLOAT_HARD   0x00000400u
#define SHN_UNDEF               0u
#define SHN_ABS                 0xfff1u
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
#define R_ARM_ABS32             2u
#define R_ARM_REL32             3u
#define R_ARM_THM_PC22          10u
#define R_ARM_THM_JUMP24        30u
#define R_ARM_THM_ALU_ABS_G0_NC 102u

#define ELF_SEC_UNSEEN          0u
#define ELF_SEC_LOADING         1u
#define ELF_SEC_LOADED          2u
#define ELF_NO_SYMBOL           0xffffffffu

#define ELF_STARTUP_NONE        0u
#define ELF_STARTUP_PREINIT     1u
#define ELF_STARTUP_INIT        2u
#define ELF_STARTUP_FINI        3u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t elf_class;
    uint8_t data;
    uint8_t ident_version;
    uint8_t abi;
    uint8_t abi_version;
    uint8_t pad[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf32_ehdr;

typedef struct {
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t addralign;
    uint32_t entsize;
} elf32_shdr;

typedef struct {
    uint32_t name;
    uint32_t value;
    uint32_t size;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
} elf32_sym;

typedef struct {
    uint32_t offset;
    uint32_t info;
} elf32_rel;

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint8_t state;
    uint8_t startup_kind;
    uint8_t startup_plain;
    uint8_t reserved;
} elf_sec_state;

typedef struct {
    elf32_ehdr eh;
    elf32_shdr symtab;
    elf32_shdr strtab;
    elf32_shdr shstrtab;
    uint32_t cursor;
    uint32_t image_store_addr;
    uint32_t preinit_array_rva;
    uint32_t preinit_array_count;
    uint32_t init_array_rva;
    uint32_t init_array_count;
    uint32_t fini_array_rva;
    uint32_t fini_array_count;
    uint32_t relocation_progress;
    uint32_t reloc_store_addr;
    uint32_t ez_reloc_count;
    uint16_t shnum;
    uint16_t symtab_index;
} elf_load_meta;

typedef struct {
    uint32_t value;
    uint8_t rebase;
} elf_symbol_ref;
#pragma pack(pop)

typedef struct {
    uint16_t segment;
    uint16_t paragraphs;
    uint8_t *ptr;
    uint32_t size;
} dos_block;

static int make_output_name(const char *input, char *output, unsigned output_size)
{
    const char *name;
    const char *dot = NULL;
    const char *p;
    unsigned prefix_len;

    if (input == NULL || input[0] == '\0' || output == NULL || output_size == 0)
        return 0;

    name = input;
    for (p = input; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
            dot = NULL;
        } else if (*p == '.') {
            dot = p;
        }
    }

    if (dot == name)
        dot = NULL;

    prefix_len = (unsigned)((dot != NULL ? dot : p) - input);
    if (prefix_len + sizeof(".exe") > output_size)
        return 0;

    memcpy(output, input, prefix_len);
    memcpy(output + prefix_len, ".exe", sizeof(".exe"));
    return 1;
}

static int is_yes_option(const char *arg)
{
    return arg != NULL && arg[0] == '/' &&
           (arg[1] == 'y' || arg[1] == 'Y') && arg[2] == '\0';
}

static int confirm_overwrite(const char *filename)
{
    int c;
    int tail;

    printf("Overwrite file: %s? (Y/N) ", filename);
    c = getchar();

    do {
        tail = getchar();
    } while (tail >= 0 && tail != '\n');

    putchar('\n');
    return c == 'y' || c == 'Y';
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    uint32_t rem;
    uint32_t add;

    if (a <= 1u)
        return v;
    rem = v % a;
    if (rem == 0u)
        return v;
    add = a - rem;
    if (v > UINT32_MAX - add)
        return UINT32_MAX;
    return v + add;
}

static int read_exact_at(int fd, uint32_t offset, void *dst, uint32_t size)
{
    uint8_t *p = (uint8_t *)dst;
    uint32_t done = 0;

    if (offset > INT32_MAX || lseek(fd, (int32_t)offset, SEEK_SET) < 0)
        return -1;

    while (done < size) {
        unsigned chunk = (unsigned)(size - done);
        int got;

        if (chunk > ELF2EZ_READ_CHUNK)
            chunk = ELF2EZ_READ_CHUNK;
        got = read(fd, p + done, chunk);
        if (got <= 0)
            return -1;
        done += (uint32_t)got;
    }
    return 0;
}

#ifdef ELF2EZ_HOST
/*
 * Host build: the DOS "largest free block" has no meaning, so simply reserve a
 * generous heap buffer with the standard allocator. RP2040/EZ images are far
 * smaller than this, and the work-block capacity checks still guard the size.
 */
#ifndef ELF2EZ_HOST_WORK_BLOCK
#define ELF2EZ_HOST_WORK_BLOCK (64u * 1024u * 1024u)
#endif

static int alloc_largest_block(dos_block *block)
{
    uint32_t size = ELF2EZ_HOST_WORK_BLOCK;

    memset(block, 0, sizeof(*block));
    block->ptr = (uint8_t *)malloc(size);
    while (block->ptr == NULL && size >= (1u << 20)) {
        size >>= 1;
        block->ptr = (uint8_t *)malloc(size);
    }
    if (block->ptr == NULL)
        return -1;

    block->size = size;
    return 0;
}

static void free_dos_block(dos_block *block)
{
    free(block->ptr);
    memset(block, 0, sizeof(*block));
}
#else
static int alloc_largest_block(dos_block *block)
{
    union REGS regs = {0};
    uint16_t largest;

    memset(block, 0, sizeof(*block));
    regs.h.ah = 0x48;
    regs.w.bx = 0xffffu;
    int386(0x21, &regs, &regs);

    if (!regs.x.cflag) {
        block->segment = regs.w.ax;
        block->paragraphs = 0xffffu;
    } else {
        largest = regs.w.bx;
        if (largest == 0)
            return -1;

        memset(&regs, 0, sizeof(regs));
        regs.h.ah = 0x48;
        regs.w.bx = largest;
        int386(0x21, &regs, &regs);
        if (regs.x.cflag)
            return -1;

        block->segment = regs.w.ax;
        block->paragraphs = largest;
    }

    block->ptr = (uint8_t *)dos_guest_far_ptr(block->segment, 0);
    block->size = (uint32_t)block->paragraphs << 4;
    return 0;
}

static void free_dos_block(dos_block *block)
{
    union REGS regs = {0};
    struct SREGS sregs;

    if (block->segment == 0)
        return;

    segread(&sregs);
    sregs.es = block->segment;
    regs.h.ah = 0x49;
    int386x(0x21, &regs, &regs, &sregs);
    memset(block, 0, sizeof(*block));
}
#endif

static elf_sec_state *elf_states(elf_load_meta *meta)
{
    return (elf_sec_state *)(meta + 1);
}

static uint32_t elf_metadata_size(uint16_t shnum)
{
    uint32_t table_size = (uint32_t)shnum * (uint32_t)sizeof(elf_sec_state);
    uint32_t total = (uint32_t)sizeof(elf_load_meta);

    if (shnum != 0 && table_size / sizeof(elf_sec_state) != shnum)
        return UINT32_MAX;
    if (total > UINT32_MAX - table_size)
        return UINT32_MAX;
    return align_up(total + table_size, 4u);
}

static int read_shdr(int fd, const elf32_ehdr *eh, uint16_t sec_num,
                     elf32_shdr *sh)
{
    uint32_t off;

    if (sec_num >= eh->shnum)
        return -1;
    off = eh->shoff + (uint32_t)sec_num * eh->shentsize;
    if (off < eh->shoff)
        return -1;
    return read_exact_at(fd, off, sh, sizeof(*sh));
}

static int ensure_capacity(const elf_load_meta *meta, uint32_t end_off)
{
    uint32_t image_end;

    if (meta->image_store_addr > UINT32_MAX - end_off)
        return -1;
    image_end = meta->image_store_addr + end_off;
    return image_end <= meta->reloc_store_addr ? 0 : -1;
}

static int emit_ez_reloc(elf_load_meta *meta, uint32_t rva, uint8_t type)
{
    uint32_t image_end;
    uint32_t reloc_addr;
    struct ez_reloc *rel;

    if (meta->image_store_addr > UINT32_MAX - meta->cursor)
        return -1;
    image_end = meta->image_store_addr + meta->cursor;
    if (meta->reloc_store_addr < sizeof(struct ez_reloc))
        return -1;
    reloc_addr = meta->reloc_store_addr - sizeof(struct ez_reloc);
    if (reloc_addr < image_end)
        return -1;

    meta->reloc_store_addr = reloc_addr;
#ifdef ELF2EZ_HOST
    rel = (struct ez_reloc *)((uint8_t *)meta + reloc_addr);
#else
    rel = (struct ez_reloc *)(uintptr_t)reloc_addr;
#endif
    ++meta->ez_reloc_count;
    rel->rva = rva;
    rel->type = type;
    memset(rel->reserved, 0, sizeof(rel->reserved));
    return 0;
}

static int read_section(int fd, uint8_t *base, uint32_t dst_off,
                        uint32_t file_off, uint32_t size)
{
    return read_exact_at(fd, file_off, base + dst_off, size);
}

static void resolve_thm_pc22(uint16_t *addr, uint32_t place_rva,
                             uint32_t sym_val)
{
    uint16_t instr0 = addr[0], instr1 = addr[1];
    uint32_t s = (instr0 >> 10) & 1u;
    uint32_t j1 = (instr1 >> 13) & 1u;
    uint32_t j2 = (instr1 >> 11) & 1u;
    uint32_t imm10 = instr0 & 0x03ffu;
    uint32_t imm11 = instr1 & 0x07ffu;
    uint32_t i1 = (~(j1 ^ s)) & 1u;
    uint32_t i2 = (~(j2 ^ s)) & 1u;
    int32_t offset = (int32_t)((i1 << 23) | (i2 << 22) |
                               (imm10 << 12) | (imm11 << 1));
    uint32_t new_offset;

    if (s)
        offset |= (int32_t)0xff800000u;
    new_offset = (uint32_t)(offset + (int32_t)sym_val -
                            (int32_t)place_rva);
    s = new_offset >> 31;
    i1 = (new_offset >> 23) & 1u;
    i2 = (new_offset >> 22) & 1u;
    imm10 = (new_offset >> 12) & 0x03ffu;
    imm11 = (new_offset >> 1) & 0x07ffu;
    j1 = (~(i1 ^ s)) & 1u;
    j2 = (~(i2 ^ s)) & 1u;
    addr[0] = (uint16_t)(0xf000u | (s << 10) | imm10);
    addr[1] = (uint16_t)((0x1au << 11) | (j1 << 13) | (j2 << 11) | imm11);
}

static int resolve_thm_jump24(uint16_t *addr, uint32_t place_rva,
                              uint32_t sym_val)
{
    uint16_t instr0 = addr[0], instr1 = addr[1];
    uint32_t s = (instr0 >> 10) & 1u;
    uint32_t j1 = (instr1 >> 13) & 1u;
    uint32_t j2 = (instr1 >> 11) & 1u;
    uint32_t imm10 = instr0 & 0x03ffu;
    uint32_t imm11 = instr1 & 0x07ffu;
    uint32_t i1 = (~(j1 ^ s)) & 1u;
    uint32_t i2 = (~(j2 ^ s)) & 1u;
    int32_t addend = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                (imm10 << 12) | (imm11 << 1));
    int32_t rel;
    uint32_t urel;

    if (s)
        addend |= (int32_t)0xff000000u;
    rel = addend + (int32_t)(sym_val & ~1u) -
          ((int32_t)place_rva + 4);
    if (rel < -(1L << 24) || rel > ((1L << 24) - 2))
        return -1;

    urel = (uint32_t)rel;
    s = (urel >> 24) & 1u;
    i1 = (urel >> 23) & 1u;
    i2 = (urel >> 22) & 1u;
    imm10 = (urel >> 12) & 0x03ffu;
    imm11 = (urel >> 1) & 0x07ffu;
    j1 = (~(i1 ^ s)) & 1u;
    j2 = (~(i2 ^ s)) & 1u;
    addr[0] = (uint16_t)(0xf000u | (s << 10) | imm10);
    addr[1] = (uint16_t)((0x1cu << 11) | (j1 << 13) | (j2 << 11) | imm11);
    return 0;
}

static void resolve_thm_alu_abs_g0_nc(uint16_t *addr, uint32_t value)
{
    uint16_t instr0 = addr[0], instr1 = addr[1];
    uint32_t imm4 = instr0 & 0x000fu;
    uint32_t ibit = (instr0 >> 10) & 1u;
    uint32_t imm3 = (instr1 >> 12) & 7u;
    uint32_t imm8 = instr1 & 0x00ffu;
    uint32_t addend = (imm4 << 12) | (ibit << 11) | (imm3 << 8) | imm8;
    uint32_t imm16 = (value + addend) & 0xffffu;

    instr0 = (uint16_t)((instr0 & ~0x040fu) |
                        ((imm16 >> 12) & 0x0fu) |
                        (((imm16 >> 11) & 1u) << 10));
    instr1 = (uint16_t)((instr1 & ~0x70ffu) |
                        (((imm16 >> 8) & 7u) << 12) |
                        (imm16 & 0xffu));
    addr[0] = instr0;
    addr[1] = instr1;
}

static int find_tables(int fd, elf_load_meta *meta)
{
    uint16_t i;

    if (meta->eh.shstrndx == SHN_UNDEF || meta->eh.shstrndx >= meta->eh.shnum)
        return -1;
    if (read_shdr(fd, &meta->eh, meta->eh.shstrndx, &meta->shstrtab) != 0 ||
        meta->shstrtab.type != SHT_STRTAB)
        return -1;

    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr sh;
        if (read_shdr(fd, &meta->eh, i, &sh) != 0)
            return -1;
        if (sh.type != SHT_SYMTAB)
            continue;
        if (sh.link >= meta->eh.shnum)
            return -1;
        if (read_shdr(fd, &meta->eh, (uint16_t)sh.link, &meta->strtab) != 0 ||
            meta->strtab.type != SHT_STRTAB)
            return -1;
        meta->symtab = sh;
        meta->symtab_index = i;
        return 0;
    }
    return -1;
}

static int read_symbol(int fd, const elf_load_meta *meta, uint32_t sym_index,
                       elf32_sym *sym)
{
    uint32_t entsize = meta->symtab.entsize ? meta->symtab.entsize : sizeof(*sym);

    if (entsize < sizeof(*sym) || sym_index >= meta->symtab.size / entsize)
        return -1;
    return read_exact_at(fd, meta->symtab.offset + sym_index * entsize,
                         sym, sizeof(*sym));
}

static int read_name(int fd, uint32_t table_off, uint32_t table_size,
                     uint32_t name_off, char *buf, unsigned buf_size)
{
    uint32_t remain;
    unsigned read_len;
    char *nul;

    if (buf == NULL || buf_size < 2 || name_off >= table_size)
        return 0;
    remain = table_size - name_off;
    read_len = remain < (uint32_t)(buf_size - 1) ? (unsigned)remain : buf_size - 1;
    if (read_len == 0)
        return 0;
    if (read_exact_at(fd, table_off + name_off, buf, read_len) != 0)
        return 0;
    nul = (char *)memchr(buf, '\0', read_len);
    if (nul == buf)
        return 0;
    if (nul == NULL)
        buf[read_len] = '\0';
    return 1;
}

static int symbol_name(int fd, const elf_load_meta *meta,
                       const elf32_sym *sym, char *buf, unsigned buf_size)
{
    return read_name(fd, meta->strtab.offset, meta->strtab.size,
                     sym->name, buf, buf_size);
}

static int section_name(int fd, const elf_load_meta *meta,
                        const elf32_shdr *sh, char *buf, unsigned buf_size)
{
    return read_name(fd, meta->shstrtab.offset, meta->shstrtab.size,
                     sh->name, buf, buf_size);
}

static uint8_t startup_kind(const char *name, uint32_t type, uint8_t *is_plain)
{
    const char *base;
    uint8_t kind;
    size_t n;

    if (type == SHT_PREINIT_ARRAY) {
        base = ".preinit_array";
        kind = ELF_STARTUP_PREINIT;
    } else if (type == SHT_INIT_ARRAY) {
        base = ".init_array";
        kind = ELF_STARTUP_INIT;
    } else if (type == SHT_FINI_ARRAY) {
        base = ".fini_array";
        kind = ELF_STARTUP_FINI;
    } else {
        return ELF_STARTUP_NONE;
    }

    n = strlen(base);
    if (strcmp(name, base) == 0) {
        *is_plain = 1;
        return kind;
    }
    if (strncmp(name, base, n) == 0 && name[n] == '.') {
        *is_plain = 0;
        return kind;
    }
    return ELF_STARTUP_NONE;
}

static int load_section(int fd, uint8_t *base, elf_load_meta *meta,
                        uint16_t sec_num, uint32_t *sec_addr);

static int discover_startup_sections(int fd, uint8_t *base, elf_load_meta *meta)
{
    elf_sec_state *states = elf_states(meta);
    uint16_t i;

    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr sh;
        char name[64];
        uint8_t plain = 0;
        uint8_t kind;
        uint32_t sec_addr;

        if (read_shdr(fd, &meta->eh, i, &sh) != 0)
            return -1;
        if ((sh.flags & SHF_ALLOC) == 0)
            continue;
        if (sh.type != SHT_PREINIT_ARRAY && sh.type != SHT_INIT_ARRAY &&
            sh.type != SHT_FINI_ARRAY)
            continue;
        if (!section_name(fd, meta, &sh, name, sizeof(name)))
            return -1;
        kind = startup_kind(name, sh.type, &plain);
        if (kind == ELF_STARTUP_NONE)
            continue;
        if ((sh.size & 3u) != 0)
            return -1;

        states[i].startup_kind = kind;
        states[i].startup_plain = plain;
        states[i].size = sh.size;
        if (load_section(fd, base, meta, i, &sec_addr) != 0)
            return -1;
    }
    return 0;
}

static int startup_section_less(int fd, const elf_load_meta *meta,
                                uint16_t lhs, uint16_t rhs)
{
    elf32_shdr lsh, rsh;
    char lname[64], rname[64];
    int cmp;

    if (rhs == 0xffffu)
        return 1;
    if (read_shdr(fd, &meta->eh, lhs, &lsh) != 0 ||
        read_shdr(fd, &meta->eh, rhs, &rsh) != 0)
        return lhs < rhs;
    if (!section_name(fd, meta, &lsh, lname, sizeof(lname)) ||
        !section_name(fd, meta, &rsh, rname, sizeof(rname)))
        return lhs < rhs;
    cmp = strcmp(lname, rname);
    return cmp < 0 || (cmp == 0 && lhs < rhs);
}

static int startup_section_after(int fd, const elf_load_meta *meta,
                                 uint16_t candidate, uint16_t previous)
{
    if (previous == 0xffffu)
        return 1;
    return startup_section_less(fd, meta, previous, candidate);
}

static int copy_startup_kind(int fd, uint8_t *base, elf_load_meta *meta,
                             uint8_t kind, uint32_t dst_off,
                             uint32_t *entry_count)
{
    elf_sec_state *states = elf_states(meta);
    uint32_t count = 0;
    uint8_t plain_pass;

    for (plain_pass = 0; plain_pass <= 1; ++plain_pass) {
        uint16_t previous = 0xffffu;
        for (;;) {
            uint16_t best = 0xffffu;
            uint16_t i;
            for (i = 0; i < meta->eh.shnum; ++i) {
                if (states[i].startup_kind != kind ||
                    states[i].startup_plain != plain_pass ||
                    !startup_section_after(fd, meta, i, previous))
                    continue;
                if (startup_section_less(fd, meta, i, best))
                    best = i;
            }
            if (best == 0xffffu)
                break;
            if (states[best].size != 0) {
                memcpy(base + dst_off + count * sizeof(uint32_t),
                       base + states[best].offset, states[best].size);
                count += states[best].size / sizeof(uint32_t);
            }
            previous = best;
        }
    }
    *entry_count = count;
    return 0;
}

static int build_startup_arrays(int fd, uint8_t *base, elf_load_meta *meta)
{
    elf_sec_state *states = elf_states(meta);
    uint32_t pre_count = 0, init_count = 0, fini_count = 0;
    uint32_t total_entries, bytes, off;
    uint32_t i;

    if (discover_startup_sections(fd, base, meta) != 0)
        return -1;

    for (i = 0; i < meta->eh.shnum; ++i) {
        uint32_t n = states[i].size / sizeof(uint32_t);
        if (states[i].startup_kind == ELF_STARTUP_PREINIT)
            pre_count += n;
        else if (states[i].startup_kind == ELF_STARTUP_INIT)
            init_count += n;
        else if (states[i].startup_kind == ELF_STARTUP_FINI)
            fini_count += n;
    }

    total_entries = pre_count + init_count + fini_count;
    if (total_entries > UINT32_MAX / sizeof(uint32_t))
        return -1;
    bytes = total_entries * sizeof(uint32_t);
    off = align_up(meta->cursor, sizeof(uint32_t));
    if (off == UINT32_MAX || off > UINT32_MAX - bytes ||
        ensure_capacity(meta, off + bytes) != 0)
        return -1;

    meta->preinit_array_rva = EZ_IMAGE_RVA + off;
    if (copy_startup_kind(fd, base, meta, ELF_STARTUP_PREINIT, off,
                          &meta->preinit_array_count) != 0)
        return -1;
    for (i = 0; i < meta->preinit_array_count; ++i) {
        uint32_t *entry = (uint32_t *)(base + off + i * sizeof(uint32_t));
        if (*entry != 0 && *entry != UINT32_MAX &&
            emit_ez_reloc(meta, EZ_IMAGE_RVA + off + i * sizeof(uint32_t),
                          EZ_RELOC_ABS32) != 0)
            return -1;
    }
    off += meta->preinit_array_count * sizeof(uint32_t);

    meta->init_array_rva = EZ_IMAGE_RVA + off;
    if (copy_startup_kind(fd, base, meta, ELF_STARTUP_INIT, off,
                          &meta->init_array_count) != 0)
        return -1;
    for (i = 0; i < meta->init_array_count; ++i) {
        uint32_t *entry = (uint32_t *)(base + off + i * sizeof(uint32_t));
        if (*entry != 0 && *entry != UINT32_MAX &&
            emit_ez_reloc(meta, EZ_IMAGE_RVA + off + i * sizeof(uint32_t),
                          EZ_RELOC_ABS32) != 0)
            return -1;
    }
    off += meta->init_array_count * sizeof(uint32_t);

    meta->fini_array_rva = EZ_IMAGE_RVA + off;
    if (copy_startup_kind(fd, base, meta, ELF_STARTUP_FINI, off,
                          &meta->fini_array_count) != 0)
        return -1;
    for (i = 0; i < meta->fini_array_count; ++i) {
        uint32_t *entry = (uint32_t *)(base + off + i * sizeof(uint32_t));
        if (*entry != 0 && *entry != UINT32_MAX &&
            emit_ez_reloc(meta, EZ_IMAGE_RVA + off + i * sizeof(uint32_t),
                          EZ_RELOC_ABS32) != 0)
            return -1;
    }
    off += meta->fini_array_count * sizeof(uint32_t);
    meta->cursor = off;
    return 0;
}

static int synthetic_symbol(const elf_load_meta *meta, const char *name,
                            elf_symbol_ref *ref)
{
    uint32_t start;
    uint32_t count;

    if (strcmp(name, "__ez_preinit_array_start") == 0) {
        ref->value = meta->preinit_array_rva;
        ref->rebase = 1;
        return 1;
    }
    if (strcmp(name, "__ez_preinit_array_end") == 0) {
        start = meta->preinit_array_rva;
        count = meta->preinit_array_count;
    } else if (strcmp(name, "__ez_init_array_start") == 0) {
        ref->value = meta->init_array_rva;
        ref->rebase = 1;
        return 1;
    } else if (strcmp(name, "__ez_init_array_end") == 0) {
        start = meta->init_array_rva;
        count = meta->init_array_count;
    } else if (strcmp(name, "__ez_fini_array_start") == 0) {
        ref->value = meta->fini_array_rva;
        ref->rebase = 1;
        return 1;
    } else if (strcmp(name, "__ez_fini_array_end") == 0) {
        start = meta->fini_array_rva;
        count = meta->fini_array_count;
    } else {
        return 0;
    }

    ref->value = start + count * sizeof(uint32_t);
    ref->rebase = 1;
    return 1;
}

static int symbol_ref(int fd, uint8_t *base, elf_load_meta *meta,
                      uint32_t sym_index, elf_symbol_ref *ref)
{
    elf32_sym sym;
    elf32_shdr sec;
    uint32_t sec_rva;
    uint8_t bind;

    if (read_symbol(fd, meta, sym_index, &sym) != 0)
        return -1;
    bind = sym.info >> 4;

    if (sym.shndx == SHN_UNDEF) {
        char name[64];

        if (bind == STB_WEAK) {
            ref->value = 0;
            ref->rebase = 0;
            return 0;
        }
        if (symbol_name(fd, meta, &sym, name, sizeof(name)) &&
            synthetic_symbol(meta, name, ref))
            return 0;

        if (symbol_name(fd, meta, &sym, name, sizeof(name)))
            printf("ELF: undefined symbol: %s\n", name);
        else
            printf("ELF: undefined symbol #%lu\n", (unsigned long)sym_index);
        return -1;
    }

    if (sym.shndx == SHN_ABS) {
        ref->value = sym.value;
        ref->rebase = 0;
        return 0;
    }
    if (sym.shndx >= meta->eh.shnum)
        return -1;
    if (read_shdr(fd, &meta->eh, sym.shndx, &sec) != 0 || sym.value > sec.size)
        return -1;
    if (load_section(fd, base, meta, sym.shndx, &sec_rva) != 0)
        return -1;

    ref->value = sec_rva + sym.value;
    ref->rebase = 1;
    return 0;
}

static void print_relocation_symbol_error(int fd, const elf_load_meta *meta,
                                          uint32_t sym_index, uint8_t type,
                                          uint16_t sec_num, uint32_t offset,
                                          const char *reason)
{
    elf32_sym sym;
    char name[64];

    if (read_symbol(fd, meta, sym_index, &sym) == 0 &&
        symbol_name(fd, meta, &sym, name, sizeof(name))) {
        printf("\nELF: %s: %s (relocation type %u, section %u, offset %lu)\n",
               reason, name, (unsigned)type, (unsigned)sec_num,
               (unsigned long)offset);
    } else {
        printf("\nELF: %s: symbol #%lu (relocation type %u, section %u, "
               "offset %lu)\n",
               reason, (unsigned long)sym_index, (unsigned)type,
               (unsigned)sec_num, (unsigned long)offset);
    }
}

static int apply_section_relocations(int fd, uint8_t *base,
                                     elf_load_meta *meta, uint16_t sec_num,
                                     const elf32_shdr *target,
                                     uint32_t target_off)
{
    uint16_t i;

    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr relsec, symtab;
        uint32_t j, rel_entsize;

        if (read_shdr(fd, &meta->eh, i, &relsec) != 0)
            return -1;
        if (relsec.type != SHT_REL || relsec.info != sec_num)
            continue;
        if (relsec.link >= meta->eh.shnum || relsec.link != meta->symtab_index)
            return -1;
        if (read_shdr(fd, &meta->eh, (uint16_t)relsec.link, &symtab) != 0 ||
            symtab.type != SHT_SYMTAB)
            return -1;

        rel_entsize = relsec.entsize ? relsec.entsize : sizeof(elf32_rel);
        if (rel_entsize < sizeof(elf32_rel))
            return -1;

        for (j = 0; j < relsec.size / rel_entsize; ++j) {
            elf32_rel rel;
            elf_symbol_ref sym;
            uint32_t sym_index;
            uint8_t type;
            uint8_t *place;
            uint32_t place_rva;

            if (read_exact_at(fd, relsec.offset + j * rel_entsize,
                              &rel, sizeof(rel)) != 0)
                return -1;
            if (rel.offset > target->size || target->size - rel.offset < 4)
                return -1;

            sym_index = rel.info >> 8;
            type = (uint8_t)(rel.info & 0xffu);
            if (symbol_ref(fd, base, meta, sym_index, &sym) != 0)
                return -1;

            place = base + target_off + rel.offset;
            place_rva = EZ_IMAGE_RVA + target_off + rel.offset;

            if (++meta->relocation_progress >= ELF2EZ_PROGRESS_RELOCS) {
                meta->relocation_progress = 0;
                putchar('.');
            }

            switch (type) {
                case R_ARM_ABS32:
                    *(uint32_t *)place += sym.value;
                    if (sym.rebase &&
                        emit_ez_reloc(meta, place_rva, EZ_RELOC_ABS32) != 0)
                        return -1;
                    break;

                case R_ARM_REL32:
                    if (!sym.rebase) {
                        print_relocation_symbol_error(
                            fd, meta, sym_index, type, sec_num, rel.offset,
                            "cannot resolve relocation symbol");
                        return -1;
                    }
                    *(uint32_t *)place =
                        sym.value + *(uint32_t *)place - place_rva;
                    break;

                case R_ARM_THM_PC22:
                    if (!sym.rebase) {
                        print_relocation_symbol_error(
                            fd, meta, sym_index, type, sec_num, rel.offset,
                            "cannot resolve relocation symbol");
                        return -1;
                    }
                    resolve_thm_pc22((uint16_t *)place, place_rva, sym.value);
                    break;

                case R_ARM_THM_JUMP24:
                    if (!sym.rebase) {
                        print_relocation_symbol_error(
                            fd, meta, sym_index, type, sec_num, rel.offset,
                            "cannot resolve relocation symbol");
                        return -1;
                    }
                    if (resolve_thm_jump24((uint16_t *)place,
                                           place_rva, sym.value) != 0) {
                        printf("\nELF: Thumb JUMP24 relocation out of range "
                               "(section %u, offset %lu)\n",
                               (unsigned)sec_num, (unsigned long)rel.offset);
                        return -1;
                    }
                    break;

                case R_ARM_THM_ALU_ABS_G0_NC:
                    if ((uintptr_t)place & 1u)
                        return -1;
                    resolve_thm_alu_abs_g0_nc((uint16_t *)place, sym.value);
                    if (sym.rebase &&
                        emit_ez_reloc(meta, place_rva,
                                      EZ_RELOC_THM_ALU_ABS_G0_NC) != 0)
                        return -1;
                    break;

                default: {
                    elf32_sym diag_sym;
                    char name[64];
                    if (read_symbol(fd, meta, sym_index, &diag_sym) == 0 &&
                        symbol_name(fd, meta, &diag_sym, name, sizeof(name)))
                        printf("\nELF: unsupported relocation type %u "
                               "(section %u, offset %lu, symbol %s)\n",
                               (unsigned)type, (unsigned)sec_num,
                               (unsigned long)rel.offset, name);
                    else
                        printf("\nELF: unsupported relocation type %u "
                               "(section %u, offset %lu, symbol #%lu)\n",
                               (unsigned)type, (unsigned)sec_num,
                               (unsigned long)rel.offset,
                               (unsigned long)sym_index);
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int load_section(int fd, uint8_t *base, elf_load_meta *meta,
                        uint16_t sec_num, uint32_t *sec_addr)
{
    elf_sec_state *states = elf_states(meta);
    elf_sec_state *state;
    elf32_shdr sh;
    uint32_t off;

    if (sec_num >= meta->shnum)
        return -1;
    state = &states[sec_num];
    if (state->state == ELF_SEC_LOADING || state->state == ELF_SEC_LOADED) {
        *sec_addr = EZ_IMAGE_RVA + state->offset;
        return 0;
    }

    if (read_shdr(fd, &meta->eh, sec_num, &sh) != 0)
        return -1;
    off = align_up(meta->cursor, sh.addralign);
    if (off == UINT32_MAX || off > UINT32_MAX - sh.size ||
        ensure_capacity(meta, off + sh.size) != 0)
        return -1;

    state->offset = off;
    state->size = sh.size;
    state->state = ELF_SEC_LOADING;
    meta->cursor = off + sh.size;

    if (sh.size != 0) {
        if (sh.type == SHT_NOBITS)
            memset(base + off, 0, sh.size);
        else if (read_section(fd, base, off, sh.offset, sh.size) != 0)
            return -1;
    }

    *sec_addr = EZ_IMAGE_RVA + off;
    if (apply_section_relocations(fd, base, meta, sec_num, &sh, off) != 0)
        return -1;
    state->state = ELF_SEC_LOADED;
    putchar('s');
    return 0;
}


static int startup_layout_from_states(elf_load_meta *meta, uint32_t off,
                                      uint32_t *end_off)
{
    elf_sec_state *states = elf_states(meta);
    uint32_t pre_count = 0;
    uint32_t init_count = 0;
    uint32_t fini_count = 0;
    uint32_t total;
    uint32_t i;

    off = align_up(off, sizeof(uint32_t));
    if (off == UINT32_MAX)
        return -1;

    for (i = 0; i < meta->eh.shnum; ++i) {
        uint32_t n;

        if (states[i].state != ELF_SEC_LOADED)
            continue;
        if ((states[i].size & 3u) != 0 &&
            states[i].startup_kind != ELF_STARTUP_NONE)
            return -1;

        n = states[i].size / sizeof(uint32_t);
        if (states[i].startup_kind == ELF_STARTUP_PREINIT)
            pre_count += n;
        else if (states[i].startup_kind == ELF_STARTUP_INIT)
            init_count += n;
        else if (states[i].startup_kind == ELF_STARTUP_FINI)
            fini_count += n;
    }

    total = pre_count + init_count;
    if (total < pre_count || total > UINT32_MAX - fini_count)
        return -1;
    total += fini_count;
    if (total > UINT32_MAX / sizeof(uint32_t))
        return -1;

    meta->preinit_array_rva = EZ_IMAGE_RVA + off;
    meta->preinit_array_count = pre_count;
    off += pre_count * sizeof(uint32_t);

    meta->init_array_rva = EZ_IMAGE_RVA + off;
    meta->init_array_count = init_count;
    off += init_count * sizeof(uint32_t);

    meta->fini_array_rva = EZ_IMAGE_RVA + off;
    meta->fini_array_count = fini_count;
    off += fini_count * sizeof(uint32_t);

    *end_off = off;
    return 0;
}

static int emit_final_startup_arrays(int fd, uint8_t *base,
                                     elf_load_meta *meta)
{
    uint32_t off;
    uint32_t count;
    uint32_t i;

    off = meta->preinit_array_rva - EZ_IMAGE_RVA;
    if (copy_startup_kind(fd, base, meta, ELF_STARTUP_PREINIT, off,
                          &count) != 0 ||
        count != meta->preinit_array_count)
        return -1;
    for (i = 0; i < count; ++i) {
        uint32_t *entry = (uint32_t *)(base + off + i * sizeof(uint32_t));
        if (*entry != 0 && *entry != UINT32_MAX &&
            emit_ez_reloc(meta, EZ_IMAGE_RVA + off + i * sizeof(uint32_t),
                          EZ_RELOC_ABS32) != 0)
            return -1;
    }

    off = meta->init_array_rva - EZ_IMAGE_RVA;
    if (copy_startup_kind(fd, base, meta, ELF_STARTUP_INIT, off,
                          &count) != 0 ||
        count != meta->init_array_count)
        return -1;
    for (i = 0; i < count; ++i) {
        uint32_t *entry = (uint32_t *)(base + off + i * sizeof(uint32_t));
        if (*entry != 0 && *entry != UINT32_MAX &&
            emit_ez_reloc(meta, EZ_IMAGE_RVA + off + i * sizeof(uint32_t),
                          EZ_RELOC_ABS32) != 0)
            return -1;
    }

    off = meta->fini_array_rva - EZ_IMAGE_RVA;
    if (copy_startup_kind(fd, base, meta, ELF_STARTUP_FINI, off,
                          &count) != 0 ||
        count != meta->fini_array_count)
        return -1;
    for (i = 0; i < count; ++i) {
        uint32_t *entry = (uint32_t *)(base + off + i * sizeof(uint32_t));
        if (*entry != 0 && *entry != UINT32_MAX &&
            emit_ez_reloc(meta, EZ_IMAGE_RVA + off + i * sizeof(uint32_t),
                          EZ_RELOC_ABS32) != 0)
            return -1;
    }

    return 0;
}

/*
 * The discovery pass above lays sections out in dependency order, which can
 * interleave SHT_NOBITS with file-backed sections.  EZ v1 requires one
 * contiguous initialized prefix followed by a zero-only memory tail, so do a
 * second deterministic layout after the complete reachable graph is known:
 *
 *   file-backed ELF sections
 *   synthesized preinit/init/fini arrays
 *   SHT_NOBITS sections
 *
 * All section relocations are then re-applied using the final RVAs.  The
 * converter still keeps the full memory image in its work block; only the
 * zero-only tail is omitted from the output file.
 */
static int finalize_image_layout(int fd, uint8_t *base, elf_load_meta *meta,
                                 uint32_t reloc_store_top,
                                 uint32_t *file_size, uint32_t *mem_size)
{
    elf_sec_state *states = elf_states(meta);
    uint32_t off = 0;
    uint32_t startup_end;
    uint32_t file_end;
    uint32_t memory_end;
    uint32_t i;

    /* First assign every reachable file-backed ELF section. */
    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr sh;

        if (states[i].state != ELF_SEC_LOADED)
            continue;
        if (read_shdr(fd, &meta->eh, (uint16_t)i, &sh) != 0)
            return -1;
        if (sh.type == SHT_NOBITS)
            continue;

        off = align_up(off, sh.addralign);
        if (off == UINT32_MAX || off > UINT32_MAX - sh.size)
            return -1;
        states[i].offset = off;
        states[i].size = sh.size;
        off += sh.size;
    }

    /*
     * Reserve the synthesized startup arrays inside the initialized prefix.
     * Their concrete RVAs must be known before crt0 relocations are re-applied.
     */
    if (startup_layout_from_states(meta, off, &startup_end) != 0)
        return -1;
    file_end = align_up(startup_end, 4u);
    if (file_end == UINT32_MAX)
        return -1;

    /* Then place every reachable NOBITS section in the zero-only tail. */
    off = file_end;
    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr sh;

        if (states[i].state != ELF_SEC_LOADED)
            continue;
        if (read_shdr(fd, &meta->eh, (uint16_t)i, &sh) != 0)
            return -1;
        if (sh.type != SHT_NOBITS)
            continue;

        off = align_up(off, sh.addralign);
        if (off == UINT32_MAX || off > UINT32_MAX - sh.size)
            return -1;
        states[i].offset = off;
        states[i].size = sh.size;
        off += sh.size;
    }

    memory_end = align_up(off, 4u);
    if (memory_end == UINT32_MAX)
        return -1;

    /*
     * Throw away relocation records from the discovery layout.  The final pass
     * emits a fresh table whose RVAs match the compacted image.
     */
    meta->cursor = memory_end;
    meta->reloc_store_addr = reloc_store_top;
    meta->ez_reloc_count = 0;
    meta->relocation_progress = 0;
    if (ensure_capacity(meta, memory_end) != 0)
        return -1;

    memset(base, 0, memory_end);

    /* Re-read file-backed data at its final offsets; NOBITS stays zero. */
    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr sh;

        if (states[i].state != ELF_SEC_LOADED)
            continue;
        if (read_shdr(fd, &meta->eh, (uint16_t)i, &sh) != 0)
            return -1;
        if (sh.type != SHT_NOBITS && sh.size != 0 &&
            read_section(fd, base, states[i].offset, sh.offset, sh.size) != 0)
            return -1;
    }

    /*
     * Re-apply every relocation now that all final section RVAs are fixed.
     * symbol_ref() can only encounter already-loaded sections here because the
     * first pass recursively discovered the complete dependency graph.
     */
    for (i = 0; i < meta->eh.shnum; ++i) {
        elf32_shdr sh;

        if (states[i].state != ELF_SEC_LOADED)
            continue;
        if (read_shdr(fd, &meta->eh, (uint16_t)i, &sh) != 0 ||
            apply_section_relocations(fd, base, meta, (uint16_t)i, &sh,
                                      states[i].offset) != 0)
            return -1;
    }

    /*
     * The source startup sections now contain their final relocated function
     * pointers.  Copy them into the contiguous synthesized arrays and emit the
     * corresponding load-base relocations.
     */
    if (emit_final_startup_arrays(fd, base, meta) != 0)
        return -1;

    *file_size = file_end;
    *mem_size = memory_end;
    return 0;
}

static int find_entry_symbol(int fd, elf_load_meta *meta, uint32_t *entry_idx)
{
    uint32_t count;
    uint32_t i;
    uint32_t entsize = meta->symtab.entsize ? meta->symtab.entsize
                                             : sizeof(elf32_sym);

    if (entsize < sizeof(elf32_sym))
        return -1;
    count = meta->symtab.size / entsize;
    *entry_idx = ELF_NO_SYMBOL;

    for (i = 0; i < count; ++i) {
        elf32_sym sym;
        char name[64];
        uint8_t bind, type;

        if (read_symbol(fd, meta, i, &sym) != 0)
            return -1;
        if ((i & 63u) == 63u)
            putchar('.');

        bind = sym.info >> 4;
        type = sym.info & 0x0fu;
        if (type != STT_FUNC || bind != STB_GLOBAL)
            continue;
        if (!symbol_name(fd, meta, &sym, name, sizeof(name)))
            continue;
        if (strcmp(name, "__ez_start") == 0) {
            *entry_idx = i;
            return 0;
        }
    }
    return 0;
}

static int read_named_data_symbol(int fd, const elf_load_meta *meta,
                                  const char *wanted, void *dst, uint32_t size)
{
    uint32_t entsize = meta->symtab.entsize ? meta->symtab.entsize
                                             : sizeof(elf32_sym);
    uint32_t count;
    uint32_t i;

    if (entsize < sizeof(elf32_sym))
        return -1;
    count = meta->symtab.size / entsize;

    for (i = 0; i < count; ++i) {
        elf32_sym sym;
        elf32_shdr sh;
        char name[64];
        uint8_t bind;
        uint8_t type;

        if (read_symbol(fd, meta, i, &sym) != 0)
            return -1;
        bind = sym.info >> 4;
        type = sym.info & 0x0fu;
        if ((bind != STB_GLOBAL && bind != STB_WEAK) || type != STT_OBJECT)
            continue;
        if (!symbol_name(fd, meta, &sym, name, sizeof(name)) ||
            strcmp(name, wanted) != 0)
            continue;
        if (sym.shndx == SHN_UNDEF)
            return 0;
        if (sym.shndx >= meta->eh.shnum ||
            read_shdr(fd, &meta->eh, sym.shndx, &sh) != 0 ||
            sh.type == SHT_NOBITS || sym.value > sh.size ||
            size > sh.size - sym.value)
            return -1;
        return read_exact_at(fd, sh.offset + sym.value, dst, size) == 0 ? 1 : -1;
    }
    return 0;
}

static int write_exact(int fd, const void *src, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)src;
    uint32_t done = 0;

    while (done < size) {
        unsigned chunk = (unsigned)(size - done);
        int written;

        if (chunk > ELF2EZ_READ_CHUNK)
            chunk = ELF2EZ_READ_CHUNK;
        written = write(fd, p + done, chunk);
        if (written <= 0)
            return -1;
        done += (uint32_t)written;
    }
    return 0;
}

static int convert_elf_to_ez(int input, int output)
{
    dos_block block;
    elf32_ehdr eh;
    elf_load_meta *meta;
    struct ez_file_header header;
    native_ez_process_requirements requirements;
    uint8_t *image;
    struct ez_reloc *relocs;
    uint32_t metadata_size;
    uint32_t image_store_off;
    uint32_t image_file_size;
    uint32_t image_mem_size;
    uint32_t reloc_store_top;
    uint32_t entry_idx;
    elf_symbol_ref entry;
    int rc = -1;

    if (read_exact_at(input, 0, &eh, sizeof(eh)) != 0) {
        printf("Bad ELF: cannot read header\n");
        return -1;
    }
    if (eh.magic != ELF32_MAGIC || eh.elf_class != ELFCLASS32 ||
        eh.data != ELFDATA2LSB || eh.ident_version != EV_CURRENT ||
        eh.version != EV_CURRENT || eh.abi != 0 || eh.type != ET_REL ||
        eh.machine != EM_ARM || (eh.flags & EF_ARM_ABI_FLOAT_HARD) != 0 ||
        eh.ehsize < sizeof(eh) || eh.shentsize != sizeof(elf32_shdr) ||
        eh.shnum == 0) {
        printf("Bad ELF: unsupported ARM ET_REL format\n");
        return -1;
    }

    metadata_size = elf_metadata_size(eh.shnum);
    if (metadata_size == UINT32_MAX) {
        printf("Bad ELF: metadata overflow\n");
        return -1;
    }

    if (alloc_largest_block(&block) != 0) {
        printf("Unable to allocate ELF work block\n");
        return -1;
    }

    image_store_off = align_up(metadata_size, 4u);
    if (image_store_off == UINT32_MAX || image_store_off > block.size) {
        printf("ELF metadata does not fit work block\n");
        goto out;
    }

    meta = (elf_load_meta *)block.ptr;
    memset(meta, 0, metadata_size);
    image = block.ptr + image_store_off;

    meta->eh = eh;
    meta->shnum = eh.shnum;
    meta->cursor = 0;
#ifdef ELF2EZ_HOST
    /*
     * On a 64-bit host a work-block pointer cannot survive truncation to the
     * uint32_t image_store_addr/reloc_store_addr fields. Store both as offsets
     * from the block base instead; the block base is meta itself, so every
     * consumer reconstructs the pointer as (uint8_t *)meta + offset. All the
     * range arithmetic on these fields is offset-vs-offset and stays correct.
     * On the 32-bit target these offsets are not used (see #else below).
     */
    meta->image_store_addr = image_store_off;
    meta->reloc_store_addr = block.size;
#else
    meta->image_store_addr = (uint32_t)(uintptr_t)image;
    meta->reloc_store_addr = (uint32_t)(uintptr_t)(block.ptr + block.size);
#endif
    reloc_store_top = meta->reloc_store_addr;

    if (find_tables(input, meta) != 0) {
        printf("Bad ELF: cannot find symbol/string tables\n");
        goto out;
    }

    memset(&requirements, 0, sizeof(requirements));
    {
        int req_rc = read_named_data_symbol(input, meta,
                                             "__native_ez_process_requirements",
                                             &requirements, sizeof(requirements));
        if (req_rc < 0) {
            printf("Bad ELF: invalid __native_ez_process_requirements\n");
            goto out;
        }
    }

    /*
     * Build the final contiguous startup arrays before loading crt0.  This
     * assigns concrete RVA values to the six __ez_*_array_start/end symbols
     * which crt0 deliberately leaves undefined in the ET_REL input.
     */
    if (build_startup_arrays(input, image, meta) != 0) {
        printf("\nBad ELF: cannot build startup arrays\n");
        goto out;
    }

    if (find_entry_symbol(input, meta, &entry_idx) != 0 ||
        entry_idx == ELF_NO_SYMBOL) {
        printf("\nBad ELF: global __ez_start() not found\n");
        goto out;
    }
    if (symbol_ref(input, image, meta, entry_idx, &entry) != 0 ||
        !entry.rebase) {
        printf("\nBad ELF: cannot load EZ entry dependencies\n");
        goto out;
    }

    if (finalize_image_layout(input, image, meta, reloc_store_top,
                              &image_file_size, &image_mem_size) != 0) {
        printf("\nBad ELF: cannot finalize compact EZ image layout\n");
        goto out;
    }

    /*
     * __ez_start may have moved with its section during final compaction.
     * Resolve it again against the final state table before writing the header.
     */
    if (symbol_ref(input, image, meta, entry_idx, &entry) != 0 ||
        !entry.rebase) {
        printf("\nBad ELF: cannot resolve final EZ entry\n");
        goto out;
    }

    memset(&header, 0, sizeof(header));
    header.magic = EZ_MAGIC;
    header.version = EZ_FORMAT_VERSION;
    header.header_size = sizeof(header);
    header.flags = EZ_FLAG_THUMB | EZ_FLAG_ARMV6M | EZ_FLAG_SOFT_FLOAT;
    header.required_dos_api_version = requirements.required_dos_api_version;
    header.native_stack_size = requirements.native_stack_size;
    header.dos_stack_size = requirements.dos_stack_size;

    header.image_file_size = image_file_size;
    header.image_mem_size = image_mem_size;
    header.entry_rva = entry.value;
    header.reloc_offset = sizeof(header) + image_file_size;
    header.reloc_count = meta->ez_reloc_count;
    header.reloc_entry_size = sizeof(struct ez_reloc);
#ifdef ELF2EZ_HOST
    relocs = (struct ez_reloc *)((uint8_t *)meta + meta->reloc_store_addr);
#else
    relocs = (struct ez_reloc *)(uintptr_t)meta->reloc_store_addr;
#endif

    if (write_exact(output, &header, sizeof(header)) != 0 ||
        write_exact(output, image, image_file_size) != 0 ||
        write_exact(output, relocs,
                    meta->ez_reloc_count * sizeof(struct ez_reloc)) != 0) {
        rc = 1;
        goto out;
    }

    printf("\nEZ image: %lu bytes file, %lu bytes memory, "
           "%lu relocations, entry=%08lx\n",
           (unsigned long)image_file_size,
           (unsigned long)image_mem_size,
           (unsigned long)meta->ez_reloc_count,
           (unsigned long)header.entry_rva);
    rc = 0;

out:
    free_dos_block(&block);
    return rc;
}


int main(int argc, char **argv)
{
    char output_name[ELF2EZ_OUTPUT_NAME_MAX];
    const char *input_name = NULL;
    int overwrite = 0;
    int input;
    int output;
    int i;

    for (i = 1; i < argc; ++i) {
        if (is_yes_option(argv[i])) {
            overwrite = 1;
        } else if (input_name == NULL) {
            input_name = argv[i];
        } else {
            input_name = NULL;
            break;
        }
    }

    if (input_name == NULL) {
        printf("Usage: elf2ez [/y] <input-file>\n");
        return 1;
    }

    if (!make_output_name(input_name, output_name, sizeof(output_name))) {
        printf("Bad filename: %s\n", input_name);
        return 1;
    }

    input = open(input_name, O_RDONLY | O_BINARY);
    if (input < 0) {
        printf("Bad filename: %s\n", input_name);
        return 1;
    }

    if (!overwrite && access(output_name, 0) == 0 &&
        !confirm_overwrite(output_name)) {
        close(input);
        return 0;
    }

    output = open(output_name, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (output < 0) {
        close(input);
        printf("Unable to write file: %s\n", output_name);
        return 1;
    }

    {
        int convert_result = convert_elf_to_ez(input, output);
        if (convert_result != 0) {
            close(input);
            close(output);
            remove(output_name);
            if (convert_result > 0)
                printf("Unable to write file: %s\n", output_name);
            return 1;
        }
    }

    close(input);
    if (close(output) < 0) {
        remove(output_name);
        printf("Unable to write file: %s\n", output_name);
        return 1;
    }

    return 0;
}
