#include "hdrs.h"
#include "bios/bios.h"
#include "fdos.h"

#define XMS_VERSION 0x00
#define REQUEST_HMA 0x01
#define RELEASE_HMA 0x02
#define GLOBAL_ENABLE_A20 0x03
#define GLOBAL_DISABLE_A20 0x04
#define LOCAL_ENABLE_A20 0x05
#define LOCAL_DISABLE_A20 0x06
#define QUERY_A20 0x07

#define QUERY_EMB 0x08
#define ALLOCATE_EMB 0x09
#define RELEASE_EMB 0x0A
#define MOVE_EMB 0x0B

#define LOCK_EMB 0x0C
#define UNLOCK_EMB 0x0D
#define EMB_HANDLE_INFO 0x0E
#define REALLOCATE_EMB 0x0F

#define REQUEST_UMB 0x10
#define RELEASE_UMB 0x11

#define XMS_HANDLES 64
/*
 * EMB pool lives ABOVE the HMA. HMA is FFFF:0010..FFFF:FFFF, i.e. physical
 * 0x100000..0x10FFEF, and is owned by the kernel (DOS=HIGH, buffers).
 * Handing EMB storage out starting at 0x100000 lets any XMS client
 * (e.g. FreeCOM swap) overwrite the kernel/buffers in HMA.
 */
#define XMS_EMB_BASE_PHYS 0x00110000ul

#define to_physical_offset(offset) (((uint16_t)(((offset) >> 16) & 0xFFFF) << 4) + (uint16_t)((offset) & 0xFFFF))

static inline uint16_t xms_memory_kb(void) {
    /* CMOS 17h/18h is initialized by BIOS POST as extended memory above 1 MiB. */
    return (uint16_t)cmos_read(cpu, 0x17) | ((uint16_t)cmos_read(cpu, 0x18) << 8);
}

/* Extended memory reported by CMOS starts at 1 MiB and includes the HMA;
 * the EMB pool excludes the first 64 KiB (HMA). */
static inline uint32_t emb_pool_size(void) {
    uint16_t kb = xms_memory_kb();
    return (kb > 64) ? ((uint32_t)(kb - 64) << 10) : 0;
}

static inline uint8_t *xms_ptr(uint32_t offset) {
    return X86_RAM_BASE + XMS_EMB_BASE_PHYS + offset;
}

typedef struct {
    uint32_t base;   /* byte offset inside the EMB pool */
    uint32_t size;   /* bytes */
    uint8_t  used;
    uint8_t  locks;
} emb_handle_t;

static emb_handle_t emb_handles[XMS_HANDLES + 1]; /* index 0 is never a valid handle */

static int emb_free_handle_count(void) {
    int n = 0;
    for (int i = 1; i <= XMS_HANDLES; ++i)
        if (!emb_handles[i].used) n++;
    return n;
}

/* First-fit search for a gap of `size` bytes in the pool. Returns true and
 * the base offset on success. size == 0 is legal per XMS spec. */
static bool emb_find_gap(uint32_t size, uint32_t *out_base) {
    const uint32_t pool = emb_pool_size();
    if (size > pool)
        return false;
    uint32_t cand = 0;
    bool moved = true;
    while (moved) {
        moved = false;
        for (int i = 1; i <= XMS_HANDLES; ++i) {
            const emb_handle_t *h = &emb_handles[i];
            if (!h->used || h->size == 0)
                continue;
            if (cand < h->base + h->size && h->base < cand + (size ? size : 1)) {
                cand = h->base + h->size;
                moved = true;
            }
        }
        if (cand + size > pool)
            return false;
    }
    *out_base = cand;
    return true;
}

static void emb_free_stats(uint32_t *largest, uint32_t *total) {
    const uint32_t pool = emb_pool_size();
    uint32_t used = 0;
    uint32_t big = 0;
    /* total free */
    for (int i = 1; i <= XMS_HANDLES; ++i)
        if (emb_handles[i].used)
            used += emb_handles[i].size;
    *total = (pool > used) ? pool - used : 0;
    /* largest gap: walk gaps between sorted extents (O(n^2), n<=64) */
    uint32_t cursor = 0;
    for (;;) {
        /* find the used extent with the smallest base >= cursor */
        uint32_t next_base = pool, next_end = pool;
        for (int i = 1; i <= XMS_HANDLES; ++i) {
            const emb_handle_t *h = &emb_handles[i];
            if (h->used && h->size && h->base + h->size > cursor && h->base < next_base) {
                next_base = h->base;
                next_end = h->base + h->size;
            }
        }
        if (next_base > cursor && next_base - cursor > big)
            big = next_base - cursor;
        if (next_base == pool)
            break;
        cursor = (next_end > cursor) ? next_end : cursor + 1;
    }
    *largest = big;
}

/// TODO: remove it on debug finished
static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 2FH - ERROR: no handler defined ");
while(1); // remove it
    return true;
}

typedef struct __attribute__((packed, aligned)) {
    uint32_t length;
    uint16_t source_handle;
    uint32_t source_offset;
    uint16_t destination_handle;
    uint32_t destination_offset;
} move_data_t;

typedef struct umb {
    uint16_t segment;
    uint16_t size; // paragraphs
    int allocated_paragraphs; // -1 for a chain
} umb_t;

static umb_t umb_blocks[] = {
    // 0xD0000–0xDFFFF (64 KB)
    {0xD000, 0x0080, 0}, {0xD080, 0x0080, 0}, {0xD100, 0x0080, 0}, {0xD180, 0x0080, 0},
    {0xD200, 0x0080, 0}, {0xD280, 0x0080, 0}, {0xD300, 0x0080, 0}, {0xD380, 0x0080, 0},
    {0xD400, 0x0080, 0}, {0xD480, 0x0080, 0}, {0xD500, 0x0080, 0}, {0xD580, 0x0080, 0},
    {0xD600, 0x0080, 0}, {0xD680, 0x0080, 0}, {0xD700, 0x0080, 0}, {0xD780, 0x0080, 0},
    {0xD800, 0x0080, 0}, {0xD880, 0x0080, 0}, {0xD900, 0x0080, 0}, {0xD980, 0x0080, 0},
    {0xDA00, 0x0080, 0}, {0xDA80, 0x0080, 0}, {0xDB00, 0x0080, 0}, {0xDB80, 0x0080, 0},
    {0xDC00, 0x0080, 0}, {0xDC80, 0x0080, 0}, {0xDD00, 0x0080, 0}, {0xDD80, 0x0080, 0},
    {0xDE00, 0x0080, 0}, {0xDE80, 0x0080, 0}, {0xDF00, 0x0080, 0}, {0xDF80, 0x0080, 0},

    // 0xE0000–0xEFFFF (64 KB)
    {0xE000, 0x0080, 0}, {0xE080, 0x0080, 0}, {0xE100, 0x0080, 0}, {0xE180, 0x0080, 0},
    {0xE200, 0x0080, 0}, {0xE280, 0x0080, 0}, {0xE300, 0x0080, 0}, {0xE380, 0x0080, 0},
    {0xE400, 0x0080, 0}, {0xE480, 0x0080, 0}, {0xE500, 0x0080, 0}, {0xE580, 0x0080, 0},
    {0xE600, 0x0080, 0}, {0xE680, 0x0080, 0}, {0xE700, 0x0080, 0}, {0xE780, 0x0080, 0},
    {0xE800, 0x0080, 0}, {0xE880, 0x0080, 0}, {0xE900, 0x0080, 0}, {0xE980, 0x0080, 0},
    {0xEA00, 0x0080, 0}, {0xEA80, 0x0080, 0}, {0xEB00, 0x0080, 0}, {0xEB80, 0x0080, 0},
    {0xEC00, 0x0080, 0}, {0xEC80, 0x0080, 0}, {0xED00, 0x0080, 0}, {0xED80, 0x0080, 0},
    {0xEE00, 0x0080, 0}, {0xEE80, 0x0080, 0}, {0xEF00, 0x0080, 0}, {0xEF80, 0x0080, 0},

    // 0xF0000–0xF7FFF (32 KB)
    {0xF000, 0x0080, 0}, {0xF080, 0x0080, 0}, {0xF100, 0x0080, 0}, {0xF180, 0x0080, 0},
    {0xF200, 0x0080, 0}, {0xF280, 0x0080, 0}, {0xF300, 0x0080, 0}, {0xF380, 0x0080, 0},
    {0xF400, 0x0080, 0}, {0xF480, 0x0080, 0}, {0xF500, 0x0080, 0}, {0xF580, 0x0080, 0},
    {0xF600, 0x0080, 0}, {0xF680, 0x0080, 0}, {0xF700, 0x0080, 0}, {0xF780, 0x0080, 0},

    // 0xF8000–0xFBFFF (16 KB)
    {0xF800, 0x0080, 0}, {0xF880, 0x0080, 0}, {0xF900, 0x0080, 0}, {0xF980, 0x0080, 0},
    {0xFA00, 0x0080, 0}, {0xFA80, 0x0080, 0}, {0xFB00, 0x0080, 0}, {0xFB80, 0x0080, 0},
};
#define UMB_BLOCKS_COUNT (sizeof(umb_blocks) / sizeof(umb_t))

static int umb_blocks_allocated = 0;

uint8_t xms_handles = 0;

void reset_umb() {
    for (int i = 0; i < UMB_BLOCKS_COUNT; ++i) {
        umb_blocks[i].allocated_paragraphs = 0;
    }
    umb_blocks_allocated = 0;
    for (int i = 0; i <= XMS_HANDLES; ++i) {
        emb_handles[i].used = 0;
        emb_handles[i].locks = 0;
        emb_handles[i].base = emb_handles[i].size = 0;
    }
    xms_handles = 0;
}

int UMB_get_largest(dos_far_ptr driverAddress, UCOUNT *seg, UCOUNT *size)
{
    int res;
    u16 save_ax = CPU_AX;
    u16 save_bx = CPU_BX;
    u16 save_dx = CPU_DX;
    CPU_DX = 0xffff;
    CPU_AX = 0x1000;
    dpb_watch_check_chain("UMB_get_largest 1");
    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));
    if (CPU_BL != 0xb0 || CPU_DX == 0) {
        res = 0;
        goto ret;
    }
    CPU_AX = 0x1000;
    dpb_watch_check_chain("UMB_get_largest 2");
    /* DX оставляем равным largest size */
    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));
    if (CPU_AX != 1) {
        res = 0;
        goto ret;
    }
    *seg = CPU_BX;
    *size = CPU_DX;
    res = CPU_AX;
ret:
    dpb_watch_check_chain("UMB_get_largest 3");
    CPU_AX = save_ax;
    CPU_BX = save_bx;
    CPU_DX = save_dx;
    return res;
}

static inline void xms_move_to(register uint32_t destination, register uint32_t source, register uint32_t length) {
    dpb_watch_check_chain("xms_move_to 1");
    if (((destination | source) & 1u) == 0) {
        uint16_t *dest_ptr = (uint16_t *)xms_ptr(destination);
        while (length >= 2) {
            *dest_ptr++ = readw86(source);
            source += 2;
            length -= 2;
        }
        destination = (uint32_t)((uint8_t *)dest_ptr - xms_ptr(0));
    } else {
        uint8_t *dest_ptr = xms_ptr(destination);
        while (length && ((destination | source) & 1u)) {
            *dest_ptr++ = read86(source++);
            destination++;
            length--;
        }
        if (length >= 2) {
            uint16_t *dest16 = (uint16_t *)dest_ptr;
            while (length >= 2) {
                *dest16++ = readw86(source);
                source += 2;
                length -= 2;
            }
            dest_ptr = (uint8_t *)dest16;
        }
        if (length)
            *dest_ptr = read86(source);
        return;
    }
    if (length)
        *xms_ptr(destination) = read86(source);
    dpb_watch_check_chain("xms_move_to 2");
}
 
static inline void xms_move_from(register uint32_t source, register uint32_t destination, register uint32_t length) {
    dpb_watch_check_chain("xms_move_from 1");
    if (((source | destination) & 1u) == 0) {
        const uint16_t *source_ptr = (const uint16_t *)xms_ptr(source);
        while (length >= 2) {
            writew86(destination, *source_ptr++);
            destination += 2;
            length -= 2;
        }
        source = (uint32_t)((const uint8_t *)source_ptr - xms_ptr(0));
    } else {
        const uint8_t *source_ptr = xms_ptr(source);
        while (length && ((source | destination) & 1u)) {
            write86(destination++, *source_ptr++);
            source++;
            length--;
        }
        if (length >= 2) {
            const uint16_t *source16 = (const uint16_t *)source_ptr;
            while (length >= 2) {
                writew86(destination, *source16++);
                destination += 2;
                length -= 2;
            }
            source_ptr = (const uint8_t *)source16;
        }
        if (length)
            write86(destination, *source_ptr);
        return;
    }
    if (length)
        write86(destination, *xms_ptr(source));
    dpb_watch_check_chain("xms_move_from 2");
}

static inline void xms_move_mem_to_mem(uint32_t destination, uint32_t source, uint32_t length) {
    dpb_watch_check_chain("xms_move_mem_to_mem 1");
    memmove(X86_RAM_BASE + destination, X86_RAM_BASE + source, length);
    dpb_watch_check_chain("xms_move_mem_to_mem 2");
}

static inline void xms_move_xms_to_xms(uint32_t destination, uint32_t source, uint32_t length) {
    dpb_watch_check_chain("xms_move_xms_to_xms 1");
    memmove(xms_ptr(destination), xms_ptr(source), length);
    dpb_watch_check_chain("xms_move_xms_to_xms 2");
}

const umb_t *get_largest_free_umb_block(uint16_t *psz) {
    const umb_t *best = NULL;
    int best_length = 0;
    int i = 0;
    while (i < UMB_BLOCKS_COUNT) {
        if (0 == umb_blocks[i].allocated_paragraphs) {
            int j = i;
            int length = 0;
            while (j < UMB_BLOCKS_COUNT && umb_blocks[j].allocated_paragraphs == 0) {
                if (j > i) {
                    const uint16_t expected_segment = umb_blocks[j - 1].segment + umb_blocks[j - 1].size;
                    if (umb_blocks[j].segment != expected_segment)
                        break;
                }
                length += umb_blocks[j].size;
                j++;
            }
            if (length > best_length) {
                best = &umb_blocks[i];
                best_length = length;
            }
            i = j;
        } else {
            i++;
        }
    }
    *psz = best_length;
    return best;
}

umb_t *get_free_umb_block(const uint16_t size) {
    umb_t *best = NULL;
    int best_size = 0;
    int i = 0;
    while (i < UMB_BLOCKS_COUNT) {
        if (umb_blocks[i].allocated_paragraphs != 0) {
            i++;
            continue;
        }
        uint16_t total_size = 0;
        int j = i;
        while (j < UMB_BLOCKS_COUNT && umb_blocks[j].allocated_paragraphs == 0) {
            if (j > i) {
                const uint16_t expected_segment = umb_blocks[j - 1].segment + umb_blocks[j - 1].size;
                if (umb_blocks[j].segment != expected_segment)
                    break;
            }
            total_size += umb_blocks[j].size;
            j++;
        }
        if (total_size >= size) {
            if (best == NULL || total_size < best_size) {
                best = &umb_blocks[i];
                best_size = total_size;
            }
        }
        i = j; // skip tested block
    }
    return best;
}

static bool xms_handler(CPU* cpu, bios_callback_params_t* params) {
//    printf("xms_handler(%02xh)\n", CPU_AH);
    switch (CPU_AH) {
        case XMS_VERSION:
            // Get XMS Version
            CPU_AX = 0x0200; // We are himem 2.06
            CPU_BX = 0x0206; // driver version
            CPU_DX = 0x0001; // HMA Exist
            break;
        case REQUEST_HMA:
            // Request HMA
            // Stub: Implement HMA request functionality
            CPU_AX = 1; // Success
            break;
        case RELEASE_HMA:
            // Release HMA
            // Stub: Implement HMA release functionality
            CPU_AX = 1; // Success
            break;
        case GLOBAL_ENABLE_A20:
        case LOCAL_ENABLE_A20:
            // Local Enable A20
            CPU_AX = 1; // Success
            CPU_BL = 0;
            cpu_set_a20(cpu, 1);
            break;
        case GLOBAL_DISABLE_A20:
        case LOCAL_DISABLE_A20:
            // Local Disable A20
            CPU_AX = 1; // Success
            CPU_BL = 0;
            cpu_set_a20(cpu, 0);
            break;
        case QUERY_A20:
            // Query A20 (Function 07h):
            CPU_AX = cpu_get_a20(cpu); // Success
            break;
        case QUERY_EMB: { // 08h
            uint32_t largest, total;
            emb_free_stats(&largest, &total);
            CPU_AX = (uint16_t)(largest >> 10); /* largest free block, KB */
            CPU_DX = (uint16_t)(total >> 10);   /* total free EMB, KB */
            CPU_BL = (largest == 0) ? 0xA0 : 0;
            break;
        }
        case ALLOCATE_EMB: { // Allocate Extended Memory Block (Function 09h), DX = size in KB
            int hnd = 0;
            for (int i = 1; i <= XMS_HANDLES; ++i)
                if (!emb_handles[i].used) { hnd = i; break; }
            if (hnd == 0) {
                CPU_AX = 0;
                CPU_BL = 0xA1; /* out of handles */
                break;
            }
            const uint32_t size = (uint32_t)CPU_DX << 10;
            uint32_t base = 0;
            if (!emb_find_gap(size, &base)) {
                CPU_AX = 0;
                CPU_BL = 0xA0; /* out of memory */
                break;
            }
            emb_handles[hnd].used = 1;
            emb_handles[hnd].locks = 0;
            emb_handles[hnd].base = base;
            emb_handles[hnd].size = size;
            xms_handles++;
            CPU_DX = (uint16_t)hnd;
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case RELEASE_EMB: { // 0Ah, DX = handle
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2; /* invalid handle */
                break;
            }
            if (emb_handles[hnd].locks) {
                CPU_AX = 0;
                CPU_BL = 0xAB; /* block is locked */
                break;
            }
            emb_handles[hnd].used = 0;
            emb_handles[hnd].base = emb_handles[hnd].size = 0;
            if (xms_handles) xms_handles--;
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case MOVE_EMB: {
            // Move Extended Memory Block (Function 0Bh)
            move_data_t move_data;
            uint32_t struct_offset = ((uint32_t) CPU_DS << 4) + CPU_SI;
            uint16_t *move_data_ptr = (uint16_t *) &move_data;
            for (int i = sizeof(move_data_t) / 2; i--;) {
                *move_data_ptr++ = readw86(struct_offset++);
                struct_offset++;
            }
            
            /* Validate handles / offsets and rebase EMB offsets onto the
             * per-handle base inside the pool. Real-mode (handle==0) sides
             * are seg:off far pointers. */
            uint8_t err = 0;
            if (move_data.source_handle) {
                const uint16_t h = move_data.source_handle;
                if (h > XMS_HANDLES || !emb_handles[h].used)
                    err = 0xA3; /* invalid source handle */
                else if (move_data.source_offset + move_data.length > emb_handles[h].size ||
                         move_data.source_offset + move_data.length < move_data.source_offset)
                    err = 0xA4; /* invalid source offset */
                else
                    move_data.source_offset += emb_handles[h].base;
            } else {
                move_data.source_offset = to_physical_offset(move_data.source_offset);
            }
            if (!err && move_data.destination_handle) {
                const uint16_t h = move_data.destination_handle;
                if (h > XMS_HANDLES || !emb_handles[h].used)
                    err = 0xA5; /* invalid destination handle */
                else if (move_data.destination_offset + move_data.length > emb_handles[h].size ||
                         move_data.destination_offset + move_data.length < move_data.destination_offset)
                    err = 0xA6; /* invalid destination offset */
                else
                    move_data.destination_offset += emb_handles[h].base;
            } else if (!err) {
                move_data.destination_offset = to_physical_offset(move_data.destination_offset);
            }
            #if XMS_DEBUG
            printf("XMS MOVE: len=%lu sh=%04x so=%08lx dh=%04x do=%08lx phys_dst=%06lx%s\n",
                (unsigned long)move_data.length,
                move_data.source_handle,
                (unsigned long)move_data.source_offset,
                move_data.destination_handle,
                (unsigned long)move_data.destination_offset,
                (unsigned long)(move_data.destination_handle
                    ? XMS_EMB_BASE_PHYS + move_data.destination_offset
                    : move_data.destination_offset),
                err ? " REJECTED" : "");
            #endif
            if (err) {
                CPU_AX = 0;
                CPU_BL = err;
                break;
            }

            if (!move_data.source_handle && !move_data.destination_handle) {
                xms_move_mem_to_mem(move_data.destination_offset, move_data.source_offset, move_data.length);
            } else if (!move_data.source_handle) {
                xms_move_to(move_data.destination_offset, move_data.source_offset, move_data.length);
            } else if (!move_data.destination_handle) {
                xms_move_from(move_data.source_offset, move_data.destination_offset, move_data.length);
            } else {
                xms_move_xms_to_xms(move_data.destination_offset, move_data.source_offset, move_data.length);
            }
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case REQUEST_UMB: {
            // Request Upper Memory Block (Function 10h):
            if (CPU_DX == 0xFFFF) {
                /*
                 * Probe largest available block by requesting an
                 * impossibly large UMB. XMS returns B0h and the
                 * largest available size in DX.
                 */
                uint16_t sz = 0;
                const umb_t *umb_block = get_largest_free_umb_block(&sz);
                CPU_AX = 0;
                CPU_DX = sz;
                if (umb_block != NULL && sz != 0) {
                    CPU_BL = 0xB0;
                } else {
                    CPU_BL = 0xB1;
                }
                break;
            } else {
                const uint16_t requested_size = CPU_DX;
                umb_t *umb_block = get_free_umb_block(requested_size);
                if (umb_block != NULL) {
                    int unmarked_size = requested_size;
                    CPU_BX = umb_block->segment;
                    CPU_AX = 0x0001;
                    umb_t *ub = umb_block;
                    int total_allocated = 0;
                    while (unmarked_size > 0) {
                        total_allocated += umb_block->size;
                        umb_block->allocated_paragraphs = -1;
                        umb_blocks_allocated++;
                        unmarked_size -= umb_block->size;
                        umb_block++;
                    }
                    ub->allocated_paragraphs = total_allocated;
                    CPU_DX = total_allocated;
                    break;
                }
            }

            uint16_t sz = 0;
            get_largest_free_umb_block(&sz);
            CPU_AX = 0x0000;
            CPU_DX = sz;
            CPU_BL = umb_blocks_allocated >= UMB_BLOCKS_COUNT ? 0xB1 : 0xB0;
            break;
        }
        case RELEASE_UMB: {
            // Release Upper Memory Block (Function 11h)
            // Stub: Release Upper Memory Block
            for (int i = 0; i < UMB_BLOCKS_COUNT; ++i)
                if (umb_blocks[i].segment == CPU_BX && umb_blocks[i].allocated_paragraphs > 0) {
                    int par = umb_blocks[i].allocated_paragraphs;
                    while (par > 0 && i < UMB_BLOCKS_COUNT) {
                        umb_blocks[i].allocated_paragraphs = 0;
                        par -= umb_blocks[i++].size;
                        umb_blocks_allocated--;
                    }
                    CPU_AX = 0x0001; // Success
                    CPU_BL = 0;
                    return 0xCB; // Early return to avoid fall-through
                }

            CPU_AX = 0x0000; // Failure
            CPU_DX = 0x0000;
            CPU_BL = 0xB2; // Error code
            break;
        }
        case LOCK_EMB: { // 0Ch, DX = handle -> DX:BX = 32-bit physical address
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            if (emb_handles[hnd].locks == 0xFF) {
                CPU_AX = 0;
                CPU_BL = 0xAC; /* lock count overflow */
                break;
            }
            emb_handles[hnd].locks++;
            const uint32_t phys = XMS_EMB_BASE_PHYS + emb_handles[hnd].base;
            CPU_DX = (uint16_t)(phys >> 16);
            CPU_BX = (uint16_t)(phys & 0xFFFF);
            CPU_AX = 1;
            break;
        }
        case UNLOCK_EMB: { // 0Dh, DX = handle
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            if (emb_handles[hnd].locks == 0) {
                CPU_AX = 0;
                CPU_BL = 0xAA; /* block is not locked */
                break;
            }
            emb_handles[hnd].locks--;
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case EMB_HANDLE_INFO: { // 0Eh, DX = handle -> BH=locks, BL=free handles, DX=size KB
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            CPU_BH = emb_handles[hnd].locks;
            CPU_BL = (uint8_t)emb_free_handle_count();
            CPU_DX = (uint16_t)(emb_handles[hnd].size >> 10);
            CPU_AX = 1;
            break;
        }
        case REALLOCATE_EMB: { // 0Fh, BX = new size KB, DX = handle
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            if (emb_handles[hnd].locks) {
                CPU_AX = 0;
                CPU_BL = 0xAB; /* block is locked */
                break;
            }
            const uint32_t new_size = (uint32_t)CPU_BX << 10;
            emb_handle_t *h = &emb_handles[hnd];
            if (new_size <= h->size) {
                h->size = new_size;
                CPU_AX = 1;
                CPU_BL = 0;
                break;
            }
            /* try to grow in place: temporarily hide the handle and search */
            const uint32_t old_base = h->base, old_size = h->size;
            h->used = 0;
            uint32_t base = 0;
            bool ok = emb_find_gap(new_size, &base);
            h->used = 1;
            if (!ok) {
                CPU_AX = 0;
                CPU_BL = 0xA0; /* out of memory */
                break;
            }
            if (base != old_base && old_size)
                memmove(xms_ptr(base), xms_ptr(old_base), old_size);
            h->base = base;
            h->size = new_size;
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        default:
            // Unhandled function
            CPU_AX = 0x0000; // Function not supported
            CPU_BL = 0x80; // Function not implemented
            break;
    }

    SET_IP ( getmem8(CPU_SS, CPU_SP)
           | ((uint16_t)getmem8(CPU_SS, (CPU_SP + 1) & 0xffff) << 8) );
    SET_CS ( getmem8(CPU_SS, (CPU_SP + 2) & 0xffff)
           | ((uint16_t)getmem8(CPU_SS, (CPU_SP + 3) & 0xffff) << 8) );
    CPU_SP += 4;
//    printf("xms_handler RETF to %04x:%04x\n", CPU_CS, CPU_IP);
    return false;
}

bool fdos_2fh(CPU* cpu) {
    static bool handler_installed = false;
    static bios_callback_params_t params = {
        .callback = xms_handler,
        .expected_cs = 0xF000,
        .expected_ip = 0xFEFF,
        .done = false,
        .owner = "INT 2fH"
    };

    if (CPU_AX == 0xAE00) {
        /*
        * INT 2F/AX=AE00h - DOS 3.3+ internal
        * INSTALLABLE COMMAND - installation check.
        *
        * Return AL=FFh to report that the installable-command interface is
        * present.  The actual CONFIG.SYS INSTALL/INSTALLHIGH queue/execution
        * is already handled in config.c by CmdInstall()/CmdInstallHigh() and
        * DoInstall(); this function is only the multiplex installation probe.
        */
        CPU_AL = 0xFF;
    }
    else
    if (CPU_AX == 0xAE01) {
        /*
         * INT 2F/AX=AE01h - DOS 3.3+ internal
         * INSTALLABLE COMMAND - execute pending INSTALL=/INSTALLHIGH=
         * commands collected while parsing CONFIG.SYS.
         */
        DoInstall();
        CPU_AL = 0x00;
        cf = 0;
    }
    else
    if (CPU_AH == 0x14)
        return fdos_nls_2fh(cpu);
    else
    if (CPU_AX == 0x4300)
        CPU_AL = 0x80;
    else 
    if (CPU_AX == 0x4310) {
        if (!handler_installed) {
            set_bios_callback(cpu, &params, true);
            handler_installed = true;
        }
        SET_ES ( params.expected_cs );
        CPU_BX = params.expected_ip ;
    }
    else { 
        /// TODO:
         no_handler(cpu);
    }
    return true;
}
