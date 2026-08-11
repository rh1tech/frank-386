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

// ST_stuff.c

#include "DoomDef.h"
#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp1, _wp2;
#else
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9;
extern int _wp10, _wp11, _wp12, _wp13, _wp14, _wp15, _wp16;
#endif
#include "P_local.h"
#include "DUtils.h"
#include "AM_map.h"
#include "ST_lib.h"
#include "ST_stuff.h"


//
// STATUS BAR CODE
//
void ST_Stop(void);

void ST_refreshBackground(void)
{

  if (st_statusbaron)
  {
#if (APPVER_DOOMREV < AV_DR_DM1666P)
#if (APPVER_DOOMREV < AV_DR_DM12)
    V_DrawPatch(ST_X, ST_Y, BG, sbarl);
    V_DrawPatch(ST_X2, ST_Y, BG, sbarr);
#else
    V_DrawPatch(ST_X, ST_Y, BG, sbar);
#endif

    if (netgame)
      V_DrawPatch(ST_FX, ST_Y+1, BG, faceback);
	
    V_CopyRect(ST_X, ST_Y, BG, ST_WIDTH, ST_HEIGHT, ST_X, ST_Y, FG);
#else
    V_DrawPatch(ST_X, 0, BG, sbar);

    if (netgame)
      V_DrawPatch(ST_FX, 0, BG, faceback);
	
    V_CopyRect(ST_X, 0, BG, ST_WIDTH, ST_HEIGHT, ST_X, ST_Y, FG);
#endif
  }

}


// Respond to keyboard input events,
//  intercept cheats.
boolean ST_Responder (event_t *ev)
{
  int		i;
    
  // Filter automap on/off.
  if (ev->type == ev_keyup && ((ev->data1 & 0xffff0000) == AM_MSGHEADER))
  {
    switch(ev->data1)
    {
    case AM_MSGENTERED:
	st_gamestate = AutomapState;
	st_firsttime = true;
	break;
	
    case AM_MSGEXITED:
#if (APPVER_DOOMREV < AV_DR_DM12)
	fprintf(stderr, "AM exited\n");
#endif
	st_gamestate = FirstPersonState;
	break;
    }
  }

  // if a user keypress...
  else if (ev->type == ev_keydown)
  {
    if (!netgame)
    {
#if (APPVER_DOOMREV >= AV_DR_DM12)
      if (gameskill != sk_nightmare)
      {
#endif
	// 'dqd' cheat for toggleable god mode
	if (cht_CheckCheat(&cheat_god, ev->data1))
	{
	  plyr->cheats ^= CF_GODMODE;
	  if (plyr->cheats & CF_GODMODE)
	  {
	    if (plyr->mo)
	      plyr->mo->health = 100;
	  
	    plyr->health = 100;
	    plyr->message = STSTR_DQDON;
	  }
	  else 
	    plyr->message = STSTR_DQDOFF;
	}
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	// 'fa' cheat for killer fucking arsenal
	else if (cht_CheckCheat(&cheat_ammonokey, ev->data1))
	{
	  plyr->armorpoints = 200;
	  plyr->armortype = 2;
	
	  for (i=0;i<NUMWEAPONS;i++)
	    plyr->weaponowned[i] = true;
	
	  for (i=0;i<NUMAMMO;i++)
	    plyr->ammo[i] = plyr->maxammo[i];
	
	  plyr->message = STSTR_FAADDED;
	}
#endif
	// 'kfa' cheat for key full ammo
	else if (cht_CheckCheat(&cheat_ammo, ev->data1))
	{
	  plyr->armorpoints = 200;
	  plyr->armortype = 2;
	
	  for (i=0;i<NUMWEAPONS;i++)
	    plyr->weaponowned[i] = true;
	
	  for (i=0;i<NUMAMMO;i++)
	    plyr->ammo[i] = plyr->maxammo[i];
	
	  for (i=0;i<NUMCARDS;i++)
	    plyr->cards[i] = true;
	
	  plyr->message = STSTR_KFAADDED;
	}
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	// 'mus' cheat for changing music
	else if (cht_CheckCheat(&cheat_mus, ev->data1))
	{
	
	  char	buf[3];
	  int		musnum;
	
	  plyr->message = STSTR_MUS;
	  cht_GetParam(&cheat_mus, buf);
	
#if (APPVER_DOOMREV >= AV_DR_DM19U)
	  if (commercial)
	  {
#endif
	    musnum = mus_runnin + (buf[0]-'0')*10 + buf[1]-'0' - 1;
	  
#if (APPVER_DOOMREV >= AV_DR_DM18FR)
	    if (((buf[0]-'0')*10 + buf[1]-'0') > 35)
	      plyr->message = STSTR_NOMUS;
	    else
#endif
	      S_ChangeMusic(musnum, 1);
#if (APPVER_DOOMREV >= AV_DR_DM19U)
	  }
	  else
	  {
	    musnum = mus_e1m1 + (buf[0]-'1')*9 + (buf[1]-'1');
	  
	    if (((buf[0]-'1')*9 + buf[1]-'1') > 31)
	      plyr->message = STSTR_NOMUS;
	    else
	      S_ChangeMusic(musnum, 1);
	  }
#endif
	}
#endif
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	else if (cht_CheckCheat(&cheat_noclip, ev->data1) )
#else
	else if (!commercial && cht_CheckCheat(&cheat_noclip, ev->data1) )
#endif
	{	
	  plyr->cheats ^= CF_NOCLIP;
	
	  if (plyr->cheats & CF_NOCLIP)
	    plyr->message = STSTR_NCON;
	  else
	    plyr->message = STSTR_NCOFF;
	}
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	else if (commercial && cht_CheckCheat(&cheat_commercial_noclip, ev->data1) )
	{	
	  plyr->cheats ^= CF_NOCLIP;
	
	  if (plyr->cheats & CF_NOCLIP)
	    plyr->message = STSTR_NCON;
	  else
	    plyr->message = STSTR_NCOFF;
	}
#endif
	// 'behold?' power-up cheats
	for (i=0;i<6;i++)
	{
	  if (cht_CheckCheat(&cheat_powerup[i], ev->data1))
	  {
	    if (!plyr->powers[i])
	      P_GivePower( plyr, i);
	    else if (i!=pw_strength)
	      plyr->powers[i] = 1;
	    else
	      plyr->powers[i] = 0;
	  
	    plyr->message = STSTR_BEHOLDX;
	  }
	}
      
	// 'behold' power-up menu
	if (cht_CheckCheat(&cheat_powerup[6], ev->data1))
	{
	  plyr->message = STSTR_BEHOLD;
	}
	// 'choppers' invulnerability & chainsaw
	else if (cht_CheckCheat(&cheat_choppers, ev->data1))
	{
	  plyr->weaponowned[wp_chainsaw] = true;
	  plyr->powers[pw_invulnerability] = true;
	  plyr->message = STSTR_CHOPPERS;
	}
#if (APPVER_DOOMREV < AV_DR_DM12)
	else
#else
	// 'mypos' for player position
	else if (cht_CheckCheat(&cheat_mypos, ev->data1))
	{
	  static char	buf[ST_MSGWIDTH];
	  sprintf(buf, "ang=0x%x;x,y=(0x%x,0x%x)",
		  players[consoleplayer].mo->angle,
		  players[consoleplayer].mo->x,
		  players[consoleplayer].mo->y);
	  plyr->message = buf;
        }
      }
#endif
    
      // 'clev' change-level cheat
      if (cht_CheckCheat(&cheat_clev, ev->data1))
      {
	char		buf[3];
	int		epsd;
	int		map;
      
	cht_GetParam(&cheat_clev, buf);
      
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (commercial)
	{
	  epsd = 0;
	  map = (buf[0] - '0')*10 + buf[1] - '0';
	}
	else
#endif
	{
	  epsd = buf[0] - '0';
	  map = buf[1] - '0';
#if APPVER_CHEX
	  if (epsd > 1)
	    epsd = 1;
	  if (map > 5)
	    map = 5;
#endif
	}

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if ( epsd > 0 && epsd < 4 && map > 0 && map < 10 )
#elif (APPVER_DOOMREV < AV_DR_DM19UP)
	if ( (!commercial && epsd > 0 && epsd < 4 && map > 0 && map < 10)
	  || (commercial && map > 0 && map <= 40 ) )
#else
	if ( (!commercial && epsd > 0 && epsd < 5 && map > 0 && map < 10)
	  || (commercial && map > 0 && map <= 40 ) )
#endif
	{
	  plyr->message = STSTR_CLEV;
	  G_DeferedInitNew(gameskill, epsd, map);
	}
      }
#if (APPVER_DOOMREV < AV_DR_DM12)
	// 'mypos' for player position
	else if (cht_CheckCheat(&cheat_mypos, ev->data1))
	{
	  static char	buf[ST_MSGWIDTH];
	  sprintf(buf, "ang=0x%x;x,y=(0x%x,0x%x)",
		  players[consoleplayer].mo->angle,
		  players[consoleplayer].mo->x,
		  players[consoleplayer].mo->y);
	  plyr->message = buf;
        }
#endif
    }    
  }
  return false;
}



int ST_calcPainOffset(void)
{
  int health;
  static int lastcalc;
  static int oldhealth = -1;
    
  health = plyr->health > 100 ? 100 : plyr->health;

  if (health != oldhealth)
  {
    lastcalc = ST_FACESTRIDE * (((100 - health) * ST_NUMPAINFACES) / 101);
    oldhealth = health;
  }
  return lastcalc;
}


//
// This is a not-very-pretty routine which handles
//  the face states and their timing.
// the precedence of expressions is:
//  dead > evil grin > turned head > straight ahead
//
void ST_updateFaceWidget(void)
{
  int i;
  angle_t badguyangle;
  angle_t diffang;
  static int lastattackdown = -1;
  static int priority = 0;
  boolean doevilgrin;

  if (priority < 10)
  {
    // dead
    if (!plyr->health)
    {
      priority = 9;
      st_faceindex = ST_DEADFACE;
      st_facecount = 1;
    }
  }

  if (priority < 9)
  {
    if (plyr->bonuscount)
    {
      // picking up bonus
      doevilgrin = false;

      for (i=0;i<NUMWEAPONS;i++)
      {
	if (oldweaponsowned[i] != plyr->weaponowned[i])
	{
	  doevilgrin = true;
	  oldweaponsowned[i] = plyr->weaponowned[i];
	}
      }
      if (doevilgrin) 
      {
	// evil grin if just picked up weapon
	priority = 8;
	st_facecount = ST_EVILGRINCOUNT;
	st_faceindex = ST_calcPainOffset() + ST_EVILGRINOFFSET;
      }
    }

  }
  
  if (priority < 8)
  {
    if (plyr->damagecount && plyr->attacker && plyr->attacker != plyr->mo)
    {
      // being attacked
      priority = 7;
	    
      if (plyr->health - st_oldhealth > ST_MUCHPAIN)
      {
	st_facecount = ST_TURNCOUNT;
	st_faceindex = ST_calcPainOffset() + ST_OUCHOFFSET;
      }
      else
      {
	badguyangle = R_PointToAngle2(plyr->mo->x, plyr->mo->y,
		plyr->attacker->x, plyr->attacker->y);
		
	if (badguyangle > plyr->mo->angle)
	{
	  // whether right or left
	  diffang = badguyangle - plyr->mo->angle;
	  i = diffang > ANG180; 
	}
	else
	{
	  // whether left or right
	  diffang = plyr->mo->angle - badguyangle;
	  i = diffang <= ANG180; 
	} // confusing, aint it?

		
	st_facecount = ST_TURNCOUNT;
	st_faceindex = ST_calcPainOffset();
		
	if (diffang < ANG45)
	{
	  // head-on    
	  st_faceindex += ST_RAMPAGEOFFSET;
	}
	else if (i)
	{
	  // turn face right
	  st_faceindex += ST_TURNOFFSET;
	}
	else
	{
	  // turn face left
	  st_faceindex += ST_TURNOFFSET+1;
	}
      }
    }
  }
  
  if (priority < 7)
  {
    // getting hurt because of your own damn stupidity
    if (plyr->damagecount)
    {
      if (plyr->health - st_oldhealth > ST_MUCHPAIN)
      {
	priority = 7;
	st_facecount = ST_TURNCOUNT;
	st_faceindex = ST_calcPainOffset() + ST_OUCHOFFSET;
      }
      else
      {
	priority = 6;
	st_facecount = ST_TURNCOUNT;
	st_faceindex = ST_calcPainOffset() + ST_RAMPAGEOFFSET;
      }

    }

  }
  
  if (priority < 6)
  {
    // rapid firing
    if (plyr->attackdown)
    {
      if (lastattackdown==-1)
	lastattackdown = ST_RAMPAGEDELAY;
      else if (!--lastattackdown)
      {
	priority = 5;
	st_faceindex = ST_calcPainOffset() + ST_RAMPAGEOFFSET;
	st_facecount = 1;
	lastattackdown = 1;
      }
    }
    else
      lastattackdown = -1;

  }
  
  if (priority < 5)
  {
    // invulnerability
    if ((plyr->cheats & CF_GODMODE) || plyr->powers[pw_invulnerability])
    {
      priority = 4;

      st_faceindex = ST_GODFACE;
      st_facecount = 1;

    }

  }

  // look left or look right if the facecount has timed out
  if (!st_facecount)
  {
    st_faceindex = ST_calcPainOffset() + (st_randomnumber % 3);
    st_facecount = ST_STRAIGHTFACECOUNT;
    priority = 0;
  }

  st_facecount--;

}

void ST_updateWidgets(void)
{
#if (APPVER_DOOMREV < AV_DR_DM1666P)
  static int largeammo = -1; // means "n/a"
#else
  static int largeammo = 1994; // means "n/a"
#endif
  int i;

// must redirect the pointer if the ready weapon has changed.
#if (APPVER_DOOMREV < AV_DR_DM12)
  if (w_ready.data != plyr->readyweapon)
  {
#endif
  if (weaponinfo[plyr->readyweapon].ammo == am_noammo)
    w_ready.num = &largeammo;
  else
    w_ready.num = &plyr->ammo[weaponinfo[plyr->readyweapon].ammo];
//{
// static int tic=0;
// static int dir=-1;
// if (!(tic&15))
//   plyr->ammo[weaponinfo[plyr->readyweapon].ammo]+=dir;
// if (plyr->ammo[weaponinfo[plyr->readyweapon].ammo] == -100)
//   dir = 1;
// tic++;
// }
  w_ready.data = plyr->readyweapon;

// if (*w_ready.on)
//  STlib_updateNum(&w_ready, true);
// refresh weapon change
#if (APPVER_DOOMREV < AV_DR_DM12)
  }
#endif

// update keycard multiple widgets
  for (i=0;i<3;i++)
  {
    keyboxes[i] = plyr->cards[i] ? i : -1;

  if (plyr->cards[i+3])
    keyboxes[i] = i+3;
  }

// refresh everything if this is him coming back to life
  ST_updateFaceWidget();

// used by the w_armsbg widget
  st_notdeathmatch = !deathmatch;
    
// used by w_arms[] widgets
  st_armson = st_statusbaron && !deathmatch; 

// used by w_frags widget
  st_fragson = deathmatch && st_statusbaron; 
  st_fragscount = 0;

  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (i != consoleplayer)
      st_fragscount += plyr->frags[i];
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
    else
      st_fragscount -= plyr->frags[i];
#endif
  }

  // get rid of chat window if up because of message
  if (!--st_msgcounter)
    st_chat = st_oldchat;

}

void ST_Ticker (void)
{

  st_clock++;
  st_randomnumber = M_Random();
  ST_updateWidgets();
  st_oldhealth = plyr->health;

}

static int st_palette = 0;

void ST_doPaletteStuff(void)
{

  int palette;
  byte *pal;
  int cnt, bzc;

  cnt = plyr->damagecount;

  if (plyr->powers[pw_strength])
  {
    // slowly fade the berzerk out
    bzc = 12 - (plyr->powers[pw_strength]>>6);

    if (bzc > cnt)
      cnt = bzc;
  }
	
  if (cnt)
  {
#if APPVER_CHEX
    palette = RADIATIONPAL;
#else
    palette = (cnt+7)>>3;
	
    if (palette >= NUMREDPALS)
      palette = NUMREDPALS-1;

    palette += STARTREDPALS;
#endif
  }

  else if (plyr->bonuscount)
  {
    palette = (plyr->bonuscount+7)>>3;

    if (palette >= NUMBONUSPALS)
      palette = NUMBONUSPALS-1;

    palette += STARTBONUSPALS;
  }

  else if ( plyr->powers[pw_ironfeet] > 4*32 || plyr->powers[pw_ironfeet]&8)
    palette = RADIATIONPAL;
  else
    palette = 0;

  if (palette != st_palette)
  {
    st_palette = palette;
    pal = (byte *) W_CacheLumpNum (lu_palette, PU_CACHE)+palette*768;
    I_SetPalette (pal);
  }

}

void ST_drawWidgets(boolean refresh)
{
  int i;

// used by w_arms[] widgets
  st_armson = st_statusbaron && !deathmatch;

// used by w_frags widget
  st_fragson = deathmatch && st_statusbaron; 

  STlib_updateNum(&w_ready, refresh);

  for (i=0;i<4;i++)
  {
    STlib_updateNum(&w_ammo[i], refresh);
    STlib_updateNum(&w_maxammo[i], refresh);
  }

  STlib_updatePercent(&w_health, refresh);
  STlib_updatePercent(&w_armor, refresh);

  STlib_updateBinIcon(&w_armsbg, refresh);

  for (i=0;i<6;i++)
    STlib_updateMultIcon(&w_arms[i], refresh);

  STlib_updateMultIcon(&w_faces, refresh);

  for (i=0;i<3;i++)
    STlib_updateMultIcon(&w_keyboxes[i], refresh);

  STlib_updateNum(&w_frags, refresh);

}

void ST_doRefresh(void)
{

  st_firsttime = false;

// draw status bar background to off-screen buff
 ST_refreshBackground();

// and refresh all widgets
 ST_drawWidgets(true);

}

void ST_diffDraw(void)
{
// update all widgets
  ST_drawWidgets(false);
}

void ST_Drawer (boolean fullscreen, boolean refresh)
{
  
  st_statusbaron = (!fullscreen) || automapactive;
  st_firsttime = st_firsttime || refresh;

// Do red-/gold-shifts from damage/items
  ST_doPaletteStuff();

// If just after ST_Start(), refresh all
  if (st_firsttime) ST_doRefresh();
// Otherwise, update as little as possible
  else ST_diffDraw();

}

void ST_loadGraphics(void)
{

  int i, j, facenum;   
  char namebuf[9];

// Load the numbers, tall and short
  for (i=0;i<10;i++)
  {
    sprintf(namebuf, "STTNUM%d", i);
    tallnum[i] = (patch_t *) W_CacheLumpName(namebuf, PU_STATIC);

    sprintf(namebuf, "STYSNUM%d", i);
    shortnum[i] = (patch_t *) W_CacheLumpName(namebuf, PU_STATIC);
  }

// Load percent key
  tallpercent = (patch_t *) W_CacheLumpName("STTPRCNT", PU_STATIC);

// key cards
  for (i=0;i<NUMCARDS;i++)
  {
    sprintf(namebuf, "STKEYS%d", i);
    keys[i] = (patch_t *) W_CacheLumpName(namebuf, PU_STATIC);
  }

// arms background
  armsbg = (patch_t *) W_CacheLumpName("STARMS", PU_STATIC);

// arms ownership widgets
  for (i=0;i<6;i++)
  {
    sprintf(namebuf, "STGNUM%d", i+2);

    // gray #
    arms[i][0] = (patch_t *) W_CacheLumpName(namebuf, PU_STATIC);

    // yellow #
    arms[i][1] = shortnum[i+2]; 
  }

// face backgrounds for different color players
  sprintf(namebuf, "STFB%d", consoleplayer);
  faceback = (patch_t *) W_CacheLumpName(namebuf, PU_STATIC);

// status bar background bits
#if (APPVER_DOOMREV < AV_DR_DM12)
  sbarl = (patch_t *) W_CacheLumpName("STMBARL", PU_STATIC);
  sbarr = (patch_t *) W_CacheLumpName("STMBARR", PU_STATIC);
#else
  sbar = (patch_t *) W_CacheLumpName("STBAR", PU_STATIC);
#endif

// face states
  facenum = 0;
  for (i=0;i<ST_NUMPAINFACES;i++)
  {
    for (j=0;j<ST_NUMSTRAIGHTFACES;j++)
    {
      sprintf(namebuf, "STFST%d%d", i, j);
      faces[facenum++] = W_CacheLumpName(namebuf, PU_STATIC);
    }
    sprintf(namebuf, "STFTR%d0", i);	// turn right
    faces[facenum++] = W_CacheLumpName(namebuf, PU_STATIC);
    sprintf(namebuf, "STFTL%d0", i);	// turn left
    faces[facenum++] = W_CacheLumpName(namebuf, PU_STATIC);
    sprintf(namebuf, "STFOUCH%d", i);	// ouch!
    faces[facenum++] = W_CacheLumpName(namebuf, PU_STATIC);
    sprintf(namebuf, "STFEVL%d", i);	// evil grin ;)
    faces[facenum++] = W_CacheLumpName(namebuf, PU_STATIC);
    sprintf(namebuf, "STFKILL%d", i);	// pissed off
    faces[facenum++] = W_CacheLumpName(namebuf, PU_STATIC);
  }
  faces[facenum++] = W_CacheLumpName("STFGOD0", PU_STATIC);
  faces[facenum++] = W_CacheLumpName("STFDEAD0", PU_STATIC);

}

void ST_loadData(void)
{
  lu_palette = W_GetNumForName ("PLAYPAL");
  ST_loadGraphics();
}

void ST_unloadGraphics(void)
{

int i;

// unload the numbers, tall and short
  for (i=0;i<10;i++)
  {
    Z_ChangeTag(tallnum[i], PU_CACHE);
    Z_ChangeTag(shortnum[i], PU_CACHE);
  }
// unload tall percent
  Z_ChangeTag(tallpercent, PU_CACHE); 

// unload arms background
  Z_ChangeTag(armsbg, PU_CACHE); 

// unload gray #'s
  for (i=0;i<6;i++)
    Z_ChangeTag(arms[i][0], PU_CACHE);
    
// unload the key cards
  for (i=0;i<NUMCARDS;i++)
    Z_ChangeTag(keys[i], PU_CACHE);
  
#if (APPVER_DOOMREV < AV_DR_DM12)
  Z_ChangeTag(sbarl, PU_CACHE);
  Z_ChangeTag(sbarr, PU_CACHE);
#else
  Z_ChangeTag(sbar, PU_CACHE);
#endif
  Z_ChangeTag(faceback, PU_CACHE);

  for (i=0;i<ST_NUMFACES;i++)
    Z_ChangeTag(faces[i], PU_CACHE);

}

void ST_unloadData(void)
{
  ST_unloadGraphics();
}

void ST_initData(void)
{

  int i;

  st_firsttime = true;
  plyr = &players[consoleplayer];

  st_clock = 0;
  st_chatstate = StartChatState;
  st_gamestate = FirstPersonState;

  st_statusbaron = true;
  st_oldchat = st_chat = false;
  st_cursoron = false;
  
#if (APPVER_DOOMREV >= AV_DR_DM12)
  st_faceindex = 0;
  st_palette = -1;
#endif

  st_oldhealth = -1;

  for (i=0;i<NUMWEAPONS;i++)
    oldweaponsowned[i] = plyr->weaponowned[i];

  for (i=0;i<3;i++)
    keyboxes[i] = -1;

  STlib_init();

}



void ST_createWidgets(void)
{

  int i;

// ready weapon ammo
  STlib_initNum(&w_ready, ST_AMMOX, ST_AMMOY, tallnum,
    &plyr->ammo[weaponinfo[plyr->readyweapon].ammo], &st_statusbaron, ST_AMMOWIDTH );

// the last weapon type
  w_ready.data = plyr->readyweapon; 

// health percentage
  STlib_initPercent(&w_health, ST_HEALTHX, ST_HEALTHY, tallnum,
    &plyr->health, &st_statusbaron, tallpercent);

// arms background
  STlib_initBinIcon(&w_armsbg, ST_ARMSBGX, ST_ARMSBGY, armsbg,
    &st_notdeathmatch, &st_statusbaron);

// weapons owned
  for(i=0;i<6;i++)
  {
    STlib_initMultIcon(&w_arms[i], ST_ARMSX+(i%3)*ST_ARMSXSPACE, ST_ARMSY+(i/3)*ST_ARMSYSPACE,
      arms[i], (int *) &plyr->weaponowned[i+1],  &st_armson);
  }

// frags sum
  STlib_initNum(&w_frags, ST_FRAGSX, ST_FRAGSY, tallnum,
    &st_fragscount, &st_fragson, ST_FRAGSWIDTH);

// faces
  STlib_initMultIcon(&w_faces, ST_FACESX, ST_FACESY, faces,
    &st_faceindex, &st_statusbaron);

// armor percentage - should be colored later
  STlib_initPercent(&w_armor, ST_ARMORX, ST_ARMORY, tallnum,
    &plyr->armorpoints, &st_statusbaron, tallpercent);

// keyboxes 0-2
  STlib_initMultIcon(&w_keyboxes[0], ST_KEY0X, ST_KEY0Y, keys,
    &keyboxes[0], &st_statusbaron);
    
  STlib_initMultIcon(&w_keyboxes[1], ST_KEY1X, ST_KEY1Y, keys,
    &keyboxes[1], &st_statusbaron);

  STlib_initMultIcon(&w_keyboxes[2], ST_KEY2X, ST_KEY2Y, keys,
    &keyboxes[2], &st_statusbaron);

// ammo count (all four kinds)
  STlib_initNum(&w_ammo[0], ST_AMMO0X, ST_AMMO0Y, shortnum,
    &plyr->ammo[0], &st_statusbaron, ST_AMMO0WIDTH);

  STlib_initNum(&w_ammo[1], ST_AMMO1X, ST_AMMO1Y, shortnum,
    &plyr->ammo[1], &st_statusbaron, ST_AMMO1WIDTH);

  STlib_initNum(&w_ammo[2], ST_AMMO2X, ST_AMMO2Y, shortnum,
    &plyr->ammo[2], &st_statusbaron, ST_AMMO2WIDTH);
    
  STlib_initNum(&w_ammo[3], ST_AMMO3X, ST_AMMO3Y, shortnum,
    &plyr->ammo[3], &st_statusbaron, ST_AMMO3WIDTH);

// max ammo count (all four kinds)
  STlib_initNum(&w_maxammo[0], ST_MAXAMMO0X, ST_MAXAMMO0Y,
    shortnum, &plyr->maxammo[0], &st_statusbaron, ST_MAXAMMO0WIDTH);

  STlib_initNum(&w_maxammo[1], ST_MAXAMMO1X, ST_MAXAMMO1Y,
    shortnum, &plyr->maxammo[1], &st_statusbaron, ST_MAXAMMO1WIDTH);

  STlib_initNum(&w_maxammo[2], ST_MAXAMMO2X, ST_MAXAMMO2Y,
    shortnum, &plyr->maxammo[2], &st_statusbaron, ST_MAXAMMO2WIDTH);
    
  STlib_initNum(&w_maxammo[3], ST_MAXAMMO3X, ST_MAXAMMO3Y,
    shortnum, &plyr->maxammo[3], &st_statusbaron, ST_MAXAMMO3WIDTH);

}

static boolean	st_stopped = true;


void ST_Start (void)
{

  if (!st_stopped)
    ST_Stop();

  ST_initData();
  ST_createWidgets();
  st_stopped = false;

}

void ST_Stop (void)
{
  if (st_stopped)
    return;

  I_SetPalette (W_CacheLumpNum (lu_palette, PU_CACHE));

  st_stopped = true;
}

void ST_Init (void)
{
  veryfirsttime = 0;
  ST_loadData();
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  screens[4] = (byte *) Z_Malloc(ST_WIDTH*ST_HEIGHT, PU_STATIC, 0);
#endif
}
