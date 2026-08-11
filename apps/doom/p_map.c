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

// P_map.c

#include "DoomDef.h"
#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp1, _wp2, _wp3, _wp4;
#else
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9, _wp10, _wp11, _wp12, _wp13, _wp14, _wp15;
extern int _wp16, _wp17, _wp18, _wp19, _wp20;
#endif
#include "P_local.h"
#include "soundst.h"

#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp21, _wp22, _wp23, _wp24, _wp25, _wp26, _wp27, _wp28, _wp29, _wp30, _wp31, _wp32, _wp33, _wp34;
#else
extern int _wp21, _wp22, _wp23, _wp24, _wp25, _wp26, _wp27, _wp28, _wp29, _wp30, _wp31, _wp32, _wp33, _wp34, _wp35, _wp36;
#endif

/*
===============================================================================

NOTES:


===============================================================================
*/

/*
===============================================================================

mobj_t NOTES

mobj_ts are used to tell the refresh where to draw an image, tell the world simulation when objects are contacted, and tell the sound driver how to position a sound.

The refresh uses the next and prev links to follow lists of things in sectors as they are being drawn.  The sprite, frame, and angle elements determine which patch_t is used to draw the sprite if it is visible.  The sprite and frame values are allmost allways set from state_t structures.  The statescr.exe utility generates the states.h and states.c files that contain the sprite/frame numbers from the statescr.txt source file.  The xyz origin point represents a point at the bottom middle of the sprite (between the feet of a biped).  This is the default origin position for patch_ts grabbed with lumpy.exe.  A walking creature will have its z equal to the floor it is standing on.
 
The sound code uses the x,y, and subsector fields to do stereo positioning of any sound effited by the mobj_t.

The play simulation uses the blocklinks, x,y,z, radius, height to determine when mobj_ts are touching each other, touching lines in the map, or hit by trace lines (gunshots, lines of sight, etc). The mobj_t->flags element has various bit flags used by the simulation.


Every mobj_t is linked into a single sector based on it's origin coordinates.
The subsector_t is found with R_PointInSubsector(x,y), and the sector_t can be found with subsector->sector.  The sector links are only used by the rendering code,  the play simulation does not care about them at all.

Any mobj_t that needs to be acted upon be something else in the play world (block movement, be shot, etc) will also need to be linked into the blockmap.  If the thing has the MF_NOBLOCK flag set, it will not use the block links. It can still interact with other things, but only as the instigator (missiles will run into other things, but nothing can run into a missile).   Each block in the grid is 128*128 units, and knows about every line_t that it contains a piece of, and every interactable mobj_t that has it's origin contained.  

A valid mobj_t is a mobj_t that has the proper subsector_t filled in for it's xy coordinates and is linked into the subsector's sector or has the MF_NOSECTOR flag set (the subsector_t needs to be valid even if MF_NOSECTOR is set), and is linked into a blockmap block or has the MF_NOBLOCKMAP flag set.  Links should only be modified by the P_[Un]SetThingPosition () functions.  Do not change the MF_NO? flags while a thing is valid.


===============================================================================
*/

fixed_t		tmbbox[4];
mobj_t		*tmthing;
int			tmflags;
fixed_t		tmx, tmy;

boolean		floatok;				// if true, move would be ok if
									// within tmfloorz - tmceilingz

fixed_t		tmfloorz, tmceilingz, tmdropoffz;

// keep track of the line that lowers the ceiling, so missiles don't explode
// against sky hack walls
line_t		*ceilingline;

// keep track of special lines as they are hit, but don't process them
// until the move is proven valid
#define	MAXSPECIALCROSS		8
line_t	*spechit[MAXSPECIALCROSS];
int			 numspechit;

#if (APPVER_DOOMREV >= AV_DR_DM12)
/*
===============================================================================

					TELEPORT MOVE
 
===============================================================================
*/

/*
==================
=
= PIT_StompThing
=
==================
*/

boolean PIT_StompThing (mobj_t *thing)
{
	fixed_t		blockdist;
		
	if (!(thing->flags & MF_SHOOTABLE) )
		return true;
		
	blockdist = thing->radius + tmthing->radius;
	if ( abs(thing->x - tmx) >= blockdist || abs(thing->y - tmy) >= blockdist )
		return true;		// didn't hit it
		
	if (thing == tmthing)
		return true;		// don't clip against self
	
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if ( !tmthing->player )
#else
	if ( !tmthing->player && gamemap != 30)
#endif
		return false;		// monsters don't stomp things except on boss level
		
	P_DamageMobj (thing, tmthing, tmthing, 10000);
	
	return true;
}


/*
===================
=
= P_TeleportMove
=
===================
*/

boolean P_TeleportMove (mobj_t *thing, fixed_t x, fixed_t y)
{
	int			xl,xh,yl,yh,bx,by;
	subsector_t		*newsubsec;

//
// kill anything occupying the position
//

	tmthing = thing;
	tmflags = thing->flags;
	
	tmx = x;
	tmy = y;
	
	tmbbox[BOXTOP] = y + tmthing->radius;
	tmbbox[BOXBOTTOM] = y - tmthing->radius;
	tmbbox[BOXRIGHT] = x + tmthing->radius;
	tmbbox[BOXLEFT] = x - tmthing->radius;

	newsubsec = R_PointInSubsector (x,y);
	ceilingline = NULL;
	
//
// the base floor / ceiling is from the subsector that contains the
// point.  Any contacted lines the step closer together will adjust them
//
	tmfloorz = tmdropoffz = newsubsec->sector->floorheight;
	tmceilingz = newsubsec->sector->ceilingheight;
			
	validcount++;
	numspechit = 0;

//
// stomp on any things contacted
//
	xl = (tmbbox[BOXLEFT] - bmaporgx - MAXRADIUS)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx + MAXRADIUS)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy - MAXRADIUS)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy + MAXRADIUS)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockThingsIterator(bx,by,PIT_StompThing))
				return false;
	
//
// the move is ok, so link the thing into its new position
//	
	P_UnsetThingPosition (thing);

	thing->floorz = tmfloorz;
	thing->ceilingz = tmceilingz;	
	thing->x = x;
	thing->y = y;

	P_SetThingPosition (thing);
	
	return true;
}
#endif

/*
===============================================================================

					MOVEMENT ITERATOR FUNCTIONS
 
===============================================================================
*/

/*
==================
=
= PIT_CheckLine
=
= Adjusts tmfloorz and tmceilingz as lines are contacted
==================
*/

boolean PIT_CheckLine(line_t *ld)
{
	if (tmbbox[BOXRIGHT] <= ld->bbox[BOXLEFT]
		||	tmbbox[BOXLEFT] >= ld->bbox[BOXRIGHT]
		||	tmbbox[BOXTOP] <= ld->bbox[BOXBOTTOM]
		||	tmbbox[BOXBOTTOM] >= ld->bbox[BOXTOP] )
		return true;

	if (P_BoxOnLineSide(tmbbox, ld) != -1)
		return true;

// a line has been hit
/*
=
= The moving thing's destination position will cross the given line.
= If this should not be allowed, return false.
= If the line is special, keep track of it to process later if the move
= 	is proven ok.  NOTE: specials are NOT sorted by order, so two special lines
= 	that are only 8 pixels apart could be crossed in either order.
*/
	if (!ld->backsector)
		return false;		// one sided line

	if (!(tmthing->flags & MF_MISSILE) )
	{
		if ( ld->flags & ML_BLOCKING )
			return false;		// explicitly blocking everything
		if ( !tmthing->player && ld->flags & ML_BLOCKMONSTERS )
			return false;		// block monsters only
	}
	P_LineOpening (ld);		// set openrange, opentop, openbottom
	// adjust floor / ceiling heights
	if (opentop < tmceilingz)
	{
		tmceilingz = opentop;
		ceilingline = ld;
	}
	if (openbottom > tmfloorz)
		tmfloorz = openbottom;
	if (lowfloor < tmdropoffz)
		tmdropoffz = lowfloor;

	if (ld->special)
	{ // if contacted a special line, add it to the list
		spechit[numspechit] = ld;
		numspechit++;
	}
	return true;
}

/*
==================
=
= PIT_CheckThing
=
==================
*/

boolean PIT_CheckThing (mobj_t *thing)
{
	fixed_t		blockdist;
	boolean		solid;
	int			damage;

	if (!(thing->flags & (MF_SOLID|MF_SPECIAL|MF_SHOOTABLE) ))
		return true;

	blockdist = thing->radius+tmthing->radius;
	if (abs(thing->x-tmx) >= blockdist || abs(thing->y-tmy) >= blockdist)
	{ // didn't hit thing
		return true;
	}

	// don't clip against self
	if (thing == tmthing)
		return true;

//
// check for skulls slamming into things
//
	if (tmthing->flags & MF_SKULLFLY)
	{
		damage = ((P_Random()%8)+1)*tmthing->info->damage;
		P_DamageMobj (thing, tmthing, tmthing, damage);
		tmthing->flags &= ~MF_SKULLFLY;
		tmthing->momx = tmthing->momy = tmthing->momz = 0;
		P_SetMobjState (tmthing, tmthing->info->spawnstate);
		return false;		// stop moving
	}


//
// missiles can hit other things
//
	if (tmthing->flags & MF_MISSILE)
	{
	// see if it went over / under
		if (tmthing->z > thing->z + thing->height)
			return true;		// overhead
		if (tmthing->z+tmthing->height < thing->z)
			return true;		// underneath
#if (APPVER_DOOMREV < AV_DR_DM1666P)
		if (tmthing->target && tmthing->target->type == thing->type )
#else
		if (tmthing->target && (tmthing->target->type == thing->type || 
			(tmthing->target->type == MT_KNIGHT && thing->type == MT_BRUISER)||
			(tmthing->target->type == MT_BRUISER && thing->type == MT_KNIGHT) ) )
#endif
		{		// Don't hit same species as originator
			if (thing == tmthing->target)
				return true;
			if (thing->type != MT_PLAYER)
				return false;	// explode, but do no damage
			// let players missile other players
		}
		if (! (thing->flags&MF_SHOOTABLE) )
			return !(thing->flags & MF_SOLID);		// didn't do any damage
		
	// damage / explode
		damage = ((P_Random()%8)+1)*tmthing->info->damage;
		P_DamageMobj (thing, tmthing, tmthing->target, damage);
		return false;			// don't traverse any more
	}

//
// check for special pickup
//
	if (thing->flags & MF_SPECIAL)
	{
		solid = thing->flags&MF_SOLID;
		if (tmflags&MF_PICKUP)
			P_TouchSpecialThing (thing, tmthing);	// can remove thing
		return !solid;
	}
	return !(thing->flags & MF_SOLID);
}

/*
===============================================================================

 						MOVEMENT CLIPPING
 
===============================================================================
*/

/*
==================
=
= P_CheckPosition
=
= This is purely informative, nothing is modified (except things picked up)

in:
a mobj_t (can be valid or invalid)
a position to be checked (doesn't need to be related to the mobj_t->x,y)

during:
special things are touched if MF_PICKUP
early out on solid lines?

out:
newsubsec
floorz
ceilingz
tmdropoffz		the lowest point contacted (monsters won't move to a dropoff)
speciallines[]
numspeciallines

==================
*/

boolean P_CheckPosition (mobj_t *thing, fixed_t x, fixed_t y)
{
	int			xl,xh,yl,yh,bx,by;
	subsector_t		*newsubsec;

	tmthing = thing;
	tmflags = thing->flags;
	
	tmx = x;
	tmy = y;
	
	tmbbox[BOXTOP] = y + tmthing->radius;
	tmbbox[BOXBOTTOM] = y - tmthing->radius;
	tmbbox[BOXRIGHT] = x + tmthing->radius;
	tmbbox[BOXLEFT] = x - tmthing->radius;

	newsubsec = R_PointInSubsector (x,y);
	ceilingline = NULL;
	
//
// the base floor / ceiling is from the subsector that contains the
// point.  Any contacted lines the step closer together will adjust them
//
	tmfloorz = tmdropoffz = newsubsec->sector->floorheight;
	tmceilingz = newsubsec->sector->ceilingheight;
			
	validcount++;
	numspechit = 0;

	if ( tmflags & MF_NOCLIP )
		return true;

//
// check things first, possibly picking things up
// the bounding box is extended by MAXRADIUS because mobj_ts are grouped
// into mapblocks based on their origin point, and can overlap into adjacent
// blocks by up to MAXRADIUS units
//
	xl = (tmbbox[BOXLEFT] - bmaporgx - MAXRADIUS)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx + MAXRADIUS)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy - MAXRADIUS)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy + MAXRADIUS)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockThingsIterator(bx,by,PIT_CheckThing))
				return false;
//
// check lines
//
	xl = (tmbbox[BOXLEFT] - bmaporgx)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockLinesIterator (bx,by,PIT_CheckLine))
				return false;

	return true;
}

/*
===================
=
= P_TryMove
=
= Attempt to move to a new position, crossing special lines unless MF_TELEPORT
= is set
=
===================
*/

boolean P_TryMove (mobj_t *thing, fixed_t x, fixed_t y)
{
	fixed_t		oldx, oldy;
	int			side, oldside;
	line_t		*ld;

	floatok = false;
	if (!P_CheckPosition (thing, x, y))
		return false;		// solid wall or thing

	if ( !(thing->flags & MF_NOCLIP) )
	{
		if (tmceilingz - tmfloorz < thing->height)
			return false;	// doesn't fit
		floatok = true;
		if ( !(thing->flags&MF_TELEPORT)
			&&tmceilingz - thing->z < thing->height)
			return false;	// mobj must lower itself to fit
		if ( !(thing->flags&MF_TELEPORT)
			&& tmfloorz - thing->z > 24*FRACUNIT )
			return false;	// too big a step up
		if ( !(thing->flags&(MF_DROPOFF|MF_FLOAT))
			&& tmfloorz - tmdropoffz > 24*FRACUNIT )
			return false;	// don't stand over a dropoff
	}

//
// the move is ok, so link the thing into its new position
//
	P_UnsetThingPosition (thing);

	oldx = thing->x;
	oldy = thing->y;
	thing->floorz = tmfloorz;
	thing->ceilingz = tmceilingz;	
	thing->x = x;
	thing->y = y;

	P_SetThingPosition (thing);

//
// if any special lines were hit, do the effect
//
	if (! (thing->flags&(MF_TELEPORT|MF_NOCLIP)) )
		while (numspechit--)
		{
		// see if the line was crossed
			ld = spechit[numspechit];
			side = P_PointOnLineSide (thing->x, thing->y, ld);
			oldside = P_PointOnLineSide (oldx, oldy, ld);
			if (side != oldside)
			{
				if (ld->special)
					P_CrossSpecialLine (ld-lines, oldside, thing);
			}
		}

	return true;
}

/*
==================
=
= P_ThingHeightClip
=
= Takes a valid thing and adjusts the thing->floorz, thing->ceilingz,
= anf possibly thing->z
=
= This is called for all nearby monsters whenever a sector changes height
=
= If the thing doesn't fit, the z will be set to the lowest value and
= false will be returned
==================
*/

boolean P_ThingHeightClip (mobj_t *thing)
{
	boolean		onfloor;
	
	onfloor = (thing->z == thing->floorz);
	
	P_CheckPosition (thing, thing->x, thing->y);	
	// what about stranding a monster partially off an edge?
	
	thing->floorz = tmfloorz;
	thing->ceilingz = tmceilingz;
	
	if (onfloor)
	// walking monsters rise and fall with the floor
		thing->z = thing->floorz;
	else
	{	// don't adjust a floating monster unless forced to
		if (thing->z+thing->height > thing->ceilingz)
			thing->z = thing->ceilingz - thing->height;
	}
	
	if (thing->ceilingz - thing->floorz < thing->height)
		return false;
		
	return true;
}


/*
==============================================================================

							SLIDE MOVE

Allows the player to slide along any angled walls

==============================================================================
*/

fixed_t		bestslidefrac, secondslidefrac;
line_t		*bestslideline, *secondslideline;
mobj_t		*slidemo;

fixed_t		tmxmove, tmymove;

/*
==================
=
= P_HitSlideLine
=
= Adjusts the xmove / ymove so that the next move will slide along the wall
==================
*/

void P_HitSlideLine (line_t *ld)
{
	int			side;
	angle_t		lineangle, moveangle, deltaangle;
	fixed_t		movelen, newlen;
	
	
	if (ld->slopetype == ST_HORIZONTAL)
	{
		tmymove = 0;
		return;
	}
	if (ld->slopetype == ST_VERTICAL)
	{
		tmxmove = 0;
		return;
	}
	
	side = P_PointOnLineSide (slidemo->x, slidemo->y, ld);
	
	lineangle = R_PointToAngle2 (0,0, ld->dx, ld->dy);
	if (side == 1)
		lineangle += ANG180;
	moveangle = R_PointToAngle2 (0,0, tmxmove, tmymove);
	deltaangle = moveangle-lineangle;
	if (deltaangle > ANG180)
		deltaangle += ANG180;
//		I_Error ("SlideLine: ang>ANG180");

	lineangle >>= ANGLETOFINESHIFT;
	deltaangle >>= ANGLETOFINESHIFT;
	
	movelen = P_AproxDistance (tmxmove, tmymove);
	newlen = FixedMul (movelen, finecosine[deltaangle]);
	tmxmove = FixedMul (newlen, finecosine[lineangle]);	
	tmymove = FixedMul (newlen, finesine[lineangle]);	
}

/*
==============
=
= PTR_SlideTraverse
=
==============
*/

boolean		PTR_SlideTraverse (intercept_t *in)
{
	line_t	*li;
	
	if (!in->isaline)
		I_Error ("PTR_SlideTraverse: not a line?");
		
	li = in->d.line;
	if ( ! (li->flags & ML_TWOSIDED) )
	{
		if (P_PointOnLineSide (slidemo->x, slidemo->y, li))
			return true;		// don't hit the back side
		goto isblocking;
	}

	P_LineOpening (li);			// set openrange, opentop, openbottom
	if (openrange < slidemo->height)
		goto isblocking;		// doesn't fit
		
	if (opentop - slidemo->z < slidemo->height)
		goto isblocking;		// mobj is too high

	if (openbottom - slidemo->z > 24*FRACUNIT )
		goto isblocking;		// too big a step up

	return true;		// this line doesn't block movement
	
// the line does block movement, see if it is closer than best so far
isblocking:		
	if (in->frac < bestslidefrac)
	{
		secondslidefrac = bestslidefrac;
		secondslideline = bestslideline;
		bestslidefrac = in->frac;
		bestslideline = li;
	}
	
	return false;	// stop
}


/*
==================
=
= P_SlideMove
=
= The momx / momy move is bad, so try to slide along a wall
=
= Find the first line hit, move flush to it, and slide along it
=
= This is a kludgy mess.
==================
*/

void P_SlideMove (mobj_t *mo)
{
	fixed_t		leadx, leady;
	fixed_t		trailx, traily;
	fixed_t		newx, newy;
	int			hitcount;
		
	slidemo = mo;
	hitcount = 0;
retry:
	if (++hitcount == 3)
		goto stairstep;			// don't loop forever
			
//
// trace along the three leading corners
//
	if (mo->momx > 0)
	{
		leadx = mo->x + mo->radius;
		trailx = mo->x - mo->radius;
	}
	else
	{
		leadx = mo->x - mo->radius;
		trailx = mo->x + mo->radius;
	}
	
	if (mo->momy > 0)
	{
		leady = mo->y + mo->radius;
		traily = mo->y - mo->radius;
	}
	else
	{
		leady = mo->y - mo->radius;
		traily = mo->y + mo->radius;
	}
		
	bestslidefrac = FRACUNIT+1;
	
	P_PathTraverse ( leadx, leady, leadx+mo->momx, leady+mo->momy,
	 PT_ADDLINES, PTR_SlideTraverse );
	P_PathTraverse ( trailx, leady, trailx+mo->momx, leady+mo->momy,
	 PT_ADDLINES, PTR_SlideTraverse );
	P_PathTraverse ( leadx, traily, leadx+mo->momx, traily+mo->momy,
	 PT_ADDLINES, PTR_SlideTraverse );

//
// move up to the wall
//
	if (bestslidefrac == FRACUNIT+1)
	{	// the move most have hit the middle, so stairstep
stairstep:
		if (!P_TryMove (mo, mo->x, mo->y + mo->momy))
			P_TryMove (mo, mo->x + mo->momx, mo->y);
		return;
	}
	
	bestslidefrac -= 0x800;	// fudge a bit to make sure it doesn't hit
	if (bestslidefrac > 0)
	{
		newx = FixedMul (mo->momx, bestslidefrac);
		newy = FixedMul (mo->momy, bestslidefrac);
		if (!P_TryMove (mo, mo->x+newx, mo->y+newy))
			goto stairstep;
	}
		
//
// now continue along the wall
//
	bestslidefrac = FRACUNIT-(bestslidefrac+0x800);	// remainder
	if (bestslidefrac > FRACUNIT)
		bestslidefrac = FRACUNIT;
	if (bestslidefrac <= 0)
		return;
		
	tmxmove = FixedMul (mo->momx, bestslidefrac);
	tmymove = FixedMul (mo->momy, bestslidefrac);

	P_HitSlideLine (bestslideline);				// clip the moves

	mo->momx = tmxmove;
	mo->momy = tmymove;
		
	if (!P_TryMove (mo, mo->x+tmxmove, mo->y+tmymove))
	{
		goto retry;
	}
}


#if (APPVER_DOOMREV < AV_DR_DM12)
/*
==============================================================================

							P_CheckSight

==============================================================================
*/
extern int __wp1, __wp2;

fixed_t		sightzstart;			// eye z of looker
fixed_t		topslope, bottomslope;	// slopes to top and bottom of target

#define sightcounts_0 sightcounts_0_lopf
#define sightcounts_1 sightcounts_1_lopg

int			sightcounts_0, sightcounts_1;

boolean P_SightReject (subsector_t *t1, subsector_t *t2)
{
	int		s1, s2;
	int		pnum, bytenum, bitnum;

//
// check for trivial rejection
//
	s1 = (t1->sector - sectors);
	s2 = (t2->sector - sectors);
	pnum = s1*numsectors + s2;
	bytenum = pnum>>3;
	bitnum = 1 << (pnum&7);
	
	if (rejectmatrix[bytenum]&bitnum)
	{
sightcounts_0++;
		return false;		// can't possibly be connected
	}
sightcounts_1++;
	return true;
}

/*
==============
=
= PTR_SightTraverse
=
==============
*/

boolean		PTR_SightTraverse (intercept_t *in)
{
	line_t	*li;
	fixed_t	slope;

	if (!in->isaline)
		I_Error("P_SightTraverse: not a line?");
	
	li = in->d.line;
	
	if ( !(li->flags & ML_TWOSIDED) )
		return false;		// stop

//
// crosses a two sided line
//
	P_LineOpening (li);

	if (openbottom >= opentop)	// quick test for totally closed doors
		return false;	// stop

	if (li->frontsector->floorheight != li->backsector->floorheight)
	{
		slope = FixedDiv (openbottom - sightzstart , in->frac);
		if (slope > bottomslope)
			bottomslope = slope;
	}
	
	if (li->frontsector->ceilingheight != li->backsector->ceilingheight)
	{
		slope = FixedDiv (opentop - sightzstart , in->frac);
		if (slope < topslope)
			topslope = slope;
	}
	
	if (topslope <= bottomslope)
		return false;	// stop
			
	return true;	// keep going
}
/*
=====================
=
= P_CheckSight
=
= Returns true if a straight line between t1 and t2 is unobstructed
= look from eyes of t1 to any part of t2
=
=====================
*/

boolean P_CheckSight (mobj_t *t1, mobj_t *t2)
{
	if (!P_SightReject(t1->subsector, t2->subsector))
		return false;
//
// check precisely
//		
	sightzstart = t1->z + t1->height - (t1->height>>2);
	topslope = (t2->z+t2->height) - sightzstart;
	bottomslope = (t2->z) - sightzstart;

	return P_PathTraverse ( t1->x, t1->y, t2->x, t2->y, PT_ADDLINES|PT_EARLYOUT, PTR_SightTraverse );
}

#endif

/*
==============================================================================

							P_LineAttack

==============================================================================
*/


mobj_t		*linetarget;			// who got hit (or NULL)
mobj_t		*shootthing;
fixed_t		shootz;					// height if not aiming up or down
									// ???: use slope for monsters?
int			la_damage;
fixed_t		attackrange;

fixed_t		aimslope;

#if (APPVER_DOOMREV >= AV_DR_DM12)
extern	fixed_t		topslope, bottomslope;	// slopes to top and bottom of target
#endif

/*
===============================================================================
=
= PTR_AimTraverse
=
= Sets linetaget and aimslope when a target is aimed at
===============================================================================
*/

boolean		PTR_AimTraverse (intercept_t *in)
{
	line_t		*li;
	mobj_t		*th;
	fixed_t		slope, thingtopslope, thingbottomslope;
	fixed_t		dist;
		
	if (in->isaline)
	{
		li = in->d.line;
		if ( !(li->flags & ML_TWOSIDED) )
			return false;		// stop
//
// crosses a two sided line
// a two sided line will restrict the possible target ranges
		P_LineOpening (li);
	
		if (openbottom >= opentop)
			return false;		// stop
	
		dist = FixedMul (attackrange, in->frac);

		if (li->frontsector->floorheight != li->backsector->floorheight)
		{
			slope = FixedDiv (openbottom - shootz , dist);
			if (slope > bottomslope)
				bottomslope = slope;
		}
		
		if (li->frontsector->ceilingheight != li->backsector->ceilingheight)
		{
			slope = FixedDiv (opentop - shootz , dist);
			if (slope < topslope)
				topslope = slope;
		}
		
		if (topslope <= bottomslope)
			return false;		// stop
			
		return true;		// shot continues
	}
	
//
// shoot a thing
//
	th = in->d.thing;
	if (th == shootthing)
		return true;		// can't shoot self
	if (!(th->flags&MF_SHOOTABLE))
		return true;		// corpse or something

// check angles to see if the thing can be aimed at

	dist = FixedMul (attackrange, in->frac);
	thingtopslope = FixedDiv (th->z+th->height - shootz , dist);
	if (thingtopslope < bottomslope)
		return true;		// shot over the thing
	thingbottomslope = FixedDiv (th->z - shootz, dist);
	if (thingbottomslope > topslope)
		return true;		// shot under the thing

//
// this thing can be hit!
//
	if (thingtopslope > topslope)
		thingtopslope = topslope;
	if (thingbottomslope < bottomslope)
		thingbottomslope = bottomslope;

	aimslope = (thingtopslope+thingbottomslope)/2;
	linetarget = th;

	return false;			// don't go any farther
}


/*
==============================================================================
=
= PTR_ShootTraverse
=
==============================================================================
*/

boolean		PTR_ShootTraverse (intercept_t *in)
{
	fixed_t		x,y,z;
	fixed_t		frac;
	line_t		*li;
	mobj_t		*th;
	fixed_t		slope;
	fixed_t		dist;
	fixed_t		thingtopslope, thingbottomslope;
	mobj_t *mo;

	if (in->isaline)
	{
		li = in->d.line;
		if (li->special)
			P_ShootSpecialLine (shootthing, li);
		if ( !(li->flags & ML_TWOSIDED) )
			goto hitline;

//
// crosses a two sided line
//
		P_LineOpening (li);
		
		dist = FixedMul (attackrange, in->frac);

		if (li->frontsector->floorheight != li->backsector->floorheight)
		{
			slope = FixedDiv (openbottom - shootz , dist);
			if (slope > aimslope)
				goto hitline;
		}
		
		if (li->frontsector->ceilingheight != li->backsector->ceilingheight)
		{
			slope = FixedDiv (opentop - shootz , dist);
			if (slope < aimslope)
				goto hitline;
		}
			
		return true;		// shot continues
//
// hit line
//
hitline:
		// position a bit closer
		frac = in->frac - FixedDiv (4*FRACUNIT,attackrange);
		x = trace.x + FixedMul (trace.dx, frac);
		y = trace.y + FixedMul (trace.dy, frac);
		z = shootz + FixedMul (aimslope, FixedMul(frac, attackrange));

		if (li->frontsector->ceilingpic == skyflatnum)
		{
			if (z > li->frontsector->ceilingheight)
				return false;		// don't shoot the sky!
			if	(li->backsector && li->backsector->ceilingpic == skyflatnum)
				return false;		// it's a sky hack wall
		}
				
		P_SpawnPuff (x,y,z);
		return false;			// don't go any farther
	}
	
//
// shoot a thing
//
	th = in->d.thing;
	if (th == shootthing)
		return true;		// can't shoot self
	if (!(th->flags&MF_SHOOTABLE))
		return true;		// corpse or something

// check angles to see if the thing can be aimed at
	dist = FixedMul (attackrange, in->frac);
	thingtopslope = FixedDiv (th->z+th->height - shootz , dist);
	if (thingtopslope < aimslope)
		return true;		// shot over the thing
	thingbottomslope = FixedDiv (th->z - shootz, dist);
	if (thingbottomslope > aimslope)
		return true;		// shot under the thing

//
// hit thing
//
	// position a bit closer
	frac = in->frac - FixedDiv (10*FRACUNIT,attackrange);
	x = trace.x + FixedMul (trace.dx, frac);
	y = trace.y + FixedMul (trace.dy, frac);
	z = shootz + FixedMul (aimslope, FixedMul(frac, attackrange));
	// Spawn bullet puffs or blod spots,
	// depending on target type.
	if (in->d.thing->flags & MF_NOBLOOD)
		P_SpawnPuff (x,y,z);
	else
		P_SpawnBlood (x,y,z, la_damage);
	if (la_damage)
		P_DamageMobj (th, shootthing, shootthing, la_damage);
	return false; // don't go any farther
}

/*
=================
=
= P_AimLineAttack
=
=================
*/

fixed_t P_AimLineAttack (mobj_t *t1, angle_t angle, fixed_t distance)
{
	fixed_t		x2, y2;
	
	angle >>= ANGLETOFINESHIFT;
	shootthing = t1;
	x2 = t1->x + (distance>>FRACBITS)*finecosine[angle];
	y2 = t1->y + (distance>>FRACBITS)*finesine[angle];
	shootz = t1->z + (t1->height>>1) + 8*FRACUNIT;
	topslope = 100*FRACUNIT/160;	// can't shoot outside view angles
	bottomslope = -100*FRACUNIT/160;
	attackrange = distance;
	linetarget = NULL;
	
	P_PathTraverse ( t1->x, t1->y, x2, y2
		, PT_ADDLINES|PT_ADDTHINGS, PTR_AimTraverse );
		
	if (linetarget)
		return aimslope;
	return 0;
}



/*
=================
=
= P_LineAttack
=
= if damage == 0, it is just a test trace that will leave linetarget set
=
=================
*/

void P_LineAttack (mobj_t *t1, angle_t angle, fixed_t distance, fixed_t slope, int damage)
{
	fixed_t		x2, y2;
	
	angle >>= ANGLETOFINESHIFT;
	shootthing = t1;
	la_damage = damage;
	x2 = t1->x + (distance>>FRACBITS)*finecosine[angle];
	y2 = t1->y + (distance>>FRACBITS)*finesine[angle];
	shootz = t1->z + (t1->height>>1) + 8*FRACUNIT;
	attackrange = distance;
	aimslope = slope;
		
	P_PathTraverse ( t1->x, t1->y, x2, y2
		, PT_ADDLINES|PT_ADDTHINGS, PTR_ShootTraverse );
}



/*
==============================================================================

							USE LINES

==============================================================================
*/

mobj_t		*usething;

boolean		PTR_UseTraverse (intercept_t *in)
{
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	int		side;
#endif
	if (!in->d.line->special)
	{
		P_LineOpening (in->d.line);
		if (openrange <= 0)
		{
			S_StartSound (usething, sfx_noway);
			return false;	// can't use through a wall
		}
		return true ;		// not a special line, but keep checking
	}
		
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (P_PointOnLineSide (usething->x, usething->y, in->d.line) == 1)
		return false;

	P_UseSpecialLine (usething, in->d.line);
#else
	side = 0;
	if (P_PointOnLineSide (usething->x, usething->y, in->d.line) == 1)
		side = 1;
//		return false;		// don't use back sides

	P_UseSpecialLine (usething, in->d.line, side);
#endif

	return false;			// can't use for than one special line in a row
}


/*
================
=
= P_UseLines
=
= Looks for special lines in front of the player to activate
================ 
*/

void P_UseLines (player_t *player)
{
	int			angle;
	fixed_t		x1, y1, x2, y2;
	
	usething = player->mo;
		
#if (APPVER_DOOMREV < AV_DR_DM12)
	angle = player->mo->angle >> 24;
	x1 = player->mo->x;
	y1 = player->mo->y;
	x2 = x1 + (USERANGE>>FRACBITS)*costable[angle];
	y2 = y1 + (USERANGE>>FRACBITS)*sintable[angle];
#else
	angle = player->mo->angle >> ANGLETOFINESHIFT;
	x1 = player->mo->x;
	y1 = player->mo->y;
	x2 = x1 + (USERANGE>>FRACBITS)*finecosine[angle];
	y2 = y1 + (USERANGE>>FRACBITS)*finesine[angle];
#endif
	
	P_PathTraverse ( x1, y1, x2, y2, PT_ADDLINES, PTR_UseTraverse );
}



/*
==============================================================================

							RADIUS ATTACK

==============================================================================
*/

mobj_t		*bombsource;
mobj_t		*bombspot;
int			bombdamage;

/*
=================
=
= PIT_RadiusAttack
=
= Source is the creature that casued the explosion at spot
=================
*/

boolean PIT_RadiusAttack (mobj_t *thing)
{
	fixed_t dx, dy, dist;

	if (!(thing->flags & MF_SHOOTABLE) )
		return true;

	// Boss spider and cyborg
	// take no damage from concussion.
	if (thing->type == MT_CYBORG || thing->type == MT_SPIDER)
		return true;

	dx = abs(thing->x - bombspot->x);
	dy = abs(thing->y - bombspot->y);
	dist = dx > dy ? dx : dy;
	dist = (dist - thing->radius) >> FRACBITS;
	if(dist < 0)
		dist = 0;
	if (dist >= bombdamage)
		return true;		// out of range
	if ( P_CheckSight (thing, bombspot) )	// must be in direct path
		P_DamageMobj (thing, bombspot, bombsource, bombdamage - dist);
	return true;
}

/*
=================
=
= P_RadiusAttack
=
= Source is the creature that casued the explosion at spot
=================
*/

void P_RadiusAttack (mobj_t *spot, mobj_t *source, int damage)
{
	int			x,y, xl, xh, yl, yh;
	fixed_t		dist;
	
	dist = (damage+MAXRADIUS)<<FRACBITS;
	yh = (spot->y + dist - bmaporgy)>>MAPBLOCKSHIFT;
	yl = (spot->y - dist - bmaporgy)>>MAPBLOCKSHIFT;
	xh = (spot->x + dist - bmaporgx)>>MAPBLOCKSHIFT;
	xl = (spot->x - dist - bmaporgx)>>MAPBLOCKSHIFT;
	bombspot = spot;
	bombsource = source;
	bombdamage = damage;
	for (y=yl ; y<=yh ; y++)
		for (x=xl ; x<=xh ; x++)
			P_BlockThingsIterator (x, y, PIT_RadiusAttack );
}


/*
==============================================================================

						SECTOR HEIGHT CHANGING

= After modifying a sectors floor or ceiling height, call this
= routine to adjust the positions of all things that touch the
= sector.
=
= If anything doesn't fit anymore, true will be returned.
= If crunch is true, they will take damage as they are being crushed
= If Crunch is false, you should set the sector height back the way it
= was and call P_ChangeSector again to undo the changes
==============================================================================
*/

boolean		crushchange;
boolean		nofit;

/*
===============
=
= PIT_ChangeSector
=
===============
*/

boolean PIT_ChangeSector (mobj_t *thing)
{
	mobj_t		*mo;
	
	if (P_ThingHeightClip (thing))
		return true;		// keep checking

	// crunch bodies to giblets
	if (thing->health <= 0)
	{
		P_SetMobjState (thing, S_GIBS);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
		thing->flags &= ~MF_SOLID;
#endif
		thing->height = 0;
		thing->radius = 0;
		return true;		// keep checking
	}

	// crunch dropped items
	if (thing->flags & MF_DROPPED)
	{
		P_RemoveMobj (thing);
		return true;		// keep checking
	}

	if (! (thing->flags & MF_SHOOTABLE) )
		return true;				// assume it is bloody gibs or something
		
	nofit = true;
	if (crushchange && !(pleveltime&3) )
	{
		P_DamageMobj(thing,NULL,NULL,10);
		// spray blood in a random direction
		mo = P_SpawnMobj (thing->x, thing->y, thing->z + thing->height/2, MT_BLOOD);
		mo->momx = (P_Random() - P_Random ())<<12;
		mo->momy = (P_Random() - P_Random ())<<12;
	}
		
	return true;		// keep checking (crush other things)	
}

/*
===============
=
= P_ChangeSector
=
===============
*/

boolean P_ChangeSector (sector_t *sector, boolean crunch)
{
	int			x,y;
	
	nofit = false;
	crushchange = crunch;
	
// recheck heights for all things near the moving sector

	for (x=sector->blockbox[BOXLEFT] ; x<= sector->blockbox[BOXRIGHT] ; x++)
		for (y=sector->blockbox[BOXBOTTOM];y<= sector->blockbox[BOXTOP] ; y++)
			P_BlockThingsIterator (x, y, PIT_ChangeSector);
	
	
	return nofit;
}

