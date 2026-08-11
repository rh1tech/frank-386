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

#include "DoomDef.h"
#include "P_local.h"
#include "soundst.h"

//==================================================================
//==================================================================
//
//								FLOORS
//
//==================================================================
//==================================================================



//==================================================================
//
//	Move a plane (floor or ceiling) and check for crushing
//
//==================================================================
result_e	T_MovePlane(sector_t *sector,fixed_t speed,
			fixed_t dest,boolean crush,int floorOrCeiling,int direction)
{
	boolean	flag;
	fixed_t	lastpos;
	
	switch(floorOrCeiling)
	{
		case 0:		// FLOOR
			switch(direction)
			{
				case -1:	// DOWN
					if (sector->floorheight - speed < dest)
					{
						lastpos = sector->floorheight;
						sector->floorheight = dest;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							sector->floorheight =lastpos;
							P_ChangeSector(sector,crush);
							//return crushed;
						}
						return pastdest;
					}
					else
					{
						lastpos = sector->floorheight;
						sector->floorheight -= speed;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							sector->floorheight = lastpos;
							P_ChangeSector(sector,crush);
							return crushed;
						}
					}
					break;
						
				case 1:		// UP
					if (sector->floorheight + speed > dest)
					{
						lastpos = sector->floorheight;
						sector->floorheight = dest;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							sector->floorheight = lastpos;
							P_ChangeSector(sector,crush);
							//return crushed;
						}
						return pastdest;
					}
					else	// COULD GET CRUSHED
					{
						lastpos = sector->floorheight;
						sector->floorheight += speed;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							if (crush == true)
								return crushed;
							sector->floorheight = lastpos;
							P_ChangeSector(sector,crush);
							return crushed;
						}
					}
					break;
			}
			break;
									
		case 1:		// CEILING
			switch(direction)
			{
				case -1:	// DOWN
					if (sector->ceilingheight - speed < dest)
					{
						lastpos = sector->ceilingheight;
						sector->ceilingheight = dest;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							sector->ceilingheight = lastpos;
							P_ChangeSector(sector,crush);
							//return crushed;
						}
						return pastdest;
					}
					else	// COULD GET CRUSHED
					{
						lastpos = sector->ceilingheight;
						sector->ceilingheight -= speed;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							if (crush == true)
								return crushed;
							sector->ceilingheight = lastpos;
							P_ChangeSector(sector,crush);
							return crushed;
						}
					}
					break;
						
				case 1:		// UP
					if (sector->ceilingheight + speed > dest)
					{
						lastpos = sector->ceilingheight;
						sector->ceilingheight = dest;
						flag = P_ChangeSector(sector,crush);
						if (flag == true)
						{
							sector->ceilingheight = lastpos;
							P_ChangeSector(sector,crush);
							//return crushed;
						}
						return pastdest;
					}
					else
					{
						lastpos = sector->ceilingheight;
						sector->ceilingheight += speed;
						flag = P_ChangeSector(sector,crush);
						#if 0
						if (flag == true)
						{
							sector->ceilingheight = lastpos;
							P_ChangeSector(sector,crush);
							return crushed;
						}
						#endif
					}
					break;
			}
			break;
		
	}
	return ok;
}

//==================================================================
//
//	MOVE A FLOOR TO IT'S DESTINATION (UP OR DOWN)
//
//==================================================================
void T_MoveFloor(floormove_t *floor)
{
	result_e	res;

	res = T_MovePlane(floor->sector,floor->speed,
			floor->floordestheight,floor->crush,0,floor->direction);
	if (!(pleveltime&7))
		S_StartSound((mobj_t *)&floor->sector->soundorg, sfx_stnmov);
	if (res == pastdest)
	{
		floor->sector->specialdata = NULL;
		if (floor->direction == 1)
			switch(floor->type)
			{
				case donutRaise:
					floor->sector->special = floor->newspecial;
					floor->sector->floorpic = floor->texture;
				default:
					break;
			}
		else if (floor->direction == -1)
			switch(floor->type)
			{
				case lowerAndChange:
					floor->sector->special = floor->newspecial;
					floor->sector->floorpic = floor->texture;
				default:
					break;
			}
		P_RemoveThinker(&floor->thinker);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
		S_StartSound((mobj_t *)&floor->sector->soundorg, sfx_pstop);
#endif
	}

}

//==================================================================
//
//	HANDLE FLOOR TYPES
//
//==================================================================
int EV_DoFloor(line_t *line,floor_e floortype)
{
	int			secnum;
	int			rtn;
	int			i;
	sector_t	*sec;
	floormove_t	*floor;

	secnum = -1;
	rtn = 0;
	while ((secnum = P_FindSectorFromLineTag(line,secnum)) >= 0)
	{
		sec = &sectors[secnum];
		
		//	ALREADY MOVING?  IF SO, KEEP GOING...
		if (sec->specialdata)
			continue;
			
		//
		//	new floor thinker
		//
		rtn = 1;
		floor = Z_Malloc (sizeof(*floor), PU_LEVSPEC, 0);
		P_AddThinker (&floor->thinker);
		sec->specialdata = floor;
		floor->thinker.function = T_MoveFloor;
		floor->type = floortype;
		floor->crush = false;
		switch(floortype)
		{
			case lowerFloor:
				floor->direction = -1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = 
					P_FindHighestFloorSurrounding(sec);
				break;
			case lowerFloorToLowest:
				floor->direction = -1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = 
					P_FindLowestFloorSurrounding(sec);
				break;
			case turboLower:
				floor->direction = -1;
				floor->sector = sec;
				floor->speed = FLOORSPEED * 4;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
				floor->floordestheight = (8*FRACUNIT) + 
						P_FindHighestFloorSurrounding(sec);
#else
				floor->floordestheight =
						P_FindHighestFloorSurrounding(sec);
				if (floor->floordestheight != sec->floorheight)
					floor->floordestheight += 8*FRACUNIT;
#endif
				break;
			case raiseFloorCrush:
				floor->crush = true;
			case raiseFloor:
				floor->direction = 1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = 
					P_FindLowestCeilingSurrounding(sec);
				if (floor->floordestheight > sec->ceilingheight)
					floor->floordestheight = sec->ceilingheight;
				floor->floordestheight -= (8*FRACUNIT)*
					(floortype == raiseFloorCrush);
				break;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
			case raiseFloorTurbo:
				floor->direction = 1;
				floor->sector = sec;
				floor->speed = FLOORSPEED*4;
				floor->floordestheight = 
					P_FindNextHighestFloor(sec,sec->floorheight);
				break;
#endif
			case raiseFloorToNearest:
				floor->direction = 1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = 
					P_FindNextHighestFloor(sec,sec->floorheight);
				break;
			case raiseFloor24:
				floor->direction = 1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = floor->sector->floorheight +
						24 * FRACUNIT;
				break;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
			case raiseFloor512:
				floor->direction = 1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = floor->sector->floorheight +
						512 * FRACUNIT;
				break;
#endif
			case raiseFloor24AndChange:
				floor->direction = 1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = floor->sector->floorheight +
						24 * FRACUNIT;
				sec->floorpic = line->frontsector->floorpic;
				sec->special = line->frontsector->special;
				break;
			case raiseToTexture:
				{
					int	minsize = MAXINT;
					side_t	*side;
				
					floor->direction = 1;
					floor->sector = sec;
					floor->speed = FLOORSPEED;
					for (i = 0; i < sec->linecount; i++)
						if (twoSided (secnum, i) )
						{
							side = getSide(secnum,i,0);
							if (side->bottomtexture >= 0)
								if (textureheight[side->bottomtexture] < 
									minsize)
									minsize = 
										textureheight[side->bottomtexture];
							side = getSide(secnum,i,1);
							if (side->bottomtexture >= 0)
								if (textureheight[side->bottomtexture] < 
									minsize)
									minsize = 
										textureheight[side->bottomtexture];
						} 
					floor->floordestheight = floor->sector->floorheight + 
						minsize;
				}
				break;
			case lowerAndChange:
				floor->direction = -1;
				floor->sector = sec;
				floor->speed = FLOORSPEED;
				floor->floordestheight = 
					P_FindLowestFloorSurrounding(sec);
				floor->texture = sec->floorpic;
				for (i = 0; i < sec->linecount; i++)
					if ( twoSided(secnum, i) )
					{
						if (getSide(secnum,i,0)->sector-sectors == secnum)
						{
							sec = getSector(secnum,i,1);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
							if (sec->floorheight == floor->floordestheight)
#endif
							{
								floor->texture = sec->floorpic;
								floor->newspecial = sec->special;
								break;
							}
						}
						else
						{
							sec = getSector(secnum,i,0);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
							if (sec->floorheight == floor->floordestheight)
#endif
							{
								floor->texture = sec->floorpic;
								floor->newspecial = sec->special;
								break;
							}
						}
					}
			default:
				break;
		}
	}
	return rtn;
}

//==================================================================
//
//	BUILD A STAIRCASE!
//
//==================================================================
#if (APPVER_DOOMREV < AV_DR_DM1666P)
int EV_BuildStairs(line_t *line)
#else
int EV_BuildStairs(line_t *line, stair_e type)
#endif
{
	int		secnum;
	int		height;
	int		i;
	int		newsecnum;
	int		texture;
	int		ok;
	int		rtn;
	sector_t	*sec, *tsec;
	floormove_t	*floor;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	fixed_t	stairsize, speed;
#endif

	secnum = -1;
	rtn = 0;
	while ((secnum = P_FindSectorFromLineTag(line,secnum)) >= 0)
	{
		sec = &sectors[secnum];
		
		// ALREADY MOVING?  IF SO, KEEP GOING...
		if (sec->specialdata)
			continue;

		//
		// new floor thinker
		//
		rtn = 1;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
		height = sec->floorheight + 8*FRACUNIT;
#endif
		floor = Z_Malloc (sizeof(*floor), PU_LEVSPEC, 0);
		P_AddThinker (&floor->thinker);
		sec->specialdata = floor;
		floor->thinker.function = T_MoveFloor;
		floor->direction = 1;
		floor->sector = sec;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
		floor->speed = FLOORSPEED/4;
#else
		switch(type)
		{
			case build8:
				speed = FLOORSPEED/4;
				stairsize = 8*FRACUNIT;
				break;
			case turbo16:
				speed = FLOORSPEED*4;
				stairsize = 16*FRACUNIT;
				break;
		}
		floor->speed = speed;
		height = sec->floorheight + stairsize;
#endif
		floor->floordestheight = height;
		
		texture = sec->floorpic;

		//
		// Find next sector to raise
		// 1.	Find 2-sided line with same sector side[0]
		// 2.	Other side is the next sector to raise
		//
		do
		{
			ok = 0;
			for (i = 0;i < sec->linecount;i++)
			{
				if ( !((sec->lines[i])->flags & ML_TWOSIDED) )
					continue;
					
				tsec = (sec->lines[i])->frontsector;
				newsecnum = tsec-sectors;
				if (secnum != newsecnum)
					continue;
				tsec = (sec->lines[i])->backsector;
				newsecnum = tsec - sectors;
				if (tsec->floorpic != texture)
					continue;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
				height += 8*FRACUNIT;
#else
				height += stairsize;
#endif
				if (tsec->specialdata)
					continue;

				sec = tsec;
				secnum = newsecnum;
				floor = Z_Malloc (sizeof(*floor), PU_LEVSPEC, 0);
				P_AddThinker (&floor->thinker);
				sec->specialdata = floor;
				floor->thinker.function = T_MoveFloor;
				floor->direction = 1;
				floor->sector = sec;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
				floor->speed = FLOORSPEED/4;
#else
				floor->speed = speed;
#endif
				floor->floordestheight = height;
				ok = 1;
				break;
			}
		} while(ok);
	}
	return rtn;
}
