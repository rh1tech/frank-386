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

// P_pspr.c

#include "DoomDef.h"
#include "P_local.h"
#include "soundst.h"
extern int _wp1, _wp2;

#define LOWERSPEED		FRACUNIT*6
#define RAISESPEED		FRACUNIT*6

#define WEAPONBOTTOM	128*FRACUNIT
#define WEAPONTOP		32*FRACUNIT


#define	BFGCELLS		40			// plasma cells for a bfg attack


/* 
================== 
= 
= P_SetPsprite
= 
================== 
*/ 

void P_SetPsprite (player_t *player, int position, statenum_t stnum) 
{
	pspdef_t	*psp;
	state_t	*state;
	
	psp = &player->psprites[position];
	
	do
	{
		if (!stnum)
		{
			psp->state = NULL;
			break;		// object removed itself
		}
		state = &states[stnum];
		psp->state = state;
		psp->tics = state->tics;  // could be 0

		if (state->misc1)
		{
			// coordinate set
			psp->sx = state->misc1 << FRACBITS;
			psp->sy = state->misc2 << FRACBITS;
		}

		// call action routine
		if (state->action)
		{
			state->action (player, psp);
			if (!psp->state)
				break;
		}
		stnum = psp->state->nextstate;
	} while (!psp->tics);	// an initial state of 0 could cycle through
}


/*
===============================================================================
						PSPRITE ACTIONS
===============================================================================
*/

weaponinfo_t	weaponinfo[NUMWEAPONS] =
{
	{	// fist
		am_noammo,		// ammo
		S_PUNCHUP,		// upstate 
		S_PUNCHDOWN,	// downstate
		S_PUNCH,		// readystate
		S_PUNCH1,		// atkstate
		S_NULL			// flashstate
	},
	{	// pistol
		am_clip,		// ammo
		S_PISTOLUP,		// upstate 
		S_PISTOLDOWN,	// downstate
		S_PISTOL,		// readystate
		S_PISTOL1,		// atkstate
		S_PISTOLFLASH	// flashstate
	},
	{	// shotgun
		am_shell,		// ammo
		S_SGUNUP,		// upstate 
		S_SGUNDOWN,		// downstate
		S_SGUN,			// readystate
		S_SGUN1,		// atkstate
		S_SGUNFLASH1	// flashstate
	},
	{	// chaingun
		am_clip,		// ammo
		S_CHAINUP,		// upstate 
		S_CHAINDOWN,	// downstate
		S_CHAIN,		// readystate
		S_CHAIN1,		// atkstate
		S_CHAINFLASH1	// flashstate
	},
	{	// missile
		am_misl,		// ammo
		S_MISSILEUP,	// upstate 
		S_MISSILEDOWN,	// downstate
		S_MISSILE,		// readystate
		S_MISSILE1,		// atkstate
		S_MISSILEFLASH1	// flashstate
	},
	{	// plasma
		am_cell,		// ammo
		S_PLASMAUP,		// upstate 
		S_PLASMADOWN,	// downstate
		S_PLASMA,		// readystate
		S_PLASMA1,		// atkstate
		S_PLASMAFLASH1	// flashstate
	},
	{	// bfg 9000
		am_cell,		// ammo
		S_BFGUP,		// upstate 
		S_BFGDOWN,		// downstate
		S_BFG,			// readystate
		S_BFG1,			// atkstate
		S_BFGFLASH1		// flashstate
	},
	{	// saw
		am_noammo,		// ammo
		S_SAWUP,		// upstate 
		S_SAWDOWN,		// downstate
		S_SAW,			// readystate
		S_SAW1,			// atkstate
		S_NULL			// flashstate
	},
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	{
		// super shotgun
		am_shell,		// ammo
		S_DSGUNUP,		// upstate 
		S_DSGUNDOWN,	// downstate
		S_DSGUN,		// readystate
		S_DSGUN1,		// atkstate
		S_DSGUNFLASH1	// flashstate
	},
#endif
};


/*
=================
=
= P_CalcSwing
=
=================
*/

fixed_t	swingx, swingy;
void P_CalcSwing (player_t *player)
{
	fixed_t	swing;
	int		angle;

// OPTIMIZE: tablify this

	swing = player->bob;
	
#if (APPVER_DOOMREV < AV_DR_DM12)
	angle = (3*gametic)&255;
	swingx = FixedMul ( swing, sintable[angle]);
	angle = (3*gametic + 128)&255;
	swingy = -FixedMul ( swingx, sintable[angle]);
#else
	angle = (FINEANGLES/70*leveltime)&FINEMASK;
	swingx = FixedMul ( swing, finesine[angle]);

	angle = (FINEANGLES/70*leveltime+FINEANGLES/2)&FINEMASK;
	swingy = -FixedMul ( swingx, finesine[angle]);
#endif
}

/*
================
=
= P_BringUpWeapon
=
= Starts bringing the pending weapon up from the bottom of the screen
= Uses player
================
*/

void P_BringUpWeapon (player_t *player)
{
	statenum_t	new;
	
	if (player->pendingweapon == wp_nochange)
		player->pendingweapon = player->readyweapon;
		
	if (player->pendingweapon == wp_chainsaw)
		S_StartSound (player->mo, sfx_sawup);
		
	new = weaponinfo[player->pendingweapon].upstate;

	player->pendingweapon = wp_nochange;
	player->psprites[ps_weapon].sy = WEAPONBOTTOM;
	P_SetPsprite (player, ps_weapon, new);
}

/*
================
=
= P_CheckAmmo
=
= returns true if there is enough ammo to shoot
= if not, selects the next weapon to use
================
*/

boolean P_CheckAmmo (player_t *player)
{
	ammotype_t	ammo;
	int			count;

	ammo = weaponinfo[player->readyweapon].ammo;
	if (player->readyweapon == wp_bfg)
		count = BFGCELLS;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	else if (player->readyweapon == wp_supershotgun)
		count = 2;
#endif
	else
		count = 1;
	if (ammo == am_noammo || player->ammo[ammo] >= count)
		return true;
		
	// out of ammo, pick a weapon to change to
	do
	{
		if (player->weaponowned[wp_plasma] && player->ammo[am_cell] && !shareware)
			player->pendingweapon = wp_plasma;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
		else if (player->weaponowned[wp_supershotgun] && player->ammo[am_shell]>2 && commercial)
			player->pendingweapon = wp_supershotgun;
#endif
		else if (player->weaponowned[wp_chaingun] && player->ammo[am_clip])
			player->pendingweapon = wp_chaingun;
		else if (player->weaponowned[wp_shotgun] && player->ammo[am_shell])
			player->pendingweapon = wp_shotgun;
		else if (player->ammo[am_clip])
			player->pendingweapon = wp_pistol;
		else if (player->weaponowned[wp_chainsaw])
			player->pendingweapon = wp_chainsaw;
		else if (player->weaponowned[wp_missile] && player->ammo[am_misl])
			player->pendingweapon = wp_missile;
		else if (player->weaponowned[wp_bfg] && player->ammo[am_cell]>40 && !shareware)
			player->pendingweapon = wp_bfg;
		else
			player->pendingweapon = wp_fist;
	} while (player->pendingweapon == wp_nochange);
	
	P_SetPsprite (player, ps_weapon
		, weaponinfo[player->readyweapon].downstate);

	return false;	
}


/*
================
=
= P_FireWeapon
=
================
*/

void P_FireWeapon (player_t *player)
{
	statenum_t	new;
	
	if (!P_CheckAmmo (player))
		return;

	P_SetMobjState (player->mo, S_PLAY_ATK1);
	new = weaponinfo[player->readyweapon].atkstate;
	P_SetPsprite (player, ps_weapon, new);
	P_NoiseAlert (player->mo, player->mo);
}


/*
================
=
= P_DropWeapon
=
= Player died, so put the weapon away
================
*/

void P_DropWeapon (player_t *player)
{
	P_SetPsprite (player, ps_weapon
	, weaponinfo[player->readyweapon].downstate);
}


/*
=================
=
= A_WeaponReady
=
= The player can fire the weapon or change to another weapon at this time
=
=================
*/
	
void A_WeaponReady (player_t *player, pspdef_t *psp)
{	
	statenum_t	new;
	int			angle;
	
    // get out of attack state
	if (player->mo->state == &states[S_PLAY_ATK1] || player->mo->state == &states[S_PLAY_ATK2] )
	{
		P_SetMobjState (player->mo, S_PLAY);
	}

	if (player->readyweapon == wp_chainsaw && psp->state == &states[S_SAW])
	{
		S_StartSound (player->mo, sfx_sawidl);
	}

//
// check for change
//  if player is dead, put the weapon away
//
	if (player->pendingweapon != wp_nochange || !player->health)
	{
	// change weapon (pending weapon should allready be validated)
		new = weaponinfo[player->readyweapon].downstate;
		P_SetPsprite (player, ps_weapon, new);
		return;	
	}
	
//
// check for fire
//
// the missile launcher and bfg do not auto fire
	if (player->cmd.buttons & BT_ATTACK)
	{
		if ( !player->attackdown
			|| (player->readyweapon != wp_missile && player->readyweapon != wp_bfg) )
		{
			player->attackdown = true;
			P_FireWeapon (player);		
			return;
		}
	}
	else
		player->attackdown = false;
	
//
// bob the weapon based on movement speed
//
#if (APPVER_DOOMREV < AV_DR_DM12)
	angle = (4*gametic)&255;
	psp->sx = FRACUNIT + FixedMul (player->bob, costable[angle]);
	angle &= 127;
	psp->sy = WEAPONTOP + FixedMul (player->bob, sintable[angle]);
#else
	angle = (128*leveltime)&FINEMASK;
	psp->sx = FRACUNIT + FixedMul (player->bob, finecosine[angle]);
	angle &= FINEANGLES/2-1;
	psp->sy = WEAPONTOP + FixedMul (player->bob, finesine[angle]);
#endif
}


/*
=================
=
= A_ReFire
=
= The player can re fire the weapon without lowering it entirely
=
=================
*/
	
void A_ReFire (player_t *player, pspdef_t *psp)
{	
//
// check for fire (if a weaponchange is pending, let it go through instead)
//
	if ( (player->cmd.buttons & BT_ATTACK)
	&& player->pendingweapon == wp_nochange && player->health)
	{
		player->refire++;
		P_FireWeapon (player);
	}
	else
	{
		player->refire = 0;
		P_CheckAmmo (player);
	}
}


void A_CheckReload (player_t *player, pspdef_t *psp)
{
	P_CheckAmmo (player);
#if 0
	if (player->ammo[am_shell]<2)
		P_SetPsprite (player, ps_weapon, S_DSNR1);
#endif
}


/*
=================
=
= A_Lower
=
=================
*/
	
void A_Lower (player_t *player, pspdef_t *psp)
{	
	psp->sy += LOWERSPEED;
	if (psp->sy < WEAPONBOTTOM )
		return;
	if (player->playerstate == PST_DEAD)
	{
		psp->sy = WEAPONBOTTOM;
		return;		// don't bring weapon back up
	}
		
//
// The old weapon has been lowered off the screen, so change the weapon
// and start raising it
//
	if (!player->health)
	{	// player is dead, so keep the weapon off screen
		P_SetPsprite (player,  ps_weapon, S_NULL);
		return;	
	}
	
	player->readyweapon = player->pendingweapon; 

	P_BringUpWeapon (player);
}


/*
=================
=
= A_Raise
=
=================
*/
	
void A_Raise (player_t *player, pspdef_t *psp)
{
	statenum_t	new;
	
	psp->sy -= RAISESPEED;

	if (psp->sy > WEAPONTOP )
		return;
	psp->sy = WEAPONTOP;	
	
//
// the weapon has been raised all the way, so change to the ready state
//
	new = weaponinfo[player->readyweapon].readystate;

	P_SetPsprite (player, ps_weapon, new);
}


/*
================
=
= A_GunFlash
=
=================
*/

void A_GunFlash (player_t *player, pspdef_t *psp) 
{
	P_SetMobjState (player->mo, S_PLAY_ATK2);
	P_SetPsprite (player,ps_flash,weaponinfo[player->readyweapon].flashstate);
}


/*
===============================================================================
						WEAPON ATTACKS
===============================================================================
*/


/* 
================== 
= 
= A_Punch
= 
================== 
*/ 
 
void A_Punch (player_t *player, pspdef_t *psp) 
{
	angle_t		angle;
	int			damage;
	int			slope;
	
	damage = ((P_Random ()%10)+1)<<1;
	if (player->powers[pw_strength])	
		damage *= 10;
	angle = player->mo->angle;
	angle += (P_Random()-P_Random())<<18;
	slope = P_AimLineAttack (player->mo, angle, MELEERANGE);
	P_LineAttack (player->mo, angle, MELEERANGE, slope, damage);
// turn to face target
	if (linetarget)
	{
		S_StartSound (player->mo, sfx_punch);
		player->mo->angle = R_PointToAngle2 (player->mo->x, player->mo->y,
		linetarget->x, linetarget->y);
	}
}

/* 
================== 
= 
= A_Saw
= 
================== 
*/ 
 
void A_Saw (player_t *player, pspdef_t *psp) 
{
	angle_t		angle;
	int			damage;
	int			slope;
	

	damage = 2*(P_Random ()%10+1);
	angle = player->mo->angle;
	angle += (P_Random()-P_Random())<<18;
// use meleerange + 1 se the puff doesn't skip the flash
	slope = P_AimLineAttack (player->mo, angle, MELEERANGE+1);
	P_LineAttack (player->mo, angle, MELEERANGE+1, slope, damage);
	if (!linetarget)
	{
		S_StartSound (player->mo, sfx_sawful);
		return;
	}
	S_StartSound (player->mo, sfx_sawhit);
	
// turn to face target
	angle = R_PointToAngle2 (player->mo->x, player->mo->y,
		linetarget->x, linetarget->y);
	if (angle - player->mo->angle > ANG180)
	{
		if (angle - player->mo->angle < -ANG90/20)
			player->mo->angle = angle + ANG90/21;
		else
			player->mo->angle -= ANG90/20;
	}
	else
	{
		if (angle - player->mo->angle > ANG90/20)
			player->mo->angle = angle - ANG90/21;
		else
			player->mo->angle += ANG90/20;
	}
	player->mo->flags |= MF_JUSTATTACKED;
}


/* 
================== 
= 
= A_FireMissile
= 
================== 
*/ 
 
void A_FireMissile (player_t *player, pspdef_t *psp) 
{
	player->ammo[weaponinfo[player->readyweapon].ammo]--;
	P_SpawnPlayerMissile (player->mo, MT_ROCKET);
}


/* 
================== 
= 
= A_FireBFG
= 
================== 
*/ 
 
void A_FireBFG (player_t *player, pspdef_t *psp) 
{
	player->ammo[weaponinfo[player->readyweapon].ammo] -= BFGCELLS;
	P_SpawnPlayerMissile (player->mo, MT_BFG);
}


/* 
================== 
= 
= A_FirePlasma
= 
================== 
*/ 
 
void A_FirePlasma (player_t *player, pspdef_t *psp) 
{
	player->ammo[weaponinfo[player->readyweapon].ammo]--;
	P_SetPsprite (player,ps_flash
		,weaponinfo[player->readyweapon].flashstate+(P_Random ()&1) );
	P_SpawnPlayerMissile (player->mo, MT_PLASMA);
}

/*
===============
=
= P_BulletSlope
=
= Sets a slope so a near miss is at aproximately the height of the
= intended target
=
===============
*/
fixed_t bulletslope;

void P_BulletSlope (mobj_t *mo)
{
	angle_t		an;

//
// see which target is to be aimed at
//
	an = mo->angle;
	bulletslope = P_AimLineAttack (mo, an, 16*64*FRACUNIT);
	if (!linetarget)
	{
		an += 1<<26;
		bulletslope = P_AimLineAttack (mo, an, 16*64*FRACUNIT);
		if (!linetarget)
		{
			an -= 2<<26;
			bulletslope = P_AimLineAttack (mo, an, 16*64*FRACUNIT);
		}
	}
}

/*
===============
=
= P_GunShot
=
===============
*/

void P_GunShot (mobj_t *mo, boolean accurate)
{
	angle_t		angle;
	int			damage;
	
	damage = 5*(P_Random ()%3+1);
	angle = mo->angle;
	if (!accurate)
		angle += (P_Random()-P_Random())<<18;
	P_LineAttack (mo, angle, MISSILERANGE, bulletslope, damage);
}

/* 
================== 
= 
= A_FirePistol
= 
================== 
*/ 
 
void A_FirePistol (player_t *player, pspdef_t *psp) 
{
	S_StartSound (player->mo, sfx_pistol);

	P_SetMobjState (player->mo, S_PLAY_ATK2);
	player->ammo[weaponinfo[player->readyweapon].ammo]--;
	P_SetPsprite (player,ps_flash,weaponinfo[player->readyweapon].flashstate);

	P_BulletSlope (player->mo);
	P_GunShot (player->mo, !player->refire);
}

/* 
================== 
= 
= A_FireShotgun
= 
================== 
*/ 
 
void A_FireShotgun (player_t *player, pspdef_t *psp) 
{
	int			i;
	
	S_StartSound (player->mo, sfx_shotgn);
    P_SetMobjState (player->mo, S_PLAY_ATK2);

	player->ammo[weaponinfo[player->readyweapon].ammo]--;
	P_SetPsprite (player,ps_flash,weaponinfo[player->readyweapon].flashstate);
	
    P_BulletSlope (player->mo);

	for (i=0 ; i<7 ; i++)
		P_GunShot (player->mo, false);
}

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
/* 
================== 
= 
= A_FireShotgun2
= 
================== 
*/ 
 
void A_FireShotgun2 (player_t *player, pspdef_t *psp) 
{
	int			i;
	angle_t		angle;
	int			damage;
	
	S_StartSound (player->mo, sfx_dshtgn);
    P_SetMobjState (player->mo, S_PLAY_ATK2);

	player->ammo[weaponinfo[player->readyweapon].ammo]-=2;
	P_SetPsprite (player,ps_flash,weaponinfo[player->readyweapon].flashstate);
	
    P_BulletSlope (player->mo);

	for (i=0 ; i<20 ; i++)
    {
		damage = 5*(P_Random ()%3+1);
		angle = player->mo->angle;
		angle += (P_Random()-P_Random())<<19;
		P_LineAttack (player->mo, angle, MISSILERANGE,
			bulletslope + ((P_Random()-P_Random())<<5), damage);
    }
}
#endif

/* 
================== 
= 
= A_FireCGun
= 
================== 
*/ 
 
void A_FireCGun (player_t *player, pspdef_t *psp) 
{
	S_StartSound (player->mo, sfx_pistol);

	if (!player->ammo[weaponinfo[player->readyweapon].ammo])
		return;
	
	P_SetMobjState (player->mo, S_PLAY_ATK2);	
	player->ammo[weaponinfo[player->readyweapon].ammo]--;

	P_SetPsprite (player,ps_flash,weaponinfo[player->readyweapon].flashstate
	+ psp->state - &states[S_CHAIN1] );

	P_BulletSlope (player->mo);

	P_GunShot (player->mo, !player->refire);
}


/*============================================================================= */


void A_Light0 (player_t *player, pspdef_t *psp)
{
	player->extralight = 0;
}

void A_Light1 (player_t *player, pspdef_t *psp)
{
	player->extralight = 1;
}

void A_Light2 (player_t *player, pspdef_t *psp)
{
	player->extralight = 2;
}

/*
================
=
= A_BFGSpray
=
= Spawn a BFG explosion on every monster in view
=
=================
*/

void A_BFGSpray (mobj_t *mo) 
{
	int			i, j, damage;
	angle_t		an;
	
	// offset angles from its attack angle
	for (i=0 ; i<40 ; i++)
	{
		an = mo->angle - ANG90/2 + ANG90/40*i;
		// mo->target is the originator (player) of the missile
		P_AimLineAttack (mo->target, an, 16*64*FRACUNIT);
		if (!linetarget)
			continue;
		P_SpawnMobj (linetarget->x, linetarget->y, linetarget->z + (linetarget->height>>2), MT_EXTRABFG);
		damage = 0;
		for (j=0;j<15;j++)
			damage += (P_Random()&7) + 1;
		P_DamageMobj (linetarget, mo->target,mo->target, damage);
	}
}


/*
================
=
= A_BFGsound
=
=================
*/

void A_BFGsound (player_t *player, pspdef_t *psp)
{
	S_StartSound (player->mo, sfx_bfg);
}

/*============================================================================= */

/* 
================== 
= 
= P_SetupPsprites
= 
= Called at start of level for each player
================== 
*/ 
 
void P_SetupPsprites (player_t *player) 
{
	int	i;
	
// remove all psprites

	for (i=0 ; i<NUMPSPRITES ; i++)
		player->psprites[i].state = NULL;
		
// spawn the gun
	player->pendingweapon = player->readyweapon;
	P_BringUpWeapon (player);
}



/* 
================== 
= 
= P_MovePsprites 
= 
= Called every tic by player thinking routine
================== 
*/ 

void P_MovePsprites (player_t *player) 
{
	int			i;
	pspdef_t	*psp;
	state_t		*state;

	psp = &player->psprites[0];
	for (i=0 ; i<NUMPSPRITES ; i++, psp++)
		if ( (state = psp->state) != 0)		// a null state means not active
		{
		// drop tic count and possibly change state
			if (psp->tics != -1)	// a -1 tic count never changes
			{
				psp->tics--;
				if (!psp->tics)
					P_SetPsprite (player, i, psp->state->nextstate);
			}				
		}
	
	player->psprites[ps_flash].sx = player->psprites[ps_weapon].sx;
	player->psprites[ps_flash].sy = player->psprites[ps_weapon].sy;
}
