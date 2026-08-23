#include <cstddef>
#include <cstdint>

#define new fdos_new
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_LOCAL_STATIC_ASSERT_MACRO 1
#endif
extern "C" {
#include "hdrs.h"
}
#ifdef FDOS_LOCAL_STATIC_ASSERT_MACRO
#undef _Static_assert
#undef FDOS_LOCAL_STATIC_ASSERT_MACRO
#endif
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"
#include "fatfs_guest.h"

namespace fdos_guest {

static constexpr linear_t media_req_linear =
    (static_cast<linear_t>(DOS_PSP) << 4) + X86_INTERNAL_DATA_OFF +
    offsetof(dos_data, MediaReqHdr);
static const request_ref media_req(media_req_linear);

#ifdef WITHFAT32
static void read_fsinfo_guest(dpb_ref d)
{
    const UWORD sec = d.dpb_xfsinfosec();
    if (sec == 0xffff)
        return;
    const UBYTE unit = d.dpb_unit();
    struct buffer *bp = getblock(sec, unit);
    if (bp == nullptr)
        return;
    bp->b_flag &= ~(BFR_DATA | BFR_DIR | BFR_FAT | BFR_DIRTY);
    bp->b_flag |= BFR_VALID;
    const struct fsinfo *fip = reinterpret_cast<const struct fsinfo *>(&bp->b_buffer[0x1e4]);
    const CLUSTER free_count = fip->fi_nfreeclst;
    const CLUSTER first_free = fip->fi_cluster;
    const ULONG max_cluster = d.dpb_xsize();
    d.xnfree(free_count >= max_cluster ? XUNKNCLSTFREE : free_count);
    d.dpb_xcluster(first_free < 2 || first_free > max_cluster ? UNKNCLUSTER : first_free);
}
#endif

} // namespace fdos_guest

extern "C" void fdos_media_request_prepare(dos_far_ptr p, UBYTE command)
{
    using namespace fdos_guest;
    const dpb_ref d(p);
    media_req.length(sizeof(request));
    media_req.unit(d.dpb_subunit());
    media_req.command(command);
    media_req.mcmdesc(d.dpb_mdb());
    media_req.status(0);
    if (command == C_BLDBPB)
        media_req.bpfat(DiskTransferBuffer);
}

extern "C" dos_far_ptr fdos_media_request_far(void)
{
    return MK_FP(DOS_PSP, X86_INTERNAL_DATA_OFF + offsetof(dos_data, MediaReqHdr));
}
extern "C" UWORD fdos_media_request_status(void) { return fdos_guest::media_req.status(); }
extern "C" BYTE fdos_media_request_mcretcode(void) { return fdos_guest::media_req.mcretcode(); }
extern "C" dos_far_ptr fdos_media_request_bpptr(void) { return fdos_guest::media_req.bpptr(); }
extern "C" BOOL fdos_bpb_is_fat32(dos_far_ptr p) { return fdos_guest::bpb_ref(p).bpb_nfsect() == 0; }
extern "C" UBYTE fdos_dpb_unit(dos_far_ptr p) { return fdos_guest::dpb_ref(p).dpb_unit(); }
extern "C" ULONG fdos_dpb_xfatsize(dos_far_ptr p) {
#ifdef WITHFAT32
    return fdos_guest::dpb_ref(p).dpb_xfatsize();
#else
    (void)p;
    return 0;
#endif
}
extern "C" UBYTE fdos_dpb_subunit(dos_far_ptr p) { return fdos_guest::dpb_ref(p).dpb_subunit(); }
extern "C" UWORD fdos_dpb_secsize(dos_far_ptr p) { return fdos_guest::dpb_ref(p).dpb_secsize(); }
extern "C" UWORD fdos_dpb_dirents(dos_far_ptr p) { return fdos_guest::dpb_ref(p).dpb_dirents(); }
extern "C" UWORD fdos_dpb_dirstrt(dos_far_ptr p) { return fdos_guest::dpb_ref(p).dpb_dirstrt(); }
extern "C" UBYTE fdos_dpb_clsmask(dos_far_ptr p) { return fdos_guest::dpb_ref(p).dpb_clsmask(); }
extern "C" ULONG fdos_dpb_root_cluster(dos_far_ptr p) {
#ifdef WITHFAT32
    const fdos_guest::dpb_ref d(p);
    return d.dpb_fatsize() == 0 ? d.dpb_xrootclst() : 0;
#else
    (void)p;
    return 0;
#endif
}
extern "C" ULONG fdos_dpb_clus2phys(dos_far_ptr p, CLUSTER cluster) {
    const fdos_guest::dpb_ref d(p);
#ifdef WITHFAT32
    const ULONG data = d.dpb_fatsize() == 0 ? d.dpb_xdata() : d.dpb_data();
#else
    const ULONG data = d.dpb_data();
#endif
    return ((ULONG)(cluster - 2u) << d.dpb_shftcnt()) + data;
}
extern "C" BYTE fdos_dpb_flags(dos_far_ptr p) { return fdos_guest::dpb_ref(p).flags(); }
extern "C" dos_far_ptr fdos_dpb_device(dos_far_ptr p) { return fdos_guest::dpb_ref(p).device(); }

extern "C" CLUSTER fdos_dpb_max_cluster(dos_far_ptr p)
{
    const fdos_guest::dpb_ref d(p);
#ifdef WITHFAT32
    if (d.dpb_fatsize() == 0)
        return d.dpb_xsize();
#endif
    return d.dpb_size();
}

extern "C" CLUSTER fdos_read_fat_guest(dos_far_ptr x86_dpbp, CLUSTER cluster1)
{
    using namespace fdos_guest;
    const dpb_ref d(x86_dpbp);
#ifdef WITHFAT32
    const bool fat32 = d.dpb_fatsize() == 0;
#else
    const bool fat32 = false;
#endif
    const UWORD dpb_size = d.dpb_size();
    const bool fat12 = (dpb_size - 1u) < FAT_MAGIC;
    const bool fat16 = !fat12 && dpb_size <= FAT_MAGIC16;
    CLUSTER max_cluster = dpb_size;
#ifdef WITHFAT32
    if (fat32)
        max_cluster = d.dpb_xsize();
#endif
    if (cluster1 <= 1 || cluster1 > max_cluster)
        return 1;

    unsigned secdiv = d.dpb_secsize();
    CLUSTER clussec = cluster1;
    if (fat12) {
        clussec = static_cast<CLUSTER>(static_cast<unsigned>(clussec) * 3u);
        secdiv *= 2u;
    } else {
        secdiv /= 2u;
#ifdef WITHFAT32
        if (fat32)
            secdiv /= 2u;
#endif
    }

    unsigned idx = static_cast<unsigned>(clussec % secdiv);
    clussec /= secdiv;
    clussec += d.dpb_fatstrt();
#ifdef WITHFAT32
    if (fat32) {
        const UWORD xflags = d.dpb_xflags();
        if (xflags & FAT_NO_MIRRORING)
            clussec += static_cast<CLUSTER>(xflags & 0x0fu) * d.dpb_xfatsize();
    }
#endif

    auto get_fat_block = [&](CLUSTER sector, dos_far_ptr &out) -> bool {
        struct buffer *native_bp = getblock(sector, d.dpb_unit());
        if (native_bp == nullptr)
            return false;

        /* Capture the guest address before any further guest access. */
        out = linear_to_far(native_bp);
        const buffer_ref b(out);
        BYTE flag = b.flag();
        flag = static_cast<BYTE>((flag & ~(BFR_DATA | BFR_DIR)) | BFR_FAT | BFR_VALID);
        b.flag(flag);
        b.dpbp(x86_dpbp);
        b.copies(d.dpb_fats());
        b.offset(d.dpb_fatsize());
#ifdef WITHFAT32
        if (fat32 && (d.dpb_xflags() & FAT_NO_MIRRORING))
            b.copies(1);
#endif
        return true;
    };

    dos_far_ptr x86_bp{};
    if (!get_fat_block(clussec, x86_bp))
        return 1;
    const buffer_ref b(x86_bp);

    if (fat12) {
        const unsigned byte_index = idx / 2u;
        UBYTE lo = b.data8(byte_index);
        UBYTE hi;
        if (byte_index >= static_cast<unsigned>(d.dpb_secsize()) - 1u) {
            dos_far_ptr x86_bp1{};
            if (!get_fat_block(clussec + 1u, x86_bp1))
                return 1;
            hi = buffer_ref(x86_bp1).data8(0);
        } else {
            hi = b.data8(byte_index + 1u);
        }
        unsigned cluster = static_cast<unsigned>(lo) | (static_cast<unsigned>(hi) << 8);
        if (cluster1 & 1u)
            cluster >>= 4;
        cluster &= 0x0fffu;
        if (cluster >= MASK12)
            return LONG_LAST_CLUSTER;
        if (cluster == BAD12)
            return LONG_BAD;
        return cluster;
    }

    if (fat16) {
        const UWORD res = b.data16(idx * 2u);
        if (res >= MASK16)
            return LONG_LAST_CLUSTER;
        if (res == BAD16)
            return LONG_BAD;
        return res;
    }

#ifdef WITHFAT32
    if (fat32) {
        const ULONG res = b.data32(idx * 4u) & LONG_LAST_CLUSTER;
        if (res > LONG_BAD)
            return LONG_LAST_CLUSTER;
        return res;
    }
#endif
    return 1;
}

extern "C" void fdos_bpb_to_dpb_guest(dos_far_ptr bp, dos_far_ptr dp, BOOL extended)
{
    using namespace fdos_guest;
    const bpb_ref b(bp);
    const dpb_ref d(dp);
    const UBYTE nsector = b.bpb_nsector();
    UWORD shftcnt;
    if (nsector == 0) shftcnt = 8;
    else for (shftcnt = 0; (nsector >> shftcnt) > 1; ++shftcnt) {}

    const UWORD secsize = b.bpb_nbyte();
    const UWORD fatstrt = b.bpb_nreserved();
    const UBYTE fats = b.bpb_nfat();
    const UWORD fatsize = b.bpb_nfsect();
    const UWORD dirents = b.bpb_ndirent();
    const UWORD dirstrt = static_cast<UWORD>(fatstrt + fats * fatsize);
    const UWORD data = static_cast<UWORD>(dirstrt +
        (dirents + secsize / DIRENT_SIZE - 1) / (secsize / DIRENT_SIZE));
    const ULONG size = b.bpb_nsize() == 0 ? b.bpb_huge() : static_cast<ULONG>(b.bpb_nsize());
    UWORD clusters = static_cast<UWORD>(((size - data) >> shftcnt) + 1u);

    unsigned fatsiz;
    const ULONG tmp = fatsize * static_cast<ULONG>(secsize / 2u);
    if (tmp < 0x10000UL) {
        fatsiz = static_cast<unsigned>(tmp);
        if (clusters > FAT_MAGIC) {
            if (fatsiz > FAT_MAGIC && clusters >= fatsiz)
                clusters = static_cast<UWORD>(fatsiz - 1u);
        } else if (fatsiz < 0x4000u) {
            fatsiz = fatsiz * 4u / 3u;
            if (clusters >= fatsiz)
                clusters = static_cast<UWORD>(fatsiz - 1u);
        }
    }

    d.dpb_shftcnt(static_cast<UBYTE>(shftcnt));
    d.dpb_mdb(b.bpb_mdesc());
    d.dpb_secsize(secsize);
    d.dpb_clsmask(static_cast<UBYTE>((nsector - 1u) & 0xffu));
    d.dpb_fatstrt(fatstrt);
    d.dpb_fats(fats);
    d.dpb_dirents(dirents);
    d.dpb_fatsize(fatsize);
    d.dpb_dirstrt(dirstrt);
    d.dpb_data(data);
    d.dpb_size(clusters);
    d.flags(0);
    d.cluster(UNKNCLUSTER);
    d.nfree(UNKNCLSTFREE);

#ifdef WITHFAT32
    if (extended) {
        const ULONG xfatsize = fatsize == 0 ? b.bpb_xnfsect() : fatsize;
        d.dpb_xfatsize(xfatsize);
        d.dpb_xcluster(UNKNCLUSTER);
        d.xnfree(XUNKNCLSTFREE);
        d.dpb_xflags(0);
        d.dpb_xfsinfosec(0xffff);
        d.dpb_xbackupsec(0xffff);
        d.dpb_xrootclst(0);
        d.dpb_xdata(data);
        d.dpb_xsize(clusters);
        if (d.dpb_fatsize() == 0) {
            d.dpb_xflags(b.bpb_xflags());
            d.dpb_xfsinfosec(b.bpb_xfsinfosec());
            d.dpb_xbackupsec(b.bpb_xbackupsec());
            d.dpb_dirents(0);
            d.dpb_dirstrt(0xffff);
            d.dpb_size(0);
            const ULONG xdata = fatstrt + static_cast<ULONG>(fats) * xfatsize;
            d.dpb_xdata(xdata);
            d.dpb_xsize(((size - xdata) >> shftcnt) + 1u);
            d.dpb_xrootclst(b.bpb_xrootclst());
            read_fsinfo_guest(d);
        }
    }
#else
    (void)extended;
#endif
}
