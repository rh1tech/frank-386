// *** VERSIONS RESTORATION ***
#ifndef GAMEVER_H
#define GAMEVER_H

// It is assumed here that:
// 1. The compiler is set up to appropriately define APPVER_EXEDEF
// as an EXE identifier.
// 2. This header is included (near) the beginning of every compilation unit,
// in order to have an impact in any place where it's expected to.

// APPVER_DOOMREV definitions
#define AV_DR_DM10 199312100
#define AV_DR_DM12 199402170
#define AV_DR_DM1666P 199408250
#define AV_DR_DM1666E 199408300
#define AV_DR_DM1666 199409010
#define AV_DR_DM17 199409210
#define AV_DR_DM17A 199410180
#define AV_DR_DM18FR 199412010
#define AV_DR_DM18 199501200
#define AV_DR_DM19 199502010
#define AV_DR_DM19UP 199503280
#define AV_DR_DM19U 199505250
#define AV_DR_DM19F 199606100
#define AV_DR_DM19F2 199610090
#define AV_DR_CHEX 199610310

// Now define APPVER_DOOMREV to one of the above, based on APPVER_EXEDEF

#define APPVER_CONCAT1(x,y) x ## y
#define APPVER_CONCAT2(x,y) APPVER_CONCAT1(x,y)
#define APPVER_DOOMREV APPVER_CONCAT2(AV_DR_,APPVER_EXEDEF)

// Convenience macro
#if (APPVER_DOOMREV == AV_DR_CHEX)
#define APPVER_CHEX 1
#endif

#endif // GAMEVER_H
