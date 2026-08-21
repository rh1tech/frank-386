#if EMULATE_LTEMS
#include <stdint.h>
// The Lo-tech EMS board driver is hardcoded to 2 MiB.
// Backing-store location follows the auto-detected PSRAM size.
extern uint8_t *ems_base_ptr;
#define EMS ems_base_ptr

extern uint8_t ems_pages[4];

static inline uint32_t physical_address(const uint32_t address) {
    const uint32_t page_addr = address & 0x3FFF;
    const uint8_t selector = ems_pages[(address >> 14) & 3];
    return selector * 0x4000 + page_addr;
}

static inline uint8_t ems_read(const uint32_t address) {
    const uint32_t phys_addr = physical_address(address);
    return EMS[phys_addr];
}

// TODO: Overlap?
static inline uint16_t ems_readw(const uint32_t address) {
    const uint32_t phys_addr = physical_address(address);
    return (*(uint16_t *) &EMS[phys_addr]);
}

static inline uint32_t ems_readdw(const uint32_t address) {
    const uint32_t phys_addr = physical_address(address);
    return (*(uint32_t *) &EMS[phys_addr]);
}

static inline void ems_write(const uint32_t address, const uint8_t data) {
    const uint32_t phys_addr = physical_address(address);
    EMS[phys_addr] = data;
}

static inline void ems_writew(const uint32_t address, const uint16_t data) {
    const uint32_t phys_addr = physical_address(address);
    *(uint16_t *) &EMS[phys_addr] = data;
}

static inline void ems_writedw(const uint32_t address, const uint32_t data) {
    const uint32_t phys_addr = physical_address(address);
    *(uint32_t *) &EMS[phys_addr] = data;
}
#endif