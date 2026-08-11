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

#ifndef __STSTUFF_H__
#define __STSTUFF_H__


//
// STATUS BAR DATA
//


// Palette indices.
// For damage/bonus red-/gold-shifts
#define STARTREDPALS 1
#define STARTBONUSPALS 9
#define NUMREDPALS 8
#define NUMBONUSPALS 4
// Radiation suit, green shift.
#define RADIATIONPAL 13

// N/256*100% probability
//  that the normal face state will change
#define ST_FACEPROBABILITY 96

// For Responder
#define ST_TOGGLECHAT KEY_ENTER

// Location of status bar
#define ST_X 0
#define ST_X2 104

#define ST_FX 143
#define ST_FY 169

// Should be set to patch width
//  for tall numbers later on
#define ST_TALLNUMWIDTH (tallnum[0]->width)

// Number of status faces.
#define ST_NUMPAINFACES 5
#define ST_NUMSTRAIGHTFACES 3
#define ST_NUMTURNFACES 2
#define ST_NUMSPECIALFACES 3

#define ST_FACESTRIDE (ST_NUMSTRAIGHTFACES+ST_NUMTURNFACES+ST_NUMSPECIALFACES)

#define ST_NUMEXTRAFACES 2

#define ST_NUMFACES (ST_FACESTRIDE*ST_NUMPAINFACES+ST_NUMEXTRAFACES)

#define ST_TURNOFFSET (ST_NUMSTRAIGHTFACES)
#define ST_OUCHOFFSET (ST_TURNOFFSET + ST_NUMTURNFACES)
#define ST_EVILGRINOFFSET (ST_OUCHOFFSET + 1)
#define ST_RAMPAGEOFFSET (ST_EVILGRINOFFSET + 1)
#define ST_GODFACE (ST_NUMPAINFACES*ST_FACESTRIDE)
#define ST_DEADFACE (ST_GODFACE+1)

#define ST_FACESX 143
#define ST_FACESY 168

#define ST_EVILGRINCOUNT (2*TICRATE)
#define ST_STRAIGHTFACECOUNT (TICRATE/2)
#define ST_TURNCOUNT (1*TICRATE)
#define ST_OUCHCOUNT (1*TICRATE)
#define ST_RAMPAGEDELAY (2*TICRATE)

#define ST_MUCHPAIN 20


// Location and size of statistics,
//  justified according to widget type.
// Problem is, within which space? STbar? Screen?
// Note: this could be read in by a lump.
//       Problem is, is the stuff rendered
//       into a buffer,
//       or into the frame buffer?

// AMMO number pos.
#define ST_AMMOWIDTH 3	
#define ST_AMMOX 44
#define ST_AMMOY 171

// HEALTH number pos.
#define ST_HEALTHWIDTH 3	
#define ST_HEALTHX 90
#define ST_HEALTHY 171

// Weapon pos.
#define ST_ARMSX 111
#define ST_ARMSY 172
#define ST_ARMSBGX 104
#define ST_ARMSBGY 168
#define ST_ARMSXSPACE 12
#define ST_ARMSYSPACE 10

// Frags pos.
#define ST_FRAGSX 138
#define ST_FRAGSY 171	
#define ST_FRAGSWIDTH 2

// ARMOR number pos.
#define ST_ARMORWIDTH 3
#define ST_ARMORX 221
#define ST_ARMORY 171

// Key icon positions.
#define ST_KEY0WIDTH 8
#define ST_KEY0HEIGHT 5
#define ST_KEY0X 239
#define ST_KEY0Y 171
#define ST_KEY1WIDTH ST_KEY0WIDTH
#define ST_KEY1X 239
#define ST_KEY1Y 181
#define ST_KEY2WIDTH ST_KEY0WIDTH
#define ST_KEY2X 239
#define ST_KEY2Y 191

// Ammunition counter.
#define ST_AMMO0WIDTH 3
#define ST_AMMO0HEIGHT 6
#define ST_AMMO0X 288
#define ST_AMMO0Y 173
#define ST_AMMO1WIDTH ST_AMMO0WIDTH
#define ST_AMMO1X 288
#define ST_AMMO1Y 179
#define ST_AMMO2WIDTH ST_AMMO0WIDTH
#define ST_AMMO2X 288
#define ST_AMMO2Y 191
#define ST_AMMO3WIDTH ST_AMMO0WIDTH
#define ST_AMMO3X 288
#define ST_AMMO3Y 185

// Indicate maximum ammunition.
// Only needed because backpack exists.
#define ST_MAXAMMO0WIDTH 3
#define ST_MAXAMMO0HEIGHT 5
#define ST_MAXAMMO0X 314
#define ST_MAXAMMO0Y 173
#define ST_MAXAMMO1WIDTH ST_MAXAMMO0WIDTH
#define ST_MAXAMMO1X 314
#define ST_MAXAMMO1Y 179
#define ST_MAXAMMO2WIDTH ST_MAXAMMO0WIDTH
#define ST_MAXAMMO2X 314
#define ST_MAXAMMO2Y 191
#define ST_MAXAMMO3WIDTH ST_MAXAMMO0WIDTH
#define ST_MAXAMMO3X 314
#define ST_MAXAMMO3Y 185

// pistol
#define ST_WEAPON0X 110 
#define ST_WEAPON0Y 172

// shotgun
#define ST_WEAPON1X 122 
#define ST_WEAPON1Y 172

// chain gun
#define ST_WEAPON2X 134 
#define ST_WEAPON2Y 172

// missile launcher
#define ST_WEAPON3X 110 
#define ST_WEAPON3Y 181

// plasma gun
#define ST_WEAPON4X 122 
#define ST_WEAPON4Y 181

 // bfg
#define ST_WEAPON5X 134
#define ST_WEAPON5Y 181

// WPNS title
#define ST_WPNSX 109 
#define ST_WPNSY 191

 // DETH title
#define ST_DETHX 109
#define ST_DETHY 191

//Incoming messages window location
// #define ST_MSGTEXTX (viewwindowx)
// #define ST_MSGTEXTY (viewwindowy+viewheight-18)
#define ST_MSGTEXTX 0
#define ST_MSGTEXTY 0
// Dimensions given in characters.
#define ST_MSGWIDTH 52
// Or shall I say, in lines?
#define ST_MSGHEIGHT 1

#define ST_OUTTEXTX 0
#define ST_OUTTEXTY 6

// Width, in characters again.
#define ST_OUTWIDTH 52 
 // Height, in lines. 
#define ST_OUTHEIGHT 1

#define ST_MAPWIDTH	(strlen(mapnames[(gameepisode-1)*9+(gamemap-1)]))

#define ST_MAPTITLEX (SCREENWIDTH - ST_MAPWIDTH * ST_CHATFONTWIDTH)

#define ST_MAPTITLEY 0
#define ST_MAPHEIGHT 1

	    
// main player in game
static player_t *plyr; 

// ST_Start() has just been called
static boolean st_firsttime;

// used to execute ST_Init() only once
static int veryfirsttime = 1;

// lump number for PLAYPAL
static int lu_palette;

// used for timing
static unsigned int st_clock;

// used for making messages go away
static int st_msgcounter=0;

// used when in chat 
static st_chatstateenum_t st_chatstate;

// whether in automap or first-person
static st_stateenum_t st_gamestate;

// whether left-side main status bar is active
static boolean st_statusbaron;

// whether status bar chat is active
static boolean st_chat;

// value of st_chat before message popped up
static boolean st_oldchat;

// whether chat window has the cursor on
static boolean st_cursoron;

// !deathmatch
static boolean st_notdeathmatch; 

// !deathmatch && st_statusbaron
static boolean st_armson;

// !deathmatch
static boolean st_fragson; 

// main bar left
#if (APPVER_DOOMREV < AV_DR_DM12)
static patch_t *sbarl;
static patch_t *sbarr;
#else
static patch_t *sbar;
#endif

// 0-9, tall numbers
static patch_t *tallnum[10];

// tall % sign
static patch_t *tallpercent;

// 0-9, short, yellow (,different!) numbers
static patch_t *shortnum[10];

// 3 key-cards, 3 skulls
static patch_t *keys[NUMCARDS]; 

// face status patches
static patch_t *faces[ST_NUMFACES];

// face background
static patch_t *faceback;

 // main bar right
static patch_t *armsbg;

// weapon ownership patches
static patch_t *arms[6][2]; 

// ready-weapon widget
static st_number_t w_ready;

 // in deathmatch only, summary of frags stats
static st_number_t w_frags;

// health widget
static st_percent_t w_health;

// arms background
static st_binicon_t w_armsbg; 


// weapon ownership widgets
static st_multicon_t w_arms[6];

// face status widget
static st_multicon_t w_faces; 

// keycard widgets
static st_multicon_t w_keyboxes[3];

// armor widget
static st_percent_t w_armor;

// ammo widgets
static st_number_t w_ammo[4];

// max ammo widgets
static st_number_t w_maxammo[4]; 



 // number of frags so far in deathmatch
static int st_fragscount;

// used to use appopriately pained face
static int st_oldhealth = -1;

// used for evil grin
static boolean oldweaponsowned[NUMWEAPONS]; 

 // count until face changes
static int st_facecount = 0;

// current face index, used by w_faces
static int st_faceindex = 0;

// holds key-type for each key box on bar
static int keyboxes[3]; 

// a random number per tick
static int st_randomnumber;  



// Massive bunches of cheat shit
//  to keep it from being easy to figure them out.
// Yeah, right...
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
unsigned char cheat_mus_seq[] =
{
  0xb2, 0x26, 0xb6, 0xae, 0xea, 1, 0, 0, 0xff
};
#endif

#if APPVER_CHEX
unsigned char cheat_choppers_seq[] =
{
  0x72, 0xf6, 0xa6, 0x36, // joelkoenigs
  0xf2, 0xf6, 0xa6, 0x76, 0xb2, 0xe6, 0xea, 0xff
};

unsigned char cheat_god_seq[] =
{
  0x26, 0xa2, 0x6e, 0xb2, 0x26, 0x62, 0x6a, 0xae, 0xea, 0xff // davidbrus
};

unsigned char cheat_ammo_seq[] =
{
  0xea, 0xe2, 0xf6, 0x2e, 0x2e,	// scottholman
  0x32, 0xf6, 0x36, 0xb6, 0xa2, 0x76, 0xff
};

unsigned char cheat_ammonokey_seq[] =
{
  0xb6, 0xb2, 0xf2, 0xa6,	// mikekoenigs
  0xf2, 0xf6, 0xa6, 0x76, 0xb2, 0xe6, 0xea, 0xff
};


// Smashing Pumpkins Into Samml Piles Of Putried Debris.
unsigned char cheat_noclip_seq[] =
{
  0xe2, 0x32, 0xa2, 0x6a, 0x36, 0xa6, 0xea,	// charlesjacobi
  0x72, 0xa2, 0xe2, 0xf6, 0x62, 0xb2, 0xff
};

//
unsigned char cheat_commercial_noclip_seq[] =
{
  0xb2, 0x26, 0xe2, 0x36, 0xb2, 0x2a, 0xff	// idclip
};



unsigned char cheat_powerup_seq[7][13] =
{
 // andrewbenson
 { 0xa2, 0x76, 0x26, 0x6a, 0xa6, 0xee, 0x62, 0xa6, 0x76, 0xea, 0xf6, 0x76, 0xff},
 // deanhyers
 { 0x26, 0xa6, 0xa2, 0x76, 0x32, 0xba, 0xa6, 0x6a, 0xea, 0xff },
 // marybregi
 { 0xb6, 0xa2, 0x6a, 0xba, 0x62, 0x6a, 0xa6, 0xe6, 0xb2, 0xff },
 // allen
 { 0xa2, 0x36, 0x36, 0xa6, 0x76, 0xff },
 // digitalcafe
 { 0x26, 0xb2, 0xe6, 0xb2, 0x2e, 0xa2, 0x36, 0xe2, 0xa2, 0x66, 0xa6, 0xff},
 // joshuastorms
 { 0x72, 0xf6, 0xea, 0x32, 0xae, 0xa2, 0xea, 0x2e, 0xf6, 0x6a, 0xb6, 0xea, 0xff},
 // idbehold
 { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0xff }
};


unsigned char cheat_clev_seq[] =
{
  0x36, 0xa6, 0xa6, 0xea, 0x76, 0xba, 0x26, 0xa6, 0x6a, 1, 0, 0, 0xff // leesnyder
};


// my position cheat
unsigned char cheat_mypos_seq[] =
{
  0xf2, 0xb2, 0xb6, 0x32, 0xba, 0xa6, 0x6a, 0xea, 0xff
};
#else // !APPVER_CHEX
unsigned char cheat_choppers_seq[] =
{
  0xb2, 0x26, 0xe2, 0x32, 0xf6, 0x2a, 0x2a, 0xa6, 0x6a, 0xea, 0xff // id...
};

unsigned char cheat_god_seq[] =
{
  0xb2, 0x26, 0x26, 0xaa, 0x26, 0xff  // iddqd
};

unsigned char cheat_ammo_seq[] =
{
  0xb2, 0x26, 0xf2, 0x66, 0xa2, 0xff	// idkfa
};

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
unsigned char cheat_ammonokey_seq[] =
{
  0xb2, 0x26, 0x66, 0xa2, 0xff	// idfa
};
#endif


// Smashing Pumpkins Into Samml Piles Of Putried Debris. 
unsigned char cheat_noclip_seq[] =
{
  0xb2, 0x26, 0xea, 0x2a, 0xb2,	// idspispopd
  0xea, 0x2a, 0xf6, 0x2a, 0x26, 0xff
};

//
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
unsigned char cheat_commercial_noclip_seq[] =
{
  0xb2, 0x26, 0xe2, 0x36, 0xb2, 0x2a, 0xff	// idclip
}; 
#endif



unsigned char cheat_powerup_seq[7][10] =
{
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0x6e, 0xff }, 	// beholdv
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0xea, 0xff }, 	// beholds
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0xb2, 0xff }, 	// beholdi
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0x6a, 0xff }, 	// beholdr
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0xa2, 0xff }, 	// beholda
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0x36, 0xff }, 	// beholdl
  { 0xb2, 0x26, 0x62, 0xa6, 0x32, 0xf6, 0x36, 0x26, 0xff }		// behold
};


unsigned char cheat_clev_seq[] =
{
  0xb2, 0x26,  0xe2, 0x36, 0xa6, 0x6e, 1, 0, 0, 0xff	// idclev
};


// my position cheat
unsigned char cheat_mypos_seq[] =
{
  0xb2, 0x26, 0xb6, 0xba, 0x2a, 0xf6, 0xea, 0xff	// idmypos
}; 
#endif // APPVER_CHEX


// Now what?
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
cheatseq_t cheat_mus = { cheat_mus_seq, 0 };
#endif
cheatseq_t cheat_god = { cheat_god_seq, 0 };
cheatseq_t cheat_ammo = { cheat_ammo_seq, 0 };
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
cheatseq_t cheat_ammonokey = { cheat_ammonokey_seq, 0 };
#endif
cheatseq_t cheat_noclip = { cheat_noclip_seq, 0 };
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
cheatseq_t cheat_commercial_noclip = { cheat_commercial_noclip_seq, 0 };
#endif

cheatseq_t cheat_powerup[7] =
{
  { cheat_powerup_seq[0], 0 },
  { cheat_powerup_seq[1], 0 },
  { cheat_powerup_seq[2], 0 },
  { cheat_powerup_seq[3], 0 },
  { cheat_powerup_seq[4], 0 },
  { cheat_powerup_seq[5], 0 },
  { cheat_powerup_seq[6], 0 }
};

cheatseq_t cheat_choppers = { cheat_choppers_seq, 0 };
cheatseq_t cheat_clev = { cheat_clev_seq, 0 };
cheatseq_t cheat_mypos = { cheat_mypos_seq, 0 };

// 
extern char	*mapnames[];

extern boolean automapactive;

#endif
