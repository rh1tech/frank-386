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

// P_telept.c

#include "DoomDef.h"
#include "P_local.h"
#include "soundst.h"

//==================================================================
//
//						TELEPORTATION
//
//==================================================================
int	EV_Teleport( line_t *line, int side, mobj_t *thing )
{
	int		i;
	int		tag;
	boolean		flag;
	mobj_t		*m,*fog;
	unsigned	an;
	thinker_t	*thinker;
	sector_t	*sector;
	fixed_t		oldx, oldy, oldz;
#if (APPVER_DOOMREV < AV_DR_DM12)
	int ret;
#endif
	
	if (thing->flags & MF_MISSILE)
		return 0;			// don't teleport missiles
		
	if (side == 1)		// don't teleport if hit back of line,
		return 0;		// so you can get out of teleporter
	
	tag = line->tag;
	for (i = 0; i < numsectors; i++)
		if (sectors[ i ].tag == tag )
		{
			thinker = thinkercap.next;
			for (thinker = thinkercap.next; thinker != &thinkercap;
				thinker = thinker->next)
			{
				if (thinker->function != P_MobjThinker)
					continue;	// not a mobj
				m = (mobj_t *)thinker;
				if (m->type != MT_TELEPORTMAN )
					continue;		// not a teleportman
				sector = m->subsector->sector;
				if (sector-sectors != i )
					continue;		// wrong sector

				oldx = thing->x;
				oldy = thing->y;
				oldz = thing->z;
#if (APPVER_DOOMREV < AV_DR_DM12)
				thing->flags |= MF_TELEPORT;
				ret = P_TryMove(thing, m->x, m->y);
				thing->flags &= ~MF_TELEPORT;
				if (!ret)
					return 0;
#else
				if (!P_TeleportMove (thing, m->x, m->y))
					return 0;
#endif
#if (APPVER_DOOMREV != AV_DR_DM19F)
				thing->z = thing->floorz;	//fixme: not needed?
#endif
				if (thing->player)
					thing->player->viewz = thing->z+thing->player->viewheight;
#if (APPVER_DOOMREV < AV_DR_DM12)
				thing->z = thing->floorz;
#endif
// spawn teleport fog at source and destination
				fog = P_SpawnMobj (oldx, oldy, oldz, MT_TFOG);
				S_StartSound (fog, sfx_telept);
				an = m->angle >> ANGLETOFINESHIFT;
				fog = P_SpawnMobj (m->x+20*finecosine[an], m->y+20*finesine[an]
					, thing->z, MT_TFOG);
				S_StartSound (fog, sfx_telept);
				if (thing->player)
					thing->reactiontime = 18;	// don't move for a bit
				thing->angle = m->angle;
				thing->momx = thing->momy = thing->momz = 0;
				return 1;
			}	
		}
	return 0;
}

