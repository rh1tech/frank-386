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

// P_user.c

#include "DoomDef.h"
#include "P_local.h"
#include "soundst.h"

/*
===============================================================================

						movement

===============================================================================
*/

#define MAXBOB			0x100000		// 16 pixels of bob

boolean onground;

/*
==================
=
= P_Thrust
=
= moves the given origin along a given angle
=
==================
*/

void P_Thrust (player_t *player, angle_t angle, fixed_t move) 
{
#if (APPVER_DOOMREV < AV_DR_DM12)
	angle >>= 24;
	player->mo->momx += FixedMul(move,costable[angle]); 
	player->mo->momy += FixedMul(move,sintable[angle]);
#else
	angle >>= ANGLETOFINESHIFT;
	player->mo->momx += FixedMul(move,finecosine[angle]); 
	player->mo->momy += FixedMul(move,finesine[angle]);
#endif
}


/*
==================
=
= P_CalcHeight
=
=Calculate the walking / running height adjustment
=
==================
*/

void P_CalcHeight (player_t *player)
{
	int			angle;
	fixed_t		bob;
	

//
// regular movement bobbing (needs to be calculated for gun swing even
// if not on ground)
// OPTIMIZE: tablify angle 
	player->bob = FixedMul (player->mo->momx, player->mo->momx)+
	FixedMul (player->mo->momy,player->mo->momy);
	player->bob >>= 2;
	if (player->bob>MAXBOB)
		player->bob = MAXBOB;

	if ((player->cheats & CF_NOMOMENTUM) || !onground)
	{
		player->viewz = player->mo->z + VIEWHEIGHT;
		if (player->viewz > player->mo->ceilingz-4*FRACUNIT)
			player->viewz = player->mo->ceilingz-4*FRACUNIT;
		player->viewz = player->mo->z + player->viewheight;
		return;
	}
		
#if (APPVER_DOOMREV < AV_DR_DM12)
	angle = (12*gametic)&255;
	bob = FixedMul ( player->bob/2, sintable[angle]);
#else
	angle = (FINEANGLES/20*leveltime)&FINEMASK;
	bob = FixedMul ( player->bob/2, finesine[angle]);
#endif
	
//
// move viewheight
//
	if (player->playerstate == PST_LIVE)
	{
		player->viewheight += player->deltaviewheight;
		if (player->viewheight > VIEWHEIGHT)
		{
			player->viewheight = VIEWHEIGHT;
			player->deltaviewheight = 0;
		}
		if (player->viewheight < VIEWHEIGHT/2)
		{
			player->viewheight = VIEWHEIGHT/2;
			if (player->deltaviewheight <= 0)
				player->deltaviewheight = 1;
		}
	
		if (player->deltaviewheight)	
		{
			player->deltaviewheight += FRACUNIT/4;
			if (!player->deltaviewheight)
				player->deltaviewheight = 1;
		}
	}
	player->viewz = player->mo->z + player->viewheight + bob;
	if (player->viewz > player->mo->ceilingz-4*FRACUNIT)
		player->viewz = player->mo->ceilingz-4*FRACUNIT;
}

/*
=================
=
= P_MovePlayer
=
=================
*/

void P_MovePlayer (player_t *player)
{
	ticcmd_t		*cmd;
	
    cmd = &player->cmd;
#if (APPVER_DOOMREV < AV_DR_DM12)
	player->mo->angle += cmd->angleturn;
#else
	player->mo->angle += (cmd->angleturn<<16);
#endif

	// don't let the player control movement if not onground
	onground = (player->mo->z <= player->mo->floorz);

#if (APPVER_DOOMREV < AV_DR_DM12)
	if (cmd->forwardmove && onground)
		P_Thrust (player, player->mo->angle, cmd->forwardmove);
	if (cmd->sidemove && onground)
		P_Thrust (player, player->mo->angle-ANG90, cmd->sidemove);
#else
	if (cmd->forwardmove && onground)
		P_Thrust (player, player->mo->angle, cmd->forwardmove*2048);
	if (cmd->sidemove && onground)
		P_Thrust (player, player->mo->angle-ANG90, cmd->sidemove*2048);
#endif

	if ( (cmd->forwardmove || cmd->sidemove)
	&& player->mo->state == &states[S_PLAY] )
		P_SetMobjState (player->mo, S_PLAY_RUN1);

}

/*
=================
=
= P_DeathThink
=
=================
*/

#define         ANG5    (ANG90/18)

void P_DeathThink (player_t *player)
{
	angle_t		angle, delta;

	P_MovePsprites (player);
	
// fall to the ground
	if (player->viewheight > 6*FRACUNIT)
		player->viewheight -= FRACUNIT;
	if (player->viewheight < 6*FRACUNIT)
		player->viewheight = 6*FRACUNIT;
	player->deltaviewheight = 0;
	onground = (player->mo->z <= player->mo->floorz);
	P_CalcHeight (player);
	
	if (player->attacker && player->attacker != player->mo)
	{
		angle = R_PointToAngle2 (player->mo->x, player->mo->y
		, player->attacker->x, player->attacker->y);
		delta = angle - player->mo->angle;
		if (delta < ANG5 || delta > (unsigned)-ANG5)
		{	// looking at killer, so fade damage flash down
			player->mo->angle = angle;
			if (player->damagecount)
				player->damagecount--;
		}
		else if (delta < ANG180)
			player->mo->angle += ANG5;
		else
			player->mo->angle -= ANG5;
	}
	else if (player->damagecount)
		player->damagecount--;
	

	if (player->cmd.buttons & BT_USE)
		player->playerstate = PST_REBORN;
}



/*
=================
=
= P_PlayerThink
=
=================
*/

void P_PlayerThink (player_t *player)
{
	ticcmd_t		*cmd;
	weapontype_t	newweapon;
	
// fixme: do this in the cheat code
	if (player->cheats & CF_NOCLIP)
		player->mo->flags |= MF_NOCLIP;
	else
		player->mo->flags &= ~MF_NOCLIP;

//
// chain saw run forward
//
#if (APPVER_DOOMREV < AV_DR_DM12)
	if (player->mo->flags & MF_JUSTATTACKED)
	{
		player->cmd.angleturn = 0;
		player->cmd.forwardmove = 0xc800;
		player->cmd.sidemove = 0;
		player->mo->flags &= ~MF_JUSTATTACKED;
	}
#else
	cmd = &player->cmd;
	if (player->mo->flags & MF_JUSTATTACKED)
	{
		cmd->angleturn = 0;
		cmd->forwardmove = 0xc800/512;
		cmd->sidemove = 0;
		player->mo->flags &= ~MF_JUSTATTACKED;
	}
#endif
			
	
	if (player->playerstate == PST_DEAD)
	{
		P_DeathThink (player);
		return;
	}
		
//
// move around
// reactiontime is used to prevent movement for a bit after a teleport
	if (player->mo->reactiontime)
		player->mo->reactiontime--;
	else
		P_MovePlayer (player);
	P_CalcHeight (player);
	if (player->mo->subsector->sector->special)
		P_PlayerInSpecialSector (player);
		
#if (APPVER_DOOMREV < AV_DR_DM12)
	cmd = &player->cmd;
#endif
//
// check for weapon change
//
	if (cmd->buttons & BT_SPECIAL)
		cmd->buttons = 0;			// A special event has no other buttons
	
	if (cmd->buttons & BT_CHANGE)
	{
		// The actual changing of the weapon is done
		//  when the weapon psprite can do it
		//  (read: not in the middle of an attack).
		newweapon = (cmd->buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT;
	
		if (newweapon == wp_fist && player->weaponowned[wp_chainsaw] &&
			!(player->readyweapon == wp_chainsaw && player->powers[pw_strength]))
			newweapon = wp_chainsaw;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
		if (commercial && newweapon == wp_shotgun && player->weaponowned[wp_supershotgun] &&
			player->readyweapon != wp_supershotgun)
			newweapon = wp_supershotgun;
#endif
	
		if (player->weaponowned[newweapon]
			&& newweapon != player->readyweapon)
		{	// Do not go to plasma or BFG in shareware,  even if cheated.
			if ((newweapon != wp_plasma && newweapon != wp_bfg) || !shareware )
				player->pendingweapon = newweapon;
		}
	}
	
//
// check for use
//
	if (cmd->buttons & BT_USE)
	{
		if (!player->usedown)
		{
			P_UseLines (player);
			player->usedown = true;
		}
	}
	else
		player->usedown = false;
			
//
// cycle psprites
//
	P_MovePsprites (player);
	
	
//
// counters
//
	if (player->powers[pw_strength])
		player->powers[pw_strength]++;	// strength counts up to diminish fade
		
	if (player->powers[pw_invulnerability])
		player->powers[pw_invulnerability]--;

	if (player->powers[pw_invisibility])
		if (! --player->powers[pw_invisibility] )
			player->mo->flags &= ~MF_SHADOW;
			
	if (player->powers[pw_infrared])
		player->powers[pw_infrared]--;
		
	if (player->powers[pw_ironfeet])
		player->powers[pw_ironfeet]--;
		
	if (player->damagecount)
		player->damagecount--;
		
	if (player->bonuscount)
		player->bonuscount--;

	if (player->powers[pw_invulnerability])
	{
		if (player->powers[pw_invulnerability] > 4*32
			|| (player->powers[pw_invulnerability]&8) )
			player->fixedcolormap = INVERSECOLORMAP;
		else
			player->fixedcolormap = 0;
	}
	else if (player->powers[pw_infrared])	
	{
		if (player->powers[pw_infrared] > 4*32
			|| (player->powers[pw_infrared]&8) )
		{	// almost full bright
			player->fixedcolormap = 1;
		}
		else
			player->fixedcolormap = 0;
	}
	else
		player->fixedcolormap = 0;
}


