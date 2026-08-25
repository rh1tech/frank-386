/****************************************************************/
/*                                                              */
/*                           portab.h                           */
/*                                                              */
/*                 DOS-C portability typedefs, etc.             */
/*                                                              */
/*                         May 1, 1995                          */
/*                                                              */
/*                      Copyright (c) 1995                      */
/*                      Pasquale J. Villani                     */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/****************************************************************/

#ifdef MAIN
#ifdef VERSION_STRINGS
static char *portab_hRcsId =
    "$Id: portab.h 1121 2005-03-15 15:25:08Z perditionc $";
#endif
#endif

/****************************************************************/
/*                                                              */
/* Machine dependant portable types. Note that this section is  */
/* used primarily for segmented architectures. Common types and */
/* types used relating to segmented operations are found here.  */
/*                                                              */
/* Be aware that segmented architectures impose on linear       */
/* architectures because they require special types to be used  */
/* throught the code that must be reduced to empty preprocessor */
/* replacements in the linear machine.                          */
/*                                                              */
/* #ifdef <segmeted machine>                                    */
/* # define FAR far                                             */
/* # define NEAR near                                           */
/* #endif                                                       */
/*                                                              */
/* #ifdef <linear machine>                                      */
/* # define FAR                                                 */
/* # define NEAR                                                */
/* #endif                                                       */
/*                                                              */
/****************************************************************/

                                                        /* commandline overflow - removing -DI86 TE */
#if defined(__TURBOC__)

#define I86
#define CDECL   cdecl
#if __TURBOC__ > 0x202
/* printf callers do the right thing for tc++ 1.01 but not tc 2.01 */
#define VA_CDECL
#else
#define VA_CDECL cdecl
#endif
#define PASCAL  pascal
void __int__(int);
#ifndef FORSYS
void __emit__(char, ...);
#define disable() __emit__(0xfa)
#define enable() __emit__(0xfb)
#endif

#elif defined(_MSC_VER)

#define I86
#define asm __asm
#pragma warning(disable: 4761) /* "integral size mismatch in argument;
                                   conversion supplied" */
#define CDECL   _cdecl
#define VA_CDECL
#define PASCAL  pascal
#define __int__(intno) asm int intno;
#define disable() asm cli
#define enable() asm sti
#define _CS getCS()
static unsigned short __inline getCS(void)
{
  asm mov ax, cs;
}
#define _SS getSS()
static unsigned short __inline getSS(void)
{
  asm mov ax, ss;
}

#elif defined(__WATCOMC__)      /* don't know a better way */

#if defined(_M_I86)

#define I86
#define __int__(intno) asm int intno;
void disable(void);
#pragma aux disable = "cli" __modify __exact [];
void enable(void);
#pragma aux enable = "sti" __modify __exact [];
#define asm __asm
#define far __far
#define CDECL   __cdecl
#define VA_CDECL
#define PASCAL  pascal
#define _CS getCS()
unsigned short getCS(void);
#pragma aux getCS = "mov dx,cs" __value [__dx] __modify __exact[__dx];
#define _SS getSS()
unsigned short getSS(void);
#pragma aux getSS = "mov dx,ss" __value [__dx] __modify __exact[__dx];
#if !defined(FORSYS) && !defined(EXEFLAT) && _M_IX86 >= 300
#pragma aux __default __parm [__ax __dx __cx] __modify [__ax __dx __es __fs] /* min.unpacked size */
#endif

/* enable Possible loss of precision warning for compatibility with Borland */
#pragma enable_message(130)

#else

/* workaround for building some utils with OpenWatcom (flat model) */
#define MC68K

#endif

#elif defined (_MYMC68K_COMILER_)

#define MC68K

#elif defined(__GNUC__)

#ifdef __FAR
#define I86
#define STRINGIFY(x) #x
#define __int__(intno) asm volatile(STRINGIFY(int $##intno))
static inline void disable(void)
{
  asm volatile("cli");
}
static inline void enable(void)
{
  asm volatile("sti");
}
#define far __far
#define CDECL __attribute__((cdecl))
#define VA_CDECL
#define PASCAL

#define _CS getCS()
static inline unsigned short getCS(void)
{
  unsigned short ret;
  asm volatile("mov %%cs, %0" : "=r"(ret));
  return ret;
}

#define _SS getSS()
static inline unsigned short getSS(void)
{
  unsigned short ret;
  asm volatile("mov %%ss, %0" : "=r"(ret));
  return ret;
}
extern char DosDataSeg[];
#else
  #ifdef PICO_RP2350
    #define ARM_M33
  #else
    /* for warnings only ! */
    #define MC68K
  #endif
#endif

#ifdef ARM_M33

#ifndef PSRAM_BASE_ADDR
#define PSRAM_BASE_ADDR   0x11000000
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
extern uint8_t *guest_ram_base;
#define X86_RAM_BASE (guest_ram_base)
#include "ega128_paging.h"
#else
#define X86_RAM_BASE ((uint8_t *)PSRAM_BASE_ADDR)
#endif

/* Shared DOS canonical-path buffer size.  Keep truename() and callers that
   retain its result on the same bound instead of duplicating the numeric
   value in individual translation units. */
#define FDOS_PATHLEN 128

/* Highest linear address any real-mode seg:off pair can name: FFFF:FFFF
   -> (0xFFFF << 4) + 0xFFFF == 0x10FFEF. Everything from 0x100000 up to
   here is the HMA and IS reachable (as FFFF:xxxx). */
#define X86_MAX_LINEAR 0x10FFEFul

/* Is p a native pointer to somewhere the guest can actually address?

   The bound is X86_MAX_LINEAR, not the nominal 1MB+64KB of the HMA
   aperture. The final 16 bytes of that aperture (0x10FFF0..0x10FFFF) are
   nameable by no seg:off pair at all, and linear_to_far() cannot encode
   them: its HMA branch would compute an offset above 0xFFFF and the
   (uint16_t) cast would silently truncate it, wrapping the pointer back
   to the START of the HMA. Cutting the window at exactly the set of
   representable addresses makes ARM_PTR() and linear_to_far() true
   inverses over the whole range, instead of almost-inverses with a
   16-byte hole that fails silently. */
static inline bool is_guest_ptr(const void *p) {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        uint32_t linear;
        return ega128_cache_ptr_to_linear(p, &linear) &&
               linear <= X86_MAX_LINEAR;
    }
#endif
    uintptr_t a = (uintptr_t)p;
    return a >= (uintptr_t)X86_RAM_BASE &&
           a <= (uintptr_t)X86_RAM_BASE + X86_MAX_LINEAR;
}

static inline uint32_t fdos_arm_linear(const void *p)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        uint32_t linear;
        if (ega128_cache_ptr_to_linear(p, &linear))
            return linear;
    }
#endif
    return (uint32_t)((uintptr_t)p - (uintptr_t)X86_RAM_BASE);
}

/*
    ================================================================
    GUEST POINTER MODEL - read this before touching anything below
    ================================================================

    There are exactly TWO kinds of pointer in this port, and they must
    never be confused:

      dos_far_ptr   a GUEST pointer: a { offset, segment } pair, exactly
                    as an x86 program sees it. This is what every field
                    that a guest can read or write must be declared as
                    (device chains, DPB/CDS/SFT/PSP links, the LoL, the
                    SDA). It is a struct - not an integer, not a native
                    pointer - so that it cannot be silently mixed with
                    either.

      T *           a NATIVE ARM pointer, obtained from a dos_far_ptr by
                    ARM_PTR(). Valid only for the duration of the call
                    that produced it. Note that FAR/far expand to nothing
                    here, so "struct dpb FAR *" is a NATIVE pointer -
                    despite how it reads.

    Direction of travel is one-way: ARM_PTR() converts guest -> native.
    Going the other way is NOT generally possible: a native pointer has
    no unique seg:off pre-image (only a unique linear address), so
    reconstructing one silently picks a *different* seg:off pair with the
    same linear address. Where a guest pointer is needed, derive it from
    another guest address (ADD_OFF(base, delta), MK_FP(psp_seg, ...), ...)
    - never by normalising a native address.

    SENTINELS. Guest pointers carry two of them, and after ARM_PTR()
    NEITHER looks special any more - both become ordinary, *mapped*,
    dereferenceable ARM addresses:

        0000:0000  "none"      ARM_PTR() -> X86_RAM_BASE + 0, i.e. the
                               guest's INTERRUPT VECTOR TABLE. A missing
                               NULL check does not fault here - it
                               silently corrupts the guest's IVT.
        FFFF:FFFF  "end/error" ARM_PTR() -> X86_RAM_BASE + 0x10FFEF,
                               i.e. somewhere inside the HMA.

    Therefore: ALWAYS test a dos_far_ptr with far_is_null() / far_is_end()
    (init-mod.h) BEFORE handing it to ARM_PTR(). Never test the result of
    ARM_PTR() against NULL - it is never NULL.

    DECLARATION RULE. Every dos_far_ptr - field, parameter, local or
    return value - states what it points AT, because the type itself no
    longer says:

        dos_far_ptr / * -> struct dpb * /  dpb_next;

    The one exception is arrays of bytes/characters (char[], UBYTE[]):
    they are indivisible primitives of known size and the comment would
    add nothing.
*/
#pragma pack(push, 1)
typedef struct dos_far_ptr {
    uint16_t offset;
    uint16_t segment;
} dos_far_ptr;
typedef int16_t dos_short_ptr;
#pragma pack(pop)
typedef char dos_far_ptr_size_check[ // like static assert
    sizeof(dos_far_ptr) == 4 ? 1 : -1
];
#define VOID void
#define CDECL
#define PASCAL
#define FAR
#define far
#define REG
#define GLOBAL extern
#define ASM
#define WIN31SUPPORT 1
#define BSS_INIT(x) = x
#define MK_FP(seg, off) ((dos_far_ptr){ .offset = (uint16_t)(off), .segment = (uint16_t)(seg) })
#define FP_SEG(fp)             ((fp).segment)
#define FP_OFF(fp)             ((fp).offset)
#define ADD_OFF(p, n) MK_FP(FP_SEG(p), FP_OFF(p) + (n))

/*
   adjust_far_x86 / add_far_x86 - the NORMALISING far-pointer advance.

   ADD_OFF() above only adds to the offset and keeps the segment, so once
   the offset passes 0xFFFF it wraps back to the START of the same 64K
   segment. That is correct for real-mode SP/PUSH wrap, but WRONG for
   walking a linear buffer across a segment boundary: the caller ends up
   pointing 64K low, corrupting whatever lives at the wrapped offset.

   add_far_x86() carries the overflow into the segment instead
   (seg += off>>4; off &= 0xF), matching upstream adjust_far() in
   memmgr.c. Use it for buffer walks (disk transfers, FAT streaming);
   use ADD_OFF()/stk_lin() only where 16-bit wrap is actually intended. */
static inline dos_far_ptr adjust_far_x86(dos_far_ptr p) {
    if (FP_SEG(p) == 0xffff)   /* HMA selector: leave as-is, like upstream */
        return p;
    return MK_FP((uint16_t)(FP_SEG(p) + (FP_OFF(p) >> 4)),
                 (uint16_t)(FP_OFF(p) & 0x000f));
}
static inline dos_far_ptr add_far_x86(dos_far_ptr p, uint32_t n) {
    /* n can be up to 0x10000 (a full 64K transfer); do the add in 32 bits
       before normalising so it cannot itself truncate. */
    uint32_t off = (uint32_t)FP_OFF(p) + n;
    return adjust_far_x86(MK_FP(
        (uint16_t)(FP_SEG(p) + (uint16_t)(off >> 4)),
        (uint16_t)(off & 0x000f)));
}
///#define DHDR_END ((void*)(uintptr_t)-1)
#define EFFECTIVE(a) (((uint32_t)(a).segment << 4) + (a).offset)
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
/*
 * In the pageable build a DOS far pointer still denotes a GUEST address, not
 * a host pointer.  Resolve it only when C code actually asks for direct
 * access.  The returned pointer addresses only the currently mapped 2-KiB
 * chunk and must never be retained across another guest-memory access: EGA128
 * deliberately has no pinned pages.  New DOS code must keep guest addresses
 * and use guest_ref/accessors.  ARM_PTR is a compatibility escape hatch; in
 * the active tree its only intentional runtime use is the QSPI-only native
 * ARM loader, after the physical-PSRAM guard has succeeded.
 * Mark the page dirty because C gives us no way to infer access direction
 * from a later -> or * operator.
 */
static inline uint8_t *fdos_guest_arm_ptr(dos_far_ptr p_x86)
{
    uint32_t linear = EFFECTIVE(p_x86);
    if (__builtin_expect(guest_ram_base != ram_pages, 1))
        return (uint8_t *)PSRAM_BASE_ADDR + linear;
    return ega128_guest_ptr(linear, true);
}
#define ARM_PTR(p_x86) fdos_guest_arm_ptr((p_x86))
#else
#define ARM_PTR(p_x86) ( X86_RAM_BASE + EFFECTIVE(p_x86) )
#endif
// N.B. use ARM_PTR only for addresses stored in x86 RAM; M33 SRAM/FLASH is not mapped there.

/*
    Two documentary aliases of dos_far_ptr. They do NOT change layout or add
    type checking (C typedefs are transparent) - their whole job is to make a
    signature state, at a glance, WHICH kind of pointer it expects, because the
    bits alone cannot tell you:

      a packed native address (NATIVE_PTR below) and a guest seg:off are
      AMBIGUOUS BY VALUE. A native ARM address like 0x11xxxxxx packs to a
      "segment" of 0x11xx, which is also a perfectly legal guest segment, so no
      runtime test can distinguish them. The distinction is carried by context
      (e.g. dhdr's ATTR_NATIVE flag), never by the value.

      native_ptr  - carries a PACKED NATIVE pointer (high16:low16 of a 32-bit
                    ARM address). Dereference with NATIVE_ARM_PTR(), NEVER
                    ARM_PTR(). A function taking native_ptr must not be handed
                    a guest pointer.
      mixed_ptr   - may carry EITHER a guest seg:off or a packed native
                    pointer; the holder must consult a discriminator before
                    dereferencing (today only dhdr.dh_next, gated on
                    ATTR_NATIVE). Never dereference a mixed_ptr blindly.

    Plain dos_far_ptr continues to mean a genuine guest seg:off.
*/
typedef dos_far_ptr native_ptr;
typedef dos_far_ptr mixed_ptr;

/* Pack / unpack a native ARM pointer inside a (native_ptr) dos_far_ptr as
   high16:low16. This is NOT seg:off arithmetic - there is no <<4 - so it can
   represent any 32-bit native address exactly. Use ONLY on native_ptr / the
   native arm of a mixed_ptr. */
#define NATIVE_PTR(arm_addr) \
    MK_FP((uint16_t)(((uintptr_t)(arm_addr) >> 16) & 0xFFFF), \
          (uint16_t)((uintptr_t)(arm_addr) & 0xFFFF))
#define NATIVE_ARM_PTR(np) \
    ((void *)(uintptr_t)(((uint32_t)FP_SEG(np) << 16) | FP_OFF(np)))

#define FP_DS_DX (MK_FP(CPU_DS, CPU_DX))
#define FP_ES_DI (MK_FP(CPU_ES, CPU_DI))

/* Legacy source-compatibility helpers.  They expose a transient host pointer
 * and therefore must not be introduced into pageable runtime code; use
 * pload/pstore or a guest_ref there. */
#define peekb(seg, ofs) (*((unsigned char far *)ARM_PTR(MK_FP(seg,ofs))))
#define peekw(seg, ofs) (*((u16*)ARM_PTR(MK_FP(seg,ofs))))
#define x86_para2far(seg) (MK_FP((seg), 0))
#define para2far(seg) ((mcb*)ARM_PTR(MK_FP((seg), 0)))

#else
#define DHDR_END 0xFFFF
#endif

#else
#error Unknown compiler
We might even deal with a pre-ANSI compiler. This will certainly not compile.
#endif

#ifdef I86
#if _M_IX86 >= 300 || defined(M_I386)
#ifndef I386
#define I386
#endif
#elif _M_IX86 >= 100 || defined(M_I286)
#ifndef I186
#define I186
#endif
#endif
#endif

#ifdef MC68K
#define far                     /* No far type          */
#define interrupt               /* No interrupt type    */
#define VOID           void
#define FAR                     /* linear architecture  */
#define NEAR                    /*    "        "        */
#define INRPT          interrupt
#define REG
#define API            int      /* linear architecture  */
#define NONNATIVE
#define PARASIZE       4096     /* "paragraph" size     */
#define CDECL
#define PASCAL
#ifdef __GNUC__
#define CONST          const
#define PROTO
typedef __SIZE_TYPE__  size_t;
#else
#define CONST
#if !(defined(_SIZE_T) || defined(_SIZE_T_DEFINED) || defined(__SIZE_T_DEFINED))
typedef unsigned       size_t;
#endif
#endif
#endif
#if defined(I86) && !defined(MC68K)
#define VOID           void
#define FAR            far      /* segment architecture */
#define NEAR           near     /*    "          "      */
#define INRPT          interrupt
#define CONST          const
#define REG
#define API            int far pascal   /* segment architecture */
#define NATIVE
#define PARASIZE       16       /* "paragraph" size     */
typedef unsigned       size_t;
#endif
           /* functions, that are shared between C and ASM _must_ 
              have a certain calling standard. These are declared
              as 'ASMCFUNC', and is (and will be ?-) cdecl */
#define ASMCFUNC CDECL
#define ASMPASCAL PASCAL
#if defined(__GNUC__)
#define ASM
#else
#define ASM ASMCFUNC
#endif

/* variables that can be near or far: redefined in init-dat.h */
#define DOSFAR
#define DOSTEXTFAR

/*                                                              */
/* Boolean type & definitions of TRUE and FALSE boolean values  */
/*                                                              */
typedef int BOOL;
#define FALSE           (1==0)
#define TRUE            (1==1)

/*                                                              */
/* Common pointer types                                         */
/*                                                              */
#ifndef NULL
#define NULL            0
#endif

/*                                                              */
/* Convienence defines                                          */
/*                                                              */
#define FOREVER         while(TRUE)
#ifndef max
#define max(a,b)       (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b)       (((a) < (b)) ? (a) : (b))
#endif

/*                                                              */
/* Common byte, 16 bit and 32 bit types                         */
/*                                                              */
typedef char BYTE;
typedef short WORD;
typedef long DWORD;

typedef unsigned char UBYTE;
typedef unsigned short UWORD;
typedef unsigned long UDWORD;

typedef short SHORT;

typedef unsigned int BITS;      /* for use in bit fields(!)     */

typedef int COUNT;
typedef unsigned int UCOUNT;
typedef unsigned long ULONG;

#ifdef WITHFAT32
typedef unsigned long CLUSTER;
#else
typedef unsigned short CLUSTER;
#endif
typedef unsigned short UNICODE;

#if defined(STATICS) || defined(__WATCOMC__) || defined(__GNUC__)
#define STATIC static		 /* local calls inside module */
#else
#define STATIC
#endif

#ifdef UNIX
typedef char FAR *ADDRESS;
#else
typedef void FAR *ADDRESS;
#endif

#ifdef STRICT
typedef signed long LONG;
#else
#define LONG long
#endif

#if USHRT_MAX == 0xFFFF
# define loword(v) ((unsigned short)(v))
#else
# define loword(v) (0xFFFF & (unsigned)(v))
#endif
#define hiword(v) loword ((v) >> 16u)

#define MK_UWORD(hib,lob) (((UWORD)(hib) <<  8u) | (UBYTE)(lob))
#define MK_ULONG(hiw,low) (((ULONG)(hiw) << 16u) | (UWORD)(low))

/* General far pointer macros                                           */
#ifdef I86
#ifndef MK_FP

#if defined(__WATCOMC__)
#define MK_FP(seg,ofs) 	      (((UWORD)(seg)):>((VOID *)(ofs)))
#elif defined(__TURBOC__) && (__TURBOC__ > 0x202)
#define MK_FP(seg,ofs)        ((void _seg *)(seg) + (void near *)(ofs))
#else
#define MK_FP(seg,ofs)        ((void FAR *)(((ULONG)(seg)<<16)|(UWORD)(ofs)))
#endif

#define pokeb(seg, ofs, b) (*((unsigned char far *)MK_FP(seg,ofs)) = b)
#define poke(seg, ofs, w) (*((unsigned far *)MK_FP(seg,ofs)) = w)
#define pokew poke
#define pokel(seg, ofs, l) (*((unsigned long far *)MK_FP(seg,ofs)) = l)
#define peekb(seg, ofs) (*((unsigned char far *)MK_FP(seg,ofs)))
#define peek(seg, ofs) (*((unsigned far *)MK_FP(seg,ofs)))
#define peekw peek
#define peekl(seg, ofs) (*((unsigned long far *)MK_FP(seg,ofs)))

#if defined(__TURBOC__) && (__TURBOC__ > 0x202)
#define FP_SEG(fp)            ((unsigned)(void _seg *)(void far *)(fp))
#else
#define FP_SEG(fp)            ((unsigned)((ULONG)(VOID FAR *)(fp)>>16))
#endif

#if defined(__GNUC__) && defined(__BUILTIN_IA16_FP_OFF)
#define FP_OFF(fp)            __builtin_ia16_FP_OFF(fp)
#else
#define FP_OFF(fp)            ((unsigned)(fp))
#endif

#endif
#endif

#ifdef MC68K
#define MK_FP(seg,ofs)         ((VOID *)(&(((BYTE *)(size_t)(seg))[(ofs)])))
#define FP_SEG(fp)             (0)
#define FP_OFF(fp)             ((size_t)(fp))
#endif

#if defined(__GNUC__) && defined(__FAR)
typedef VOID FAR *intvec;
#elif defined(ARM_M33)
typedef dos_far_ptr intvec;
#else
typedef VOID (FAR ASMCFUNC * intvec) (void);
#endif

#define MK_PTR(type,seg,ofs) ((type FAR*) MK_FP (seg, ofs))
#if __TURBOC__ > 0x202
# define MK_SEG_PTR(type,seg) ((type _seg*) (seg))
#else
# define _seg FAR
# define MK_SEG_PTR(type,seg) MK_PTR (type, seg, 0)
#endif

/*
	this suppresses the warning
	unreferenced parameter 'x'
	and (hopefully) generates no code
*/
#define UNREFERENCED_PARAMETER(x) (void)x;

/*
    KEEP_UNUSED - a function that is deliberately kept although nothing calls
    it yet: a finished helper whose feature has not been wired up.

    Marking it is not the same as deleting it. -Wunused-function (enabled for
    src/fdos) is valuable precisely because it finds code that fell out of the
    call graph by accident - SetverGetVersion() was exactly that, and the fix
    was to CALL it, not to silence it. So use KEEP_UNUSED only where the code
    is complete and intentionally parked, and say what it is waiting for.
    Anything else should either be wired up or removed.
*/
#define KEEP_UNUSED __attribute__((unused))

#ifdef I86                      /* commandline overflow - removing /DPROTO TE */
#define PROTO
#endif

#define LENGTH(x) (sizeof(x)/sizeof(x[0]))
