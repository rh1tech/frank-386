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

// R_draw.c

extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9, _wp10;
extern int _wp11, _wp12, _wp13, _wp14, _wp15, _wp16, _wp17, _wp18, _wp19, _wp20, _wp21;
#include "DoomDef.h"
#if (APPVER_DOOMREV < AV_DR_DM12)
extern int _wp22, _wp23, _wp24, _wp25, _wp26, _wp27, _wp28, _wp29, _wp30, _wp31, _wp32;
extern int _wp33, _wp34, _wp35, _wp36, _wp37, _wp38, _wp39, _wp40, _wp41, _wp42, _wp43;
#else
extern int _wp22, _wp23, _wp24, _wp25, _wp26, _wp27, _wp28, _wp29, _wp30, _wp31, _wp32;
extern int _wp33, _wp34, _wp35, _wp36, _wp37, _wp38, _wp39, _wp40, _wp41;
#endif
#include "R_local.h"

#define SC_INDEX			0x3c4
#define SC_MAPMASK			2
#define GC_INDEX			0x3ce
#define GC_READMAP			4
#define GC_MODE				5

/*

All drawing to the view buffer is accomplished in this file.  The other refresh
files only know about ccordinates, not the architecture of the frame buffer.

*/

byte *viewimage;
int viewwidth, scaledviewwidth, viewheight, viewwindowx, viewwindowy;
byte *ylookup[MAXHEIGHT];
int columnofs[MAXWIDTH];
byte translations[3][256]; // color tables for different players

/*
==================
=
= R_DrawColumn
=
= Source is the top of the column to scale
=
==================
*/

lighttable_t	*dc_colormap;
int				dc_x;
int				dc_yl;
int				dc_yh;
fixed_t			dc_iscale;
fixed_t			dc_texturemid;
byte			*dc_source;		// first pixel in a column (possibly virtual)

int				dccount;		// just for profiling

#ifndef __WATCOMC__
#ifndef __i386
#ifndef __m68k
void R_DrawColumn (void)
{
	int			count;
	byte		*dest;
	fixed_t		frac, fracstep;	

	count = dc_yh - dc_yl;
	if (count < 0)
		return;
				
#ifdef RANGECHECK
	if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
		I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
#endif

	dest = ylookup[dc_yl] + columnofs[dc_x]; 
	
	fracstep = dc_iscale;
	frac = dc_texturemid + (dc_yl-centery)*fracstep;

	do
	{
		*dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
		dest += SCREENWIDTH;
		frac += fracstep;
	} while (count--);
}
#endif		// __m68k
#endif		// __i386
#endif

#ifndef __WATCOMC__
#ifndef __i386
#ifndef __m68k
void R_DrawColumnLow (void)
{
	int			count;
	byte		*dest;
	fixed_t		frac, fracstep;	

	count = dc_yh - dc_yl;
	if (count < 0)
		return;
				
#ifdef RANGECHECK
	if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
		I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
//	dccount++;
#endif

	dest = ylookup[dc_yl] + columnofs[dc_x]; 
	
	fracstep = dc_iscale;
	frac = dc_texturemid + (dc_yl-centery)*fracstep;

	do
	{
		*dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
		dest += SCREENWIDTH;
		frac += fracstep;
	} while (count--);
}
#endif		// __m68k
#endif		// __i386
#endif


#define FUZZTABLE	50

#ifdef __WATCOMC__
#define FUZZOFF	(SCREENWIDTH/4)
#else
#define FUZZOFF	(SCREENWIDTH)
#endif
int		fuzzoffset[FUZZTABLE] = {
FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF
};
int fuzzpos = 0;

void R_DrawFuzzColumn (void)
{
	int			count;
	byte		*dest;
	fixed_t		frac, fracstep;	

	if (!dc_yl)
		dc_yl = 1;
	if (dc_yh == viewheight-1)
		dc_yh = viewheight - 2;
		
	count = dc_yh - dc_yl;
	if (count < 0)
		return;
				
#ifdef RANGECHECK
	if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
		I_Error ("R_DrawFuzzColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
#endif

#ifdef __WATCOMC__
	if (detailshift)
	{
		if (dc_x & 1)
		{
			outpw (GC_INDEX,GC_READMAP+(2<<8) ); 
			outp (SC_INDEX+1,12); 
		}
		else
		{
			outpw (GC_INDEX,GC_READMAP); 
			outp (SC_INDEX+1,3); 
		}
		dest = destview + dc_yl*80 + (dc_x>>1); 
	}
	else
	{
		outpw (GC_INDEX,GC_READMAP+((dc_x&3)<<8) ); 
		outp (SC_INDEX+1,1<<(dc_x&3)); 
		dest = destview + dc_yl*80 + (dc_x>>2); 
	}
#else
	dest = ylookup[dc_yl] + columnofs[dc_x];
#endif

	fracstep = dc_iscale;
	frac = dc_texturemid + (dc_yl-centery)*fracstep;

	do
	{
		*dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]];
		if (++fuzzpos == FUZZTABLE)
			fuzzpos = 0;
#ifdef __WATCOMC__
		dest += SCREENWIDTH/4;
#else
		dest += SCREENWIDTH;
#endif
		frac += fracstep;
	} while (count--);
}

/*
========================
=
= R_DrawTranslatedColumn
=
========================
*/

byte *dc_translation;
byte *translationtables;

void R_DrawTranslatedColumn (void)
{
	int			count;
	byte		*dest;
	fixed_t		frac, fracstep;	

	count = dc_yh - dc_yl;
	if (count < 0)
		return;
				
#ifdef RANGECHECK
	if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
		I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
#endif

#ifdef __WATCOMC__
	if (detailshift)
	{
		if (dc_x & 1)
			outp (SC_INDEX+1,12); 
		else
			outp (SC_INDEX+1,3);
	
		dest = destview + dc_yl*80 + (dc_x>>1); 
	}
	else
	{
		outp (SC_INDEX+1,1<<(dc_x&3)); 
		dest = destview + dc_yl*80 + (dc_x>>2); 
	}
#else
	dest = ylookup[dc_yl] + columnofs[dc_x];
#endif
	
	fracstep = dc_iscale;
	frac = dc_texturemid + (dc_yl-centery)*fracstep;

	do
	{
		*dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
#ifdef __WATCOMC__
		dest += SCREENWIDTH/4;
#else
		dest += SCREENWIDTH;
#endif
		frac += fracstep;
	} while (count--);
}

/*
========================
=
= R_InitTranslationTables
=
========================
*/

void R_InitTranslationTables (void)
{
	int		i;

	translationtables = Z_Malloc (256*3+255, PU_STATIC, 0);
	translationtables = (byte *)(( (int)translationtables + 255 )& ~255);

//
// translate just the 16 green colors
//
	for(i = 0; i < 256; i++)
	{
		if (i >= 0x70 && i<= 0x7f)
		{	// map green ramp to gray, brown, red
			translationtables[i] = 0x60 + (i&0xf);
			translationtables [i+256] = 0x40 + (i&0xf);
			translationtables [i+512] = 0x20 + (i&0xf);
		}
		else
		{
			translationtables[i] = translationtables[i+256] 
			= translationtables[i+512] = i;
		}
	}
}

/*
================
=
= R_DrawSpan
=
================
*/

int				ds_y;
int				ds_x1;
int				ds_x2;
lighttable_t	*ds_colormap;
fixed_t			ds_xfrac;
fixed_t			ds_yfrac;
fixed_t			ds_xstep;
fixed_t			ds_ystep;
byte			*ds_source;		// start of a 64*64 tile image

int				dscount;		// just for profiling

#ifndef __WATCOMC__
#ifndef __i386
#ifndef __m68k
void R_DrawSpan (void)
{
	fixed_t		xfrac, yfrac;
	byte		*dest;
	int			count, spot;
	
#ifdef RANGECHECK
	if (ds_x2 < ds_x1 || ds_x1<0 || ds_x2>=SCREENWIDTH 
	|| (unsigned)ds_y>SCREENHEIGHT)
		I_Error ("R_DrawSpan: %i to %i at %i",ds_x1,ds_x2,ds_y);
//	dscount++;
#endif
	
	xfrac = ds_xfrac;
	yfrac = ds_yfrac;
	
	dest = ylookup[ds_y] + columnofs[ds_x1];	
	count = ds_x2 - ds_x1;
	do
	{
		spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
		*dest++ = ds_colormap[ds_source[spot]];
		xfrac += ds_xstep;
		yfrac += ds_ystep;
	} while (count--);
}
#endif
#endif
#endif

#ifndef __WATCOMC__
#ifndef __i386
#ifndef __m68k
void R_DrawSpanLow (void)
{
	fixed_t		xfrac, yfrac;
	byte		*dest;
	int			count, spot;
	
#ifdef RANGECHECK
	if (ds_x2 < ds_x1 || ds_x1<0 || ds_x2>=SCREENWIDTH 
	|| (unsigned)ds_y>SCREENHEIGHT)
		I_Error ("R_DrawSpan: %i to %i at %i",ds_x1,ds_x2,ds_y);
//	dscount++;
#endif
	
	xfrac = ds_xfrac;
	yfrac = ds_yfrac;
	
	dest = ylookup[ds_y] + columnofs[ds_x1];	
	count = ds_x2 - ds_x1;
	do
	{
		spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
		*dest++ = ds_colormap[ds_source[spot]];
		xfrac += ds_xstep;
		yfrac += ds_ystep;
	} while (count--);
}
#endif
#endif
#endif



/*
================
=
= R_InitBuffer
=
=================
*/

void R_InitBuffer (int width, int height)
{
	int		i;
	
	viewwindowx = (SCREENWIDTH-width) >> 1;
	for (i=0 ; i<width ; i++)
		columnofs[i] = viewwindowx + i;
	if (width == SCREENWIDTH)
		viewwindowy = 0;
	else
		viewwindowy = (SCREENHEIGHT-SBARHEIGHT-height) >> 1;
	for (i=0 ; i<height ; i++)
		ylookup[i] = screens[0] + (i+viewwindowy)*SCREENWIDTH;
}

 


/*
================
=
= R_FillBackScreen
=
= Fills the back screen with a pattern for variable screen sizes
= Also draws a beveled edge.
=================
*/

void R_FillBackScreen (void)
{
	byte		*src, *dest;
	int			i, j;
	int			x, y;
	patch_t		*patch;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	char		name1[] = "FLOOR7_2";
	char		name2[] = "GRNROCK";	
	char		*name;
#endif
	
	if (scaledviewwidth == 320)
		return;

#if (APPVER_DOOMREV < AV_DR_DM1666P)
	src = W_CacheLumpName ("FLOOR7_2", PU_CACHE);
#else
	if (commercial)
		name = name2;
	else
		name = name1;
    
	src = W_CacheLumpName (name, PU_CACHE);
#endif
	dest = screens[1];
	 
	for (y=0 ; y<SCREENHEIGHT-SBARHEIGHT ; y++)
	{
		for (x=0 ; x<SCREENWIDTH/64 ; x++)
		{
			memcpy (dest, src+((y&63)<<6), 64);
			dest += 64;
		}
		if (SCREENWIDTH&63)
		{
			memcpy (dest, src+((y&63)<<6), SCREENWIDTH&63);
			dest += (SCREENWIDTH&63);
		}
	}
	
	patch = W_CacheLumpName ("brdr_t",PU_CACHE);
	for (x=0 ; x<scaledviewwidth ; x+=8)
		V_DrawPatch (viewwindowx+x,viewwindowy-8,1,patch);
	patch = W_CacheLumpName ("brdr_b",PU_CACHE);
	for (x=0 ; x<scaledviewwidth ; x+=8)
		V_DrawPatch (viewwindowx+x,viewwindowy+viewheight,1,patch);
	patch = W_CacheLumpName ("brdr_l",PU_CACHE);
	for (y=0 ; y<viewheight ; y+=8)
		V_DrawPatch (viewwindowx-8,viewwindowy+y,1,patch);
	patch = W_CacheLumpName ("brdr_r",PU_CACHE);
	for (y=0 ; y<viewheight ; y+=8)
		V_DrawPatch (viewwindowx+scaledviewwidth,viewwindowy+y,1,patch);

	V_DrawPatch (viewwindowx-8, viewwindowy-8, 1,
		W_CacheLumpName ("brdr_tl",PU_CACHE));
	V_DrawPatch (viewwindowx+scaledviewwidth, viewwindowy-8, 1,
		W_CacheLumpName ("brdr_tr",PU_CACHE));
	V_DrawPatch (viewwindowx-8, viewwindowy+viewheight, 1,
		W_CacheLumpName ("brdr_bl",PU_CACHE));
	V_DrawPatch (viewwindowx+scaledviewwidth, viewwindowy+viewheight, 1,
		W_CacheLumpName ("brdr_br",PU_CACHE));

#ifdef __WATCOMC__
	dest = (byte*)0xac000;
	src = screens[1];
	for (i = 0; i < 4; i++, src++)
	{
		outp (SC_INDEX, 2);
		outp (SC_INDEX+1, 1<<i);
		for (j = 0; j < (SCREENHEIGHT-SBARHEIGHT)*SCREENWIDTH/4; j++)
			dest[j] = src[j*4];
	}
#endif
}


void R_VideoErase (unsigned ofs, int count)
{ 
#ifdef __WATCOMC__
	int		i;
	byte	*src, *dest;
	outp (SC_INDEX, SC_MAPMASK);
	outp (SC_INDEX+1, 15);
	outp (GC_INDEX, GC_MODE);
	outp (GC_INDEX+1, inp (GC_INDEX+1)|1);
	src = (byte*)0xac000+(ofs>>2);
	dest = destscreen+(ofs>>2);
	for (i = (count>>2)-1; i >= 0; i--)
	{
		dest[i] = src[i];
	}
	outp (GC_INDEX, GC_MODE);
	outp (GC_INDEX+1, inp (GC_INDEX+1)&~1);
#else
	memcpy (screens[0]+ofs, screens[1]+ofs, count);
#endif
}


/*
================
=
= R_DrawViewBorder
=
= Draws the border around the view for different size windows
=
=================
*/

void V_MarkRect (int x, int y, int width, int height);
 
void R_DrawViewBorder (void)
{ 
    int		top, side, ofs, i;
 
	if (scaledviewwidth == SCREENWIDTH)
		return;
  
	top = ((SCREENHEIGHT-SBARHEIGHT)-viewheight)/2;
	side = (SCREENWIDTH-scaledviewwidth)/2;

//
// copy top and one line of left side
//
	R_VideoErase (0, top*SCREENWIDTH+side);
 
//
// copy one line of right side and bottom
//
	ofs = (viewheight+top)*SCREENWIDTH-side;
	R_VideoErase (ofs, top*SCREENWIDTH+side);
 
//
// copy sides using wraparound
//
	ofs = top*SCREENWIDTH + SCREENWIDTH-side;
	side <<= 1;
    
	for (i=1 ; i<viewheight ; i++)
	{
		R_VideoErase (ofs, side);
		ofs += SCREENWIDTH;
	}

#ifndef __WATCOMC__
	V_MarkRect (0,0,SCREENWIDTH, SCREENHEIGHT-SBARHEIGHT);
#endif
}
 
 
