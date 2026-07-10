
#pragma pack(push, 1)
struct DynS {
  UWORD Allocated;
};
#pragma pack(pop)

/*
 * Original layout: Dyn is part of the resident DOS image immediately after
 * _BSS, and first_mcb is computed from DynLast().  Keep the same absolute
 * guest address instead of moving Dyn to a separate paragraph/segment.
 */
#define DYN_BUFFER MK_FP(DOS_PSP, 0x240E)

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size);
dos_far_ptr DynLast(void);
