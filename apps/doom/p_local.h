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

// P_local.h

#ifndef __P_LOCAL__
#define __P_LOCAL__

#ifndef __R_LOCAL__
#include "R_local.h"
#endif

extern int _bp1, _bp2, _bp3, _bp4, _bp5, _bp6, _bp7;
extern int _bp8, _bp9, _bp10, _bp11, _bp12, _bp13, _bp14;
extern int _bp15, _bp16, _bp17, _bp18, _bp19, _bp20, _bp21;

#define FLOATSPEED		(FRACUNIT*4)


#define MAXHEALTH		100
#define VIEWHEIGHT		(41*FRACUNIT)

// mapblocks are used to check movement against lines and things
#define MAPBLOCKUNITS	128
#define MAPBLOCKSIZE	(MAPBLOCKUNITS*FRACUNIT)
#define MAPBLOCKSHIFT	(FRACBITS+7)
#define MAPBMASK		(MAPBLOCKSIZE-1)
#define MAPBTOFRAC		(MAPBLOCKSHIFT-FRACBITS)

// player radius for movement checking
#define PLAYERRADIUS	16*FRACUNIT

// MAXRADIUS is for precalculated sector block boxes
// the spider demon is larger, but we don't have any moving sectors
// nearby
#define MAXRADIUS		32*FRACUNIT

#define GRAVITY		FRACUNIT
#define MAXMOVE		(30*FRACUNIT)

#define USERANGE		(64*FRACUNIT)
#define MELEERANGE		(64*FRACUNIT)
#define MISSILERANGE	(32*64*FRACUNIT)

typedef enum
{
	DI_EAST,
	DI_NORTHEAST,
	DI_NORTH,
	DI_NORTHWEST,
	DI_WEST,
	DI_SOUTHWEST,
	DI_SOUTH,
	DI_SOUTHEAST,
	DI_NODIR,
	NUMDIRS
} dirtype_t;

#define	BASETHRESHOLD	 	100		// follow a player exlusively for 3 seconds

/*
===============================================================================

							P_TICK

===============================================================================
*/

extern	thinker_t	thinkercap;	// both the head and tail of the thinker list


void P_InitThinkers (void);
void P_AddThinker (thinker_t *thinker);
void P_RemoveThinker (thinker_t *thinker);

/*
===============================================================================

							P_PSPR

===============================================================================
*/

void P_SetupPsprites (player_t *curplayer);
void P_MovePsprites (player_t *curplayer);

void P_DropWeapon (player_t *player);

/*
===============================================================================

							P_USER

===============================================================================
*/

void	P_PlayerThink (player_t *player);

/*
===============================================================================
							P_MOBJ
===============================================================================
*/

#define ONFLOORZ	MININT
#define	ONCEILINGZ	MAXINT

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
// Time interval for item respawning.
#define ITEMQUESIZE		128

extern mapthing_t	itemrespawnque[ITEMQUESIZE];
extern int		itemrespawntime[ITEMQUESIZE];
extern int		iquehead, iquetail;


void P_RespawnSpecials (void);
#endif

mobj_t *P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);

void 	P_RemoveMobj (mobj_t *th);
boolean	P_SetMobjState (mobj_t *mobj, statenum_t state);
void 	P_MobjThinker (mobj_t *mobj);

void	P_SpawnPuff (fixed_t x, fixed_t y, fixed_t z);
void 	P_SpawnBlood (fixed_t x, fixed_t y, fixed_t z, int damage);
#if (APPVER_DOOMREV < AV_DR_DM1666P)
void	P_SpawnMissile (mobj_t *source, mobj_t *dest, mobjtype_t type);
#else
mobj_t *P_SpawnMissile (mobj_t *source, mobj_t *dest, mobjtype_t type);
#endif
void	P_SpawnPlayerMissile (mobj_t *source, mobjtype_t type);

/*
===============================================================================

							P_ENEMY

===============================================================================
*/

void P_NoiseAlert (mobj_t *target, mobj_t *emmiter);

/*
===============================================================================

							P_MAPUTL

===============================================================================
*/

typedef struct
{
	fixed_t x, y, dx, dy;
} divline_t;

typedef struct
{
	fixed_t		frac;		// along trace line
	boolean		isaline;
	union {
		mobj_t	*thing;
		line_t	*line;
	}			d;
} intercept_t;

#define	MAXINTERCEPTS	128
extern	intercept_t		intercepts[MAXINTERCEPTS], *intercept_p;
typedef boolean (*traverser_t) (intercept_t *in);


fixed_t P_AproxDistance (fixed_t dx, fixed_t dy);
int 	P_PointOnLineSide (fixed_t x, fixed_t y, line_t *line);
int 	P_PointOnDivlineSide (fixed_t x, fixed_t y, divline_t *line);
void 	P_MakeDivline (line_t *li, divline_t *dl);
fixed_t P_InterceptVector (divline_t *v2, divline_t *v1);
int 	P_BoxOnLineSide (fixed_t *tmbox, line_t *ld);

extern	fixed_t opentop, openbottom, openrange;
extern	fixed_t	lowfloor;
void 	P_LineOpening (line_t *linedef);

boolean P_BlockLinesIterator (int x, int y, boolean(*func)(line_t*) );
boolean P_BlockThingsIterator (int x, int y, boolean(*func)(mobj_t*) );

#define PT_ADDLINES		1
#define	PT_ADDTHINGS	2
#define	PT_EARLYOUT		4

extern	divline_t 	trace;
boolean P_PathTraverse (fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2,
	int flags, boolean (*trav) (intercept_t *));

void 	P_UnsetThingPosition (mobj_t *thing);
void	P_SetThingPosition (mobj_t *thing);

/*
===============================================================================

							P_MAP

===============================================================================
*/

extern boolean		floatok;				// if true, move would be ok if
extern fixed_t		tmfloorz, tmceilingz;	// within tmfloorz - tmceilingz

extern line_t *ceilingline;


boolean P_CheckPosition (mobj_t *thing, fixed_t x, fixed_t y);
boolean P_TryMove (mobj_t *thing, fixed_t x, fixed_t y);
#if (APPVER_DOOMREV >= AV_DR_DM12)
boolean P_TeleportMove (mobj_t *thing, fixed_t x, fixed_t y);
#endif
void	P_SlideMove (mobj_t *mo);
boolean P_CheckSight (mobj_t *t1, mobj_t *t2);
void 	P_UseLines (player_t *player);

boolean P_ChangeSector (sector_t *sector, boolean crunch);

extern	mobj_t		*linetarget;			// who got hit (or NULL)
fixed_t P_AimLineAttack (mobj_t *t1, angle_t angle, fixed_t distance);

void P_LineAttack (mobj_t *t1, angle_t angle, fixed_t distance, fixed_t slope, int damage);

void P_RadiusAttack (mobj_t *spot, mobj_t *source, int damage);

/*
===============================================================================

							P_SETUP

===============================================================================
*/

extern byte		*rejectmatrix;			// for fast sight rejection
extern short		*blockmaplump;		// offsets in blockmap are from here
extern short		*blockmap;
extern int			bmapwidth, bmapheight;	// in mapblocks
extern fixed_t		bmaporgx, bmaporgy;		// origin of block map
extern mobj_t		**blocklinks;			// for thing chains

extern int _bp21, _bp22;

/*
===============================================================================

							P_INTER

===============================================================================
*/

extern int		maxammo[NUMAMMO];
extern int		clipammo[NUMAMMO];

void P_TouchSpecialThing (mobj_t *special, mobj_t *toucher);

void P_DamageMobj (mobj_t *target, mobj_t *inflictor, mobj_t *source, int damage);

void P_SetMessage(player_t *player, char *message, boolean ultmsg);
void P_TouchSpecialThing(mobj_t *special, mobj_t *toucher);
void P_DamageMobj(mobj_t *target, mobj_t *inflictor, mobj_t *source,
	int damage);
boolean P_GiveAmmo(player_t *player, ammotype_t ammo, int count);
boolean P_GiveBody(player_t *player, int num);
boolean P_GivePower(player_t *player, powertype_t power);

#include "P_spec.h"

#endif // __P_LOCAL__
