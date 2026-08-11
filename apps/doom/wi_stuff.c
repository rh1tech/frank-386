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

// WI_stuff.c
#include "DoomDef.h"
#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9;
extern int _wp10, _wp11, _wp12, _wp13, _wp14;
#else
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9;
extern int _wp10, _wp11, _wp12, _wp13, _wp14, _wp15, _wp16, _wp17;
extern int _wp18;
#endif
#include "WI_stuff.h"
#include "wi_data.h"

#define RANGECHECKING
#define RNGCHECK(a, b, c) { if ((a) < (b) || (a) > (c)) { I_Error("%s=%d in %s:%d", #a, (a), __FILE__,__LINE__);}}

//
// GENERAL DATA
//

//
// Locally used stuff.
//
#define FB 0


// States for single-player
#define SP_KILLS 0
#define SP_ITEMS 2
#define SP_SECRET 4
#define SP_FRAGS 6 
#define SP_TIME 8 
#define SP_PAR ST_TIME

#define SP_PAUSE 1

// in seconds
#define SHOWNEXTLOCDELAY 4
//#define SHOWLASTLOCDELAY SHOWNEXTLOCDELAY

static int acceleratestage; // used to accelerate or skip a stage
static int me; // wbs->pnum
static stateenum_t state;  // specifies current state
static wbstartstruct_t *wbs; // contains information passed into intermission
static wbplayerstruct_t *plrs;  // wbs->plyr[]
static int cnt;  // used for general timing
static int bcnt;// used for timing of background animation
static int firstrefresh; // signals to refresh everything for one frame
static int cnt_kills[MAXPLAYERS], cnt_items[MAXPLAYERS], cnt_secret[MAXPLAYERS];
static int cnt_time, cnt_par, cnt_pause;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
static int NUMCMAPS; // # of commercial levels
#endif

//
//	GRAPHICS
//
static patch_t *bg;// background (map of levels).
static patch_t *yah[2]; // You Are Here graphic
static patch_t *splat;// splat
static patch_t *percent, *colon; // %, : graphics
static patch_t *num[10]; // 0-9 graphic
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
static patch_t *wiminus; // minus sign
#endif
static patch_t *finished; // "Finished!" graphics
static patch_t *entering; // "Entering" graphic
static patch_t *sp_secret; // "secret"
static patch_t *kills, *secret, *items, *frags; // "Kills", "Scrt", "Items", "Frags"
static patch_t *time, *par, *sucks; // Time sucks.
static patch_t *killers, *victims; // "killers", "victims"
static patch_t *total, *star, *bstar;// "Total", your face, your dead face
static patch_t *p[MAXPLAYERS];// "red P[1..MAXPLAYERS]"
static patch_t *bp[MAXPLAYERS];// "gray P[1..MAXPLAYERS]"
#if (APPVER_DOOMREV < AV_DR_DM1666P)
static patch_t *lnames[NUMMAPS]; // Name graphics of each level (centered)
#else
static patch_t **lnames; // Name graphics of each level (centered)
#endif

//
// CODE
//

// slam background
#if (APPVER_DOOMREV < AV_DR_DM1666P)
#define WI_slamBackground() V_DrawPatch(0,0,0,bg);
#else
static unsigned char *background=0;
void WI_slamBackground(void)
{
  memcpy(screens[0], screens[1], SCREENWIDTH * SCREENHEIGHT);
  V_MarkRect (0, 0, SCREENWIDTH, SCREENHEIGHT);
}
#endif

// The ticker is used to detect keys
//  because of timing issues in netgames.
boolean WI_Responder(event_t *ev)
{
  return false;
}

// Draws "<Levelname> Finished!"
void WI_drawLF(void)
{
  int y = WI_TITLEY;
  // draw <LevelName> 
  V_DrawPatch((SCREENWIDTH - SHORT(lnames[wbs->last]->width))/2,
    y, FB, lnames[wbs->last]);
  // draw "Finished!"
  y += (5*SHORT(lnames[wbs->last]->height))/4;
  V_DrawPatch((SCREENWIDTH - SHORT(finished->width))/2,
    y, FB, finished);
}

// Draws "Entering <LevelName>"
void WI_drawEL(void)
{
  int y = WI_TITLEY;
  // draw "Entering"
  V_DrawPatch((SCREENWIDTH - SHORT(entering->width))/2,
    y, FB, entering);
  // draw level
  y += (5*SHORT(lnames[wbs->next]->height))/4;
  V_DrawPatch((SCREENWIDTH - SHORT(lnames[wbs->next]->width))/2,
    y, FB, lnames[wbs->next]);
}

void WI_drawOnLnode(int n, patch_t *c[])
{
  int i, left, top, right, bottom;
  boolean fits = false;

  i = 0;
  do
  {
    left = lnodes[wbs->epsd][n].x - SHORT(c[i]->leftoffset);
    top = lnodes[wbs->epsd][n].y - SHORT(c[i]->topoffset);
    right = left + SHORT(c[i]->width);
    bottom = top + SHORT(c[i]->height);

    if (left >= 0 && right < SCREENWIDTH && top >= 0 && bottom < SCREENHEIGHT)
    {
      fits = true;
    }
    else
    {
      i++;
    }
  } while (!fits && i!=2);

  if (fits && i<2)
  {
    V_DrawPatch(lnodes[wbs->epsd][n].x, lnodes[wbs->epsd][n].y,
      FB, c[i]);
  }
  else
  {
    // DEBUG
    printf("Could not place patch on level %d", n+1); 
  }
}

void WI_initAnimatedBack(void)
{
  int i;
  anim_t *a;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (commercial)
    return;
#endif
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
  if (wbs->epsd > 2)
    return;
#endif
  for (i=0;i<NUMANIMS[wbs->epsd];i++)
  {
    a = &anims[wbs->epsd][i];
    // init variables
    a->ctr = -1;
    // specify the next time to draw it
    if (a->type == ANIM_ALWAYS)
      a->nexttic = bcnt + 1 + (M_Random()%a->period);
    else if (a->type == ANIM_RANDOM)
      a->nexttic = bcnt + 1 + a->data2+(M_Random()%a->data1);
    else if (a->type == ANIM_LEVEL)
      a->nexttic = bcnt + 1;
  }
}

void WI_updateAnimatedBack(void)
{
  int i;
  anim_t *a;

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (commercial)
    return;
#endif
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
  if (wbs->epsd > 2)
    return;
#endif
  for (i=0;i<NUMANIMS[wbs->epsd];i++)
  {
    a = &anims[wbs->epsd][i];

    if (bcnt == a->nexttic)
    {
      switch (a->type)
      {
	case ANIM_ALWAYS:
	  if (++a->ctr >= a->nanims) a->ctr = 0;
	  a->nexttic = bcnt + a->period;
	  break;

	case ANIM_RANDOM:
	  a->ctr++;
	  if (a->ctr == a->nanims)
	  {
	    a->ctr = -1;
	    a->nexttic = bcnt+a->data2+(M_Random()%a->data1);
	  }
	  else a->nexttic = bcnt + a->period;
	  break;
		
	case ANIM_LEVEL:
	  // gawd-awful hack for level anims
	  if (!(state == StatCount && i == 7) && wbs->next == a->data1)
	  {
	    a->ctr++;
	    if (a->ctr == a->nanims) a->ctr--;
	    a->nexttic = bcnt + a->period;
	  }
	  break;
      }
    }
  }
}

void WI_drawAnimatedBack(void)
{
    int i;
    anim_t *a;
    
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
    if (commercial)
	return;
#endif
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
    if (wbs->epsd > 2)
	return;
#endif
    for (i=0 ; i<NUMANIMS[wbs->epsd] ; i++)
    {
	a = &anims[wbs->epsd][i];

	if (a->ctr >= 0)
	    V_DrawPatch(a->loc.x, a->loc.y, FB, a->p[a->ctr]);
    }
}

//
// Draws a number.
// If digits > 0, then use that many digits minimum,
//  otherwise only use as many as necessary.
// Returns new x position.
//

int WI_drawNum (int x, int y, int n, int digits)
{
#if (APPVER_DOOMREV < AV_DR_DM1666P)
    int digits2 = digits;
    int v;
    int vdi;

    if (n < 0)
        return -1;

    vdi = 1;
    while (n / vdi != 0 || vdi == 1 || (digits2 && digits))
    {
        v = (n / vdi) % 10;

        x -= SHORT(num[0]->width);
        
        V_DrawPatch(x, y, FB, num[ v ]);
        vdi *= 10;
#if (APPVER_DOOMREV >= AV_DR_DM12)
        if (digits)
#endif
            digits--;
    }
    return x;
#else
  int fontwidth = SHORT(num[0]->width);
  int neg, temp;

  if (digits < 0)
  {
    if (!n)
      digits = 1; // make variable-length zeros 1 digit long
    else
    { // figure out # of digits in #
      digits = 0;
      temp = n;

      while (temp)
      {
	temp /= 10;
	digits++;
      }
    }
  }

  neg = n < 0;
  if (neg)
    n = -n;

  // if non-number, do not draw it
  if (n == 1994)
    return 0;

  // draw the new number
  while (digits--)
  {
    x -= fontwidth;
    V_DrawPatch(x, y, FB, num[ n % 10 ]);
    n /= 10;
  }

  // draw a minus sign if necessary
  if (neg)
    V_DrawPatch(x-=8, y, FB, wiminus);

  return x;
#endif
}

void WI_drawPercent (int x, int y, int p)
{
  if (p < 0)
    return;
  V_DrawPatch(x, y, FB, percent);
#if (APPVER_DOOMREV < AV_DR_DM1666P)
  WI_drawNum(x, y, p, 0);
#else
  WI_drawNum(x, y, p, -1);
#endif
}

//
// Display level completion time and par,
//  or "sucks" message if overflow.
//
void WI_drawTime (int x, int y, int t)
{
  int div, n;

  if (t<0)
    return;
  if (t <= 61*59)
  {
    div = 1;

    do
    {
      n = (t / div) % 60;
      x = WI_drawNum(x, y, n, 2) - SHORT(colon->width);
      div *= 60;

      // draw
      if (div==60 || t / div)
        V_DrawPatch(x, y, FB, colon);
    } while (t / div);
  }
  else
    V_DrawPatch(x - SHORT(sucks->width), y, FB, sucks); // "sucks"
}


void WI_End(void)
{
  void WI_unloadData(void);
  WI_unloadData();
}

void WI_initNoState(void)
{
  state = NoState;
  acceleratestage = 0;
  cnt = 10;
}

void WI_updateNoState(void) {
  WI_updateAnimatedBack();
  if (!--cnt)
  {
    WI_End();
    G_WorldDone();
  }
}

static boolean snl_pointeron = false;
void WI_initShowNextLoc(void)
{
  state = ShowNextLoc;
  acceleratestage = 0;
  cnt = SHOWNEXTLOCDELAY * TICRATE;
  WI_initAnimatedBack();
}

void WI_updateShowNextLoc(void)
{
  WI_updateAnimatedBack();
  if (!--cnt || acceleratestage)
    WI_initNoState();
  else
    snl_pointeron = (cnt & 31) < 20;
}

void WI_drawShowNextLoc(void)
{
  int i, last;

  WI_slamBackground();
  // draw animated background
  WI_drawAnimatedBack();
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (!commercial)
#endif
  {
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
    if (wbs->epsd > 2)
    {
      WI_drawEL();
      return;
    }
#endif
    last = (wbs->last == 8) ? wbs->next - 1 : wbs->last;
    // draw a splat on taken cities.
    for (i=0 ; i<=last ; i++)
      WI_drawOnLnode(i, &splat);
    // splat the secret level?
    if (wbs->didsecret)
      WI_drawOnLnode(8, &splat);
    // draw flashing ptr
    if (snl_pointeron)
      WI_drawOnLnode(wbs->next, yah); 
  }

  // draws which level you are entering..
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (!commercial || wbs->next != 30)
#endif
    WI_drawEL();  
}

void WI_drawNoState(void)
{
  snl_pointeron = true;
  WI_drawShowNextLoc();
}

int WI_fragSum(int playernum)
{
  int i;
  int frags = 0;
    
  for (i=0 ; i<MAXPLAYERS ; i++)
    if (playeringame[i] && i!=playernum)
      frags += plrs[playernum].frags[i];

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
// JDC hack - negative frags.
  frags -= plrs[playernum].frags[playernum];
//if (frags < 0)
//  frags = 0;
#endif

  return frags;
}

static int dm_state, dm_frags[MAXPLAYERS][MAXPLAYERS], dm_totals[MAXPLAYERS];

void WI_initDeathmatchStats(void)
{
  int	i, j;

  state = StatCount;
  acceleratestage = 0;
  dm_state = 1;

  cnt_pause = TICRATE;

  for (i=0 ; i<MAXPLAYERS ; i++)
    if (playeringame[i])
    {
      for (j=0 ; j<MAXPLAYERS ; j++)
	if (playeringame[j])
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	  dm_frags[i][j] = -1;
      dm_totals[i] = -1;
#else
	  dm_frags[i][j] = 0;
      dm_totals[i] = 0;
#endif
    }
    
  WI_initAnimatedBack();
}

void WI_updateDeathmatchStats(void)
{
  int i, j;
  boolean stillticking;

  WI_updateAnimatedBack();
  if (acceleratestage && dm_state != 4)
  {
    acceleratestage = 0;

    for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (playeringame[i])
      {
#if (APPVER_DOOMREV < AV_DR_DM12)
	for (i=0 ; i<MAXPLAYERS ; i++)
#else
	for (j=0 ; j<MAXPLAYERS ; j++)
#endif
	  if (playeringame[j])
	    dm_frags[i][j] = plrs[i].frags[j];
	dm_totals[i] = WI_fragSum(i);
      }
    }	
    S_StartSound(0, sfx_barexp);
    dm_state = 4;
  }

  if (dm_state == 2)
  {
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    stillticking = false;
    for (i=0 ; i<MAXPLAYERS ; i++)
      if (playeringame[i])
      {
	for (j=0 ; j<MAXPLAYERS ; j++)
	  if (playeringame[j] && dm_frags[i][j] != plrs[i].frags[j])
      {
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	    dm_frags[i][j]++;
	    if (dm_frags[i][j] > 99)
	      dm_frags[i][j] = 99;
#else
	    if (plrs[i].frags[j] < 0)
	      dm_frags[i][j]--;
	    else
	      dm_frags[i][j]++;
	    if (dm_frags[i][j] > 99)
	      dm_frags[i][j] = 99;
	    if (dm_frags[i][j] < -99)
              dm_frags[i][j] = -99;	
#endif
	    stillticking = true;
	  }
	dm_totals[i] = WI_fragSum(i);
	if (dm_totals[i] > 99)
	  dm_totals[i] = 99;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (dm_totals[i] < -99)
	  dm_totals[i] = -99;
#endif
      }

    if (!stillticking)
    {
      S_StartSound(0, sfx_barexp);
      dm_state++;
    }
  }
  else if (dm_state == 4)
  {
    if (acceleratestage)
    {
      S_StartSound(0, sfx_slop);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
      if (commercial)
	WI_initNoState();
      else
#endif
	WI_initShowNextLoc();
    }
  }
  else if (dm_state & 1)
  {
    if (!--cnt_pause)
    {
      dm_state++;
      cnt_pause = TICRATE;
    }
  }
}


void WI_drawDeathmatchStats(void)
{
  int i, j, x, y, w;
  int lh;	// line height

  lh = WI_SPACINGY;

  WI_slamBackground();
  WI_drawAnimatedBack(); // draw animated background
  WI_drawLF();
  // draw stat titles (top line)
  V_DrawPatch(DM_TOTALSX-SHORT(total->width)/2, DM_MATRIXY-WI_SPACINGY+10, FB, total);
  V_DrawPatch(DM_KILLERSX, DM_KILLERSY, FB, killers);
  V_DrawPatch(DM_VICTIMSX, DM_VICTIMSY, FB, victims);
  // draw P?
  x = DM_MATRIXX + DM_SPACINGX;
  y = DM_MATRIXY;
  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (playeringame[i])
    {
      V_DrawPatch(x-SHORT(p[i]->width)/2,
	DM_MATRIXY - WI_SPACINGY, FB, p[i]);
      V_DrawPatch(DM_MATRIXX-SHORT(p[i]->width)/2,
	y, FB, p[i]);
      if (i == me)
      {
	V_DrawPatch(x-SHORT(p[i]->width)/2,
	  DM_MATRIXY - WI_SPACINGY, FB, bstar);
	V_DrawPatch(DM_MATRIXX-SHORT(p[i]->width)/2,
	  y, FB, star);
      }
    }
    else
    {
      // V_DrawPatch(x-SHORT(bp[i]->width)/2,
      //   DM_MATRIXY - WI_SPACINGY, FB, bp[i]);
      // V_DrawPatch(DM_MATRIXX-SHORT(bp[i]->width)/2,
      //   y, FB, bp[i]);
    }
    x += DM_SPACINGX;
    y += WI_SPACINGY;
  }
  // draw stats
  y = DM_MATRIXY+10;
  w = SHORT(num[0]->width);
  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    x = DM_MATRIXX + DM_SPACINGX;
    if (playeringame[i])
    {
      for (j=0 ; j<MAXPLAYERS ; j++)
      {
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (playeringame[j] && dm_frags[i][j] >= 0)
#else
	if (playeringame[j])
#endif
	  WI_drawNum(x+w, y, dm_frags[i][j], 2);
	x += DM_SPACINGX;
      }
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	if (dm_totals[i] >= 0)
#endif
      WI_drawNum(DM_TOTALSX+w, y, dm_totals[i], 2);
    }
    y += WI_SPACINGY;
  }
}


static int cnt_frags[MAXPLAYERS], dofrags, ng_state;

void WI_initNetgameStats(void)
{
  int i;

  state = StatCount;
  acceleratestage = 0;
  ng_state = 1;
  cnt_pause = TICRATE;
  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (!playeringame[i])
      continue;
#if (APPVER_DOOMREV < AV_DR_DM12)
    cnt_kills[i] = cnt_items[i] = cnt_secret[i] = cnt_frags[i] = -1;
#else
    cnt_kills[i] = cnt_items[i] = cnt_secret[i] = cnt_frags[i] = 0;
#endif
    dofrags += WI_fragSum(i);
  }
  dofrags = !!dofrags;
  WI_initAnimatedBack();
}


void WI_updateNetgameStats(void)
{
  int i, fsum;
  boolean stillticking;

  WI_updateAnimatedBack();
  if (acceleratestage && ng_state != 10)
  {
    acceleratestage = 0;

    for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (!playeringame[i])
	continue;
      cnt_kills[i] = (plrs[i].skills * 100) / wbs->maxkills;
      cnt_items[i] = (plrs[i].sitems * 100) / wbs->maxitems;
      cnt_secret[i] = (plrs[i].ssecret * 100) / wbs->maxsecret;
      if (dofrags)
	cnt_frags[i] = WI_fragSum(i);
    }
    S_StartSound(0, sfx_barexp);
    ng_state = 10;
  }
  if (ng_state == 2)
  {
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    stillticking = false;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (!playeringame[i])
	continue;
      cnt_kills[i] += 2;
      if (cnt_kills[i] >= (plrs[i].skills * 100) / wbs->maxkills)
	cnt_kills[i] = (plrs[i].skills * 100) / wbs->maxkills;
      else
	stillticking = true;
    }
    if (!stillticking)
    {
      S_StartSound(0, sfx_barexp);
      ng_state++;
    }
  }
  else if (ng_state == 4)
  {
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    stillticking = false;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (!playeringame[i])
	continue;
      cnt_items[i] += 2;
      if (cnt_items[i] >= (plrs[i].sitems * 100) / wbs->maxitems)
	cnt_items[i] = (plrs[i].sitems * 100) / wbs->maxitems;
      else
	stillticking = true;
    }
    if (!stillticking)
    {
      S_StartSound(0, sfx_barexp);
      ng_state++;
    }
  }
  else if (ng_state == 6)
  {
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    stillticking = false;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (!playeringame[i])
	continue;
      cnt_secret[i] += 2;
      if (cnt_secret[i] >= (plrs[i].ssecret * 100) / wbs->maxsecret)
	cnt_secret[i] = (plrs[i].ssecret * 100) / wbs->maxsecret;
      else
	stillticking = true;
    }
    if (!stillticking)
    {
      S_StartSound(0, sfx_barexp);
      ng_state += 1 + 2*!dofrags;
    }
  }
  else if (ng_state == 8)
  {
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    stillticking = false;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (!playeringame[i])
	continue;
      cnt_frags[i] += 1;
      if (cnt_frags[i] >= (fsum = WI_fragSum(i)))
	cnt_frags[i] = fsum;
      else
	stillticking = true;
    }
    if (!stillticking)
    {
      S_StartSound(0, sfx_pldeth);
      ng_state++;
    }
  }
  else if (ng_state == 10)
  {
    if (acceleratestage)
    {
      S_StartSound(0, sfx_sgcock);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
      if (commercial)
	WI_initNoState();
      else
#endif
	WI_initShowNextLoc();
    }
  }
  else if (ng_state & 1)
  {
    if (!--cnt_pause)
    {
      ng_state++;
      cnt_pause = TICRATE;
    }
  }
}


void WI_drawNetgameStats(void)
{
  int i, x, y;
  int pwidth = SHORT(percent->width);

  WI_slamBackground();
  WI_drawAnimatedBack();  // draw animated background
  WI_drawLF();
  // draw stat titles (top line)
  V_DrawPatch(NG_STATSX+NG_SPACINGX-SHORT(kills->width),
    NG_STATSY, FB, kills);
  V_DrawPatch(NG_STATSX+2*NG_SPACINGX-SHORT(items->width),
    NG_STATSY, FB, items);
  V_DrawPatch(NG_STATSX+3*NG_SPACINGX-SHORT(secret->width),
    NG_STATSY, FB, secret);
  if (dofrags)
    V_DrawPatch(NG_STATSX+4*NG_SPACINGX-SHORT(frags->width),
      NG_STATSY, FB, frags);
  // draw stats
  y = NG_STATSY + SHORT(kills->height);
  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (!playeringame[i])
      continue;
    x = NG_STATSX;
    V_DrawPatch(x-SHORT(p[i]->width), y, FB, p[i]);
    if (i == me)
      V_DrawPatch(x-SHORT(p[i]->width), y, FB, star);
	x += NG_SPACINGX;
    WI_drawPercent(x-pwidth, y+10, cnt_kills[i]);	x += NG_SPACINGX;
    WI_drawPercent(x-pwidth, y+10, cnt_items[i]);	x += NG_SPACINGX;
    WI_drawPercent(x-pwidth, y+10, cnt_secret[i]);	x += NG_SPACINGX;
    if (dofrags)
#if (APPVER_DOOMREV < AV_DR_DM1666P)
      WI_drawNum(x, y+10, cnt_frags[i], 0);
#else
      WI_drawNum(x, y+10, cnt_frags[i], -1);
#endif
    y += WI_SPACINGY;
  }
}


static int sp_state;

void WI_initStats(void)
{
  state = StatCount;
  acceleratestage = 0;
  sp_state = 1;
  cnt_kills[0] = cnt_items[0] = cnt_secret[0] = -1;
  cnt_time = cnt_par = -1;
  cnt_pause = TICRATE;
  WI_initAnimatedBack();
}

void WI_updateStats(void)
{
  WI_updateAnimatedBack();
  if (acceleratestage && sp_state != 10)
  {
    acceleratestage = 0;
    cnt_kills[0] = (plrs[me].skills * 100) / wbs->maxkills;
    cnt_items[0] = (plrs[me].sitems * 100) / wbs->maxitems;
    cnt_secret[0] = (plrs[me].ssecret * 100) / wbs->maxsecret;
    cnt_time = plrs[me].stime / TICRATE;
    cnt_par = wbs->partime / TICRATE;
    S_StartSound(0, sfx_barexp);
    sp_state = 10;
  }
  if (sp_state == 2)
  {
    cnt_kills[0] += 2;
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    if (cnt_kills[0] >= (plrs[me].skills * 100) / wbs->maxkills)
    {
      cnt_kills[0] = (plrs[me].skills * 100) / wbs->maxkills;
      S_StartSound(0, sfx_barexp);
      sp_state++;
    }
  }
  else if (sp_state == 4)
  {
    cnt_items[0] += 2;
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    if (cnt_items[0] >= (plrs[me].sitems * 100) / wbs->maxitems)
    {
      cnt_items[0] = (plrs[me].sitems * 100) / wbs->maxitems;
      S_StartSound(0, sfx_barexp);
      sp_state++;
    }
  }
  else if (sp_state == 6)
  {
   cnt_secret[0] += 2;
   if (!(bcnt&3))
     S_StartSound(0, sfx_pistol);
   if (cnt_secret[0] >= (plrs[me].ssecret * 100) / wbs->maxsecret)
   {
     cnt_secret[0] = (plrs[me].ssecret * 100) / wbs->maxsecret;
     S_StartSound(0, sfx_barexp);
     sp_state++;
   }
  }
  else if (sp_state == 8)
  {
    if (!(bcnt&3))
      S_StartSound(0, sfx_pistol);
    cnt_time += 3;
    if (cnt_time >= plrs[me].stime / TICRATE)
      cnt_time = plrs[me].stime / TICRATE;
    cnt_par += 3;
    if (cnt_par >= wbs->partime / TICRATE)
    {
      cnt_par = wbs->partime / TICRATE;
      if (cnt_time >= plrs[me].stime / TICRATE)
      {
	S_StartSound(0, sfx_barexp);
	sp_state++;
      }
    }
  }
  else if (sp_state == 10)
  {
    if (acceleratestage)
    {
      S_StartSound(0, sfx_sgcock);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
      if (commercial)
	WI_initNoState();
      else
#endif
	WI_initShowNextLoc();
    }
  }
  else if (sp_state & 1)
  {
    if (!--cnt_pause)
    {
      sp_state++;
      cnt_pause = TICRATE;
    }
  }
}

void WI_drawStats(void)
{
  int lh;	// line height

  lh = (3*SHORT(num[0]->height))/2;
  WI_slamBackground();
  WI_drawAnimatedBack(); // draw animated background
  WI_drawLF();
  V_DrawPatch(SP_STATSX, SP_STATSY, FB, kills);
  WI_drawPercent(SCREENWIDTH - SP_STATSX, SP_STATSY, cnt_kills[0]);
  V_DrawPatch(SP_STATSX, SP_STATSY+lh, FB, items);
  WI_drawPercent(SCREENWIDTH - SP_STATSX, SP_STATSY+lh, cnt_items[0]);
  V_DrawPatch(SP_STATSX, SP_STATSY+2*lh, FB, sp_secret);
  WI_drawPercent(SCREENWIDTH - SP_STATSX, SP_STATSY+2*lh, cnt_secret[0]);
  V_DrawPatch(SP_TIMEX, SP_TIMEY, FB, time);
  WI_drawTime(SCREENWIDTH/2 - SP_TIMEX, SP_TIMEY, cnt_time);
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
  if (wbs->epsd < 3)
#endif
  {
    V_DrawPatch(SCREENWIDTH/2 + SP_TIMEX, SP_TIMEY, FB, par);
    WI_drawTime(SCREENWIDTH - SP_TIMEX, SP_TIMEY, cnt_par);
  }
}

void WI_checkForAccelerate(void)
{
  int i;
  player_t *player;

  // check for button presses to skip delays
  for (i=0, player = players ; i<MAXPLAYERS ; i++, player++)
  {
    if (playeringame[i])
    {
      if (player->cmd.buttons & BT_ATTACK)
      {
	if (!player->attackdown)
	  acceleratestage = 1;
	player->attackdown = true;
      }
      else
	player->attackdown = false;
      if (player->cmd.buttons & BT_USE)
      {
	if (!player->usedown)
	 acceleratestage = 1;
	player->usedown = true;
      }
      else
	player->usedown = false;
    }
  }
}

// Updates stuff each tick
void WI_Ticker(void)
{
  bcnt++;  // counter for general background animation
  if (bcnt == 1)
  {
    // intermission music
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
    if (commercial)
      S_ChangeMusic(mus_dm2int, true);
    else
#endif
      S_ChangeMusic(mus_inter, true); 
  }
  WI_checkForAccelerate();
  switch (state)
  {
    case StatCount:
      if (deathmatch) WI_updateDeathmatchStats();
      else if (netgame) WI_updateNetgameStats();
      else WI_updateStats();
      break;
    case ShowNextLoc:
      WI_updateShowNextLoc();
      break;
    case NoState:
      WI_updateNoState();
      break;
  }
}


void WI_loadData(void)
{
  int i, j;
  char name[9];
  anim_t *a;
  
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (commercial)
    strcpy(name, "INTERPIC");
  else 
#endif
    sprintf(name, "WIMAP%d", wbs->epsd);
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
  if (wbs->epsd == 3)
    strcpy(name,"INTERPIC");
#endif
#if (APPVER_DOOMREV < AV_DR_DM1666P)
  bg = W_CacheLumpName(name, PU_STATIC);    // background
#else
  bg = W_CacheLumpName(name, PU_CACHE);    // background
  V_DrawPatch(0, 0, 1, bg);
#endif
// unsigned char *pic = screens[1];
// if (commercial)
// {
// darken the background image
// while (pic != screens[1] + SCREENHEIGHT*SCREENWIDTH)
// {
//   *pic = colormaps[256*25 + *pic];
//   pic++;
// }
//}
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (commercial)
  {
    NUMCMAPS = 32;								
    lnames = (patch_t **) Z_Malloc(sizeof(patch_t*) * NUMCMAPS, PU_STATIC, 0);
    for (i=0 ; i<NUMCMAPS ; i++)
    {								
      sprintf(name, "CWILV%2.2d", i);
      lnames[i] = W_CacheLumpName(name, PU_STATIC);
    }					
  }
  else
#endif
  {
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
    lnames = (patch_t **) Z_Malloc(sizeof(patch_t*) * NUMMAPS, PU_STATIC, 0);
#endif
#if (APPVER_DOOMREV == AV_DR_DM19UP)
    if (wbs->epsd == 3)
      for (i=0 ; i<6 ; i++)
      {
        sprintf(name, "WILV3%d", i);
        lnames[i] = W_CacheLumpName(name, PU_STATIC);
      }
    else
#endif
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
    for (i=0 ; i<NUMMAPS ; i++)
    {
      sprintf(name, "WILV%d%d", wbs->epsd, i);
      lnames[i] = W_CacheLumpName(name, PU_STATIC);
    }
#endif
    yah[0] = W_CacheLumpName("WIURH0", PU_STATIC);// you are here
    yah[1] = W_CacheLumpName("WIURH1", PU_STATIC);// you are here (alt.)
    splat = W_CacheLumpName("WISPLAT", PU_STATIC); // splat
#if (APPVER_DOOMREV < AV_DR_DM1666P)
    for (i=0 ; i<NUMMAPS ; i++)
    {
      sprintf(name, "WILV%d%d", wbs->epsd, i);
      lnames[i] = W_CacheLumpName(name, PU_STATIC);
    }
#endif
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
#if (APPVER_DOOMREV >= AV_DR_DM19UP)
    if (wbs->epsd < 3)
#endif
      for (j=0;j<NUMANIMS[wbs->epsd];j++)
      {
	a = &anims[wbs->epsd][j];
	for (i=0;i<a->nanims;i++)
	  if (wbs->epsd != 1 || j != 8) // MONDO HACK!
	  {			
	    sprintf(name, "WIA%d%.2d%.2d", wbs->epsd, j, i);  // animations
	    a->p[i] = W_CacheLumpName(name, PU_STATIC);
          }
          else
            a->p[i] = anims[1][4].p[i]; // HACK ALERT!
      }
#endif
  }
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  wiminus = W_CacheLumpName("WIMINUS", PU_STATIC); // More hacks on minus sign.
#endif
  for (i=0;i<10;i++)
  {
    sprintf(name, "WINUM%d", i);     // numbers 0-9
    num[i] = W_CacheLumpName(name, PU_STATIC);
  }
  percent = W_CacheLumpName("WIPCNT", PU_STATIC);// percent sign
  finished = W_CacheLumpName("WIF", PU_STATIC);// "finished"
  entering = W_CacheLumpName("WIENTER", PU_STATIC);// "entering"
  kills = W_CacheLumpName("WIOSTK", PU_STATIC);   // "kills"
  secret = W_CacheLumpName("WIOSTS", PU_STATIC);// "scrt"
  sp_secret = W_CacheLumpName("WISCRT2", PU_STATIC);// "secret"
#if (APPVER_DOOMREV >= AV_DR_DM18FR)
  if (french) // Yuck. 
  {
    if (netgame && !deathmatch)
      items = W_CacheLumpName("WIOBJ", PU_STATIC);    // "items"
    else
      items = W_CacheLumpName("WIOSTI", PU_STATIC);
  } else
#endif
    items = W_CacheLumpName("WIOSTI", PU_STATIC);
  frags = W_CacheLumpName("WIFRGS", PU_STATIC);    // "frgs"
  colon = W_CacheLumpName("WICOLON", PU_STATIC); // ":"
  time = W_CacheLumpName("WITIME", PU_STATIC);   // "time"
  sucks = W_CacheLumpName("WISUCKS", PU_STATIC);  // "sucks"
  par = W_CacheLumpName("WIPAR", PU_STATIC);   // "par"
  killers = W_CacheLumpName("WIKILRS", PU_STATIC); // "killers" (vertical)
  victims = W_CacheLumpName("WIVCTMS", PU_STATIC); // "victims" (horiz)
  total = W_CacheLumpName("WIMSTT", PU_STATIC);   // "total"
  star = W_CacheLumpName("STFST01", PU_STATIC);	// your face
  bstar = W_CacheLumpName("STFDEAD0", PU_STATIC);    // dead face
  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    sprintf(name, "STPB%d", i);      // "1,2,3,4"
    p[i] = W_CacheLumpName(name, PU_STATIC);
    sprintf(name, "WIBP%d", i+1);     // "1,2,3,4"
    bp[i] = W_CacheLumpName(name, PU_STATIC);
  }
#if (APPVER_DOOMREV < AV_DR_DM1666P)
      for (j=0;j<NUMANIMS[wbs->epsd];j++)
      {
	a = &anims[wbs->epsd][j];
	for (i=0;i<a->nanims;i++)
	  if (wbs->epsd != 1 || j != 8) // MONDO HACK!
	  {			
	    sprintf(name, "WIA%d%.2d%.2d", wbs->epsd, j, i);  // animations
	    a->p[i] = W_CacheLumpName(name, PU_STATIC);
          }
          else
            a->p[i] = anims[1][4].p[i]; // HACK ALERT!
      }
#endif
}

void WI_unloadData(void)
{
  int i, j;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
  Z_ChangeTag(bg, PU_CACHE);
  Z_ChangeTag(yah[0], PU_CACHE); Z_ChangeTag(yah[1], PU_CACHE);
  Z_ChangeTag(splat, PU_CACHE);
#endif
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  Z_ChangeTag(wiminus, PU_CACHE);
#endif
  for (i=0 ; i<10 ; i++) Z_ChangeTag(num[i], PU_CACHE);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (commercial)
  {
    for (i=0 ; i<NUMCMAPS ; i++) Z_ChangeTag(lnames[i], PU_CACHE);
  }
  else
#endif
  {
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
    Z_ChangeTag(yah[0], PU_CACHE); Z_ChangeTag(yah[1], PU_CACHE);
    Z_ChangeTag(splat, PU_CACHE);
#endif
    for (i=0 ; i<NUMMAPS ; i++) Z_ChangeTag(lnames[i], PU_CACHE);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
#if (APPVER_DOOMREV >= AV_DR_DM19U)
    if (wbs->epsd < 3)
#endif
    {
      for (j=0;j<NUMANIMS[wbs->epsd];j++)
      {
	if (wbs->epsd != 1 || j != 8)
	  for (i=0;i<anims[wbs->epsd][j].nanims;i++)
	    Z_ChangeTag(anims[wbs->epsd][j].p[i], PU_CACHE);
      }
    }
#endif
  }
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  Z_Free(lnames);
#endif
  Z_ChangeTag(percent, PU_CACHE);
  Z_ChangeTag(colon, PU_CACHE);
  Z_ChangeTag(finished, PU_CACHE);
  Z_ChangeTag(entering, PU_CACHE);
  Z_ChangeTag(kills, PU_CACHE);
  Z_ChangeTag(secret, PU_CACHE);
  Z_ChangeTag(sp_secret, PU_CACHE);
  Z_ChangeTag(items, PU_CACHE);
  Z_ChangeTag(frags, PU_CACHE);
  Z_ChangeTag(time, PU_CACHE);
  Z_ChangeTag(sucks, PU_CACHE);
  Z_ChangeTag(par, PU_CACHE);

  Z_ChangeTag(victims, PU_CACHE);
  Z_ChangeTag(killers, PU_CACHE);
  Z_ChangeTag(total, PU_CACHE);
//  Z_ChangeTag(star, PU_CACHE);
//  Z_ChangeTag(bstar, PU_CACHE);
  for (i=0 ; i<MAXPLAYERS ; i++) Z_ChangeTag(p[i], PU_CACHE);
  for (i=0 ; i<MAXPLAYERS ; i++) Z_ChangeTag(bp[i], PU_CACHE);
#if (APPVER_DOOMREV < AV_DR_DM1666P)
      for (j=0;j<NUMANIMS[wbs->epsd];j++)
      {
	if (wbs->epsd != 1 || j != 8)
	  for (i=0;i<anims[wbs->epsd][j].nanims;i++)
	    Z_ChangeTag(anims[wbs->epsd][j].p[i], PU_CACHE);
      }
#endif
}


void WI_Drawer (void)
{
switch (state)
{
  case StatCount:
    if (deathmatch)
      WI_drawDeathmatchStats();
    else if (netgame)
      WI_drawNetgameStats();
    else
      WI_drawStats();
    break;
  case ShowNextLoc:
    WI_drawShowNextLoc();
    break;	
  case NoState:
    WI_drawNoState();
    break;
  }
}

void WI_initVariables(wbstartstruct_t *wbstartstruct)
{
#if (APPVER_DOOMREV < AV_DR_DM12)
  acceleratestage = 0;
  wbs = wbstartstruct;
  cnt = bcnt = 0;
  firstrefresh = 1;
  me = wbs->pnum;
  plrs = wbs->plyr;
  if (!wbs->maxkills) wbs->maxkills = 1;
  if (!wbs->maxitems) wbs->maxitems = 1;
  if (!wbs->maxsecret) wbs->maxsecret = 1;
  if (wbs->epsd > 2) wbs->epsd -= 3;
#else
  wbs = wbstartstruct;

#ifdef RANGECHECKING
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
  if (!commercial)
#endif
  {
#if (APPVER_DOOMREV < AV_DR_DM19UP)
    RNGCHECK(wbs->epsd, 0, 2);
#else
    RNGCHECK(wbs->epsd, 0, 3);
#endif



    RNGCHECK(wbs->last, 0, 8);
    RNGCHECK(wbs->next, 0, 8);
  }
  RNGCHECK(wbs->pnum, 0, MAXPLAYERS);
  RNGCHECK(wbs->pnum, 0, MAXPLAYERS);
#endif

  acceleratestage = 0;
  cnt = bcnt = 0;
  firstrefresh = 1;
  me = wbs->pnum;
  plrs = wbs->plyr;
  if (!wbs->maxkills) wbs->maxkills = 1;
  if (!wbs->maxitems) wbs->maxitems = 1;
  if (!wbs->maxsecret) wbs->maxsecret = 1;
#if (APPVER_DOOMREV < AV_DR_DM19UP)
  if (wbs->epsd > 2) wbs->epsd -= 3;
#endif
#endif
}

void WI_Start(wbstartstruct_t *wbstartstruct)
{
  WI_initVariables(wbstartstruct);
  WI_loadData();
#if (APPVER_DOOMREV < AV_DR_DM12)
  if (wbs->last != -1)
  {
      if (deathmatch)
          WI_initDeathmatchStats();
      else if (netgame)
          WI_initNetgameStats();
      else
          WI_initStats();
  }
  else
      WI_initShowNextLoc();
#else
  if (deathmatch)
    WI_initDeathmatchStats();
  else if (netgame)
    WI_initNetgameStats();
  else
    WI_initStats();
#endif
}
