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

// W_wad.c

#ifdef NeXT
#include <libc.h>
#include <ctype.h>

// next doesn't need a binary flag in open call
#define	O_BINARY	0

#else

#include <malloc.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "DoomDef.h"
extern int _wp1, _wp2, _wp3, _wp4, _wp5, _wp6, _wp7, _wp8, _wp9;
extern int _wp10, _wp11, _wp12, _wp13;


//===============
//   TYPES
//===============


typedef struct
{
	char		identification[4];		// should be IWAD
	int			numlumps;
	int			infotableofs;
} wadinfo_t;


typedef struct
{
	int			filepos;
	int			size;
	char		name[8];
} filelump_t;


//=============
// GLOBALS
//=============

lumpinfo_t	*lumpinfo;		// location of each lump on disk
int			numlumps;

void		**lumpcache;


//===================

#ifdef NeXT

#define strcmpi strcasecmp

void strupr (char *s)
{
    while (*s)
	*s++ = toupper(*s);
}

/*
================
=
= filelength
=
================
*/

int filelength (int handle)
{
    struct stat	fileinfo;
    
    if (fstat (handle,&fileinfo) == -1)
	I_Error ("Error fstating");

    return fileinfo.st_size;
}

#endif


void ExtractFileBase (char *path, char *dest)
{
	char	*src;
	int		length;

	src = path + strlen(path) - 1;

//
// back up until a \ or the start
//
	while (src != path && *(src-1) != '\\' && *(src-1) != '/')
		src--;

//
// copy up to eight characters
//
	memset (dest,0,8);
	length = 0;
	while (*src && *src != '.')
	{
		if (++length == 9)
			I_Error ("Filename base of %s >8 chars",path);
		*dest++ = toupper((int)*src++);
	}
}

/*
============================================================================

						LUMP BASED ROUTINES

============================================================================
*/

/*
====================
=
= W_AddFile
=
= All files are optional, but at least one file must be found
= Files with a .wad extension are wadlink files with multiple lumps
= Other files are single lumps with the base filename for the lump name
=
====================
*/

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
int				reloadlump;
char			*reloadname;
#endif

void W_AddFile (char *filename)
{
	wadinfo_t		header;
	lumpinfo_t		*lump_p;
	unsigned		i;
	int				handle, length;
	int				startlump;
	filelump_t		*fileinfo, singleinfo;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	int				storehandle;
#endif
	
//
// open the file and add to directory
//	
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (filename[0] == '~')		// handle reload indicator
	{
		filename++;
		reloadname = filename;
		reloadlump = numlumps;
	}
#endif
	if ( (handle = open (filename,O_RDONLY | O_BINARY)) == -1)
	{
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
		printf ("	couldn't open %s\n",filename);
#endif
		return;
	}

#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	printf ("	adding %s\n",filename);
#endif
	startlump = numlumps;
	
	if (strcmpi (filename+strlen(filename)-3 , "wad" ) )
	{
	// single lump file
		fileinfo = &singleinfo;
		singleinfo.filepos = 0;
		singleinfo.size = LONG(filelength(handle));
		ExtractFileBase (filename, singleinfo.name);
		numlumps++;
	}
	else 
	{
	// WAD file
		read (handle, &header, sizeof(header));
		if (strncmp(header.identification,"IWAD",4))
		{
			if (strncmp(header.identification,"PWAD",4))
				I_Error ("Wad file %s doesn't have IWAD or PWAD id\n"
				,filename);
			modifiedgame = true;
		}
		header.numlumps = LONG(header.numlumps);
		header.infotableofs = LONG(header.infotableofs);
		length = header.numlumps*sizeof(filelump_t);
		fileinfo = alloca (length);
		lseek (handle, header.infotableofs, SEEK_SET);
		read (handle, fileinfo, length);
		numlumps += header.numlumps;
	}

//
// Fill in lumpinfo
//
	lumpinfo = realloc (lumpinfo, numlumps*sizeof(lumpinfo_t));
#if (APPVER_DOOMREV >= AV_DR_DM12)
	if (!lumpinfo)
		I_Error ("Couldn't realloc lumpinfo");
#endif
	lump_p = &lumpinfo[startlump];
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	storehandle = reloadname ? -1 : handle;
#endif
	for (i=startlump ; i<numlumps ; i++,lump_p++, fileinfo++)
	{
#if (APPVER_DOOMREV < AV_DR_DM1666P)
		lump_p->handle = handle;
#else
		lump_p->handle = storehandle;
#endif
		lump_p->position = LONG(fileinfo->filepos);
		lump_p->size = LONG(fileinfo->size);
		strncpy (lump_p->name, fileinfo->name, 8);
	}
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (reloadname)
		close (handle);
#endif
}




#if (APPVER_DOOMREV >= AV_DR_DM1666P)
/*
====================
=
= W_Reload
=
= Flushes any of the reloadable lumps in memory and reloads the directory
=
====================
*/

void W_Reload (void)
{
	wadinfo_t		header;
	int				lumpcount;
	lumpinfo_t		*lump_p;
	unsigned		i;
	int				handle;
	int				length;
	filelump_t		*fileinfo;
	
	if (!reloadname)
		return;
		
	if ( (handle = open (reloadname,O_RDONLY | O_BINARY)) == -1)
		I_Error ("W_Reload: couldn't open %s",reloadname);

	read (handle, &header, sizeof(header));
	lumpcount = LONG(header.numlumps);
	header.infotableofs = LONG(header.infotableofs);
	length = lumpcount*sizeof(filelump_t);
	fileinfo = alloca (length);
	lseek (handle, header.infotableofs, SEEK_SET);
	read (handle, fileinfo, length);
    
//
// Fill in lumpinfo
//
	lump_p = &lumpinfo[reloadlump];
	
	for (i=reloadlump ; i<reloadlump+lumpcount ; i++,lump_p++, fileinfo++)
	{
		if (lumpcache[i])
			Z_Free (lumpcache[i]);

		lump_p->position = LONG(fileinfo->filepos);
		lump_p->size = LONG(fileinfo->size);
	}
	
	close (handle);
}
#endif



/*
====================
=
= W_InitMultipleFiles
=
= Pass a null terminated list of files to use.
=
= All files are optional, but at least one file must be found
=
= Files with a .wad extension are idlink files with multiple lumps
=
= Other files are single lumps with the base filename for the lump name
=
= Lump names can appear multiple times. The name searcher looks backwards,
= so a later file can override an earlier one.
=
====================
*/

void W_InitMultipleFiles (char **filenames)
{
#if (APPVER_DOOMREV >= AV_DR_DM12)
	int		size;
#endif
	
//
// open all the files, load headers, and count lumps
//
	numlumps = 0;
	lumpinfo = malloc(1);	// will be realloced as lumps are added

	for ( ; *filenames ; filenames++)
		W_AddFile (*filenames);

	if (!numlumps)
		I_Error ("W_InitFiles: no files found");
		
//
// set up caching
//
#if (APPVER_DOOMREV < AV_DR_DM12)
	lumpcache = calloc (numlumps, 4);
#else
	size = numlumps * sizeof(*lumpcache);
	lumpcache = malloc (size);
	if (!lumpcache)
		I_Error ("Couldn't allocate lumpcache");
	memset (lumpcache,0, size);
#endif
}



/*
====================
=
= W_InitFile
=
= Just initialize from a single file
=
====================
*/

void W_InitFile (char *filename)
{
	char	*names[2];

	names[0] = filename;
	names[1] = NULL;
	W_InitMultipleFiles (names);
}



/*
====================
=
= W_NumLumps
=
====================
*/

int	W_NumLumps (void)
{
	return numlumps;
}



/*
====================
=
= W_CheckNumForName
=
= Returns -1 if name not found
=
====================
*/

int	W_CheckNumForName (char *name)
{
	char	name8[9];
	int		v1,v2;
	lumpinfo_t	*lump_p;

// make the name into two integers for easy compares

	strncpy (name8,name,8);
	name8[8] = 0;			// in case the name was a fill 8 chars
	strupr (name8);			// case insensitive

	v1 = *(int *)name8;
	v2 = *(int *)&name8[4];


// scan backwards so patch lump files take precedence

	lump_p = lumpinfo + numlumps;

	while (lump_p-- != lumpinfo)
		if ( *(int *)lump_p->name == v1 && *(int *)&lump_p->name[4] == v2)
			return lump_p - lumpinfo;


	return -1;
}


/*
====================
=
= W_GetNumForName
=
= Calls W_CheckNumForName, but bombs out if not found
=
====================
*/

int	W_GetNumForName (char *name)
{
	int	i;

	i = W_CheckNumForName (name);
	if (i != -1)
		return i;

	I_Error ("W_GetNumForName: %s not found!",name);
	return -1;
}


/*
====================
=
= W_LumpLength
=
= Returns the buffer size needed to load the given lump
=
====================
*/

int W_LumpLength (int lump)
{
	if (lump >= numlumps)
		I_Error ("W_LumpLength: %i >= numlumps",lump);
	return lumpinfo[lump].size;
}


/*
====================
=
= W_ReadLump
=
= Loads the lump into the given buffer, which must be >= W_LumpLength()
=
====================
*/

void W_ReadLump (int lump, void *dest)
{
	int			c;
	lumpinfo_t	*l;
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	int			handle;
#endif
	
	if (lump >= numlumps)
		I_Error ("W_ReadLump: %i >= numlumps",lump);
	l = lumpinfo+lump;
	
	I_BeginRead ();
#if (APPVER_DOOMREV < AV_DR_DM1666P)
	lseek (l->handle, l->position, SEEK_SET);
	c = read (l->handle, dest, l->size);
#else
	if (l->handle == -1)
	{	// reloadable file, so use open / read / close
		if ( (handle = open (reloadname,O_RDONLY | O_BINARY)) == -1)
			I_Error ("W_ReadLump: couldn't open %s",reloadname);
	}
	else
		handle = l->handle;
	lseek (handle, l->position, SEEK_SET);
	c = read (handle, dest, l->size);
#endif
	if (c < l->size)
		I_Error ("W_ReadLump: only read %i of %i on lump %i",c,l->size,lump);
#if (APPVER_DOOMREV >= AV_DR_DM1666P)
	if (l->handle == -1)
		close (handle);
#endif
	I_EndRead ();
}



/*
====================
=
= W_CacheLumpNum
=
====================
*/

void	*W_CacheLumpNum (int lump, int tag)
{
byte *ptr;

	if ((unsigned)lump >= numlumps)
		I_Error ("W_CacheLumpNum: %i >= numlumps",lump);
		
	if (!lumpcache[lump])
	{	// read the lump in
//printf ("cache miss on lump %i\n",lump);
		ptr = Z_Malloc (W_LumpLength (lump), tag, &lumpcache[lump]);
		W_ReadLump (lump, lumpcache[lump]);
	}
	else
	{
//printf ("cache hit on lump %i\n",lump);
		Z_ChangeTag (lumpcache[lump],tag);
	}
	
	return lumpcache[lump];
}


/*
====================
=
= W_CacheLumpName
=
====================
*/

void	*W_CacheLumpName (char *name, int tag)
{
	return W_CacheLumpNum (W_GetNumForName(name), tag);
}


#if (APPVER_DOOMREV >= AV_DR_DM12)
/*
====================
=
= W_Profile
=
====================
*/

int	info[2500][10];
int	profilecount;

void W_Profile (void)
{
	int		i;
	memblock_t	*block;
	void	*ptr;
	char	ch;
	FILE	*f;
	int		j;
	char	name[9];
	
	
	for (i=0 ; i<numlumps ; i++)
	{	
		ptr = lumpcache[i];
		if (!ptr)
		{
			ch = ' ';
			continue;
		}
		else
		{
			block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
			if (block->tag < PU_PURGELEVEL)
				ch = 'S';
			else
				ch = 'P';
		}
		info[i][profilecount] = ch;
	}
	profilecount++;
	
	f = fopen ("waddump.txt","w");
	name[8] = 0;
	for (i=0 ; i<numlumps ; i++)
	{
		memcpy (name,lumpinfo[i].name,8);
		for (j=0 ; j<8 ; j++)
			if (!name[j])
				break;
		for ( ; j<8 ; j++)
			name[j] = ' ';
		fprintf (f,"%s ",name);
		for (j=0 ; j<profilecount ; j++)
			fprintf (f,"    %c",info[i][j]);
		fprintf (f,"\n");
	}
	fclose (f);
}
#endif
