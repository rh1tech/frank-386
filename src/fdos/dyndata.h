
#pragma pack(push, 1)
struct DynS {
  UWORD Allocated;
};
#pragma pack(pop)

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size);
