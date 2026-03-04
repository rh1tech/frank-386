#if EMULATE_LTEMS
#include <stdint.h>
// The Lo-tech EMS board driver is hardcoded to 2MB.
#define EMS_PSRAM_OFFSET ((EMU_MEM_SIZE_MB * 1024 - 2048ul) << 10)
#define EMS ((uint8_t*)0x11000000 + EMS_PSRAM_OFFSET)

#define EMS_START (0xD0000ul)
#define EMS_END   (0xE0000ul)

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