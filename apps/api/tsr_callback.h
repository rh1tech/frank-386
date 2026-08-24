#ifndef __NATIVE_TSR_CALLBACK_H__
#define __NATIVE_TSR_CALLBACK_H__

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

static const unsigned long * const _tsr_sys_table_ptrs =
    (const unsigned long * const)DOS_OS_API_SYS_TABLE_BASE;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tsr_callback_t)(void);

/* TSR1 runs from the VGA scanline DMA IRQ on core1. Keep callbacks very
   short: blocking or lengthy work here will miss video deadlines and can
   cause sync loss or a blank display. */
inline static tsr_callback_t set_tsr0_callback(tsr_callback_t cb)
{
    typedef tsr_callback_t (*fn_ptr_t)(tsr_callback_t);
    return ((fn_ptr_t)_tsr_sys_table_ptrs[117])(cb);
}

inline static tsr_callback_t set_tsr1_callback(tsr_callback_t cb)
{
    typedef tsr_callback_t (*fn_ptr_t)(tsr_callback_t);
    return ((fn_ptr_t)_tsr_sys_table_ptrs[118])(cb);
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_TSR_CALLBACK_H__ */
