
#pragma pack(push, 1)
struct DynS {
  UWORD Allocated;
};
#pragma pack(pop)

#define DYN_BUFFER_SEG 0x9000
#define DYN_BUFFER_PARAS 0x1000

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size);
