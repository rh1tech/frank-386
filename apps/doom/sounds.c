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

// sounds.c

#include "DoomDef.h"
#include "sounds.h"

// Music info

musicinfo_t S_music[] =
{
	{ 0 },
	{ "e1m1", 0 },
	{ "e1m2", 0 },
	{ "e1m3", 0 },
	{ "e1m4", 0 },
	{ "e1m5", 0 },
	{ "e1m6", 0 },
	{ "e1m7", 0 },
	{ "e1m8", 0 },
	{ "e1m9", 0 },
	{ "e2m1", 0 },
	{ "e2m2", 0 },
	{ "e2m3", 0 },
	{ "e2m4", 0 },
	{ "e2m5", 0 },
	{ "e2m6", 0 },
	{ "e2m7", 0 },
	{ "e2m8", 0 },
	{ "e2m9", 0 },
	{ "e3m1", 0 },
	{ "e3m2", 0 },
	{ "e3m3", 0 },
	{ "e3m4", 0 },
	{ "e3m5", 0 },
	{ "e3m6", 0 },
	{ "e3m7", 0 },
	{ "e3m8", 0 },
	{ "e3m9", 0 },
	{ "inter", 0 },
	{ "intro", 0 },
	{ "bunny", 0 },
	{ "victor", 0 },
#if (APPVER_DOOMREV >= AV_DR_DM12)
	{ "introa", 0 },
#endif
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "runnin", 0 },
	{ "stalks", 0 },
	{ "countd", 0 },
	{ "betwee", 0 },
	{ "doom", 0 },
	{ "the_da", 0 },
	{ "shawn", 0 },
	{ "ddtblu", 0 },
	{ "in_cit", 0 },
	{ "dead", 0 },
	{ "stlks2", 0 },
	{ "theda2", 0 },
	{ "doom2", 0 },
	{ "ddtbl2", 0 },
	{ "runni2", 0 },
	{ "dead2", 0 },
	{ "stlks3", 0 },
	{ "romero", 0 },
	{ "shawn2", 0 },
	{ "messag", 0 },
	{ "count2", 0 },
	{ "ddtbl3", 0 },
	{ "ampie", 0 },
	{ "theda3", 0 },
	{ "adrian", 0 },
	{ "messg2", 0 },
	{ "romer2", 0 },
	{ "tense", 0 },
	{ "shawn3", 0 },
	{ "openin", 0 },
	{ "evil", 0 },
	{ "ultima", 0 },
	{ "read_m", 0 },
	{ "dm2ttl", 0 },
	{ "dm2int", 0 }
#endif
};

// Sound info

sfxinfo_t S_sfx[] =
{
	{ 0 },
	{ "pistol", false, 64, 0, -1, -1, 0 },
	{ "shotgn", false, 64, 0, -1, -1, 0 },
	{ "sgcock", false, 64, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "dshtgn", false, 64, 0, -1, -1, 0 },
	{ "dbopn", false, 64, 0, -1, -1, 0 },
	{ "dbcls", false, 64, 0, -1, -1, 0 },
	{ "dbload", false, 64, 0, -1, -1, 0 },
#endif
	{ "plasma", false, 64, 0, -1, -1, 0 },
	{ "bfg", false, 64, 0, -1, -1, 0 },
	{ "sawup", false, 64, 0, -1, -1, 0 },
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	{ "sawidl", false, 128, 0, -1, -1, 0 },
#else
	{ "sawidl", false, 118, 0, -1, -1, 0 },
#endif
	{ "sawful", false, 64, 0, -1, -1, 0 },
	{ "sawhit", false, 64, 0, -1, -1, 0 },
	{ "rlaunc", false, 64, 0, -1, -1, 0 },
	{ "rxplod", false, 70, 0, -1, -1, 0 },
	{ "firsht", false, 70, 0, -1, -1, 0 },
	{ "firxpl", false, 70, 0, -1, -1, 0 },
	{ "pstart", false, 100, 0, -1, -1, 0 },
	{ "pstop", false, 100, 0, -1, -1, 0 },
	{ "doropn", false, 100, 0, -1, -1, 0 },
	{ "dorcls", false, 100, 0, -1, -1, 0 },
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	{ "stnmov", false, 100, 0, -1, -1, 0 },
#else
	{ "stnmov", false, 119, 0, -1, -1, 0 },
#endif
	{ "swtchn", false, 78, 0, -1, -1, 0 },
	{ "swtchx", false, 78, 0, -1, -1, 0 },
	{ "plpain", false, 96, 0, -1, -1, 0 },
	{ "dmpain", false, 96, 0, -1, -1, 0 },
	{ "popain", false, 96, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "vipain", false, 96, 0, -1, -1, 0 },
	{ "mnpain", false, 96, 0, -1, -1, 0 },
	{ "pepain", false, 96, 0, -1, -1, 0 },
#endif
	{ "slop", false, 78, 0, -1, -1, 0 },
	{ "itemup", true, 78, 0, -1, -1, 0 },
	{ "wpnup", true, 78, 0, -1, -1, 0 },
	{ "oof", false, 96, 0, -1, -1, 0 },
	{ "telept", false, 32, 0, -1, -1, 0 },
	{ "posit1", true, 98, 0, -1, -1, 0 },
	{ "posit2", true, 98, 0, -1, -1, 0 },
	{ "posit3", true, 98, 0, -1, -1, 0 },
	{ "bgsit1", true, 98, 0, -1, -1, 0 },
	{ "bgsit2", true, 98, 0, -1, -1, 0 },
	{ "sgtsit", true, 98, 0, -1, -1, 0 },
	{ "cacsit", true, 98, 0, -1, -1, 0 },
	{ "brssit", true, 94, 0, -1, -1, 0 },
	{ "cybsit", true, 92, 0, -1, -1, 0 },
	{ "spisit", true, 90, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "bspsit", true, 90, 0, -1, -1, 0 },
	{ "kntsit", true, 90, 0, -1, -1, 0 },
	{ "vilsit", true, 90, 0, -1, -1, 0 },
	{ "mansit", true, 90, 0, -1, -1, 0 },
	{ "pesit", true, 90, 0, -1, -1, 0 },
#endif
	{ "sklatk", false, 70, 0, -1, -1, 0 },
	{ "sgtatk", false, 70, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "skepch", false, 70, 0, -1, -1, 0 },
	{ "vilatk", false, 70, 0, -1, -1, 0 },
#endif
	{ "claw", false, 70, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "skeswg", false, 70, 0, -1, -1, 0 },
#endif
	{ "pldeth", false, 32, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "pdiehi", false, 32, 0, -1, -1, 0 },
#endif
	{ "podth1", false, 70, 0, -1, -1, 0 },
	{ "podth2", false, 70, 0, -1, -1, 0 },
	{ "podth3", false, 70, 0, -1, -1, 0 },
	{ "bgdth1", false, 70, 0, -1, -1, 0 },
	{ "bgdth2", false, 70, 0, -1, -1, 0 },
	{ "sgtdth", false, 70, 0, -1, -1, 0 },
	{ "cacdth", false, 70, 0, -1, -1, 0 },
	{ "skldth", false, 70, 0, -1, -1, 0 },
	{ "brsdth", false, 32, 0, -1, -1, 0 },
	{ "cybdth", false, 32, 0, -1, -1, 0 },
	{ "spidth", false, 32, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "bspdth", false, 32, 0, -1, -1, 0 },
	{ "vildth", false, 32, 0, -1, -1, 0 },
	{ "kntdth", false, 32, 0, -1, -1, 0 },
	{ "pedth", false, 32, 0, -1, -1, 0 },
	{ "skedth", false, 32, 0, -1, -1, 0 },
#endif
	{ "posact", true, 120, 0, -1, -1, 0 },
	{ "bgact", true, 120, 0, -1, -1, 0 },
	{ "dmact", true, 120, 0, -1, -1, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "bspact", true, 100, 0, -1, -1, 0 },
	{ "bspwlk", true, 100, 0, -1, -1, 0 },
	{ "vilact", true, 100, 0, -1, -1, 0 },
#endif
	{ "noway", false, 78, 0, -1, -1, 0 },
	{ "barexp", false, 60, 0, -1, -1, 0 },
	{ "punch", false, 64, 0, -1, -1, 0 },
	{ "hoof", false, 70, 0, -1, -1, 0 },
	{ "metal", false, 70, 0, -1, -1, 0 },
	{ "chgun", false, 64, &S_sfx[sfx_pistol], 150, 0, 0 },
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{ "tink", false, 60, 0, -1, -1, 0 },
	{ "bdopn", false, 100, 0, -1, -1, 0 },
	{ "bdcls", false, 100, 0, -1, -1, 0 },
	{ "itmbk", false, 100, 0, -1, -1, 0 },
	{ "flame", false, 32, 0, -1, -1, 0 },
	{ "flamst", false, 32, 0, -1, -1, 0 },
	{ "getpow", false, 60, 0, -1, -1, 0 },
	{ "bospit", false, 70, 0, -1, -1, 0 },
	{ "boscub", false, 70, 0, -1, -1, 0 },
	{ "bossit", false, 70, 0, -1, -1, 0 },
	{ "bospn", false, 70, 0, -1, -1, 0 },
	{ "bosdth", false, 70, 0, -1, -1, 0 },
	{ "manatk", false, 70, 0, -1, -1, 0 },
	{ "mandth", false, 70, 0, -1, -1, 0 },
	{ "sssit", false, 70, 0, -1, -1, 0 },
	{ "ssdth", false, 70, 0, -1, -1, 0 },
	{ "keenpn", false, 70, 0, -1, -1, 0 },
	{ "keendt", false, 70, 0, -1, -1, 0 },
	{ "skeact", false, 70, 0, -1, -1, 0 },
	{ "skesit", false, 70, 0, -1, -1, 0 },
	{ "skeatk", false, 70, 0, -1, -1, 0 },
	{ "radio", false, 60, 0, -1, -1, 0 } 
#endif
};
