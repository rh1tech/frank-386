#pragma once

/* Explicit native/guest string reference for canonical DOS paths.  Guest
 * pointers are never guessed from their bit pattern and are never exposed as
 * host pointers while paging is active. */
typedef struct fdos_path_ref {
  const char *native_ptr;
  uint32_t guest_linear;
  UBYTE is_guest;
} fdos_path_ref;

static inline fdos_path_ref fdos_path_native(const char *p)
{
  fdos_path_ref r;
  r.native_ptr = p;
  r.guest_linear = 0;
  r.is_guest = FALSE;
  return r;
}

static inline fdos_path_ref fdos_path_guest(dos_far_ptr p)
{
  fdos_path_ref r;
  r.native_ptr = NULL;
  r.guest_linear = ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
  r.is_guest = TRUE;
  return r;
}

static inline fdos_path_ref fdos_path_guest_linear(uint32_t linear)
{
  fdos_path_ref r;
  r.native_ptr = NULL;
  r.guest_linear = linear;
  r.is_guest = TRUE;
  return r;
}

static inline UBYTE fdos_path_get(fdos_path_ref r, size_t off)
{
  return r.is_guest ? pload8(r.guest_linear + (uint32_t)off)
                    : (UBYTE)r.native_ptr[off];
}

static inline size_t fdos_path_len(fdos_path_ref r, size_t maxlen)
{
  if (r.is_guest)
    return guest_strnlen_block(r.guest_linear, maxlen);
  return strnlen(r.native_ptr, maxlen);
}

static inline fdos_path_ref fdos_path_sub(fdos_path_ref r, size_t off)
{
  if (r.is_guest)
    r.guest_linear += (uint32_t)off;
  else
    r.native_ptr += off;
  return r;
}

#ifdef __cplusplus
extern "C" {
#endif
f_node_ptr dir_open_ref(fdos_path_ref dirname, BOOL split, f_node_ptr fnp);
f_node_ptr split_path_ref(fdos_path_ref path, f_node_ptr fnp);
int find_fname_ref(fdos_path_ref path, int attr, f_node_ptr fnp);
int dos_open_ref(fdos_path_ref path, unsigned flags, unsigned attrib, int fd);
COUNT dos_rename_ref(fdos_path_ref path1, fdos_path_ref path2, int attrib);
COUNT dos_getfattr_ref(fdos_path_ref name);
COUNT dos_setfattr_ref(fdos_path_ref name, UWORD attrp);
COUNT dos_mkdir_ref(fdos_path_ref dir);
COUNT dos_delete_ref(fdos_path_ref path, int attrib);
COUNT dos_rmdir_ref(fdos_path_ref path);
int dos_cd_ref(fdos_path_ref path);
COUNT dos_findfirst_ref(UCOUNT attr, fdos_path_ref name);
#ifdef __cplusplus
}
#endif
