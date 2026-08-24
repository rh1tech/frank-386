#define new fdos_new
extern "C" {
#include "hdrs.h"
}
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"
#include "mcb_proxy.h"

using fdos_guest::mcb_ref;

extern "C" uint8_t fdos_mcb_type(uint16_t s) { return mcb_ref(s).type(); }
extern "C" uint16_t fdos_mcb_owner(uint16_t s) { return mcb_ref(s).psp(); }
extern "C" uint16_t fdos_mcb_size(uint16_t s) { return mcb_ref(s).size(); }
extern "C" void fdos_mcb_set_type(uint16_t s, uint8_t v) { mcb_ref(s).type(v); }
extern "C" void fdos_mcb_set_owner(uint16_t s, uint16_t v) { mcb_ref(s).psp(v); }
extern "C" void fdos_mcb_set_size(uint16_t s, uint16_t v) { mcb_ref(s).size(v); }
extern "C" void fdos_mcb_add_size(uint16_t s, uint16_t add)
{
    const mcb_ref r(s);
    r.size((UWORD)(r.size() + add));
}
extern "C" uint8_t fdos_mcb_name_byte(uint16_t s, unsigned index)
{
    return index < 8u ? mcb_ref(s).name(index) : 0;
}
extern "C" void fdos_mcb_set_name_byte(uint16_t s, unsigned index, uint8_t value)
{
    if (index < 8u)
        mcb_ref(s).name(index, value);
}
extern "C" void fdos_mcb_set_name8(uint16_t s, const char name[8])
{
    const mcb_ref r(s);
    for (unsigned i = 0; i < 8u; ++i)
        r.name(i, (BYTE)name[i]);
}
