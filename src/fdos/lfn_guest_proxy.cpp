#define new fdos_new
#define _Static_assert static_assert
extern "C" {
#include "hdrs.h"
#include "fdos.h"
}
#undef _Static_assert
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"
#include "lfn_guest_proxy.h"

namespace fdos_guest {

class lfn_inode_ref final : private ref_base<lfn_inode> {
public:
    __attribute__((always_inline)) explicit lfn_inode_ref(dos_far_ptr p)
        : ref_base<lfn_inode>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

    __attribute__((always_inline)) UNICODE name(UWORD index) const {
        return scalar_load<UNICODE>(offsetof(lfn_inode, l_name) +
                                    static_cast<std::size_t>(index) * sizeof(UNICODE));
    }

    __attribute__((always_inline)) void name(UWORD index, UNICODE value) const {
        scalar_store<UNICODE>(offsetof(lfn_inode, l_name) +
                              static_cast<std::size_t>(index) * sizeof(UNICODE), value);
    }

    __attribute__((always_inline)) void dir_to_native(struct dirent *dst) const {
        guest_read_block(base_linear() + offsetof(lfn_inode, l_dir), dst,
                         sizeof(struct dirent));
    }

    __attribute__((always_inline)) void dir_from_native(const struct dirent *src) const {
        guest_write_block(base_linear() + offsetof(lfn_inode, l_dir), src,
                          sizeof(struct dirent));
    }

    __attribute__((always_inline)) UBYTE sfn_name(UWORD index) const {
        return scalar_load<UBYTE>(offsetof(lfn_inode, l_dir) +
                                  offsetof(dirent, dir_name) + index);
    }

    __attribute__((always_inline)) void diroff(UWORD value) const {
        scalar_store<UWORD>(offsetof(lfn_inode, l_diroff), value);
    }
};

} // namespace fdos_guest

extern "C" COUNT fdos_lfn_name_length(dos_far_ptr inode)
{
    const fdos_guest::lfn_inode_ref ref(inode);
    COUNT count = 0;
    while (count < 261 && ref.name(static_cast<UWORD>(count)) != 0)
        ++count;
    return count;
}

extern "C" UBYTE fdos_lfn_sfn_checksum(dos_far_ptr inode)
{
    const fdos_guest::lfn_inode_ref ref(inode);
    UBYTE sum = 0;
    for (UWORD i = 0; i < 11; ++i)
    {
        sum = static_cast<UBYTE>((sum << 7) | (sum >> 1));
        sum = static_cast<UBYTE>(sum + ref.sfn_name(i));
    }
    return sum;
}

extern "C" void fdos_lfn_dir_to_native(dos_far_ptr inode, struct dirent *dst)
{
    fdos_guest::lfn_inode_ref(inode).dir_to_native(dst);
}

extern "C" void fdos_lfn_dir_from_native(dos_far_ptr inode, const struct dirent *src)
{
    fdos_guest::lfn_inode_ref(inode).dir_from_native(src);
}

extern "C" UNICODE fdos_lfn_name_get(dos_far_ptr inode, UWORD index)
{
    return fdos_guest::lfn_inode_ref(inode).name(index);
}

extern "C" void fdos_lfn_name_set(dos_far_ptr inode, UWORD index, UNICODE value)
{
    fdos_guest::lfn_inode_ref(inode).name(index, value);
}

extern "C" void fdos_lfn_set_diroff(dos_far_ptr inode, UWORD value)
{
    fdos_guest::lfn_inode_ref(inode).diroff(value);
}
