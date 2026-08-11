//
// Copyright (C) 1993-1996 Id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

// sounds.h

#ifndef __SOUNDSH__
#define __SOUNDSH__

#include "soundst.h"

// Music identifiers

typedef enum
{
	mus_None,
	mus_e1m1,
	mus_e1m2,
	mus_e1m3,
	mus_e1m4,
	mus_e1m5,
	mus_e1m6,
	mus_e1m7,
	mus_e1m8,
	mus_e1m9,
	mus_e2m1,
	mus_e2m2,
	mus_e2m3,
	mus_e2m4,
	mus_e2m5,
	mus_e2m6,
	mus_e2m7,
	mus_e2m8,
	mus_e2m9,
	mus_e3m1,
	mus_e3m2,
	mus_e3m3,
	mus_e3m4,
	mus_e3m5,
	mus_e3m6,
	mus_e3m7,
	mus_e3m8,
	mus_e3m9,
	mus_inter,
	mus_intro,
	mus_bunny,
	mus_victor,
#if (APPVER_DOOMREV >= AV_DR_DM12)
	mus_introa,
#endif
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	mus_runnin,
	mus_stalks,
	mus_countd,
	mus_betwee,
	mus_doom,
	mus_the_da,
	mus_shawn,
	mus_ddtblu,
	mus_in_cit,
	mus_dead,
	mus_stlks2,
	mus_theda2,
	mus_doom2,
	mus_ddtbl2,
	mus_runni2,
	mus_dead2,
	mus_stlks3,
	mus_romero,
	mus_shawn2,
	mus_messag,
	mus_count2,
	mus_ddtbl3,
	mus_ampie,
	mus_theda3,
	mus_adrian,
	mus_messg2,
	mus_romer2,
	mus_tense,
	mus_shawn3,
	mus_openin,
	mus_evil,
	mus_ultima,
	mus_read_m,
	mus_dm2ttl,
	mus_dm2int,
#endif
	NUMMUSIC
} musicenum_t;

// Sound identifiers

typedef enum
{
	sfx_None,
	sfx_pistol,
	sfx_shotgn,
	sfx_sgcock,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_dshtgn,
	sfx_dbopn,
	sfx_dbcls,
	sfx_dbload,
#endif
	sfx_plasma,
	sfx_bfg,
	sfx_sawup,
	sfx_sawidl,
	sfx_sawful,
	sfx_sawhit,
	sfx_rlaunc,
	sfx_rxplod,
	sfx_firsht,
	sfx_firxpl,
	sfx_pstart,
	sfx_pstop,
	sfx_doropn,
	sfx_dorcls,
	sfx_stnmov,
	sfx_swtchn,
	sfx_swtchx,
	sfx_plpain,
	sfx_dmpain,
	sfx_popain,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_vipain,
	sfx_mnpain,
	sfx_pepain,
#endif
	sfx_slop,
	sfx_itemup,
	sfx_wpnup,
	sfx_oof,
	sfx_telept,
	sfx_posit1,
	sfx_posit2,
	sfx_posit3,
	sfx_bgsit1,
	sfx_bgsit2,
	sfx_sgtsit,
	sfx_cacsit,
	sfx_brssit,
	sfx_cybsit,
	sfx_spisit,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_bspsit,
	sfx_kntsit,
	sfx_vilsit,
	sfx_mansit,
	sfx_pesit,
#endif
	sfx_sklatk,
	sfx_sgtatk,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_skepch,
	sfx_vilatk,
#endif
	sfx_claw,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_skeswg,
#endif
	sfx_pldeth,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_pdiehi,
#endif
	sfx_podth1,
	sfx_podth2,
	sfx_podth3,
	sfx_bgdth1,
	sfx_bgdth2,
	sfx_sgtdth,
	sfx_cacdth,
	sfx_skldth,
	sfx_brsdth,
	sfx_cybdth,
	sfx_spidth,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_bspdth,
	sfx_vildth,
	sfx_kntdth,
	sfx_pedth,
	sfx_skedth,
#endif
	sfx_posact,
	sfx_bgact,
	sfx_dmact,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_bspact,
	sfx_bspwlk,
	sfx_vilact,
#endif
	sfx_noway,
	sfx_barexp,
	sfx_punch,
	sfx_hoof,
	sfx_metal,
	sfx_chgun,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	sfx_tink,
	sfx_bdopn,
	sfx_bdcls,
	sfx_itmbk,
	sfx_flame,
	sfx_flamst,
	sfx_getpow,
	sfx_bospit,
	sfx_boscub,
	sfx_bossit,
	sfx_bospn,
	sfx_bosdth,
	sfx_manatk,
	sfx_mandth,
	sfx_sssit,
	sfx_ssdth,
	sfx_keenpn,
	sfx_keendt,
	sfx_skeact,
	sfx_skesit,
	sfx_skeatk,
	sfx_radio,
#endif
	NUMSFX
} sfxenum_t;

#endif
