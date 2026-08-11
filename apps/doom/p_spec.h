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

// P_spec.h

/*
===============================================================================

							P_SPEC

===============================================================================
*/

//
//	Animating textures and planes
//
typedef struct
{
	boolean	istexture;
	int		picnum;
	int		basepic;
	int		numpics;
	int		speed;
} anim_t;

//
//	source animation definition
//
typedef struct
{
	boolean	istexture;		// if false, it's a flat
	char	endname[9];
	char	startname[9];
	int		speed;
} animdef_t;

#define	MAXANIMS		32

extern	anim_t	anims[MAXANIMS], *lastanim;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
//
// End-level timer (-TIMER option)
//
extern	boolean levelTimer;
extern	int	levelTimeCount;
#endif

//
//	Animating line specials
//
#define	MAXLINEANIMS		64
extern	short	numlinespecials;
extern	line_t	*linespeciallist[MAXLINEANIMS];


//	Define values for map objects
#define	MO_TELEPORTMAN		14

// at game start
void	P_InitPicAnims (void);

// at map load
void	P_SpawnSpecials (void);

// every tic
void 	P_UpdateSpecials (void);

// when needed
#if (APPVER_DOOMREV < AV_DR_DM1666P)
boolean	P_UseSpecialLine ( mobj_t *thing, line_t *line);
#else
boolean	P_UseSpecialLine ( mobj_t *thing, line_t *line, int side);
#endif
void	P_ShootSpecialLine ( mobj_t *thing, line_t *line);
void 	P_CrossSpecialLine (int linenum, int side, mobj_t *thing);

void 	P_PlayerInSpecialSector (player_t *player);

int		twoSided(int sector,int line);
sector_t *getSector(int currentSector,int line,int side);
side_t	*getSide(int currentSector,int line, int side);
fixed_t	P_FindLowestFloorSurrounding(sector_t *sec);
fixed_t	P_FindHighestFloorSurrounding(sector_t *sec);
fixed_t	P_FindNextHighestFloor(sector_t *sec,int currentheight);
fixed_t	P_FindLowestCeilingSurrounding(sector_t *sec);
fixed_t	P_FindHighestCeilingSurrounding(sector_t *sec);
int		P_FindSectorFromLineTag(line_t	*line,int start);
int		P_FindMinSurroundingLight(sector_t *sector,int max);
sector_t *getNextSector(line_t *line,sector_t *sec);

//
//	SPECIAL
//
int EV_DoDonut(line_t *line);

/*
===============================================================================

							P_LIGHTS

===============================================================================
*/
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
typedef struct
{
	thinker_t	thinker;
	sector_t	*sector;
	int			count;
	int			maxlight;
	int			minlight;
} fireflicker_t;
#endif

typedef struct
{
	thinker_t	thinker;
	sector_t	*sector;
	int			count;
	int			maxlight;
	int			minlight;
	int			maxtime;
	int			mintime;
} lightflash_t;

typedef struct
{
	thinker_t	thinker;
	sector_t	*sector;
	int			count;
	int			minlight;
	int			maxlight;
	int			darktime;
	int			brighttime;
} strobe_t;

typedef struct
{
	thinker_t	thinker;
	sector_t	*sector;
	int			minlight;
	int			maxlight;
	int			direction;
} glow_t;

#define GLOWSPEED		8
#define	STROBEBRIGHT	5
#define	FASTDARK		15
#define	SLOWDARK		35

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
void    P_SpawnFireFlicker (sector_t *sector);
#endif
void	T_LightFlash (lightflash_t *flash);
void	P_SpawnLightFlash (sector_t *sector);
void	T_StrobeFlash (strobe_t *flash);
void 	P_SpawnStrobeFlash (sector_t *sector, int fastOrSlow, int inSync);
void	EV_StartLightStrobing(line_t *line);
void	EV_TurnTagLightsOff(line_t	*line);
void	EV_LightTurnOn(line_t *line, int bright);
void	T_Glow(glow_t *g);
void	P_SpawnGlowingLight(sector_t *sector);

/*
===============================================================================

							P_SWITCH

===============================================================================
*/
typedef struct
{
	char	name1[9];
	char	name2[9];
	short	episode;
} switchlist_t;

typedef enum
{
	top,
	middle,
	bottom
} bwhere_e;

typedef struct
{
	line_t		*line;
	bwhere_e	where;
	int			btexture;
	int			btimer;
	mobj_t		*soundorg;
} button_t;

#define	MAXSWITCHES	50		// max # of wall switches in a level
#define	MAXBUTTONS	16		// 4 players, 4 buttons each at once, max.
#define BUTTONTIME	35		// 1 second

extern	button_t	buttonlist[MAXBUTTONS];	

void	P_ChangeSwitchTexture(line_t *line,int useAgain);
void 	P_InitSwitchList(void);

/*
===============================================================================

							P_PLATS

===============================================================================
*/
typedef enum
{
	up,
	down,
	waiting,
	in_stasis
} plat_e;

typedef enum
{
	perpetualRaise,
	downWaitUpStay,
	raiseAndChange,
	raiseToNearestAndChange,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	blazeDWUS
#endif
} plattype_e;

typedef struct
{
	thinker_t	thinker;
	sector_t	*sector;
	fixed_t		speed;
	fixed_t		low;
	fixed_t		high;
	int			wait;
	int			count;
	plat_e		status;
	plat_e		oldstatus;
	boolean		crush;
	int			tag;
	plattype_e	type;
} plat_t;

#define	PLATWAIT	3
#define	PLATSPEED	FRACUNIT
#define	MAXPLATS	30

extern	plat_t	*activeplats[MAXPLATS];

void	T_PlatRaise(plat_t	*plat);
int		EV_DoPlat(line_t *line,plattype_e type,int amount);
void	P_AddActivePlat(plat_t *plat);
void	P_RemoveActivePlat(plat_t *plat);
void	EV_StopPlat(line_t *line);
void	P_ActivateInStasis(int tag);

/*
===============================================================================

							P_DOORS

===============================================================================
*/
typedef enum
{
	normal,
	close30ThenOpen,
	close,
	open,
	raiseIn5Mins,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	blazeRaise,
	blazeOpen,
	blazeClose
#endif
} vldoor_e;

typedef struct
{
	thinker_t	thinker;
	vldoor_e	type;
	sector_t	*sector;
	fixed_t		topheight;
	fixed_t		speed;
	int			direction;		// 1 = up, 0 = waiting at top, -1 = down
	int			topwait;		// tics to wait at the top
								// (keep in case a door going down is reset)
	int			topcountdown;	// when it reaches 0, start going down
} vldoor_t;
	
#define	VDOORSPEED	FRACUNIT*2
#define	VDOORWAIT		150

void	EV_VerticalDoor (line_t *line, mobj_t *thing);
int		EV_DoDoor (line_t *line, vldoor_e type);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
int		EV_DoLockedDoor (line_t *line, vldoor_e type, mobj_t *thing);
#endif
void	T_VerticalDoor (vldoor_t *door);
void	P_SpawnDoorCloseIn30 (sector_t *sec);
void	P_SpawnDoorRaiseIn5Mins (sector_t *sec, int secnum);

#if 0
//
//      Sliding doors...
//
typedef enum
{
    sd_opening,
    sd_waiting,
    sd_closing
} sd_e;

typedef enum
{
    sdt_openOnly,
    sdt_closeOnly,
    sdt_openAndClose
} sdt_e;

typedef struct
{
	thinker_t	thinker;
	sdt_e		type;
	line_t		*line;
	int			frame;
	int			whichDoorIndex;
	int			timer;
	sector_t	*frontsector;
	sector_t	*backsector;
	sd_e		status;
} slidedoor_t;

typedef struct
{
	char	frontFrame1[9];
	char	frontFrame2[9];
	char	frontFrame3[9];
	char	frontFrame4[9];
	char	backFrame1[9];
	char	backFrame2[9];
	char	backFrame3[9];
	char	backFrame4[9];
} slidename_t;

typedef struct
{
	int             frontFrames[4];
	int             backFrames[4];
} slideframe_t;

// how many frames of animation
#define SNUMFRAMES		4

#define SDOORWAIT		35*3
#define SWAITTICS		4

// how many diff. types of anims
#define MAXSLIDEDOORS	5                            

void	P_InitSlidingDoorFrames (void);

void	EV_SlidingDoor (line_t *line, mobj_t *thing);
#endif

/*
===============================================================================

							P_CEILNG

===============================================================================
*/
typedef enum
{
	lowerToFloor,
	raiseToHighest,
	lowerAndCrush,
	crushAndRaise,
	fastCrushAndRaise,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	silentCrushAndRaise
#endif
} ceiling_e;

typedef struct
{
	thinker_t	thinker;
	ceiling_e	type;
	sector_t	*sector;
	fixed_t		bottomheight, topheight;
	fixed_t		speed;
	boolean		crush;
	int			direction;		// 1 = up, 0 = waiting, -1 = down
	int			tag;			// ID
	int			olddirection;
} ceiling_t;

#define	CEILSPEED		FRACUNIT
#define	CEILWAIT		150
#define MAXCEILINGS		30

extern	ceiling_t	*activeceilings[MAXCEILINGS];

int		EV_DoCeiling (line_t *line, ceiling_e  type);
void	T_MoveCeiling (ceiling_t *ceiling);
void	P_AddActiveCeiling(ceiling_t *c);
void	P_RemoveActiveCeiling(ceiling_t *c);
int		EV_CeilingCrushStop(line_t	*line);
void	P_ActivateInStasisCeiling(line_t *line);

/*
===============================================================================

							P_FLOOR

===============================================================================
*/
typedef enum
{
	lowerFloor,			// lower floor to highest surrounding floor
	lowerFloorToLowest,	// lower floor to lowest surrounding floor
	turboLower,			// lower floor to highest surrounding floor VERY FAST
	raiseFloor,			// raise floor to lowest surrounding CEILING
	raiseFloorToNearest,	// raise floor to next highest surrounding floor
	raiseToTexture,		// raise floor to shortest height texture around it
	lowerAndChange,		// lower floor to lowest surrounding floor and change
						// floorpic
	raiseFloor24,
	raiseFloor24AndChange,
	raiseFloorCrush,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	raiseFloorTurbo,	// raise to next highest floor, turbo-speed
#endif
	donutRaise,
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	raiseFloor512
#endif
} floor_e;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
typedef enum
{
	build8,	// slowly build by 8
	turbo16	// quickly build by 16
} stair_e;
#endif

typedef struct
{
	thinker_t	thinker;
	floor_e		type;
	boolean		crush;
	sector_t	*sector;
	int			direction;
	int			newspecial;
	short		texture;
	fixed_t		floordestheight;
	fixed_t		speed;
} floormove_t;

#define	FLOORSPEED	FRACUNIT

typedef enum
{
	ok,
	crushed,
	pastdest
} result_e;

result_e	T_MovePlane(sector_t *sector,fixed_t speed,
			fixed_t dest,boolean crush,int floorOrCeiling,int direction);

#if (APPVER_DOOMREV < AV_DR_DM1666P)
int		EV_BuildStairs(line_t *line);
#else
int		EV_BuildStairs(line_t *line, stair_e type);
#endif
int		EV_DoFloor(line_t *line,floor_e floortype);
void	T_MoveFloor(floormove_t *floor);

/*
===============================================================================

							P_TELEPT

===============================================================================
*/

boolean EV_Teleport(line_t *line, int side, mobj_t *thing);
