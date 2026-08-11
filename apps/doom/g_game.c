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

// G_game.c

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "DoomDef.h"
#include "P_local.h"
#include "soundst.h"

#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6;
extern int _wp7, _wp8, _wp9, _wp10, _wp11, _wp12;
extern int _wp13, _wp14, _wp15, _wp16, _wp17, _wp18;
extern int _wp19, _wp20;
#else
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9, _wp10, _wp11, _wp12, _wp13, _wp14, _wp15;
#endif

boolean G_CheckDemoStatus (void);
void G_ReadDemoTiccmd (ticcmd_t *cmd);
void G_WriteDemoTiccmd (ticcmd_t *cmd);
void G_PlayerReborn (int player);
void G_InitNew (skill_t skill, int episode, int map);

void G_DoReborn (int playernum);

void G_DoLoadLevel (void);
void G_DoNewGame (void);
void G_DoLoadGame (void);
void G_DoPlayDemo (void);
void G_DoCompleted (void);
void G_DoVictory (void);
void G_DoWorldDone (void);
void G_DoSaveGame (void);

void D_PageTicker(void);
void D_AdvanceDemo(void);


gameaction_t    gameaction;
gamestate_t     gamestate;
skill_t         gameskill;
#if (APPVER_DOOMREV >= AV_DR_DM12)
boolean         respawnmonsters;
#endif
int             gameepisode;
int             gamemap;

boolean         paused;
boolean         sendpause;              // send a pause event next tic
boolean         sendsave;               // send a save event next tic
boolean         usergame;               // ok to save / end game

boolean         timingdemo;             // if true, exit with report on completion
boolean         nodrawers;              // for comparative timing purposes 
boolean         noblit;                 // for comparative timing purposes 
int             starttime;              // for comparative timing purposes

boolean         viewactive;

boolean         deathmatch;             // only if started as net death
boolean         netgame;                // only true if packets are broadcast
boolean         playeringame[MAXPLAYERS];
player_t        players[MAXPLAYERS];

int             consoleplayer;          // player taking events and displaying
int             displayplayer;          // view being displayed
int             gametic;
int             levelstarttic;          // gametic at level start
int             totalkills, totalitems, totalsecret;    // for intermission

char            demoname[32];
boolean         demorecording;
boolean         demoplayback;
boolean			netdemo;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
byte            *demobuffer, *demo_p;
#else
byte            *demobuffer, *demo_p, *demoend;
#endif
boolean         singledemo;             // quit after playing a demo from cmdline

boolean         precache = true;        // if true, load all graphics at start
 
wbstartstruct_t wminfo;               	// parms for world map / intermission 

#if (APPVER_DOOMREV < AV_DR_DM12)
int             consistancy[MAXPLAYERS][BACKUPTICS];
#else
short            consistancy[MAXPLAYERS][BACKUPTICS];
#endif

byte            *savebuffer, *save_p;


//
// controls (have defaults)
//
int             key_right, key_left, key_up, key_down;
int             key_strafeleft, key_straferight;
int             key_fire, key_use, key_strafe, key_speed;

int             mousebfire;
int             mousebstrafe;
int             mousebforward;

int             joybfire;
int             joybstrafe;
int             joybuse;
int             joybspeed;



#if (APPVER_DOOMREV < AV_DR_DM12)
#define MAXPLMOVE       0x19000
#elif (APPVER_DOOMREV < AV_DR_DM1666P)
#define MAXPLMOVE       0x32
#else
#define MAXPLMOVE       (forwardmove[1])
#endif
 
#define TURBOTHRESHOLD	0x32

#if (APPVER_DOOMREV < AV_DR_DM12)
fixed_t         forwardmove[2] = {0xc800, 0x19000};
fixed_t         sidemove[2] = {0xc000, 0x14000};
fixed_t         angleturn[4] = {0x2800000, 0x5000000, 0x1400000};     // + slow turn
#else
fixed_t         forwardmove[2] = {0x19, 0x32};
fixed_t         sidemove[2] = {0x18, 0x28};
fixed_t         angleturn[3] = {640, 1280, 320};     // + slow turn
#endif
#define SLOWTURNTICS    6

#define NUMKEYS 256
boolean         gamekeydown[NUMKEYS];
int             turnheld;                   // for accelerative turning


#if (APPVER_DOOMREV < AV_DR_DM12)
boolean         mousebuttons[3];
#else
boolean         mousearray[4];
boolean         *mousebuttons = &mousearray[1];
#endif
	// allow [-1]
int             mousex, mousey;             // mouse values are used once
int             dclicktime, dclickstate, dclicks;
int             dclicktime2, dclickstate2, dclicks2;

int             joyxmove, joyymove;         // joystick values are repeated
#if (APPVER_DOOMREV < AV_DR_DM12)
boolean         joybuttons[4];
#else
boolean         joyarray[5];
boolean         *joybuttons = &joyarray[1];     // allow [-1]
#endif

int     savegameslot;
char    savedescription[32];
 

#if (APPVER_DOOMREV >= AV_DR_DM12)
#define	BODYQUESIZE	32

mobj_t *bodyque[BODYQUESIZE]; 
int bodyqueslot; 
#endif

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
void *statcopy;				// for statistics driver
#endif


int G_CmdChecksum(ticcmd_t *cmd)
{
	int     i;
	int sum;

	sum = 0;
	for(i = 0; i < sizeof(*cmd)/4-1; i++)
	{
		sum += ((int *)cmd)[i];
	}
	return(sum);
}

/*
====================
=
= G_BuildTiccmd
=
= Builds a ticcmd from all of the available inputs or reads it from the
= demo buffer.
= If recording a demo, write it out
====================
*/

extern  int             isCyberPresent;     // is CyberMan present?
void I_ReadCyberCmd (ticcmd_t *cmd);

void G_BuildTiccmd (ticcmd_t *cmd)
{
	int             i;
#if (APPVER_DOOMREV < AV_DR_DM12)
	boolean         strafe;
#else
	boolean         strafe, bstrafe;
#endif
	int             speed, tspeed;
#if (APPVER_DOOMREV >= AV_DR_DM12)
	int             forward, side;
#endif

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	memset (cmd,0,sizeof(*cmd));
#if (APPVER_DOOMREV < AV_DR_DM12)
	cmd->consistancy =
	      consistancy[consoleplayer][maketic&(BACKUPTICS-1)];
#else
	cmd->consistancy =
	      consistancy[consoleplayer][(maketic*ticdup)%BACKUPTICS];
#endif
#else
	ticcmd_t		*base;

	base = I_BaseTiccmd ();		// empty, or external driver
	memcpy (cmd,base,sizeof(*cmd)); 
	//cmd->consistancy =
	//      consistancy[consoleplayer][(maketic*ticdup)%BACKUPTICS];
	cmd->consistancy =
		consistancy[consoleplayer][maketic%BACKUPTICS];
#endif
#if (APPVER_DOOMREV >= AV_DR_DM12)
	if (isCyberPresent)
		I_ReadCyberCmd (cmd);
#endif

//printf ("cons: %i\n",cmd->consistancy);

	strafe = gamekeydown[key_strafe] || mousebuttons[mousebstrafe]
		|| joybuttons[joybstrafe];
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	speed = gamekeydown[key_speed] || joybuttons[joybspeed]
		|| joybuttons[joybspeed];
#else
	speed = gamekeydown[key_speed] || joybuttons[joybspeed];
#endif

#if (APPVER_DOOMREV < AV_DR_DM12)
#define forward cmd->forwardmove
#define side cmd->sidemove
#else
	forward = side = 0;
#endif

//
// use two stage accelerative turning on the keyboard and joystick
//
	if (joyxmove < 0 || joyxmove > 0
	|| gamekeydown[key_right] || gamekeydown[key_left])
#if (APPVER_DOOMREV < AV_DR_DM12)
		turnheld++;
#else
		turnheld += ticdup;
#endif
	else
		turnheld = 0;
	if (turnheld < SLOWTURNTICS)
		tspeed = 2;             // slow turn
	else
		tspeed = speed;

//
// let movement keys cancel each other out
//
	if(strafe)
	{
		if (gamekeydown[key_right])
			side += sidemove[speed];
		if (gamekeydown[key_left])
			side -= sidemove[speed];
		if (joyxmove > 0)
			side += sidemove[speed];
		if (joyxmove < 0)
			side -= sidemove[speed];
	}
	else
	{
		if (gamekeydown[key_right])
			cmd->angleturn -= angleturn[tspeed];
		if (gamekeydown[key_left])
			cmd->angleturn += angleturn[tspeed];
		if (joyxmove > 0)
			cmd->angleturn -= angleturn[tspeed];
		if (joyxmove < 0)
			cmd->angleturn += angleturn[tspeed];
	}

	if (gamekeydown[key_up])
		forward += forwardmove[speed];
	if (gamekeydown[key_down])
		forward -= forwardmove[speed];
	if (joyymove < 0)
		forward += forwardmove[speed];
	if (joyymove > 0)
		forward -= forwardmove[speed];
	if (gamekeydown[key_straferight])
		side += sidemove[speed];
	if (gamekeydown[key_strafeleft])
		side -= sidemove[speed];

//
// buttons
//
	cmd->chatchar = HU_dequeueChatChar();

#if (APPVER_DOOMREV < AV_DR_DM12)
	cmd->buttons = 0;
#endif

	if (gamekeydown[key_fire] || mousebuttons[mousebfire]
		|| joybuttons[joybfire])
		cmd->buttons |= BT_ATTACK;

	if (gamekeydown[key_use] || joybuttons[joybuse] )
	{
		cmd->buttons |= BT_USE;
		dclicks = 0;                    // clear double clicks if hit use button
	}

//
// chainsaw overrides 
//
	for(i = 0; i < NUMWEAPONS-1; i++)
	{
		if(gamekeydown['1'+i])
		{
			cmd->buttons |= BT_CHANGE;
			cmd->buttons |= i<<BT_WEAPONSHIFT;
			break;
		}
	}

//
// mouse
//
	if (mousebuttons[mousebforward])
	{
		forward += forwardmove[speed];
	}

//
// forward double click
//
	if (mousebuttons[mousebforward] != dclickstate && dclicktime > 1 )
	{
		dclickstate = mousebuttons[mousebforward];
		if (dclickstate)
			dclicks++;
		if (dclicks == 2)
		{
			cmd->buttons |= BT_USE;
			dclicks = 0;
		}
		else
			dclicktime = 0;
	}
	else
	{
#if (APPVER_DOOMREV < AV_DR_DM12)
		dclicktime++;
#else
		dclicktime += ticdup;
#endif
		if (dclicktime > 20)
		{
			dclicks = 0;
			dclickstate = 0;
		}
	}

//
// strafe double click
//
#if (APPVER_DOOMREV < AV_DR_DM12)
	if (mousebuttons[mousebstrafe] != dclickstate2 && dclicktime2 > 1 )
	{
		dclickstate2 = mousebuttons[mousebstrafe];
#else
	bstrafe = mousebuttons[mousebstrafe]
|| joybuttons[joybstrafe];
	if (bstrafe != dclickstate2 && dclicktime2 > 1 )
	{
		dclickstate2 = bstrafe;
#endif
		if (dclickstate2)
			dclicks2++;
		if (dclicks2 == 2)
		{
			cmd->buttons |= BT_USE;
			dclicks2 = 0;
		}
		else
			dclicktime2 = 0;
	}
	else
	{
#if (APPVER_DOOMREV < AV_DR_DM12)
		dclicktime2++;
#else
		dclicktime2 += ticdup;
#endif
		if (dclicktime2 > 20)
		{
			dclicks2 = 0;
			dclickstate2 = 0;
		}
	}

#if (APPVER_DOOMREV < AV_DR_DM12)
	cmd->forwardmove += mousey * 2048;
	if (strafe)
		side += mousex * 3072;
	else
		cmd->angleturn -= mousex << 19;
#else
	forward += mousey;
	if (strafe)
		side += mousex*2;
	else
		cmd->angleturn -= mousex*0x8;
#endif

	mousex = mousey = 0;

	if (forward > MAXPLMOVE)
		forward = MAXPLMOVE;
	else if (forward < -MAXPLMOVE)
		forward = -MAXPLMOVE;
	if (side > MAXPLMOVE)
		side = MAXPLMOVE;
	else if (side < -MAXPLMOVE)
		side = -MAXPLMOVE;

#if (APPVER_DOOMREV < AV_DR_DM12)
#undef forward
#undef side
#else
	cmd->forwardmove += forward;
	cmd->sidemove += side;
#endif

//
// special buttons
//
	if (sendpause)
	{
		sendpause = false;
		cmd->buttons = BT_SPECIAL | BTS_PAUSE;
	}

	if (sendsave)
	{
		sendsave = false;
#if (APPVER_DOOMREV < AV_DR_DM12)
		cmd->buttons = BT_SPECIAL | BTS_SAVEGAME | (savegameslot<<8);
#else
		cmd->buttons = BT_SPECIAL | BTS_SAVEGAME | (savegameslot<<BTS_SAVESHIFT);
#endif
	}

#if (APPVER_DOOMREV < AV_DR_DM12)
	cmd->f_18 = G_CmdChecksum(cmd);
#endif
}


/*
==============
=
= G_DoLoadLevel
=
==============
*/

void G_DoLoadLevel (void)
{
	int             i;

#if (APPVER_DOOMREV >= AV_DR_DM19F2) // "Sky never changes in Doom II" bug fix
	if (commercial)
	{
		skytexture = R_TextureNumForName ("SKY3");
		if (gamemap < 12)
			skytexture = R_TextureNumForName ("SKY1");
		else
			if (gamemap < 21)
				skytexture = R_TextureNumForName ("SKY2");
	}
#endif

	levelstarttic = gametic;        // for time calculation
	
	if (wipegamestate == GS_LEVEL) 
		wipegamestate = -1;             // force a wipe 

	gamestate = GS_LEVEL;
	for (i=0 ; i<MAXPLAYERS ; i++)
	{
		if (playeringame[i] && players[i].playerstate == PST_DEAD)
			players[i].playerstate = PST_REBORN;
		memset (players[i].frags,0,sizeof(players[i].frags));
	}

	P_SetupLevel (gameepisode, gamemap, 0, gameskill);
	displayplayer = consoleplayer;      // view the guy you are playing
	starttime = I_GetTime ();
	gameaction = ga_nothing;
	Z_CheckHeap ();

//
// clear cmd building stuff
//

	memset (gamekeydown, 0, sizeof(gamekeydown));
	joyxmove = joyymove = 0;
	mousex = mousey = 0;
	sendpause = sendsave = paused = false;
	memset (mousebuttons, 0, sizeof(mousebuttons));
	memset (joybuttons, 0, sizeof(joybuttons));
}


/*
===============================================================================
=
= G_Responder
=
= get info needed to make ticcmd_ts for the players
=
===============================================================================
*/

boolean G_Responder(event_t *ev)
{
	// allow spy mode changes even during the demo
	if(gamestate == GS_LEVEL && ev->type == ev_keydown
#if (APPVER_DOOMREV < AV_DR_DM1666P)
		&& ev->data1 == KEY_F12 && !deathmatch )
#else
		&& ev->data1 == KEY_F12 && (singledemo || !deathmatch) )
#endif
	{
		// spy mode 
		do
		{
			displayplayer++;
			if(displayplayer == MAXPLAYERS)
			{
				displayplayer = 0;
			}
		} while(!playeringame[displayplayer]
			&& displayplayer != consoleplayer);
		return(true);
	}
    
	// any other key pops up menu if in demos
	if (
#if (APPVER_DOOMREV < AV_DR_DM12)
#elif (APPVER_DOOMREV < AV_DR_DM1666P)
		gameaction == ga_nothing &&
#else
		gameaction == ga_nothing && !singledemo &&
#endif
		(demoplayback || gamestate == GS_DEMOSCREEN) 
		) 
	{ 
		if (ev->type == ev_keydown ||  
			(ev->type == ev_mouse && ev->data1) || 
			(ev->type == ev_joystick && ev->data1) ) 
		{ 
			M_StartControlPanel (); 
			return true; 
		} 
		return false; 
	} 

	if(gamestate == GS_LEVEL)
	{
#if 0 
		if (devparm && ev->type == ev_keydown && ev->data1 == ';') 
		{ 
			G_DeathMatchSpawnPlayer (0); 
			return true; 
		} 
#endif 
		if (HU_Responder(ev))
		{
			return true;	// chat ate the event
		}
		if (ST_Responder(ev))
		{
			return true;	// status window ate it
		}
		if (AM_Responder(ev))
		{
			return true;	// automap ate it
		}
	}

	if (gamestate == GS_FINALE) 
	{ 
		if (F_Responder(ev))
		{
			return true;	// finale ate the event
		}
	} 

	switch(ev->type)
	{
		case ev_keydown:
			if(ev->data1 == KEY_PAUSE)
			{
				sendpause = true;
				return(true);
			}
			if(ev->data1 < NUMKEYS)
			{
				gamekeydown[ev->data1] = true;
			}
			return(true); // eat key down events

		case ev_keyup:
			if(ev->data1 < NUMKEYS)
			{
				gamekeydown[ev->data1] = false;
			}
			return(false); // always let key up events filter down

		case ev_mouse:
			mousebuttons[0] = ev->data1&1;
			mousebuttons[1] = ev->data1&2;
			mousebuttons[2] = ev->data1&4;
			mousex = ev->data2*(mouseSensitivity+5)/10;
			mousey = ev->data3*(mouseSensitivity+5)/10;
			return(true); // eat events

		case ev_joystick:
			joybuttons[0] = ev->data1&1;
			joybuttons[1] = ev->data1&2;
			joybuttons[2] = ev->data1&4;
			joybuttons[3] = ev->data1&8;
			joyxmove = ev->data2;
			joyymove = ev->data3;
			return(true); // eat events

		default:
			break;
	}
	return(false);
}

/*
===============================================================================
=
= G_Ticker
=
===============================================================================
*/

void G_Ticker (void)
{
	int                     i, buf;
	ticcmd_t        *cmd;

//
// do player reborns if needed
//
	for (i=0 ; i<MAXPLAYERS ; i++)
		if (playeringame[i] && players[i].playerstate == PST_REBORN)
			G_DoReborn (i);

//
// do things to change the game state
//
	while (gameaction != ga_nothing)
	{
		switch (gameaction)
		{
		case ga_loadlevel:
			G_DoLoadLevel ();
			break;
		case ga_newgame:
			G_DoNewGame ();
			break;
		case ga_loadgame:
			G_DoLoadGame ();
			break;
		case ga_savegame:
			G_DoSaveGame ();
			break;
		case ga_playdemo:
			G_DoPlayDemo ();
			break;
		case ga_completed:
			G_DoCompleted ();
			break;
		case ga_victory:
			F_StartFinale();
			break;
		case ga_worlddone:
			G_DoWorldDone();
			break;
		case ga_screenshot:
			M_ScreenShot ();
			gameaction = ga_nothing;
			break;
		case ga_nothing:
			break;
		}
	}


//
// get commands, check consistancy, and build new consistancy check
//
#if (APPVER_DOOMREV < AV_DR_DM12)
	buf = gametic&(BACKUPTICS-1);
#elif (APPVER_DOOMREV < AV_DR_DM1666P)
	buf = gametic%BACKUPTICS;
#else
	buf = (gametic/ticdup)%BACKUPTICS;
#endif

	for (i=0 ; i<MAXPLAYERS ; i++)
		if (playeringame[i])
		{
			cmd = &players[i].cmd;

			memcpy (cmd, &netcmds[i][buf], sizeof(ticcmd_t));

#if (APPVER_DOOMREV < AV_DR_DM12)
			if (cmd->f_18 != G_CmdChecksum(cmd))
				I_Error ("G_Ticker: failed cmd checksum!");
#endif

			if (demoplayback)
				G_ReadDemoTiccmd (cmd);
			if (demorecording)
				G_WriteDemoTiccmd (cmd);

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
			//
			// check for turbo cheats
			//
			if (cmd->forwardmove > TURBOTHRESHOLD && !(gametic&31) && ((gametic>>5)&3) == i )
			{
				static char turbomessage[80];
				extern char *player_names[4];
				sprintf (turbomessage, "%s is turbo!",player_names[i]);
				players[consoleplayer].message = turbomessage;
			}
#endif

			if (netgame && !netdemo
#if (APPVER_DOOMREV >= AV_DR_DM12)
				&& !(gametic%ticdup)
#endif
				)
			{
				if (gametic > BACKUPTICS
				&& consistancy[i][buf] != cmd->consistancy)
				{
#if (APPVER_DOOMREV < AV_DR_DM12)
					I_Error ("consistency failure");
#else
					I_Error ("consistency failure (%i should be %i)",cmd->consistancy, consistancy[i][buf]);
#endif
				}
				if (players[i].mo)
					consistancy[i][buf] = players[i].mo->x;
				else
					consistancy[i][buf] = rndindex;
			}
		}

//
// check for special buttons
//
	for (i=0 ; i<MAXPLAYERS ; i++)
		if (playeringame[i])
		{
			if (players[i].cmd.buttons & BT_SPECIAL)
			{
				switch (players[i].cmd.buttons & BT_SPECIALMASK)
				{
				case BTS_PAUSE:
					paused ^= 1;
					if(paused)
					{
						S_PauseSound();
					}
					else
					{
						S_ResumeSound();
					}
					break;

				case BTS_SAVEGAME:
					if (!savedescription[0])
					{
						strcpy (savedescription, "NET GAME");
					}
#if (APPVER_DOOMREV < AV_DR_DM12)
					savegameslot = players[i].cmd.buttons >> 8;
#else
					savegameslot =
						(players[i].cmd.buttons & BTS_SAVEMASK)>>BTS_SAVESHIFT;
#endif
					gameaction = ga_savegame;
					break;
				}
			}
		}
//
// do main actions
//
	switch (gamestate)
	{
		case GS_LEVEL:
			P_Ticker ();
			ST_Ticker ();
			AM_Ticker ();
			HU_Ticker ();
			break;
		case GS_INTERMISSION:
			WI_Ticker ();
			break;
		case GS_FINALE:
			F_Ticker ();
			break;
		case GS_DEMOSCREEN:
			D_PageTicker ();
			break;
	}
}


/*
==============================================================================

						PLAYER STRUCTURE FUNCTIONS

also see P_SpawnPlayer in P_Things
==============================================================================
*/

/*
====================
=
= G_InitPlayer
=
= Called at the start
= Called by the game initialization functions
====================
*/

void G_InitPlayer (int player)
{
	player_t        *p;

// set up the saved info
	p = &players[player];

// clear everything else to defaults
	G_PlayerReborn (player);

}


/*
====================
=
= G_PlayerFinishLevel
=
= Can when a player completes a level
====================
*/

void G_PlayerFinishLevel(int player)
{
	player_t *p;

	p = &players[player];

	memset(p->powers, 0, sizeof(p->powers));
	memset(p->cards, 0, sizeof(p->cards));
	p->mo->flags &= ~MF_SHADOW; // cancel invisibility
	p->extralight = 0; // cancel gun flashes
	p->fixedcolormap = 0; // cancel ir gogles
	p->damagecount = 0; // no palette changes
	p->bonuscount = 0;
}

/*
====================
=
= G_PlayerReborn
=
= Called after a player dies
= almost everything is cleared and initialized
====================
*/

void G_PlayerReborn(int player)
{
	player_t *p;
	int i;
	int frags[MAXPLAYERS];
	int killcount, itemcount, secretcount;

	memcpy(frags, players[player].frags, sizeof(frags));
	killcount = players[player].killcount;
	itemcount = players[player].itemcount;
	secretcount = players[player].secretcount;

	p = &players[player];
	memset(p, 0, sizeof(*p));

	memcpy(players[player].frags, frags, sizeof(players[player].frags));
	players[player].killcount = killcount;
	players[player].itemcount = itemcount;
	players[player].secretcount = secretcount;

	p->usedown = p->attackdown = true; // don't do anything immediately
	p->playerstate = PST_LIVE;
	p->health = MAXHEALTH;
	p->readyweapon = p->pendingweapon = wp_pistol;
	p->weaponowned[wp_fist] = true;
	p->weaponowned[wp_pistol] = true;
	p->ammo[am_clip] = 50;
	for(i = 0; i < NUMAMMO; i++)
	{
		p->maxammo[i] = maxammo[i];
	}
}

/*
====================
=
= G_CheckSpot
=
= Returns false if the player cannot be respawned at the given mapthing_t spot
= because something is occupying it
====================
*/

void P_SpawnPlayer (mapthing_t *mthing);

boolean G_CheckSpot (int playernum, mapthing_t *mthing)
{
	fixed_t         x,y;
	subsector_t *ss;
	unsigned        an;
	mobj_t      *mo;
	int         i;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (!players[playernum].mo)
	{
		// first spawn of level, before corpses
		for (i=0 ; i<playernum ; i++)
			if (players[i].mo->x == mthing->x << FRACBITS
				&& players[i].mo->y == mthing->y << FRACBITS)
				return false;	
		return true;
	}
#endif

	x = mthing->x << FRACBITS;
	y = mthing->y << FRACBITS;

	if (!P_CheckPosition (players[playernum].mo, x, y) )
		return false;
 
// flush an old corpse if needed 
#if (APPVER_DOOMREV >= AV_DR_DM12)
	if (bodyqueslot >= BODYQUESIZE) 
		P_RemoveMobj (bodyque[bodyqueslot%BODYQUESIZE]); 
	bodyque[bodyqueslot%BODYQUESIZE] = players[playernum].mo; 
	bodyqueslot++; 
#endif

// spawn a teleport fog
	ss = R_PointInSubsector (x,y);
	an = ( ANG45 * (mthing->angle/45) ) >> ANGLETOFINESHIFT;

	mo = P_SpawnMobj (x+20*finecosine[an], y+20*finesine[an]
	, ss->sector->floorheight, MT_TFOG);

#if (APPVER_DOOMREV >= AV_DR_DM12)
	if (players[consoleplayer].viewz != 1)
#endif
		S_StartSound (mo, sfx_telept);  // don't start sound on first frame

	return true;
}

/*
====================
=
= G_DeathMatchSpawnPlayer
=
= Spawns a player at one of the random death match spots
= called at level load and each death
====================
*/

void G_DeathMatchSpawnPlayer (int playernum)
{
	int             i,j;
	int             selections;

	selections = deathmatch_p - deathmatchstarts;
	if (selections < 4)
		I_Error ("Only %i deathmatch spots, 4 required", selections);

	for (j=0 ; j<20 ; j++)
	{
		i = P_Random() % selections;
		if (G_CheckSpot (playernum, &deathmatchstarts[i]) )
		{
			deathmatchstarts[i].type = playernum+1;
			P_SpawnPlayer (&deathmatchstarts[i]);
			return;
		}
	}

// no good spot, so the player will probably get stuck
	P_SpawnPlayer (&playerstarts[playernum]);
}

/*
====================
=
= G_DoReborn
=
====================
*/

void G_DoReborn (int playernum)
{
	int                             i;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (G_CheckDemoStatus())
		return;
#endif

	if (!netgame)
		gameaction = ga_loadlevel;                      // reload the level from scratch
	else
	{       // respawn at the start
		players[playernum].mo->player = NULL;   // dissasociate the corpse

		// spawn at random spot if in death match
		if (deathmatch)
		{
			G_DeathMatchSpawnPlayer (playernum);
			return;
		}

		if (G_CheckSpot (playernum, &playerstarts[playernum]) )
		{
			P_SpawnPlayer (&playerstarts[playernum]);
			return;
		}
		// try to spawn at one of the other players spots
		for (i=0 ; i<MAXPLAYERS ; i++)
			if (G_CheckSpot (playernum, &playerstarts[i]) )
			{
				playerstarts[i].type = playernum+1;             // fake as other player
				P_SpawnPlayer (&playerstarts[i]);
				playerstarts[i].type = i+1;                             // restore
				return;
			}
		// he's going to be inside something.  Too bad.
		P_SpawnPlayer (&playerstarts[playernum]);
	}
}


void G_ScreenShot (void)
{
	gameaction = ga_screenshot;
}



// DOOM Par Times
int pars[4][10] =
{
	{0},
#if APPVER_CHEX
	{0,120,360,480,200,360,180,180,30,165},
#else
	{0,30,75,120,90,165,180,180,30,165},
#endif
	{0,90,90,90,120,90,360,240,30,170},
	{0,90,45,90,150,90,90,165,30,135}
};

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
// DOOM II Par Times
int cpars[32] =
{
	30,90,120,120,90,150,120,120,270,90,	//  1-10
	210,150,150,150,210,150,420,150,210,150,	// 11-20
	240,150,180,150,150,300,330,420,300,180,	// 21-30
	120,30					// 31-32
};
#endif

#if (APPVER_DOOMREV < AV_DR_DM12)
int g_unkvar_ggg;
#endif

/*
====================
=
= G_DoCompleted
=
====================
*/

boolean         secretexit;
extern char     *pagename;

void G_ExitLevel (void)
{
	secretexit = false;
	gameaction = ga_completed;
}

// Here's for the german edition
void G_SecretExitLevel (void)
{
#if (APPVER_DOOMREV >= AV_DR_DM1666E)
	// IF NO WOLF3D LEVELS, NO SECRET EXIT!
	if (commercial && (W_CheckNumForName("map31")<0))
		secretexit = false;
	else
#endif
		secretexit = true; 
	gameaction = ga_completed;
}

void G_DoCompleted(void)
{
	int i;

	gameaction = ga_nothing;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (G_CheckDemoStatus())
		return;
#endif

	for(i = 0; i < MAXPLAYERS; i++)
	{
		if(playeringame[i])
		{
			G_PlayerFinishLevel(i);        // take away cards and stuff 
		}
	}

	if (automapactive)
		AM_Stop();

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (gamemap == 8)
	{
		// victory 
		gameaction = ga_victory;
		return;
	}

	if (gamemap == 9)
	{
		// exit secret level 
		for (i = 0; i < MAXPLAYERS; i++)
			players[i].didsecret = true;
	}
#else
	if (!commercial)
	{
		switch (gamemap)
		{
#if APPVER_CHEX
			case 5:
#else
			case 8:
#endif
				gameaction = ga_victory;
				return;
			case 9:
				for (i = 0; i < MAXPLAYERS; i++)
					players[i].didsecret = true;
				break;
		}
	}

#if 0
	if ((gamemap == 8) && !commercial)
	{
		// victory 
		gameaction = ga_victory;
		return;
	}

	if ((gamemap == 9) && !commercial)
	{
		// exit secret level 
		for (i = 0; i < MAXPLAYERS; i++)
			players[i].didsecret = true;
	}
#endif
#endif


	wminfo.didsecret = players[consoleplayer].didsecret;
	wminfo.epsd = gameepisode - 1;
	wminfo.last = gamemap - 1;

// wminfo.next is 0 biased, unlike gamemap
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (commercial)
	{
		if (secretexit)
			switch (gamemap)
			{
				case 15: wminfo.next = 30; break;
				case 31: wminfo.next = 31; break;
			}
		else
			switch (gamemap)
			{
				case 31:
				case 32: wminfo.next = 15; break;
				default: wminfo.next = gamemap;
			}
	}
	else
#endif
	{
		if (secretexit)
			wminfo.next = 8; 	// go to secret level 
		else if (gamemap == 9)
		{
			// returning from secret level 
			switch (gameepisode)
			{
				case 1:
					wminfo.next = 3;
					break;
				case 2:
					wminfo.next = 5;
					break;
				case 3:
					wminfo.next = 6;
					break;
#if (APPVER_DOOMREV >= AV_DR_DM19U)
				case 4:
					wminfo.next = 2;
					break;
#endif
			}
		}
		else
			wminfo.next = gamemap;          // go to next level 
	}

	wminfo.maxkills = totalkills;
	wminfo.maxitems = totalitems;
	wminfo.maxsecret = totalsecret;
	wminfo.maxfrags = 0;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (commercial)
		wminfo.partime = 35*cpars[gamemap-1];
	else
#endif
		wminfo.partime = 35*pars[gameepisode][gamemap];
	wminfo.pnum = consoleplayer;

	for (i=0 ; i<MAXPLAYERS ; i++) 
	{
		wminfo.plyr[i].in = playeringame[i];
		wminfo.plyr[i].skills = players[i].killcount;
		wminfo.plyr[i].sitems = players[i].itemcount;
		wminfo.plyr[i].ssecret = players[i].secretcount;
		wminfo.plyr[i].stime = leveltime;
		memcpy (wminfo.plyr[i].frags, players[i].frags
				, sizeof(wminfo.plyr[i].frags));
	}

	gamestate = GS_INTERMISSION;
	viewactive = false;
	automapactive = false;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (statcopy)
		memcpy(statcopy, &wminfo, sizeof(wminfo));
#endif

	WI_Start(&wminfo);
}

//============================================================================
//
// G_WorldDone
//
//============================================================================

void G_WorldDone(void)
{
	gameaction = ga_worlddone;
	if (secretexit)
		players[consoleplayer].didsecret = true;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (commercial)
	{
		switch (gamemap)
		{
			case 15:
			case 31:
				if (!secretexit)
					break;
			case 6:
			case 11:
			case 20:
			case 30:
				F_StartFinale ();
				break;
		}
	}
#endif
}

void G_DoWorldDone(void)
{
	gamestate = GS_LEVEL;
	gamemap = wminfo.next+1;
	G_DoLoadLevel();
	gameaction = ga_nothing;
	viewactive = true;
}

/*
====================
=
= G_InitFromSavegame
=
= Can be called by the startup code or the menu task. 
=
====================
*/

extern boolean setsizeneeded;
void R_ExecuteSetViewSize (void);

char	savename[256];

void G_LoadGame (char* name) 
{ 
	strcpy (savename, name); 
	gameaction = ga_loadgame; 
}

#if (APPVER_DOOMREV >= AV_DR_DM12)
#define VERSIONSIZE		16
#endif

void G_DoLoadGame(void)
{
	int length;
	int i;
	int a, b, c;
#if (APPVER_DOOMREV >= AV_DR_DM12)
	char vcheck[VERSIONSIZE];
#endif

	gameaction = ga_nothing;

	length = M_ReadFile(savename, &savebuffer);
	save_p = savebuffer+SAVESTRINGSIZE;
	// Skip the description field
#if (APPVER_DOOMREV >= AV_DR_DM12)
	memset(vcheck, 0, sizeof(vcheck));
	sprintf(vcheck, "version %i", VERSION);
	if (strcmp (save_p, vcheck))
	{ // Bad version
		return;
	}
	save_p += VERSIONSIZE;
#endif
	gameskill = *save_p++;
	gameepisode = *save_p++;
	gamemap = *save_p++;
	for(i = 0; i < MAXPLAYERS; i++)
	{
		playeringame[i] = *save_p++;
	}
	// Load a base level
	G_InitNew(gameskill, gameepisode, gamemap);

	// Get the times
	a = *save_p++;
	b = *save_p++;
	c = *save_p++;
	leveltime = (a<<16)+(b<<8)+c;

	// De-archive all the modifications
	P_UnArchivePlayers();
	P_UnArchiveWorld();
	P_UnArchiveThinkers();
	P_UnArchiveSpecials();

	if(*save_p != 0x1d)
	{
		I_Error("Bad savegame");
	}
	Z_Free(savebuffer);

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (setsizeneeded)
		R_ExecuteSetViewSize ();

	// Draw the pattern into the back screen
	R_FillBackScreen();
#endif
}


//==========================================================================
//
// G_SaveGame
//
// Called by the menu task.  <description> is a 24 byte text string.
//
//==========================================================================

void G_SaveGame(int slot, char *description)
{
	savegameslot = slot;
	strcpy(savedescription, description);
	sendsave = true;
}

//==========================================================================
//
// G_DoSaveGame
//
// Called by G_Ticker based on gameaction.
//
//==========================================================================

void G_DoSaveGame(void)
{
	char name[100];
#if (APPVER_DOOMREV >= AV_DR_DM12)
	char name2[VERSIONSIZE];
#endif
	char *description;
	int length;
	int i;

#if (APPVER_DOOMREV >= AV_DR_DM1666E)
	if (M_CheckParm("-cdrom"))
	{
		sprintf(name, "c:\\doomdata\\"SAVEGAMENAME"%d.dsg", savegameslot);
	}
	else
#endif
	{
		sprintf(name, SAVEGAMENAME"%d.dsg", savegameslot);
	}
	description = savedescription;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	save_p = savebuffer = screens[2];
#else
	save_p = savebuffer = screens[1]+0x4000;
#endif

	memcpy(save_p, description, SAVESTRINGSIZE);
	save_p += SAVESTRINGSIZE;
#if (APPVER_DOOMREV >= AV_DR_DM12)
	memset (name2,0,sizeof(name2));
	sprintf (name2,"version %i",VERSION);
	memcpy (save_p, name2, VERSIONSIZE);
	save_p += VERSIONSIZE;
#endif

	*save_p++ = gameskill;
	*save_p++ = gameepisode;
	*save_p++ = gamemap;
	for(i = 0; i < MAXPLAYERS; i++)
	{
		*save_p++ = playeringame[i];
	}
	*save_p++ = leveltime>>16;
	*save_p++ = leveltime>>8;
	*save_p++ = leveltime;

	P_ArchivePlayers();
	P_ArchiveWorld();
	P_ArchiveThinkers();
	P_ArchiveSpecials();
	
	*save_p++ = 0x1d;		// consistancy marker
	 
	length = save_p - savebuffer;
	if (length > SAVEGAMESIZE)
		I_Error ("Savegame buffer overrun");
	M_WriteFile (name, savebuffer, length);
	gameaction = ga_nothing;
	savedescription[0] = 0;

	players[consoleplayer].message = GGSAVED;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	// Draw the pattern into the back screen
	R_FillBackScreen();
#endif
}


/*
====================
=
= G_InitNew
=
= Can be called by the startup code or the menu task
= consoleplayer, displayplayer, playeringame[] should be set
====================
*/

#if (APPVER_DOOMREV < AV_DR_DM12)
extern int ___wp1;
#endif

skill_t d_skill;
int     d_episode;
int     d_map;

void G_DeferedInitNew (skill_t skill, int episode, int map)
{
	d_skill = skill;
	d_episode = episode;
	d_map = map;
	gameaction = ga_newgame;
}

void G_DoNewGame (void)
{
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	demoplayback = false;
	netdemo = false;
	netgame = false;
	deathmatch = false;
	playeringame[1] = playeringame[2] = playeringame[3] = 0;
	respawnparm = false;
	fastparm = false;
	nomonsters = false;
	consoleplayer = 0;
#endif
	G_InitNew (d_skill, d_episode, d_map);
	gameaction = ga_nothing;
}

extern  int                     skytexture;

void G_InitNew(skill_t skill, int episode, int map)
{
	int i;

	if(paused)
	{
		paused = false;
		S_ResumeSound();
	}


#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (skill < sk_baby)
		skill = sk_baby;
#endif
#if (APPVER_DOOMREV < AV_DR_DM12)
	if (skill > sk_hard)
		skill = sk_hard;
#else
	if(skill > sk_nightmare)
		skill = sk_nightmare;
#endif
#if (APPVER_DOOMREV < AV_DR_DM19UP)
	if(episode < 1)
		episode = 1;
	if(episode > 3)
		episode = 3;
#else
	if(episode == 0)
		episode = 4;
#endif
	if(episode > 1 && shareware)
		episode = 1;
	if(map < 1)
		map = 1;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if(map > 9)
#else
	if(map > 9 && !commercial)
#endif
		map = 9;
	M_ClearRandom();
#if (APPVER_DOOMREV >= AV_DR_DM12)
	if(skill == sk_nightmare || respawnparm)
	{
		respawnmonsters = true;
	}
	else
	{
		respawnmonsters = false;
	}

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (skill == sk_nightmare && gameskill != sk_nightmare)
#else
	if (fastparm || (skill == sk_nightmare && gameskill != sk_nightmare))
#endif
	{
		for (i=S_SARG_RUN1 ; i<=S_SARG_PAIN2 ; i++)
			states[i].tics >>= 1;
		mobjinfo[MT_BRUISERSHOT].speed = 20*FRACUNIT;
		mobjinfo[MT_HEADSHOT].speed = 20*FRACUNIT;
		mobjinfo[MT_TROOPSHOT].speed = 20*FRACUNIT;
	}
	else if (skill != sk_nightmare && gameskill == sk_nightmare)
	{
		for (i=S_SARG_RUN1 ; i<=S_SARG_PAIN2 ; i++)
			states[i].tics <<= 1;
		mobjinfo[MT_BRUISERSHOT].speed = 15*FRACUNIT;
		mobjinfo[MT_HEADSHOT].speed = 10*FRACUNIT;
		mobjinfo[MT_TROOPSHOT].speed = 10*FRACUNIT;
	}
#endif


	// Force players to be initialized upon first level load
	for(i = 0; i < MAXPLAYERS; i++)
	{
		players[i].playerstate = PST_REBORN;
	}

	usergame = true; // will be set false if a demo
	paused = false;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	demorecording = false;
#endif
	demoplayback = false;
	automapactive = false;
	viewactive = true;
	gameepisode = episode;
	gamemap = map;
	gameskill = skill;

	viewactive = true;

	// Set the sky map for the episode
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (commercial)
	{
		skytexture = R_TextureNumForName("SKY3");
		if (gamemap < 12)
			skytexture = R_TextureNumForName("SKY1");
		else
			if (gamemap < 21)
				skytexture = R_TextureNumForName("SKY2");
	}
	else
#endif
		switch (episode)
		{
			case 1:
				skytexture = R_TextureNumForName("SKY1");
				break;
			case 2:
				skytexture = R_TextureNumForName("SKY2");
				break;
			case 3:
				skytexture = R_TextureNumForName("SKY3");
				break;
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
			case 4:	// Special Edition sky
#if (APPVER_DOOMREV < AV_DR_DM19U)
				skytexture = R_TextureNumForName("SKY2");
#else
				skytexture = R_TextureNumForName("SKY4");
#endif
				break;
#endif
		}

//
// give one null ticcmd_t
//
#if (APPVER_DOOMREV < AV_DR_DM12)
	gametic = 0;
	maketic = 1;
	for (i=0 ; i<MAXPLAYERS ; i++)
		nettics[i] = 1;                 // one null event for this gametic
	memset (localcmds,0,sizeof(localcmds));
	memset (netcmds,0,sizeof(netcmds));
#endif
	G_DoLoadLevel();
}


/*
===============================================================================

							DEMO RECORDING

===============================================================================
*/

#define DEMOMARKER      0x80

void G_ReadDemoTiccmd (ticcmd_t *cmd)
{
	if (*demo_p == DEMOMARKER)
	{       // end of demo data stream
		G_CheckDemoStatus ();
		return;
	}
#if (APPVER_DOOMREV < AV_DR_DM12)
	cmd->forwardmove = ((signed char)*demo_p++) * 900;
	cmd->sidemove = ((signed char)*demo_p++) * 900;
	cmd->angleturn = ((unsigned char)*demo_p++)<<24;
	cmd->buttons = (unsigned char)*demo_p++;
	cmd->f_18 = G_CmdChecksum(cmd);
#else
	cmd->forwardmove = ((signed char)*demo_p++);
	cmd->sidemove = ((signed char)*demo_p++);
	cmd->angleturn = ((unsigned char)*demo_p++)<<8;
	cmd->buttons = (unsigned char)*demo_p++;
#endif
}

void G_WriteDemoTiccmd (ticcmd_t *cmd)
{
	if (gamekeydown['q'])           // press q to end demo recording
		G_CheckDemoStatus ();
#if (APPVER_DOOMREV < AV_DR_DM12)
	*demo_p++ = cmd->forwardmove / 900;
	*demo_p++ = cmd->sidemove / 900;
	*demo_p++ = cmd->angleturn>>24;
	*demo_p++ = cmd->buttons;
	demo_p -= 4;
#else
	*demo_p++ = cmd->forwardmove;
	*demo_p++ = cmd->sidemove;
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	*demo_p++ = cmd->angleturn>>8;
#else
	*demo_p++ = (cmd->angleturn+128)>>8;
#endif
	*demo_p++ = cmd->buttons;
	demo_p -= 4;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (demo_p > demoend - 16)
	{
		// no more space 
		G_CheckDemoStatus();
		return;
	}
#endif
#endif

	G_ReadDemoTiccmd (cmd);         // make SURE it is exactly the same
}



/*
===================
=
= G_RecordDemo
=
===================
*/

#if (APPVER_DOOMREV < AV_DR_DM1666P)
void G_RecordDemo (skill_t skill, int numplayers, int episode, int map, char *name)
{
	int             i;

	G_InitNew (skill, episode, map);
	usergame = false;
	strcpy (demoname, name);
	strcat (demoname, ".lmp");
	demobuffer = demo_p = Z_Malloc (0x20000,PU_STATIC,NULL);
	*demo_p++ = skill;
	*demo_p++ = episode;
	*demo_p++ = map;

	for (i=0 ; i<MAXPLAYERS ; i++)
		*demo_p++ = playeringame[i];

	demorecording = true;
}

#else
void G_RecordDemo (char *name)
{
	int             i;
	int             maxsize;

	usergame = false;
	strcpy (demoname, name);
	strcat (demoname, ".lmp");
	maxsize = 0x20000;
	i = M_CheckParm ("-maxdemo");
	if (i && i<myargc-1)
		maxsize = atoi(myargv[i+1])*1024;
	demobuffer = Z_Malloc (maxsize,PU_STATIC,NULL);
	demoend = demobuffer + maxsize;

	demorecording = true;
}
#endif


#if (APPVER_DOOMREV >= AV_DR_DM1666P)
void G_BeginRecording(void)
{
	int             i;

	demo_p = demobuffer;

	*demo_p++ = VERSION;
	*demo_p++ = gameskill;
	*demo_p++ = gameepisode;
	*demo_p++ = gamemap;
	*demo_p++ = deathmatch;
	*demo_p++ = respawnparm;
	*demo_p++ = fastparm;
	*demo_p++ = nomonsters;
	*demo_p++ = consoleplayer;

	for (i=0 ; i<MAXPLAYERS; i++)
		*demo_p++ = playeringame[i];
}
#endif


/*
===================
=
= G_PlayDemo
=
===================
*/

char    *defdemoname;

void G_DeferedPlayDemo (char *name)
{
	defdemoname = name;
	gameaction = ga_playdemo;
}

void G_DoPlayDemo (void)
{
	skill_t skill;
	int             i, episode, map;

	gameaction = ga_nothing;
	demobuffer = demo_p = W_CacheLumpName (defdemoname, PU_STATIC);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (*demo_p++ != VERSION)
		I_Error("Demo is from a different game version!");
#endif

	skill = *demo_p++;
	episode = *demo_p++;
	map = *demo_p++;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	deathmatch = *demo_p++;
	respawnparm = *demo_p++;
	fastparm = *demo_p++;
	nomonsters = *demo_p++;
	consoleplayer = *demo_p++;
#endif

	for (i=0 ; i<MAXPLAYERS ; i++)
		playeringame[i] = *demo_p++;
	if (playeringame[1])
	{
		netgame = true;
		netdemo = true;
	}

	precache = false;               // don't spend a lot of time in loadlevel
	G_InitNew (skill, episode, map);
	precache = true;
	usergame = false;
	demoplayback = true;
}


/*
===================
=
= G_TimeDemo
=
===================
*/

void G_TimeDemo (char *name)
{
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	skill_t skill;
	int             episode, map;
	
	nodrawers = M_CheckParm ("-nodraw");
	noblit = M_CheckParm ("-noblit");
	demobuffer = demo_p = W_CacheLumpName (name, PU_STATIC);
	skill = *demo_p++;
	episode = *demo_p++;
	map = *demo_p++;
	G_InitNew (skill, episode, map);
	usergame = false;
	demoplayback = true;
	timingdemo = true;
	singletics = true;
#else
	nodrawers = M_CheckParm ("-nodraw");
	noblit = M_CheckParm ("-noblit");
	timingdemo = true;
	singletics = true;

	defdemoname = name;
	gameaction = ga_playdemo;
#endif
}


/*
===================
=
= G_CheckDemoStatus
=
= Called after a death or level completion to allow demos to be cleaned up
= Returns true if a new demo loop action will take place
===================
*/

boolean G_CheckDemoStatus (void)
{
	int             endtime;

	if (timingdemo)
	{
		endtime = I_GetTime ();
		I_Error ("timed %i gametics in %i realtics",gametic
		, endtime-starttime);
	}

	if (demoplayback)
	{
		if (singledemo)
			I_Quit ();

		Z_ChangeTag (demobuffer, PU_CACHE);
		demoplayback = false;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
		netdemo = false;
		netgame = false;
		deathmatch = false;
		playeringame[1] = playeringame[2] = playeringame[3] = 0;
		respawnparm = false;
		fastparm = false;
		nomonsters = false;
		consoleplayer = 0;
#endif
		D_AdvanceDemo ();
		return true;
	}

	if (demorecording)
	{
		*demo_p++ = DEMOMARKER;
		M_WriteFile (demoname, demobuffer, demo_p - demobuffer);
		Z_Free (demobuffer);
		demorecording = false;
		I_Error ("Demo %s recorded",demoname);
	}

	return false;
}


