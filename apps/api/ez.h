#ifndef __NATIVE_DOS_EZ_H__
#define __NATIVE_DOS_EZ_H__

#include <stdint.h>

/*
 * EZ is the native ARM executable format used by DOS applications.
 *
 * An EZ file contains three logical parts:
 *
 *   +-----------------------------+ file offset 0
 *   | struct ez_file_header       |
 *   | optional future header data |
 *   +-----------------------------+ file offset header_size
 *   | initialized image           |
 *   +-----------------------------+
 *   | optional future file data   |
 *   +-----------------------------+ file offset reloc_offset
 *   | struct ez_reloc[]           |
 *   +-----------------------------+
 *
 * The image is already laid out by elf2ez.  All references which depend only
 * on relative positions inside that image must already have been resolved.
 * The relocation table contains only fixups which still depend on the actual
 * address chosen by DOS when the process is loaded.
 *
 * Runtime address convention
 * --------------------------
 *
 * DOS owns the first 0x100 bytes of the process block as the PSP.  EZ RVAs are
 * therefore measured from the beginning of the process/PSP block, not from the
 * first byte stored in the EZ image:
 *
 *     runtime_address = process_base + rva
 *
 * where process_base is ARM_PTR(MK_FP(psp_segment, 0)).  The first byte of the
 * loaded EZ image is always at EZ_IMAGE_RVA (0x100).
 */

#define EZ_MAGIC              0x5a45u /* "EZ" in little-endian byte order. */
#define EZ_FORMAT_VERSION     1u
#define EZ_IMAGE_RVA          0x00000100ul

/*
 * Header flags describe properties required by the already linked image.
 * Unknown required flags must cause the loader to reject the file.
 *
 * EZ_FLAG_THUMB
 *     entry_rva and executable code use the Thumb instruction set.  EZ v1
 *     native applications are expected to set this flag.
 *
 * EZ_FLAG_ARMV6M
 *     The image is compatible with the ARMv6-M/Thumb-1 instruction subset.
 *     This allows the loader or tooling to distinguish an RP2040-compatible
 *     image from one which requires later Thumb instructions.
 *
 * EZ_FLAG_THUMB2
 *     The image requires Thumb-2 instructions and therefore must not be run
 *     on an ARMv6-M-only CPU.  ARMV6M and THUMB2 are mutually exclusive image
 *     requirements in EZ v1.
 *
 * EZ_FLAG_SOFT_FLOAT
 *     The executable follows the soft-float procedure-call ABI used by the
 *     native DOS API.  Hard-float EZ v1 images are not supported.
 */
#define EZ_FLAG_THUMB         0x0001u
#define EZ_FLAG_ARMV6M        0x0002u
#define EZ_FLAG_THUMB2        0x0004u
#define EZ_FLAG_SOFT_FLOAT    0x0008u
#define EZ_FLAG_KNOWN_MASK    0x000fu

/*
 * Runtime relocation kinds.
 *
 * These values are EZ format values, not ELF R_ARM_* values.  elf2ez is
 * responsible for interpreting ELF relocation records and emitting only the
 * remaining load-base-dependent operation required by the finished image.
 * Consequently an EZ loader must never need ELF symbol, string or section
 * tables in order to apply one of these records.
 */
enum ez_reloc_type {
    EZ_RELOC_NONE = 0,

    /*
     * A 32-bit little-endian word at rva already contains the complete
     * link-time RVA/addend.  Add process_base to that word.
     */
    EZ_RELOC_ABS32 = 1,

    /*
     * Thumb-1 absolute-address materialization.  Patch the corresponding
     * 8-bit group of the immediate encoded by the instruction at rva after
     * adding process_base to the link-time absolute value represented by the
     * complete instruction sequence.
     */
    EZ_RELOC_THM_ALU_ABS_G0_NC = 2,
    EZ_RELOC_THM_ALU_ABS_G1_NC = 3,
    EZ_RELOC_THM_ALU_ABS_G2_NC = 4,
    EZ_RELOC_THM_ALU_ABS_G3    = 5,

    /*
     * Thumb-2 MOVW/MOVT absolute-address materialization.  The instruction at
     * rva contributes the low/high 16 bits respectively of an address which
     * must be rebased from link-time RVA space to process_base.
     */
    EZ_RELOC_THM_MOVW_ABS_NC = 6,
    EZ_RELOC_THM_MOVT_ABS    = 7
};

/*
 * Static process requirements consumed by elf2ez.
 *
 * crt0 provides a weak zero-filled definition.  An application may provide a
 * strong definition with the same name to request a DOS API version and
 * explicit stack sizes.  elf2ez reads this object from ET_REL data and copies
 * the values into the EZ file header; no application code is executed during
 * conversion.
 */
#pragma pack(push, 4)
typedef struct native_ez_process_requirements {
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
    /*
     * Minimum DOS native API version required by this executable.  This is
     * compiled into the ET_REL application itself; elf2ez merely copies the
     * value into ez_file_header.required_dos_api_version.
     */
    uint32_t required_dos_api_version;
} native_ez_process_requirements;
#pragma pack(pop)

/*
 * Runtime information for the currently executing EZ process.
 *
 * Unlike native_ez_process_requirements this structure is not file metadata.
 * It is filled by the kernel after the executable has been loaded and resource
 * sizes have been aligned/assigned.  The pointer returned by
 * native_ez_get_process_info() is valid only while the current EZ process is
 * running.
 */
#pragma pack(push, 4)
typedef struct native_ez_process_info {
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
} native_ez_process_info;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif
#define NATIVE_EZ_PROCESS_REQUIREMENTS_SECTION ".native_process.requirements"
#define NATIVE_EZ_PROCESS_DEFAULT_SECTION      ".native_process.default"

#ifndef NATIVE_EZ_PROCESS_REQUIREMENTS_ATTR
#define NATIVE_EZ_PROCESS_REQUIREMENTS_ATTR \
    __attribute__((section(NATIVE_EZ_PROCESS_REQUIREMENTS_SECTION)))
#endif

extern const native_ez_process_requirements NATIVE_EZ_PROCESS_REQUIREMENTS_ATTR
    __native_ez_process_requirements;
const native_ez_process_info *native_ez_get_process_info(void);
#ifdef __cplusplus
}
#endif

/*
 * One runtime relocation record.
 *
 * rva
 *     RVA, relative to process_base, of the word or Thumb instruction which
 *     must be patched.  The value normally lies in the initialized image
 *     range [EZ_IMAGE_RVA, EZ_IMAGE_RVA + image_file_size), because a fixup
 *     has to modify bytes which exist in the executable image.
 *
 * type
 *     One of enum ez_reloc_type.  A loader must reject unknown non-zero
 *     values rather than silently skipping them.
 *
 * reserved
 *     Must be zero in EZ v1.  Keeping each entry eight bytes wide makes the
 *     table naturally aligned and leaves room for a future relocation flag or
 *     small operand without changing the v1 header layout.
 */
#pragma pack(push, 1)

struct ez_reloc {
    uint32_t rva;
    uint8_t type;
    uint8_t reserved[3];
};

/*
 * On-disk EZ v1 header.
 *
 * The header deliberately contains only information which the DOS process
 * loader itself needs.  C/C++ startup details such as main(), _init(), _fini()
 * and preinit/init/fini array boundaries belong to crt0 and the linked image,
 * not to the executable-file ABI.
 */
struct ez_file_header {
    /*
     * File signature.  Must be EZ_MAGIC.  It occupies the first two bytes so
     * DOS can distinguish an EZ native executable before doing any expensive
     * parsing or allocation.
     */
    uint16_t magic;

    /*
     * EZ file-format version.  EZ v1 loaders accept EZ_FORMAT_VERSION.
     * Incompatible future layouts must use a different version.
     */
    uint16_t version;

    /*
     * Total number of bytes occupied by the header area in this file.
     * The initialized image begins exactly at this file offset.  For EZ v1 it
     * is at least sizeof(struct ez_file_header).  A larger value allows a
     * future producer to append header fields while old tooling can still find
     * the image without assuming a compile-time structure size.
     */
    uint16_t header_size;

    /*
     * EZ_FLAG_* image requirements.  Bits outside EZ_FLAG_KNOWN_MASK are
     * reserved in v1 and must be zero when a v1 file is produced.
     */
    uint16_t flags;

    /*
     * Minimum native DOS API version required by this executable.  This
     * replaces the current executable callback used to return the required API
     * version: the value is static build metadata and must be checked before
     * any application code is executed.
     */
    uint32_t required_dos_api_version;

    /*
     * Requested native ARM stack size in bytes.  Zero means that the loader's
     * default native-stack size is acceptable.  A non-zero value is a minimum
     * request; the loader may round it upward to satisfy its alignment rules.
     */
    uint32_t native_stack_size;

    /*
     * Requested guest DOS stack size in bytes.  Zero selects the loader's
     * default.  A non-zero value is a minimum request and may be rounded upward
     * to a paragraph or other loader-required alignment.
     */
    uint32_t dos_stack_size;

    /*
     * Number of initialized image bytes physically stored in the file,
     * beginning at file offset header_size.  This includes code, read-only
     * data, writable initialized data, crt0 and any startup arrays which were
     * retained by elf2ez.  It does not include the PSP or zero-only tail.
     */
    uint32_t image_file_size;

    /*
     * Total number of bytes occupied by the EZ image in memory, starting at
     * EZ_IMAGE_RVA.  This is image_file_size plus the zero-initialized tail
     * (BSS/NOBITS and any required end padding).  It does not include the
     * 0x100-byte PSP or loader-owned argv/runtime storage appended afterwards.
     */
    uint32_t image_mem_size;

    /*
     * RVA of the single native executable entry point.  DOS transfers control
     * here only after the image has been loaded, zeroed, relocated, the process
     * stacks have been prepared and argc/argv are available.  The entry point
     * belongs to apps/api crt0; main/_init/_fini are intentionally invisible to
     * the kernel loader.
     *
     * This is a callable ARM function RVA.  For a Thumb entry point bit 0 is
     * set, exactly as in an ARM function pointer.  The underlying instruction
     * address is therefore (entry_rva & ~1u), while the loader obtains the
     * callable runtime pointer simply as process_base + entry_rva.
     */
    uint32_t entry_rva;

    /*
     * File offset of the first struct ez_reloc.  It must not point inside the
     * header or initialized image.  Keeping this explicit rather than deriving
     * it from image_file_size permits future optional file data between the
     * image and relocation table.
     */
    uint32_t reloc_offset;

    /* Number of relocation records beginning at reloc_offset. */
    uint32_t reloc_count;

    /*
     * Size in bytes of one relocation table entry.  EZ v1 producers write
     * sizeof(struct ez_reloc).  A loader must reject values smaller than the
     * v1 record size; future tooling may use a larger record while preserving
     * the common prefix and allowing records to be skipped correctly.
     */
    uint16_t reloc_entry_size;

    /* Must be zero in EZ v1; reserved to keep subsequent 32-bit fields aligned. */
    uint16_t reserved0;

    /*
     * Reserved for future file/kernel ABI fields (for example checksums or a
     * build identifier).  EZ v1 producers must write zeros and v1 loaders must
     * ignore these words after validating the known part of the header.
     */
    uint32_t reserved[5];
};

/*
 * Prefix view of an EZ file.
 *
 * Only the fixed header is represented here: the initialized image begins at
 * byte offset header.header_size, not necessarily at sizeof(struct ez_file) in
 * a future format revision.  Code which parses an EZ file must therefore use
 * the offsets and sizes from the header rather than C pointer arithmetic past
 * this structure.
 */
struct ez_file {
    struct ez_file_header header;
};

#pragma pack(pop)

/* Shared EZ process ABI structures also have fixed layouts. */
#ifdef __cplusplus
static_assert(sizeof(native_ez_process_requirements) == 8,
              "EZ process requirements size");
static_assert(sizeof(native_ez_process_info) == 8,
              "EZ process info size");
#else
_Static_assert(sizeof(native_ez_process_requirements) == 12,
               "EZ process requirements size");
_Static_assert(sizeof(native_ez_process_info) == 8,
               "EZ process info size");
#endif

/* EZ v1 structures are part of an on-disk ABI; their byte sizes are fixed. */
#ifdef __cplusplus
static_assert(sizeof(struct ez_reloc) == 8, "EZ v1 relocation size");
static_assert(sizeof(struct ez_file_header) == 64, "EZ v1 header size");
#else
_Static_assert(sizeof(struct ez_reloc) == 8, "EZ v1 relocation size");
_Static_assert(sizeof(struct ez_file_header) == 64, "EZ v1 header size");
#endif

#endif /* __NATIVE_DOS_EZ_H__ */
