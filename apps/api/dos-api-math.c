/*
 * Complete native-DOS math/compiler-runtime layer.
 *
 * Most wrappers intentionally follow the Murmulator OS 2 approach: a normal
 * typed C call through the public system table.  This is simpler, smaller and
 * lets the compiler generate the correct Thumb-1 call sequence.
 *
 * Only the four EABI divmod helpers are special: they return quotient and
 * remainder simultaneously in r0/r1.  Those four tiny tail shims live in
 * dos-api-divmod.S so no ordinary C wrapper can accidentally destroy r1.
 */

#include <stdint.h>
#include "dos_math_api.h"
#include "math.h"

#define DOS_API_TABLE_BASE 0x10100000u

static const unsigned long *const dos_math_table =
    (const unsigned long *const)DOS_API_TABLE_BASE;

#define WRAP1(ret, name, slot, t1)                   \
    ret name(t1 a)                                   \
    {                                                 \
        typedef ret (*fn_t)(t1);                     \
        return ((fn_t)dos_math_table[slot])(a);      \
    }

#define WRAP2(ret, name, slot, t1, t2)               \
    ret name(t1 a, t2 b)                             \
    {                                                 \
        typedef ret (*fn_t)(t1, t2);                 \
        return ((fn_t)dos_math_table[slot])(a, b);   \
    }

#define WRAP4(ret, name, slot, t1, t2, t3, t4)           \
    ret name(t1 a, t2 b, t3 c, t4 d)                    \
    {                                                     \
        typedef ret (*fn_t)(t1, t2, t3, t4);            \
        return ((fn_t)dos_math_table[slot])(a, b, c, d); \
    }

/* Single-precision arithmetic, comparisons and conversions. */
WRAP2(float, __aeabi_fadd,   DOS_API_MATH_AEABI_FADD,   float, float)
WRAP2(float, __aeabi_fsub,   DOS_API_MATH_AEABI_FSUB,   float, float)
WRAP2(float, __aeabi_fmul,   DOS_API_MATH_AEABI_FMUL,   float, float)
WRAP2(float, __aeabi_fdiv,   DOS_API_MATH_AEABI_FDIV,   float, float)
WRAP1(float, __aeabi_fneg,   DOS_API_MATH_AEABI_FNEG,   float)

WRAP2(int, __aeabi_fcmpeq, DOS_API_MATH_AEABI_FCMPEQ, float, float)
WRAP2(int, __aeabi_fcmpge, DOS_API_MATH_AEABI_FCMPGE, float, float)
WRAP2(int, __aeabi_fcmpgt, DOS_API_MATH_AEABI_FCMPGT, float, float)
WRAP2(int, __aeabi_fcmple, DOS_API_MATH_AEABI_FCMPLE, float, float)
WRAP2(int, __aeabi_fcmplt, DOS_API_MATH_AEABI_FCMPLT, float, float)
WRAP2(int, __aeabi_fcmpun, DOS_API_MATH_AEABI_FCMPUN, float, float)

WRAP1(float,              __aeabi_i2f,   DOS_API_MATH_AEABI_I2F,   int)
WRAP1(float,              __aeabi_ui2f,  DOS_API_MATH_AEABI_UI2F,  unsigned)
WRAP1(int,                __aeabi_f2iz,  DOS_API_MATH_AEABI_F2IZ,  float)
WRAP1(unsigned,           __aeabi_f2uiz, DOS_API_MATH_AEABI_F2UIZ, float)
WRAP1(float,              __aeabi_l2f,   DOS_API_MATH_AEABI_L2F,   long long)
WRAP1(float,              __aeabi_ul2f,  DOS_API_MATH_AEABI_UL2F,  unsigned long long)
WRAP1(long long,          __aeabi_f2lz,  DOS_API_MATH_AEABI_F2LZ,  float)
WRAP1(unsigned long long, __aeabi_f2ulz, DOS_API_MATH_AEABI_F2ULZ, float)

/* Double-precision arithmetic, comparisons and conversions. */
WRAP2(double, __aeabi_dadd, DOS_API_MATH_AEABI_DADD, double, double)
WRAP2(double, __aeabi_dsub, DOS_API_MATH_AEABI_DSUB, double, double)
WRAP2(double, __aeabi_dmul, DOS_API_MATH_AEABI_DMUL, double, double)
WRAP2(double, __aeabi_ddiv, DOS_API_MATH_AEABI_DDIV, double, double)
WRAP1(double, __aeabi_dneg, DOS_API_MATH_AEABI_DNEG, double)

WRAP2(int, __aeabi_dcmpeq, DOS_API_MATH_AEABI_DCMPEQ, double, double)
WRAP2(int, __aeabi_dcmpge, DOS_API_MATH_AEABI_DCMPGE, double, double)
WRAP2(int, __aeabi_dcmplt, DOS_API_MATH_AEABI_DCMPLT, double, double)
WRAP2(int, __aeabi_dcmpgt, DOS_API_MATH_AEABI_DCMPGT, double, double)
WRAP2(int, __aeabi_dcmple, DOS_API_MATH_AEABI_DCMPLE, double, double)
WRAP2(int, __aeabi_dcmpun, DOS_API_MATH_AEABI_DCMPUN, double, double)

WRAP1(double,             __aeabi_f2d,   DOS_API_MATH_AEABI_F2D,   float)
WRAP1(float,              __aeabi_d2f,   DOS_API_MATH_AEABI_D2F,   double)
WRAP1(double,             __aeabi_i2d,   DOS_API_MATH_AEABI_I2D,   int)
WRAP1(double,             __aeabi_ui2d,  DOS_API_MATH_AEABI_UI2D,  unsigned)
WRAP1(int,                __aeabi_d2iz,  DOS_API_MATH_AEABI_D2IZ,  double)
WRAP1(unsigned,           __aeabi_d2uiz, DOS_API_MATH_AEABI_D2UIZ, double)
WRAP1(double,             __aeabi_l2d,   DOS_API_MATH_AEABI_L2D,   long long)
WRAP1(double,             __aeabi_ul2d,  DOS_API_MATH_AEABI_UL2D,  unsigned long long)
WRAP1(long long,          __aeabi_d2lz,  DOS_API_MATH_AEABI_D2LZ,  double)
WRAP1(unsigned long long, __aeabi_d2ulz, DOS_API_MATH_AEABI_D2ULZ, double)

/*
 * Ordinary integer/64-bit helpers.  The four *divmod names are provided by
 * dos-api-divmod.S, not by C.
 */
WRAP2(int,      __aeabi_idiv,  DOS_API_MATH_AEABI_IDIV,  int, int)
WRAP2(unsigned, __aeabi_uidiv, DOS_API_MATH_AEABI_UIDIV, unsigned, unsigned)
WRAP2(long long, __aeabi_lmul, DOS_API_MATH_AEABI_LMUL, long long, long long)

WRAP2(unsigned long long, __aeabi_llsr, DOS_API_MATH_AEABI_LLSR,
      unsigned long long, int)
WRAP2(unsigned long long, __aeabi_llsl, DOS_API_MATH_AEABI_LLSL,
      unsigned long long, int)
WRAP2(long long, __aeabi_lasr, DOS_API_MATH_AEABI_LASR,
      long long, int)
WRAP2(int, __aeabi_lcmp, DOS_API_MATH_AEABI_LCMP,
      long long, long long)

/* Bit helpers. */
WRAP1(int, __clzsi2,      DOS_API_MATH_CLZSI2,      unsigned int)
WRAP1(int, __ctzsi2,      DOS_API_MATH_CTZSI2,      unsigned int)
WRAP1(int, __popcountsi2, DOS_API_MATH_POPCOUNTSI2, unsigned int)

/* Complex and integer-power compiler helpers. */
WRAP4(double _Complex, __muldc3, DOS_API_MATH_MULDC3,
      double, double, double, double)
WRAP4(double _Complex, __divdc3, DOS_API_MATH_DIVDC3,
      double, double, double, double)
WRAP4(float _Complex, __mulsc3, DOS_API_MATH_MULSC3,
      float, float, float, float)
WRAP4(float _Complex, __divsc3, DOS_API_MATH_DIVSC3,
      float, float, float, float)

WRAP2(float,  __powisf2, DOS_API_MATH_POWISF2, float, int)
WRAP2(double, __powidf2, DOS_API_MATH_POWIDF2, double, int)

/* MOS2 math-wrapper.c helper API. */
WRAP2(uint32_t, __u32u32u32_div, DOS_API_MATH_U32_DIV, uint32_t, uint32_t)
WRAP2(uint32_t, __u32u32u32_rem, DOS_API_MATH_U32_REM, uint32_t, uint32_t)
WRAP2(float,  __fff_div,   DOS_API_MATH_FFF_DIV,   float, float)
WRAP2(float,  __fff_mul,   DOS_API_MATH_FFF_MUL,   float, float)
WRAP2(float,  __ffu32_mul, DOS_API_MATH_FFU32_MUL, float, uint32_t)
WRAP2(double, __ddd_div,   DOS_API_MATH_DDD_DIV,   double, double)
WRAP2(double, __ddd_mul,   DOS_API_MATH_DDD_MUL,   double, double)
WRAP2(double, __ddu32_mul, DOS_API_MATH_DDU32_MUL, double, uint32_t)
WRAP2(double, __ddf_mul,   DOS_API_MATH_DDF_MUL,   double, float)
WRAP2(float,  __ffu32_div, DOS_API_MATH_FFU32_DIV, float, uint32_t)
WRAP2(double, __ddu32_div, DOS_API_MATH_DDU32_DIV, double, uint32_t)

/* Standard libm surface. */
WRAP1(double, trunc, DOS_API_MATH_TRUNC, double)
WRAP1(double, floor, DOS_API_MATH_FLOOR, double)
WRAP2(double, pow,   DOS_API_MATH_POW,   double, double)
WRAP1(double, sqrt,  DOS_API_MATH_SQRT,  double)
WRAP1(double, sin,   DOS_API_MATH_SIN,   double)
WRAP1(double, cos,   DOS_API_MATH_COS,   double)
WRAP1(double, tan,   DOS_API_MATH_TAN,   double)
WRAP1(double, atan,  DOS_API_MATH_ATAN,  double)
WRAP1(double, log,   DOS_API_MATH_LOG,   double)
WRAP1(double, exp,   DOS_API_MATH_EXP,   double)
WRAP2(float,  powf,  DOS_API_MATH_POWF,  float, float)

/* Original DOOM 16.16 helpers are merely consumers of the generic runtime. */
int FixedMul(int a, int b)
{
    return (int)(((int64_t)a * (int64_t)b) >> 16);
}

int FixedDiv2(int a, int b)
{
    return (int)(((int64_t)a << 16) / (int64_t)b);
}
