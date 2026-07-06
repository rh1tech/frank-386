/****************************************************************/
/*                                                              */
/*                          proto.h                             */
/*                                                              */
/*                   Global Function Prototypes                 */
/*                                                              */
/*                   Copyright (c) 1995, 1996                   */
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
static BYTE *Proto_hRcsId =
    "$Id: proto.h 1491 2009-07-18 20:48:44Z bartoldeman $";
#endif
#endif

/* blockio.c */
struct buffer FAR *getblk(ULONG blkno, COUNT dsk, BOOL overwrite);
#define getblock(blkno, dsk) getblk(blkno, dsk, FALSE);
#define getblockOver(blkno, dsk) getblk(blkno, dsk, TRUE);
VOID setinvld(REG COUNT dsk);
BOOL dirty_buffers(REG COUNT dsk);
BOOL flush_buffers(REG COUNT dsk);
BOOL flush(void);
BOOL fill(REG struct buffer FAR * bp, ULONG blkno, COUNT dsk);
BOOL DeleteBlockInBufferCache(ULONG blknolow, ULONG blknohigh, COUNT dsk, int mode);
/* *** Changed on 9/4/00  BER */
UWORD dskxfer(COUNT dsk, ULONG blkno, dos_far_ptr buf, UWORD numblocks, COUNT mode);
/* *** End of change */
void AllocateHMASpace (size_t lowbuffer, size_t highbuffer);

/* break.c */
unsigned char ctrl_break_pressed(void);
unsigned char check_handle_break(dos_far_ptr *pdev);
void handle_break(dos_far_ptr *pdev, int sft_out);
#ifdef __WATCOMC__
#pragma aux handle_break __aborts;
#endif

/* chario.c */
dos_far_ptr/*struct dhdr*/ sft_to_dev(sft *s);
long BinaryCharIO(/*struct dhdr*/dos_far_ptr *pdev, size_t n, dos_far_ptr bp, unsigned command);
int ndread(dos_far_ptr *pdev);
int StdinBusy(void);
void con_flush(dos_far_ptr *pdev);
unsigned char read_char(int sft_in, int sft_out, BOOL check_break);
unsigned char read_char_stdin(BOOL check_break);
long cooked_read(dos_far_ptr/*struct dhdr*/ *pdev, size_t n, char *bp);
void read_line(int sft_in, int sft_out, keyboard FAR * kp);
size_t read_line_handle(int sft_idx, size_t n, char FAR * bp);
void write_char(int c, int sft_idx);
void write_char_stdout(int c);
void update_scr_pos(unsigned char c, unsigned char count);
long cooked_write(dos_far_ptr/*struct dhdr*/ *pdev, size_t n, char *bp);

sft FAR *get_sft(UCOUNT);

/* dosfns.c */
const char FAR *get_root(const char FAR *);
BOOL check_break(void);
UCOUNT GenericReadSft(sft far * sftp, UCOUNT n, void FAR * bp,
                      COUNT * err, BOOL force_binary);
COUNT SftSeek(int sft_idx, LONG new_pos, unsigned mode);
/*COUNT DosRead(COUNT hndl, UCOUNT n, BYTE FAR * bp, COUNT FAR * err); */
void BinarySftIO(int sft_idx, void *bp, int mode);
#define BinaryIO(hndl, bp, mode) BinarySftIO(get_sft_idx(hndl), bp, mode)
long DosRWSft(int sft_idx, size_t n, dos_far_ptr bp, int mode);
#define DosRead(hndl, n, bp) DosRWSft(get_sft_idx(hndl), n, bp, XFR_READ)
#define DosWrite(hndl, n, bp) DosRWSft(get_sft_idx(hndl), n, bp, XFR_WRITE)
ULONG DosSeek(unsigned hndl, LONG new_pos, COUNT mode, int *rc);
long DosOpen(dos_far_ptr fname, unsigned flags, unsigned attrib);
COUNT CloneHandle(unsigned hndl);
long DosDup(unsigned Handle);
COUNT DosForceDup(unsigned OldHandle, unsigned NewHandle);
long DosOpenSft(dos_far_ptr fname, unsigned flags, unsigned attrib);
COUNT DosClose(COUNT hndl);
COUNT DosCloseSft(int sft_idx, BOOL commitonly);
#define DosCommit(hndl) DosCloseSft(get_sft_idx(hndl), TRUE)
UWORD DosGetFree(UBYTE drive, UWORD * navc, UWORD * bps, UWORD * nc);
COUNT DosGetCuDir(UBYTE drive, dos_far_ptr * s);
COUNT DosChangeDir(dos_far_ptr s);
COUNT DosFindFirst(UCOUNT attr, BYTE FAR * name);
COUNT DosFindNext(void);
COUNT DosGetFtime(COUNT hndl, ddate * dp, dtime * tp);
COUNT DosSetFtimeSft(int sft_idx, ddate dp, dtime tp);
#define DosSetFtime(hndl, dp, tp) DosSetFtimeSft(get_sft_idx(hndl), (dp), (tp))
COUNT DosGetFattr(dos_far_ptr name);
COUNT DosSetFattr(dos_far_ptr name, UWORD attrp);
UBYTE DosSelectDrv(UBYTE drv);
COUNT DosDelete(dos_far_ptr path, int attrib);
COUNT DosRename(dos_far_ptr path1, dos_far_ptr path2);
COUNT DosRenameTrue(char* path1, char* path2, int attrib);
COUNT DosMkRmdir(const dos_far_ptr dir, int action);
struct dhdr FAR *IsDevice(const char FAR * FileName);
BOOL IsShareInstalled(BOOL recheck);
COUNT DosLockUnlock(COUNT hndl, LONG pos, LONG len, COUNT unlock);
int idx_to_sft_(int SftIndex);
sft FAR *idx_to_sft(int SftIndex);
int get_sft_idx(UCOUNT hndl);
struct cds FAR *get_cds_unvalidated(unsigned dsk);
/*struct cds*/ dos_far_ptr get_cds(unsigned drive);
struct cds FAR *get_cds1(unsigned dsk);
COUNT DosTruename(dos_far_ptr src, dos_far_ptr dest);

/* dosidle.asm */
VOID ASMCFUNC DosIdle_int(void);
VOID ASMCFUNC DosIdle_hlt(void);
#ifdef __WATCOMC__
#pragma aux (__cdecl) DosIdle_int __modify __exact []
#pragma aux (__cdecl) DosIdle_hlt __modify __exact []
#endif

/* error.c */
VOID dump(void);
#ifdef DEBUG
VOID panic(BYTE * s);
#endif
VOID fatal(BYTE * err_msg);

/* fatdir.c */
VOID dir_init_fnode(f_node_ptr fnp, CLUSTER dirstart);
f_node_ptr dir_open(const char *dirname, BOOL split, f_node_ptr fnp);
COUNT dir_read(REG f_node_ptr fnp);
BOOL dir_write_update(REG f_node_ptr fnp, BOOL update);
#define dir_write(fnp) dir_write_update(fnp, FALSE)
COUNT dos_findfirst(UCOUNT attr, BYTE * name);
COUNT dos_findnext(void);
void ConvertName83ToNameSZ(BYTE FAR * destSZ, BYTE FAR * srcFCBName);
const char *ConvertNameSZToName83(char *destFCBName, const char *srcSZ);

/* fatfs.c */
struct dpb FAR *get_dpb(COUNT dsk);
ULONG clus2phys(CLUSTER cl_no, struct dpb FAR * dpbp);
int dos_open(char * path, unsigned flag, unsigned attrib, int fd);
BOOL fcbmatch(const char *fcbname1, const char *fcbname2);
BOOL fcmp_wild(const char * s1, const char * s2, unsigned n);
VOID touc(BYTE * s, COUNT n);
COUNT dos_close(COUNT fd);
COUNT dos_delete(BYTE * path, int attrib);
COUNT dos_rmdir(BYTE * path);
COUNT dos_rename(BYTE * path1, BYTE * path2, int attrib);
ddate dos_getdate(void);
dtime dos_gettime(void);
UWORD GetBiosKey(int timeout);
BYTE *GetStringArg(BYTE * pLine, BYTE * pszString);
COUNT dos_mkdir(BYTE * dir);
BOOL last_link(f_node_ptr fnp);
COUNT map_cluster(REG f_node_ptr fnp, COUNT mode);
long rwblock(COUNT fd, dos_far_ptr buffer, UCOUNT count, int mode);
COUNT dos_read(COUNT fd, VOID FAR * buffer, UCOUNT count);
COUNT dos_write(COUNT fd, const VOID FAR * buffer, UCOUNT count);
CLUSTER dos_free(struct dpb FAR * dpbp);
BOOL dir_exists(char * path);
VOID dpb16to32(struct dpb FAR *dpbp);

f_node_ptr split_path(const char *, f_node_ptr fnp);

int dos_cd(char * PathName);

COUNT dos_getfattr(BYTE * name);
COUNT dos_setfattr(BYTE * name, UWORD attrp);
COUNT media_check(REG struct dpb FAR * dpbp);
f_node_ptr xlt_fd(COUNT fd);
COUNT xlt_fnp(f_node_ptr fnp);
struct dhdr FAR * select_unit(COUNT drive);
void dos_merge_file_changes(int fd);

/* fattab.c */
void read_fsinfo(struct dpb FAR * dpbp);
void write_fsinfo(struct dpb FAR * dpbp);
CLUSTER link_fat(struct dpb FAR * dpbp, CLUSTER Cluster1,
                 REG CLUSTER Cluster2);
CLUSTER next_cluster(struct dpb FAR * dpbp, REG CLUSTER ClusterNum);
BOOL is_free_cluster(struct dpb FAR * dpbp, REG CLUSTER ClusterNum);

/* fcbfns.c */
VOID DosOutputString(BYTE FAR * s);
int DosCharInputEcho(VOID);
int DosCharInput(VOID);
VOID DosDirectConsoleIO(iregs FAR * r);
VOID DosCharOutput(COUNT c);
VOID DosDisplayOutput(COUNT c);
BYTE FAR *FatGetDrvData(UBYTE drive, UBYTE * spc, UWORD * bps,
                   UWORD * nc);
UWORD FcbParseFname(UBYTE *wTestMode, const BYTE FAR *lpFileName, fcb FAR * lpFcb);
const BYTE FAR *ParseSkipWh(const BYTE FAR * lpFileName);
BOOL TestCmnSeps(BYTE FAR * lpFileName);
BOOL TestFieldSeps(BYTE FAR * lpFileName);
const BYTE FAR *GetNameField(const BYTE FAR * lpFileName, BYTE FAR * lpDestField,
                       COUNT nFieldSize, BOOL * pbWildCard);
UBYTE FcbReadWrite(xfcb FAR *, UCOUNT, int);
UBYTE FcbGetFileSize(xfcb FAR * lpXfcb);
void FcbSetRandom(xfcb FAR * lpXfcb);
UBYTE FcbRandomBlockIO(xfcb FAR * lpXfcb, UWORD *nRecords, int mode);
UBYTE FcbRandomIO(xfcb FAR * lpXfcb, int mode);
UBYTE FcbOpen(xfcb FAR * lpXfcb, unsigned flags);
UBYTE FcbDelete(xfcb FAR * lpXfcb);
UBYTE FcbRename(xfcb FAR * lpXfcb);
UBYTE FcbClose(xfcb FAR * lpXfcb);
void FcbCloseAll(void);
UBYTE FcbFindFirstNext(xfcb FAR * lpXfcb, BOOL First);

/* intr.asm */
UWORD ASMPASCAL call_intr(WORD nr, iregs FAR * rp);
COUNT ASMPASCAL res_DosExec(COUNT mode, exec_blk * ep, BYTE * lp);
UCOUNT res_read(CPU* cpu, int fd, dos_far_ptr buf, UCOUNT count);
#ifdef __WATCOMC__
#pragma aux (__pascal) call_intr __modify __exact [__ax]
#pragma aux (__pascal) res_DosExec __modify __exact [__ax __bx __dx __es]
#pragma aux (__pascal) res_read __modify __exact [__ax __bx __cx __dx]
#endif

/* ioctl.c */
COUNT DosDevIOctl();

/* memmgr.c */
seg far2para(VOID FAR * p);
seg long2para(ULONG size);
void FAR *add_far(void FAR * fp, unsigned off);
VOID FAR *adjust_far(const void FAR * fp);
COUNT DosMemAlloc(UWORD size, COUNT mode, seg * para, UWORD * asize);
COUNT DosMemLargest(UWORD * size);
COUNT DosMemFree(UWORD para);
COUNT DosMemChange(UWORD para, UWORD size, UWORD * maxSize);
COUNT DosMemCheck(void);
COUNT FreeProcessMem(UWORD ps);
COUNT DosGetLargestBlock(UWORD * block);
VOID show_chain(void);
void DosUmbLink(unsigned n);
VOID mcb_print(mcb FAR * mcbp);

/* lfnapi.c */
COUNT lfn_allocate_inode(VOID);
COUNT lfn_free_inode(COUNT handle);

COUNT lfn_setup_inode(COUNT handle, ULONG dirstart, ULONG diroff);

COUNT lfn_create_entries(COUNT handle, lfn_inode_ptr lip);
COUNT lfn_remove_entries(COUNT handle);

COUNT lfn_dir_read(COUNT handle, lfn_inode_ptr lip);
COUNT lfn_dir_write(COUNT handle);

/* nls.c */
BYTE DosYesNo(UWORD ch);
#ifndef DosUpMem
VOID DosUpMem(VOID FAR * str, unsigned len);
#endif
unsigned char ASMCFUNC DosUpChar(unsigned char ch);
VOID DosUpString(char FAR * str);
VOID DosUpFMem(VOID FAR * str, unsigned len);
unsigned char DosUpFChar(unsigned char ch);
VOID DosUpFString(char FAR * str);
COUNT DosGetData(int subfct, UWORD cp, UWORD cntry, UWORD bufsize,
                 VOID FAR * buf);
#ifndef DosGetCountryInformation
COUNT DosGetCountryInformation(UWORD cntry, VOID FAR * buf);
#endif
#ifndef DosSetCountry
COUNT DosSetCountry(UWORD cntry);
#endif
COUNT DosGetCodepage(UWORD * actCP, UWORD * sysCP);
COUNT DosSetCodepage(UWORD actCP, UWORD sysCP);
dos_far_ptr DosGetDBCS(void);
UWORD ASMCFUNC syscall_MUX14(iregs FAR *);

/* prf.c */
#ifdef DEBUG
int VA_CDECL printf(CONST char * fmt, ...);
int VA_CDECL sprintf(char * buff, CONST char * fmt, ...);
#endif
VOID hexd(char *title, VOID FAR * p, COUNT numBytes);
void put_unsigned(unsigned n, int base, int width);
/// TODO: void put_string(const char *s);
#define put_string(x)  do { dos_printf(x); dos_printf("\n"); } while(0)
void put_console(int);

/* strings.c */
size_t /* ASMCFUNC */ ASMPASCAL strlen(const char * s);
#define fstrlen strlen
char FAR * /*ASMCFUNC*/ ASMPASCAL _fstrcpy(char FAR * d, const char FAR * s);
int /*ASMCFUNC*/ ASMPASCAL strcmp(const char * d, const char * s);
#define fstrcmp strcmp
int /*ASMCFUNC*/ ASMPASCAL fstrncmp(const char FAR * d, const char FAR * s, size_t l);
int /*ASMCFUNC*/ ASMPASCAL strncmp(const char * d, const char * s, size_t l);
char * /*ASMCFUNC*/ ASMPASCAL strchr(const char * s, int c);
char FAR * /*ASMCFUNC*/ ASMPASCAL fstrchr(const char FAR * s, int c);
#define fmemchr memchr

/* misc.c */
char * /*ASMCFUNC*/ ASMPASCAL strcpy(char * d, const char * s);
void /*ASMCFUNC*/ ASMPASCAL fmemcpyBack(void FAR * d, const void FAR * s, size_t n);
void /*ASMCFUNC*/ ASMPASCAL fmemcpy(dos_far_ptr d, const dos_far_ptr s, size_t n);
#define fstrcpy strcpy
void * /*ASMCFUNC*/ ASMPASCAL memcpy(void *d, const void * s, size_t n);
void * /*ASMCFUNC*/ ASMPASCAL fmemset(dos_far_ptr s, int ch, size_t n);
void * /*ASMCFUNC*/ ASMPASCAL memset(void * s, int ch, size_t n);

int /*ASMCFUNC*/ ASMPASCAL memcmp(const void *m1, const void *m2, size_t n);
#define fmemcmp memcmp

/* sysclk.c */
COUNT BcdToByte(COUNT x);
COUNT BcdToWord(BYTE * x, UWORD * mon, UWORD * day, UWORD * yr);
LONG WordToBcd(BYTE * x, UWORD * mon, UWORD * day, UWORD * yr);

/* syspack.c */
#ifdef NONNATIVE
VOID getdirent(UBYTE FAR * vp, struct dirent FAR * dp);
VOID putdirent(struct dirent FAR * dp, UBYTE FAR * vp);
#else
#define getdirent(vp, dp) fmemcpy(dp, vp, sizeof(struct dirent))
#define putdirent(dp, vp) fmemcpy(vp, dp, sizeof(struct dirent))
#endif

/* systime.c */
void DosGetTime(CPU*);
int DosSetTime(CPU*);
unsigned char DosGetDate(CPU*);
int DosSetDate(CPU*);

const UWORD *is_leap_year_monthdays(UWORD year);
UWORD DaysFromYearMonthDay(UWORD Year, UWORD Month, UWORD DayOfMonth);

/* task.c */
COUNT DosExec(COUNT mode, exec_blk FAR * ep, BYTE FAR * lp);
COUNT DosComLoader(BYTE * namep, exec_blk * exp, COUNT mode, COUNT fd);
COUNT DosExeLoader(BYTE * namep, exec_blk * exp, COUNT mode, COUNT fd);
ULONG SftGetFsize(int sft_idx);
void request_terminate(UBYTE exit_code, UBYTE exit_type);
UWORD DosGetRetCode(void);

/* newstuff.c */
int SetJFTSize(UWORD nHandles);
long DosMkTmp(BYTE FAR * pathname, UWORD attr);
COUNT truename(dos_far_ptr src, char * dest, COUNT t);

/* network.c */
/* /// TODO:
int network_redirector(unsigned cmd);
int network_redirector_fp(unsigned cmd, void far *s);
long ASMPASCAL network_redirector_mx(unsigned cmd, void far *s, void *arg);
#define remote_rw(cmd,s,arg) network_redirector_mx(cmd, s, (void *)arg)
#define remote_getfree(s,d) (int)network_redirector_mx(REM_GETSPACE, s, d)
#define remote_getfree_11a3(s,d) (int)network_redirector_mx(REM_GETLARGESPACE, s, d)
#define remote_lseek(s,new_pos) network_redirector_mx(REM_LSEEK, s, &new_pos)
#define remote_setfattr(attr) (int)network_redirector_mx(REM_SETATTR, NULL, (void *)attr)
#define remote_printredir(dx,ax) (int)network_redirector_mx(REM_PRINTREDIR, MK_FP(0,dx),(void *)ax)
#define QRemote_Fn(d,s) (int)network_redirector_mx(REM_FILENAME, d, (void *)&s)
*/
UWORD get_machine_name(BYTE FAR * netname);
VOID set_machine_name(BYTE FAR * netname, UWORD name_num);

/* procsupt.asm */
VOID ASMCFUNC exec_user(iregs FAR * irp, int disable_a20);

/* new by TE */

/*
    assert at compile time, that something is true.
    
    use like 
        ASSERT_CONST( SECSIZE == 512) 
        ASSERT_CONST( (BYTE FAR *)x->fcb_ext - (BYTE FAR *)x->fcbname == 8)
*/

#define ASSERT_CONST(x) { typedef struct { char _xx[x ? 1 : -1]; } xx ; }

