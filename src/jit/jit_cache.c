/*
 * murm386 JIT Compiler - Code Cache Management
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * MIT License
 */

#include "jit.h"
#include "../i386.h"
#include <string.h>
#include <stdlib.h>

#ifdef RP2350_BUILD
#include "pico/stdlib.h"
#include "hardware/sync.h"
#endif

/*
 * Code cache is allocated in RAM for execution.
 * On RP2350, we need to ensure the cache is in RAM (not flash).
 */

#ifdef RP2350_BUILD
/* Allocate code cache in RAM section */
static uint8_t __attribute__((section(".data")))
    jit_code_cache[JIT_CACHE_SIZE] __attribute__((aligned(4)));
#else
static uint8_t *jit_code_cache = NULL;
#endif

/*
 * Simple hash function for block lookup
 */
static inline uint32_t jit_hash(uint32_t addr)
{
    /* XOR folding hash */
    addr ^= (addr >> 16);
    addr ^= (addr >> 8);
    return addr & (JIT_HASH_SIZE - 1);
}

/*
 * Initialize JIT compiler
 */
jit_context_t *jit_init(CPUI386 *cpu)
{
    jit_context_t *jit = malloc(sizeof(jit_context_t));
    if (!jit) {
        return NULL;
    }

    memset(jit, 0, sizeof(jit_context_t));
    jit->cpu = cpu;

#ifdef RP2350_BUILD
    /* Use statically allocated cache in RAM */
    jit->cache_base = jit_code_cache;
#else
    /* Dynamically allocate cache for desktop testing */
    jit_code_cache = malloc(JIT_CACHE_SIZE);
    if (!jit_code_cache) {
        free(jit);
        return NULL;
    }
    jit->cache_base = jit_code_cache;
#endif

    jit->cache_ptr = jit->cache_base;
    jit->cache_end = jit->cache_base + JIT_CACHE_SIZE;

    /* Clear hash table */
    memset(jit->hash_table, 0, sizeof(jit->hash_table));

    return jit;
}

/*
 * Destroy JIT compiler
 */
void jit_destroy(jit_context_t *jit)
{
    if (!jit) return;

#ifndef RP2350_BUILD
    if (jit_code_cache) {
        free(jit_code_cache);
        jit_code_cache = NULL;
    }
#endif

    free(jit);
}

/*
 * Flush entire code cache
 */
void jit_flush_cache(jit_context_t *jit)
{
    if (!jit) return;

    /* Reset allocation pointer */
    jit->cache_ptr = jit->cache_base;

    /* Clear hash table */
    memset(jit->hash_table, 0, sizeof(jit->hash_table));

    jit->cache_flushes++;

#ifdef RP2350_BUILD
    /* Ensure instruction cache coherency */
    __dsb();
    __isb();
#endif
}

/*
 * Lookup compiled block by i386 address
 */
jit_block_t *jit_lookup_block(jit_context_t *jit, uint32_t addr)
{
    uint32_t hash = jit_hash(addr);
    jit_hash_entry_t *entry = &jit->hash_table[hash];

    /* Direct mapped - check if address matches */
    if (entry->i386_addr == addr && entry->block) {
        jit_block_t *block = entry->block;
        if (block->flags & JIT_BLOCK_FLAG_VALID) {
            return block;
        }
    }

    return NULL;
}

/*
 * Insert block into hash table
 */
void jit_hash_insert(jit_context_t *jit, jit_block_t *block)
{
    uint32_t hash = jit_hash(block->i386_addr);
    jit_hash_entry_t *entry = &jit->hash_table[hash];

    /* Direct mapped - just overwrite */
    entry->i386_addr = block->i386_addr;
    entry->block = block;
}

/*
 * Remove block from hash table
 */
void jit_hash_remove(jit_context_t *jit, uint32_t addr)
{
    uint32_t hash = jit_hash(addr);
    jit_hash_entry_t *entry = &jit->hash_table[hash];

    if (entry->i386_addr == addr) {
        entry->i386_addr = 0;
        entry->block = NULL;
    }
}

/*
 * Invalidate blocks in address range
 * Called when memory is written to detect self-modifying code
 */
void jit_invalidate_range(jit_context_t *jit, uint32_t start, uint32_t len)
{
    /* Simple approach: scan hash table for overlapping blocks */
    uint32_t end = start + len;

    for (int i = 0; i < JIT_HASH_SIZE; i++) {
        jit_hash_entry_t *entry = &jit->hash_table[i];
        if (entry->block && (entry->block->flags & JIT_BLOCK_FLAG_VALID)) {
            jit_block_t *block = entry->block;
            uint32_t block_end = block->i386_addr + block->i386_len;

            /* Check for overlap */
            if (block->i386_addr < end && block_end > start) {
                /* Invalidate this block */
                block->flags &= ~JIT_BLOCK_FLAG_VALID;
                entry->i386_addr = 0;
                entry->block = NULL;
            }
        }
    }
}

/*
 * Allocate space in code cache for a new block
 */
static jit_block_t *jit_alloc_block(jit_context_t *jit, int estimated_size)
{
    int total_size = sizeof(jit_block_t) + estimated_size;

    /* Align to 4 bytes */
    total_size = (total_size + 3) & ~3;

    /* Check if we need to flush the cache */
    if (jit->cache_ptr + total_size > jit->cache_end) {
        jit_flush_cache(jit);

        /* If still not enough space, the block is too large */
        if (jit->cache_ptr + total_size > jit->cache_end) {
            return NULL;
        }
    }

    jit_block_t *block = (jit_block_t *)jit->cache_ptr;
    memset(block, 0, sizeof(jit_block_t));

    /* Advance cache pointer past header, ARM code will follow */
    jit->cache_ptr += sizeof(jit_block_t);

    return block;
}

/*
 * Finalize block after compilation
 */
static void jit_finalize_block(jit_context_t *jit, jit_block_t *block)
{
    /* Calculate ARM code size */
    uint8_t *code_start = (uint8_t *)(block + 1);
    block->arm_len = jit->emit_ptr - code_start;

    /* Advance cache pointer to end of ARM code (aligned) */
    jit->cache_ptr = (uint8_t *)(((uintptr_t)jit->emit_ptr + 3) & ~3);

    /* Mark as valid and insert into hash table */
    block->flags |= JIT_BLOCK_FLAG_VALID;
    jit_hash_insert(jit, block);

    jit->blocks_compiled++;

#ifdef RP2350_BUILD
    /* Ensure instruction cache coherency */
    __dsb();
    __isb();
#endif
}

/*
 * Compile a new block starting at i386 address
 * This is the main compilation entry point
 */
jit_block_t *jit_compile_block(jit_context_t *jit, uint32_t addr)
{
    CPUI386 *cpu = jit->cpu;

    /* Allocate block in cache */
    jit_block_t *block = jit_alloc_block(jit, JIT_BLOCK_MAX_SIZE);
    if (!block) {
        return NULL;
    }

    /* Initialize block header */
    block->i386_addr = addr;
    block->i386_cs_base = cpu->seg[1].base; /* CS segment base */

    /* Set up compilation state */
    jit->current_block = block;
    jit->emit_ptr = (uint8_t *)(block + 1);  /* Code follows header */
    jit->block_start_addr = addr;
    jit->insn_count = 0;
    jit->block_terminated = false;

    /* Emit prologue */
    jit_emit_prologue(jit);

    /* Translate i386 instructions until block terminator or limit */
    uint32_t current_addr = addr;
    int total_i386_bytes = 0;

    while (!jit->block_terminated && jit->insn_count < JIT_MAX_BLOCK_INSNS) {
        /* Read i386 instruction bytes from memory */
        uint8_t insn_buf[16];
        uint32_t phys_addr = current_addr; /* TODO: proper address translation */

        /* Read up to 16 bytes for instruction decoding */
        for (int i = 0; i < 16 && (phys_addr + i) < (1 << 24); i++) {
            insn_buf[i] = cpu->phys_mem[phys_addr + i];
        }

        /* Translate instruction */
        int consumed = jit_translate_insn(jit, insn_buf, 16);

        if (consumed < 0) {
            /* Unsupported instruction - emit fallback and terminate */
            jit_emit_epilogue(jit, JIT_EXIT_FALLBACK);
            jit->block_terminated = true;
            break;
        }

        current_addr += consumed;
        total_i386_bytes += consumed;
        jit->insn_count++;

        /* Check if we're exceeding block size limit */
        uint8_t *code_start = (uint8_t *)(block + 1);
        if (jit->emit_ptr - code_start > JIT_BLOCK_MAX_SIZE - 64) {
            /* Running out of space, terminate block */
            jit_emit_epilogue(jit, JIT_EXIT_NORMAL);
            jit->block_terminated = true;
        }
    }

    /* If block wasn't terminated by control flow, emit normal exit */
    if (!jit->block_terminated) {
        jit_emit_epilogue(jit, JIT_EXIT_NORMAL);
    }

    block->i386_len = total_i386_bytes;

    /* Finalize the block */
    jit_finalize_block(jit, block);

    return block;
}

/*
 * Get JIT statistics
 */
void jit_get_stats(jit_context_t *jit, uint32_t *compiled, uint32_t *executed,
                   uint32_t *flushes, uint32_t *fallbacks)
{
    if (compiled) *compiled = jit->blocks_compiled;
    if (executed) *executed = jit->blocks_executed;
    if (flushes) *flushes = jit->cache_flushes;
    if (fallbacks) *fallbacks = jit->fallback_count;
}

/*
 * Check if JIT is enabled
 */
bool jit_is_enabled(jit_context_t *jit)
{
    return jit != NULL && jit->cache_base != NULL;
}

/*
 * Main JIT execution entry point
 * Looks up or compiles a block, then executes it
 */
int jit_execute(jit_context_t *jit)
{
    if (!jit || !jit->cpu) {
        return -1;
    }

    CPUI386 *cpu = jit->cpu;

    /* Calculate linear address of current IP */
    uint32_t linear_addr = cpu->seg[1].base + cpu->next_ip; /* CS:IP */

    /* Look up existing compiled block */
    jit_block_t *block = jit_lookup_block(jit, linear_addr);

    if (!block) {
        /* Compile new block */
        block = jit_compile_block(jit, linear_addr);
        if (!block) {
            /* Compilation failed, fall back to interpreter */
            jit->fallback_count++;
            return -1;
        }
    }

    /* Execute the compiled block */
    block->exec_count++;
    jit->blocks_executed++;

    /* Get pointer to generated ARM code */
    uint8_t *arm_code = (uint8_t *)(block + 1);

    /*
     * Call the generated code
     * The generated code expects:
     *   r0 = pointer to CPUI386 struct (moved to r12 in prologue)
     *   r1 = pointer to physical memory base (moved to r3 in prologue)
     * And returns:
     *   r0 = exit reason
     */
#ifdef RP2350_BUILD
    typedef int (*jit_block_func_t)(CPUI386 *cpu, uint8_t *phys_mem);
    jit_block_func_t func = (jit_block_func_t)((uintptr_t)arm_code | 1); /* Thumb bit */

    int exit_reason = func(cpu, cpu->phys_mem);

    /* Handle exit reason */
    switch (exit_reason) {
    case JIT_EXIT_NORMAL:
        /* Block completed normally, IP already updated */
        break;

    case JIT_EXIT_FALLBACK:
        /* Need interpreter for this instruction */
        jit->fallback_count++;
        return -1;

    case JIT_EXIT_EXCEPTION:
        /* CPU exception occurred */
        return -1;

    default:
        break;
    }

    return jit->insn_count; /* Return instructions executed */
#else
    /* On non-RP2350, we can't execute ARM code */
    jit->fallback_count++;
    return -1;
#endif
}
