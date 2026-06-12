#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"

#pragma pack(push, 1)
typedef struct KernelConfig {
    char     signature[6];              // "CONFIG"
    uint16_t config_size;               // configend - configstart

    uint8_t  DLASortByDriveNo;          // 0
    uint8_t  InitDiskShowDriveAssignment;// 1
    uint8_t  SkipConfigSeconds;         // 2
    uint8_t  ForceLBA;                  // 0
    uint8_t  GlobalEnableLBAsupport;    // 1
    uint8_t  BootHarddiskSeconds;       // 0

    uint8_t  Version_OemID;             // 0xFD
    uint8_t  Version_Major;             // 2
    uint16_t Version_Revision;          // 43
    uint16_t Version_Release;           // 1

    uint8_t  CheckDebugger;             // 0
    uint8_t  Verbose;                   // 0, но по смыслу signed: -1/0/1

    uint8_t  PartitionMode;             // 0x1F
} KernelConfig;
#pragma pack(pop)

static KernelConfig kernel_config = {
    .signature = {'C','O','N','F','I','G'},
    .config_size = sizeof(KernelConfig) - 8, // без signature[6] и config_size

    .DLASortByDriveNo = 0,
    .InitDiskShowDriveAssignment = 1,
    .SkipConfigSeconds = 2,
    .ForceLBA = 0,
    .GlobalEnableLBAsupport = 1,
    .BootHarddiskSeconds = 0,

    .Version_OemID = 0xFD,
    .Version_Major = 2,
    .Version_Revision = 43,
    .Version_Release = 1,

    .CheckDebugger = 0,
    .Verbose = 0,

    .PartitionMode = 0x1F
};

#pragma pack(push, 1)
#define FAR
#define far
#define WITHFAT32 1
#include "hdr/portab.h"
#include "hdr/lol.h"
#pragma pack(pop)

static struct lol lol = { 0 };

void kernel(CPU* cpu) {
    struct lol* LoL = &lol;
    // adjust boot drive to DOS format
    LoL->BootDrive  = CPU_BL + 1;
    if (LoL->BootDrive > 0x80) {
        LoL->BootDrive -= (0x80 - 3);
    }

    // entry_common
#ifndef QUIET
    CPU_AX = 0x0e31; // '1' Tracecode - kernel entered
    CPU_BX = 0x00f0;
    bios_10h(cpu);
#endif

    CPU_AX = 0;
    CPU_DX = 0;
    bios_13h(cpu); // reset floppy disk system

    CPU_AX = 0;
    CPU_DL = 0x80;
    bios_13h(cpu); // reset hdd system

    // stop it before complete
    while(1);
}
