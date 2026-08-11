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

// DoomData.h

// all external data is defined here
// most of the data is loaded into different structures at run time

#ifndef __DOOMDATA__
#define __DOOMDATA__

#include <stdint.h>

#ifndef __BYTEBOOL__
#define __BYTEBOOL__
typedef enum {false, true} boolean;
typedef unsigned char byte;
#endif

/*
===============================================================================

						map level types

===============================================================================
*/

// lump order in a map wad
enum {ML_LABEL, ML_THINGS, ML_LINEDEFS, ML_SIDEDEFS, ML_VERTEXES, ML_SEGS,
ML_SSECTORS, ML_NODES, ML_SECTORS , ML_REJECT, ML_BLOCKMAP};


typedef struct
{
	int16_t		x,y;
} mapvertex_t;

typedef struct
{
	int16_t		textureoffset;
	int16_t		rowoffset;
	char		toptexture[8], bottomtexture[8], midtexture[8];
	int16_t		sector;				// on viewer's side
} mapsidedef_t;

typedef struct
{
	int16_t		v1, v2;
	int16_t		flags;
	int16_t		special, tag;
	int16_t		sidenum[2];			// sidenum[1] will be -1 if one sided
} maplinedef_t;

#define	ML_BLOCKING			1
#define	ML_BLOCKMONSTERS	2
#define	ML_TWOSIDED			4		// backside will not be present at all 
									// if not two sided

// if a texture is pegged, the texture will have the end exposed to air held
// constant at the top or bottom of the texture (stairs or pulled down things)
// and will move with a height change of one of the neighbor sectors
// Unpegged textures allways have the first row of the texture at the top
// pixel of the line for both top and bottom textures (windows)
#define	ML_DONTPEGTOP		8
#define	ML_DONTPEGBOTTOM	16

#define ML_SECRET			32	// don't map as two sided: IT'S A SECRET!
#define ML_SOUNDBLOCK		64	// don't let sound cross two of these
#define	ML_DONTDRAW			128	// don't draw on the automap
#define	ML_MAPPED			256	// set if allready drawn in automap


typedef	struct
{
	int16_t		floorheight, ceilingheight;
	char		floorpic[8], ceilingpic[8];
	int16_t		lightlevel;
	int16_t		special, tag;
} mapsector_t;

typedef struct
{
	int16_t		numsegs;
	int16_t		firstseg;			// segs are stored sequentially
} mapsubsector_t;

typedef struct
{
	int16_t		v1, v2;
	int16_t		angle;		
	int16_t		linedef, side;
	int16_t		offset;
} mapseg_t;

enum {BOXTOP,BOXBOTTOM,BOXLEFT,BOXRIGHT};	// bbox coordinates

#define	NF_SUBSECTOR	0x8000
typedef struct
{
	int16_t		x,y,dx,dy;			// partition line
	int16_t		bbox[2][4];			// bounding box for each child
	uint16_t	children[2];		// if NF_SUBSECTOR its a subsector
} mapnode_t;

typedef struct
{
	int16_t		x,y;
	int16_t		angle;
	int16_t		type;
	int16_t		options;
} mapthing_t;

#define	MTF_EASY		1
#define	MTF_NORMAL		2
#define	MTF_HARD		4
#define	MTF_AMBUSH		8

/*
===============================================================================

						texture definition

===============================================================================
*/

typedef struct
{
	int16_t	originx;
	int16_t	originy;
	int16_t	patch;
	int16_t	stepdir;
	int16_t	colormap;
} mappatch_t;

typedef struct
{
	char		name[8];
	int32_t		masked;	
	int16_t		width;
	int16_t		height;
	int32_t		columndirectory;	// OBSOLETE
	int16_t		patchcount;
	mappatch_t	patches[1];
} maptexture_t;


/*
===============================================================================

							graphics

===============================================================================
*/

// posts are runs of non masked source pixels
typedef struct
{
	byte		topdelta;		// -1 is the last post in a column
	byte		length;
// length data bytes follows
} post_t;

// column_t is a list of 0 or more post_t, (byte)-1 terminated
typedef post_t	column_t;

// a patch holds one or more columns
// patches are used for sprites and all masked pictures
typedef struct
{
	int16_t		width;				// bounding box size
	int16_t		height;
	int16_t		leftoffset;			// pixels to the left of origin
	int16_t		topoffset;			// pixels below the origin
	int32_t			columnofs[8];		// only [width] used
									// the [0] is &columnofs[width]
} patch_t;

// a pic is an unmasked block of pixels
typedef struct
{
	byte		width,height;
	byte		data;
} pic_t;




/*
===============================================================================

							status

===============================================================================
*/




/* WAD/on-disk layout must not depend on the compiler ABI. */
_Static_assert(sizeof(mapvertex_t) == 4, "mapvertex_t WAD layout");
_Static_assert(sizeof(mapsidedef_t) == 30, "mapsidedef_t WAD layout");
_Static_assert(sizeof(maplinedef_t) == 14, "maplinedef_t WAD layout");
_Static_assert(sizeof(mapsector_t) == 26, "mapsector_t WAD layout");
_Static_assert(sizeof(mapsubsector_t) == 4, "mapsubsector_t WAD layout");
_Static_assert(sizeof(mapseg_t) == 12, "mapseg_t WAD layout");
_Static_assert(sizeof(mapnode_t) == 28, "mapnode_t WAD layout");
_Static_assert(sizeof(mapthing_t) == 10, "mapthing_t WAD layout");
_Static_assert(sizeof(mappatch_t) == 10, "mappatch_t WAD layout");
_Static_assert(sizeof(maptexture_t) == 32, "maptexture_t WAD layout");
_Static_assert(sizeof(patch_t) == 40, "patch_t WAD layout");

#endif			// __DOOMDATA__

