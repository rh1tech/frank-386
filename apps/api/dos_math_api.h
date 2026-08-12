#ifndef __NATIVE_DOS_MATH_API_H__
#define __NATIVE_DOS_MATH_API_H__

/*
 * Public native-DOS math/compiler-runtime table indices.
 *
 * Use preprocessor constants rather than a C enum because the four divmod
 * ABI shims are implemented in a tiny preprocessed .S file and include this
 * same header.  This keeps C and assembly on one authoritative slot map.
 */
#define DOS_API_MATH_U32_DIV               19
#define DOS_API_MATH_U32_REM               20
#define DOS_API_MATH_FFF_DIV               21
#define DOS_API_MATH_FFF_MUL               22
#define DOS_API_MATH_FFU32_MUL             23
#define DOS_API_MATH_DDD_DIV               24
#define DOS_API_MATH_DDD_MUL               25
#define DOS_API_MATH_DDU32_MUL             26
#define DOS_API_MATH_DDF_MUL               27
#define DOS_API_MATH_FFU32_DIV             28
#define DOS_API_MATH_DDU32_DIV             29
#define DOS_API_MATH_TRUNC                 30
#define DOS_API_MATH_FLOOR                 31
#define DOS_API_MATH_POW                   32
#define DOS_API_MATH_SQRT                  33
#define DOS_API_MATH_SIN                   34
#define DOS_API_MATH_COS                   35
#define DOS_API_MATH_TAN                   36
#define DOS_API_MATH_ATAN                  37
#define DOS_API_MATH_LOG                   38
#define DOS_API_MATH_EXP                   39
#define DOS_API_MATH_POWF                  40
#define DOS_API_MATH_AEABI_FADD            41
#define DOS_API_MATH_AEABI_FSUB            42
#define DOS_API_MATH_AEABI_FMUL            43
#define DOS_API_MATH_AEABI_FDIV            44
#define DOS_API_MATH_AEABI_FNEG            45
#define DOS_API_MATH_AEABI_FCMPEQ          46
#define DOS_API_MATH_AEABI_FCMPGE          47
#define DOS_API_MATH_AEABI_FCMPGT          48
#define DOS_API_MATH_AEABI_FCMPLE          49
#define DOS_API_MATH_AEABI_FCMPLT          50
#define DOS_API_MATH_AEABI_FCMPUN          51
#define DOS_API_MATH_AEABI_I2F             52
#define DOS_API_MATH_AEABI_UI2F            53
#define DOS_API_MATH_AEABI_F2IZ            54
#define DOS_API_MATH_AEABI_F2UIZ           55
#define DOS_API_MATH_AEABI_L2F             56
#define DOS_API_MATH_AEABI_UL2F            57
#define DOS_API_MATH_AEABI_F2LZ            58
#define DOS_API_MATH_AEABI_F2ULZ           59
#define DOS_API_MATH_AEABI_DADD            60
#define DOS_API_MATH_AEABI_DSUB            61
#define DOS_API_MATH_AEABI_DMUL            62
#define DOS_API_MATH_AEABI_DDIV            63
#define DOS_API_MATH_AEABI_DNEG            64
#define DOS_API_MATH_AEABI_DCMPEQ          65
#define DOS_API_MATH_AEABI_DCMPGE          66
#define DOS_API_MATH_AEABI_DCMPLT          67
#define DOS_API_MATH_AEABI_DCMPGT          68
#define DOS_API_MATH_AEABI_DCMPLE          69
#define DOS_API_MATH_AEABI_DCMPUN          70
#define DOS_API_MATH_AEABI_F2D             71
#define DOS_API_MATH_AEABI_D2F             72
#define DOS_API_MATH_AEABI_I2D             73
#define DOS_API_MATH_AEABI_UI2D            74
#define DOS_API_MATH_AEABI_D2IZ            75
#define DOS_API_MATH_AEABI_D2UIZ           76
#define DOS_API_MATH_AEABI_L2D             77
#define DOS_API_MATH_AEABI_UL2D            78
#define DOS_API_MATH_AEABI_D2LZ            79
#define DOS_API_MATH_AEABI_D2ULZ           80
#define DOS_API_MATH_AEABI_IDIVMOD         81
#define DOS_API_MATH_AEABI_IDIV            82
#define DOS_API_MATH_AEABI_UIDIV           83
#define DOS_API_MATH_AEABI_UIDIVMOD        84
#define DOS_API_MATH_AEABI_LMUL            85
#define DOS_API_MATH_AEABI_ULDIVMOD        86
#define DOS_API_MATH_AEABI_LDIVMOD         87
#define DOS_API_MATH_AEABI_LLSR            88
#define DOS_API_MATH_AEABI_LLSL            89
#define DOS_API_MATH_AEABI_LASR            90
#define DOS_API_MATH_AEABI_LCMP            91
#define DOS_API_MATH_CLZSI2                92
#define DOS_API_MATH_CTZSI2                93
#define DOS_API_MATH_POPCOUNTSI2           94
#define DOS_API_MATH_MULDC3                95
#define DOS_API_MATH_DIVDC3                96
#define DOS_API_MATH_MULSC3                97
#define DOS_API_MATH_DIVSC3                98
#define DOS_API_MATH_POWISF2               99
#define DOS_API_MATH_POWIDF2               100
#define DOS_API_MATH_END                   101

#endif /* __NATIVE_DOS_MATH_API_H__ */
