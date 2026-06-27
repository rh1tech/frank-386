/****************************************************************/
/*                                                              */
/*                           fnode.h                            */
/*                                                              */
/*              Internal File Node for FAT File System          */
/*                                                              */
/*                       January 4, 1992                        */
/*                                                              */
/*                      Copyright (c) 1995                      */
/*                      Pasquale J. Villani                     */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/****************************************************************/

#ifdef MAIN
#ifdef VERSION_STRINGS
static BYTE *fnode_hRcsId =
    "$Id: fnode.h 1432 2009-06-10 16:10:54Z bartoldeman $";
#endif
#endif

struct f_node {
  UWORD f_flags;                /* file flags                   */

  dmatch *f_dmp;                /* this file's dir match        */
  struct dirent f_dir;          /* this file's dir entry image  */

  ULONG f_dirsector;            /* the sector containing dir entry*/
  UBYTE f_diridx;               /* offset/32 of dir entry in sec*/
  /* when dir is not root         */

  /* f_dpb/f_dmp are plain native ARM pointers, not dos_far_ptr, unlike
     sft_dcb/cdsDpb. f_node (the fnode[] array, see kernel.c) is purely
     an internal scratch structure used while servicing a single DOS
     API call - no guest code/DOS API ever sees an f_node or holds a
     pointer to one, so there is nothing for it to need a far pointer
     for. Contrast with sft (sft.h) and cds (cds.h), which guest-visible
     INT 21h/2Fh handles and tables actually point into, and so must
     stay in dos_far_ptr form. f_dpb itself still points at a struct
     dpb that lives in guest memory (allocated via KernelAlloc()/
     DynAlloc()) - sft_to_fnode()/fnode_to_sft() below convert between
     this native pointer and sft_dcb's dos_far_ptr form explicitly. */
  struct dpb *f_dpb;             /* the block device for file    */

  ULONG f_offset;               /* byte offset for next op      */
  CLUSTER f_cluster_offset;     /* relative cluster number within file */
  CLUSTER f_cluster;            /* the cluster we are at        */
  UBYTE f_sft_idx;              /* corresponding SFT index      */
};

typedef struct f_node *f_node_ptr;

struct lfn_inode {
  UNICODE l_name[261];          /* Long file name string          */
                                /* If the string is empty,        */
                                /* then file has the 8.3 name     */
  struct dirent l_dir;          /* Directory entry image          */
  UWORD l_diroff;               /* Current directory entry offset */
};
  
typedef struct lfn_inode FAR * lfn_inode_ptr;
