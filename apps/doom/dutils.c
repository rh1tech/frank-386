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

// DUtils.c
#include "DoomDef.h"
#include "DUtils.h"

list_t *dll_NewList(void)
{
  list_t *list;
  list = (list_t*)Z_Malloc(sizeof(list_t), PU_STATIC, 0);
  list->start = list->end = NULL;
  return list;
}

lnode_t *dll_AddEndNode(list_t *list, void *value)
{
  lnode_t *node;

  if (!list)
    I_Error("Bad list in dll_AddEndNode");
  node = (lnode_t*)Z_Malloc(sizeof(lnode_t), PU_STATIC, 0);
  node->value = value;
  node->next = NULL;
  node->prev = list->end;
  if (list->end)
    list->end->next = node;
  else
    list->start = node;
  list->end = node;
  return node;
}

lnode_t *dll_AddStartNode(list_t *list, void *value)
{
  lnode_t *node;

  if (!list)
    I_Error("Bad list in dll_AddStartNode");
  node = (lnode_t*)Z_Malloc(sizeof(lnode_t), PU_STATIC, 0);
  node->value = value;
  node->next = list->start;
  node->prev = NULL;
  if (list->start)
    list->start->prev = node;
  else
    list->end = node;
  list->start = node;
  return node;
}

void *dll_DelNode(list_t *list, lnode_t *node)
{
  void *value;

  if (!list)
    I_Error("Bad list in dll_DelNode");
  if (!list->start)
    I_Error("Empty list in dll_DelNode");
  value = node->value;
  if (node->prev)
    node->prev->next = node->next;
  if (node->next)
    node->next->prev = node->prev;
  if (node == list->start)
    list->start = node->next;
  if (node == list->end)
    list->end = node->prev;
  Z_Free(node);
  return value;
}

void *dll_DelEndNode(list_t *list)
{
  return dll_DelNode(list, list->end);
}

void *dll_DelStartNode(list_t *list)
{
  return dll_DelNode(list, list->start);
}

//
// CHEAT SEQUENCE PACKAGE
//

static int firsttime = 1;
static unsigned char cheat_xlate_table[256];


//
// Called in st_stuff module, which handles the input.
// Returns a 1 if the cheat was successful, 0 if failed.
//
int cht_CheckCheat(cheatseq_t *cht, char key)
{
  int i;
  int rc = 0;

  if (firsttime)
  {
    firsttime = 0;
    for (i=0;i<256;i++) cheat_xlate_table[i] = SCRAMBLE(i);
  }

  if (!cht->p)
    cht->p = cht->sequence; // initialize if first time

  if (*cht->p == 0)
    *(cht->p++) = key;
  else if
    (cheat_xlate_table[(unsigned char)key] == *cht->p) cht->p++;
  else
    cht->p = cht->sequence;

  if (*cht->p == 1)
    cht->p++;
  else if (*cht->p == 0xff) // end of sequence character
  {
    cht->p = cht->sequence;
    rc = 1;
  }

  return rc;
}

void cht_GetParam(cheatseq_t *cht, char *buffer)
{
  unsigned char *p, c;

  p = cht->sequence;
  while (*(p++) != 1);
    
  do
  {
    c = *p;
    *(buffer++) = c;
    *(p++) = 0;
  }
  while (c && *p!=0xff );

  if (*p==0xff)
    *buffer = 0;
}

//
//                       SCREEN WIPE PACKAGE
//

// when zero, stop the wipe
static boolean go = 0;

static byte *wipe_scr_start, *wipe_scr_end, *wipe_scr;


void wipe_shittyColMajorXform(short *array, int width, int height)
{
  int x, y;
  short *dest;

  dest = (short*) Z_Malloc(width*height*2, PU_STATIC, 0);

  for(y=0;y<height;y++)
    for(x=0;x<width;x++)
      dest[x*height+y] = array[y*width+x];

  memcpy(array, dest, width*height*2);

  Z_Free(dest);
}

int wipe_initColorXForm(int width, int height, int ticks)
{
  memcpy(wipe_scr, wipe_scr_start, width*height);
  return 0;
}

int wipe_doColorXForm(int width, int height, int ticks)
{
  boolean changed;
  byte *w, *e;
  int newval;

  changed = false;
  w = wipe_scr;
  e = wipe_scr_end;
  while (w!=wipe_scr+width*height)
  {
    if (*w != *e)
    {
      if (*w > *e)
      {
	newval = *w - ticks;
	if (newval < *e)
	  *w = *e;
	else
	  *w = newval;
	changed = true;
      }
      else if (*w < *e)
      {
	newval = *w + ticks;
	if (newval > *e)
	  *w = *e;
	else
	  *w = newval;
	changed = true;
      }
    }
    w++;
    e++;
  }
  return !changed;
}

int wipe_exitColorXForm(int width, int height, int ticks)
{
    return 0;
}


static int *y;

int wipe_initMelt(int width, int height, int ticks)
{
  int i, r;
    
  // copy start screen to main screen
  memcpy(wipe_scr, wipe_scr_start, width*height); 
  // makes this wipe faster (in theory)
  // to have stuff in column-major format
  wipe_shittyColMajorXform((short*)wipe_scr_start, width/2, height);
  wipe_shittyColMajorXform((short*)wipe_scr_end, width/2, height);
  // setup initial column positions
  // (y<0 => not ready to scroll yet)
  y = (int *) Z_Malloc(width*sizeof(int), PU_STATIC, 0);
  y[0] = -(M_Random()%16);
  for (i=1;i<width;i++)
  {
#if (APPVER_DOOMREV < AV_DR_DM12)
    r = M_Random()%3;
    r = (r == 1) * -1 + (r == 2);
#else
    r = (M_Random()%3) - 1;
#endif
    y[i] = y[i-1] + r;
    if (y[i] > 0) y[i] = 0;
    else if (y[i] == -16) y[i] = -15;
  }
  return 0;
}

int wipe_doMelt(int width, int height, int ticks)
{
  int i, j, dy, idx;
  short *s, *d;
  boolean done = true;

  width/=2;

  while (ticks--)
  {
    for (i=0;i<width;i++)
    {
      if (y[i]<0)
      {
	y[i]++; done = false;
      }
      else if (y[i] < height)
      {
	dy = (y[i] < 16) ? y[i]+1 : 8;
	if (y[i]+dy >= height) dy = height - y[i];
	s = &((short *)wipe_scr_end)[i*height+y[i]];
	d = &((short *)wipe_scr)[y[i]*width+i];
	idx = 0;
	for (j=dy;j;j--)
	{
	  d[idx] = *(s++);
	  idx += width;
	}
	y[i] += dy;
	s = &((short *)wipe_scr_start)[i*height];
	d = &((short *)wipe_scr)[y[i]*width+i];
	idx = 0;
	for (j=height-y[i];j;j--)
	{
	  d[idx] = *(s++);
	  idx += width;
	}
	done = false;
      }
    }
  }

  return done;
}

int wipe_exitMelt(int width, int height, int ticks)
{
  Z_Free(y);
  return 0;
}

int wipe_StartScreen(int x, int y, int width, int height)
{
  wipe_scr_start = screens[2];
  I_ReadScreen(wipe_scr_start);
  return 0;
}

int wipe_EndScreen(int x, int y, int width, int height)
{
  wipe_scr_end = screens[3];
  I_ReadScreen(wipe_scr_end);
  V_DrawBlock(x, y, 0, width, height, wipe_scr_start); // restore start scr.
  return 0;
}

int wipe_ScreenWipe(int wipeno, int x, int y, int width, int height, int ticks)
{
  int rc;
  static int (*wipes[])(int, int, int) =
  {
    wipe_initColorXForm, wipe_doColorXForm, wipe_exitColorXForm,
    wipe_initMelt, wipe_doMelt, wipe_exitMelt
  };

  void V_MarkRect(int, int, int, int);

  // initial stuff
  if (!go)
  {
    go = 1;
    // wipe_scr = (byte *) Z_Malloc(width*height, PU_STATIC, 0); // DEBUG
    wipe_scr = screens[0];
    (*wipes[wipeno*3])(width, height, ticks);
  }

  // do a piece of wipe-in
  V_MarkRect(0, 0, width, height);
  rc = (*wipes[wipeno*3+1])(width, height, ticks);
  //  V_DrawBlock(x, y, 0, width, height, wipe_scr); // DEBUG

  // final stuff
  if (rc)
  {
    go = 0;
    (*wipes[wipeno*3+2])(width, height, ticks);
  }

  return !go;
}
