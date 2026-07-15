#ifndef CPU_286_H
#define CPU_286_H

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "i386.h"
#include "emulator.h"

#define reges 0
#define regcs 1
#define regss 2
#define regds 3
#define regfs 4
#define reggs 5

#define regal AL_REG_IDX
#define regah AH_REG_IDX
#define regcl CL_REG_IDX
#define regch CH_REG_IDX
#define regdl DL_REG_IDX
#define regdh DH_REG_IDX
#define regbl BL_REG_IDX
#define regbh BH_REG_IDX

#define segregs ((uint16_t*)segregs32)
#define getmem8(x, y) read86(segbase(x) + (y))
#define getmem16(x, y)  readw86(segbase(x) + (y))
#define getmem32(x, y)  readdw86(segbase(x) + (y))
#define putmem8(x, y, z)  write86(segbase(x) + (y), z)
#define putmem16(x, y, z) writew86(segbase(x) + (y), z)
#define putmem32(x, y, z) writedw86(segbase(x) + (y), z)
#define signext(value)  (int16_t)(int8_t)(value)
#define signext32(value)  (int32_t)(int16_t)(value)
#define getreg16(regid) (cpu->gprx[regid].r16)
#define getreg32(regid) (cpu->gprx[regid].r32)
#define getreg8(i)  ((i) > 3 ? cpu->gprx[i - 4].r8[1] : cpu->gprx[i].r8[0])
#define putreg16(regid, writeval) cpu->gprx[regid].r16 = writeval
#define putreg32(regid, writeval) cpu->gprx[regid].r32 = writeval
#define putreg8(i, v) ((i) > 3 ? (cpu->gprx[i - 4].r8[1] = (v)) : (cpu->gprx[i].r8[0] = (v)))
#define getsegreg(regid)            segregs[(regid) << 1]
#define putsegreg(regid, writeval)  segregs[(regid) << 1] = writeval
#define segbase(x)  ((uint32_t) (x) << 4)

#define cf  cpu->flags.bits.CF
#define pf  cpu->flags.bits.PF
#define af  cpu->flags.bits.AF
#define zf  cpu->flags.bits.ZF
#define sf  cpu->flags.bits.SF
#define tf  cpu->flags.bits.TF
#define ifl cpu->flags.bits.IF
#define df  cpu->flags.bits.DF
#define of  cpu->flags.bits.OF

#define CPU_FL_CF    cf
#define CPU_FL_PF    pf
#define CPU_FL_AF    af
#define CPU_FL_ZF    zf
#define CPU_FL_SF    sf
#define CPU_FL_TF    tf
#define CPU_FL_IFL   ifl
#define CPU_FL_DF    df
#define CPU_FL_OF    of

#define FLAG_CF_OF_MASK ((1u << 11) | 1)
#define FLAG_CF_AF_MASK ((1u << 4) | 1)

void modregrm(CPU* cpu);
void getea(CPU* cpu, uint8_t rmval);

#define CPU_ES cpu->ext_accessors->get_seg16(cpu, SEG_ES)
#define CPU_CS cpu->ext_accessors->get_seg16(cpu, SEG_CS)
#define CPU_SS cpu->ext_accessors->get_seg16(cpu, SEG_SS)
#define CPU_DS cpu->ext_accessors->get_seg16(cpu, SEG_DS)

#define SET_ES(x) cpu->ext_accessors->set_seg16(cpu, SEG_ES, (x))
#define SET_CS(x) cpu->ext_accessors->set_seg16(cpu, SEG_CS, (x))
#define SET_SS(x) cpu->ext_accessors->set_seg16(cpu, SEG_SS, (x))
#define SET_DS(x) cpu->ext_accessors->set_seg16(cpu, SEG_DS, (x))

#if I386_MODE
#define SET_IP(x) do { cpu->ip = (x); cpu->next_ip = (x); PREFETCH_RESET } while(0)
#define CPU_IP    ((uint16_t)(cpu->next_ip))
#else
#define SET_IP(x) do { cpu->ip = (x); } while(0)
#define CPU_IP    ((uint16_t)(cpu->ip))
#endif

#endif // CPU_286_H
