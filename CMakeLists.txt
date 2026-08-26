# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Board configuration variant (M1, M2, PC, Z2 or C2)
set(BOARD "M1" CACHE STRING "Board variant: M1, M2, PC, Z2 or C2")
# Alias for backwards compatibility
if(DEFINED BOARD_VARIANT AND NOT DEFINED BOARD)
    set(BOARD ${BOARD_VARIANT})
endif()

# Enable LTEMM support (performance degradation is expected)
option(EMM "Enable LTEMM support" OFF)

# Video adapter profile: one mutually exclusive build mode.
set(VIDEO_MODE "VGA128" CACHE STRING "Video adapter: MCGA, EGA128, VGA128 or VGA256")
set_property(CACHE VIDEO_MODE PROPERTY STRINGS MCGA EGA128 VGA128 VGA256)
string(TOUPPER "${VIDEO_MODE}" VIDEO_MODE)

# Audio output configuration (I2S or PWM)
set(AUDIO_TYPE "PWM" CACHE STRING "Audio output: I2S or PWM")

if(VIDEO_MODE STREQUAL "MCGA")
    add_compile_definitions(MCGA=1)
    set(EMU_VGA_MEM_SIZE_KB 64)
elseif(VIDEO_MODE STREQUAL "EGA128")
    add_compile_definitions(EGA128=1)
    set(EMU_VGA_MEM_SIZE_KB 128)
elseif(VIDEO_MODE STREQUAL "VGA128")
    add_compile_definitions(VGA128=1)
    set(EMU_VGA_MEM_SIZE_KB 128)
elseif(VIDEO_MODE STREQUAL "VGA256")
    add_compile_definitions(VGA256=1)
    set(EMU_VGA_MEM_SIZE_KB 256)
else()
    message(FATAL_ERROR "Invalid VIDEO_MODE='${VIDEO_MODE}'; expected MCGA, EGA128, VGA128 or VGA256")
endif()

if(BOARD STREQUAL "Z2")
    # Waveshare RP2350-PiZero uses RP2350B (48 GPIOs). The stock pico2
    # board definition selects RP2350A and therefore caps GPIOs at 0..29,
    # breaking Z2 peripherals on GPIO30..47 and PIO's movable GPIO window.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Pico board type" FORCE)
elseif(BOARD STREQUAL "C2")
    # FRANK Core 2 master is an RP2350B. The stock pico2 definition
    # declares PICO_RP2350A 1, capping NUM_BANK0_GPIOS at 30 and
    # switching off PIO's movable GPIO window — the link bus (GPIO20..42),
    # the WS2812 (GPIO46) and PSRAM CS (GPIO47) would alias down into
    # 0..29. Our own header sets PICO_RP2350A 0 / PICO_PIO_USE_GPIO_BASE 1.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "frank_core2_master" CACHE STRING "Pico board type" FORCE)
    # USB HOST is the only keyboard input path on C2 — no PS/2 header.
    # HDMI is the default video output; there is no analog VGA DAC to
    # auto-detect, so the pin probe is skipped either way. -DFORCE_VGA=ON
    # drives VGA timing out of GPIO12..19 instead, for bench debug.
    if(NOT FORCE_VGA)
        set(FORCE_HDMI ON CACHE BOOL "Force HDMI" FORCE)
    endif()
    # The TDA1387T I2S DAC sits on GPIO9..11 — the same pins the M2 PWM
    # path would drive. I2S is not optional here.
    set(AUDIO_TYPE "I2S" CACHE STRING "Audio output" FORCE)
else()
    # Set board to Pico 2 (RP2350)
    set(PICO_BOARD pico2 CACHE STRING "Pico board type")
endif()

# CPU target: "386" (full i386/486/586 core, I386_MODE=1) or "286"
# (8086/186/286 core). This single switch sets BOTH the output base name
# and the I386_MODE compile definition, so the binary name can no longer
# disagree with the core actually compiled in (it used to always say
# "386" while I386_MODE was left commented out - i.e. a 286 build).
set(CPU_TARGET "286" CACHE STRING "Emulated CPU core: 286 or 386")
set_property(CACHE CPU_TARGET PROPERTY STRINGS 286 386)
if(NOT CPU_TARGET STREQUAL "286" AND NOT CPU_TARGET STREQUAL "386")
    message(FATAL_ERROR "CPU_TARGET must be 286 or 386, got '${CPU_TARGET}'")
endif()
set(BUILD_NAME "${CPU_TARGET}-${VIDEO_MODE}")
#
# NOTE: the tree previously shipped with I386_MODE commented out, so the
# binary named "386" was in fact a 286-mode core. This switch now makes
# the two agree. Defaulting CPU_TARGET to 386 therefore selects the real
# i386 core - a behavioural change from the old default. Configure with
#   -DCPU_TARGET=286
# to reproduce the previous (286-mode) build exactly.
if(CPU_TARGET STREQUAL "386")
    message(STATUS "CPU target: 386 (I386_MODE=1, full i386 core)")
else()
    message(STATUS "CPU target: 286 (I386_MODE off, 8086/186/286 core)")
endif()

# Firmware version string (set by release.sh or manually)
# FIRMWARE_VERSION is derived from version.txt after parsing (see below)

# CPU overclock configuration (252, 378, 504 MHz)
# 252 - very basic overclock, 378 = stable overclock, 504 = high overclock
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 378, 504")

# PSRAM speed configuration (133, 166 MHz target)
set(PSRAM_SPEED "66" CACHE STRING "PSRAM max frequency in MHz: 66, 84, 100, 133, 166")

# USB role is selected at runtime (HOST by default, DEVICE via config.ini).
# Both TinyUSB roles are compiled into every firmware image.

# Force HDMI output (skip VGA/HDMI pin auto-detection)
option(FORCE_HDMI "Force HDMI output instead of VGA" OFF)

# Force VGA output (skip auto-detection). Takes precedence over FORCE_HDMI.
option(FORCE_VGA "Force VGA output instead of HDMI" OFF)

# Debug output (disabled by default for cleaner console)
option(DEBUG_ENABLED "Enable verbose debug output" OFF)

# Profiling (for performance analysis)
option(PROFILE_ENABLED "Enable i386 instruction profiling" OFF)
option(DIAG_ENABLED "Enable on-device core0 fault/hang catcher (src/diag.c)" OFF)
# Refuse a new EXEC level (task.c DosExec) and refuse opening the OSD dialogs
# (main.c Win+F11/F12) when the core0 native stack is too close to its floor.
option(CONTROL_STACK "Guard core0 stack: refuse deep EXEC / OSD when stack low" ON)

# Coarse per-subsystem cycle accounting on core 0 (docs/C2_SPLIT_PLAN.md).
# Distinct from PROFILE_ENABLED, which profiles instructions.
option(SUBSYS_PROFILE "Enable core-0 subsystem cycle profiling" OFF)

if(BOARD STREQUAL "M1")
    SET(BUILD_NAME "m1p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "PC")
    SET(BUILD_NAME "PCp2-${BUILD_NAME}")
elseif(BOARD STREQUAL "Z2")
    SET(BUILD_NAME "z0p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "C2")
    SET(BUILD_NAME "c2p2-${BUILD_NAME}")
else()  # M2
    SET(BUILD_NAME "m2p2-${BUILD_NAME}")
endif()

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
endif()

message(STATUS "frank-386: Board=${BOARD}, CPU=${CPU_SPEED}MHz, PSRAM=${PSRAM_SPEED}MHz, Voltage=${CPU_VOLTAGE} ${AUDIO_TYPE}")

include(pico_sdk_import.cmake)

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

project(${BUILD_NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Read version from version.txt (format: "MAJOR MINOR" e.g. "1 00" = v1.00)
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" VERSION_CONTENT)
string(STRIP "${VERSION_CONTENT}" VERSION_CONTENT)
string(REPLACE " " ";" VERSION_LIST "${VERSION_CONTENT}")
list(GET VERSION_LIST 0 FRANK_386_VERSION_MAJOR)
list(GET VERSION_LIST 1 FRANK_386_VERSION_MINOR)
string(LENGTH "${FRANK_386_VERSION_MINOR}" _minor_len)
if(_minor_len LESS 2)
    set(FIRMWARE_VERSION "v${FRANK_386_VERSION_MAJOR}.0${FRANK_386_VERSION_MINOR}")
else()
    set(FIRMWARE_VERSION "v${FRANK_386_VERSION_MAJOR}.${FRANK_386_VERSION_MINOR}")
endif()
message(STATUS "frank-386 version: ${FIRMWARE_VERSION}")

pico_sdk_init()

#=============================================================================
# SRAM reclaim, step 1: drop DWARF/ARM exception-unwinding.
#
# This C firmware never throws, catches, or backtraces. GCC still emits
# .ARM.exidx/.extab for every function by default, and the C++ TUs pull in
# libstdc++ (snprintf_lite.o -> __aeabi_unwind_cpp_pr0), which drags the whole
# libgcc unwinder (unwind-arm.o/pr-support.o/libunwind.o, ~4.4 KiB) into the
# .data RAM catch-all. Disabling unwind tables removes that dead weight from
# SRAM (and the exidx/extab from FLASH). Applied to every target defined below
# (our libs, the executable, drivers/usbhid); rtti/exceptions are C++-only.
#
# If a C++ TU turns out to need exceptions the build will fail loudly here;
# that is intended for this verification step.
#=============================================================================
add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-fno-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>
    $<$<COMPILE_LANGUAGE:C>:-fno-asynchronous-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

#=============================================================================
# FatFS Library
#=============================================================================
add_library(fatfs
    drivers/fatfs/ff.c
    drivers/fatfs/ffsystem.c
    drivers/fatfs/ffunicode.c
    drivers/fatfs/f_util.c
)
target_include_directories(fatfs PUBLIC drivers/fatfs)
target_link_libraries(fatfs pico_stdlib)

#=============================================================================
# SD Card Driver
#=============================================================================
add_library(sdcard
    drivers/sdcard/sdcard.c
    drivers/sdcard/pio_spi.c
)
target_include_directories(sdcard PUBLIC drivers/sdcard drivers/fatfs src)
target_compile_definitions(sdcard PRIVATE
    BOARD_${BOARD}
    RP2350_BUILD=1
    # SDCARD_PIO=1  # Disabled - use hardware SPI instead
    # SDCARD_VERIFY_CRC=1
)
target_link_libraries(sdcard pico_stdlib hardware_spi hardware_pio hardware_dma)
pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard/spi.pio)

#=============================================================================
# Unified PS/2 Driver (keyboard + mouse on shared PIO)
#=============================================================================
add_library(ps2_driver
    drivers/ps2/ps2.c
)
target_include_directories(ps2_driver PUBLIC drivers/ps2 src)
target_compile_definitions(ps2_driver PRIVATE
    BOARD_${BOARD}
)
target_link_libraries(ps2_driver pico_stdlib hardware_pio hardware_gpio hardware_clocks)
pico_generate_pio_header(ps2_driver ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2/ps2.pio)

#=============================================================================
# NES/SNES Gamepad Driver
#=============================================================================
add_library(nespad
    drivers/nespad/nespad.c
)
target_include_directories(nespad PUBLIC drivers/nespad src)
target_compile_definitions(nespad PRIVATE
    BOARD_${BOARD}
)
target_link_libraries(nespad pico_stdlib hardware_pio hardware_gpio)

#=============================================================================
# PSRAM Driver
#=============================================================================
add_library(psram
    drivers/psram/psram_init.c
)
target_include_directories(psram PUBLIC drivers/psram src)
target_compile_definitions(psram PRIVATE
    BOARD_${BOARD}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
target_link_libraries(psram pico_stdlib hardware_clocks hardware_xip_cache)

#=============================================================================
# VGA Video Driver
#=============================================================================
add_library(video_driver
    drivers/hdmi/hdmi.c
    drivers/vga/test_pins.c
    drivers/vga/vga_hw.c
    drivers/vga/vga_osd.c
    drivers/vga/font8x16.c
)
target_include_directories(video_driver PUBLIC drivers/vga drivers/hdmi src)
target_compile_definitions(video_driver PRIVATE
    BOARD_${BOARD}
    RP2350_BUILD=1
)
target_link_libraries(video_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync)

#=============================================================================
# I2S Audio Driver
#=============================================================================
add_library(audio_driver
    drivers/audio/audio.c
)
target_include_directories(audio_driver PUBLIC drivers/audio src)
target_compile_definitions(audio_driver PRIVATE
    BOARD_${BOARD}
)
pico_generate_pio_header(audio_driver ${CMAKE_CURRENT_LIST_DIR}/drivers/audio/audio_i2s.pio)
if(AUDIO_TYPE STREQUAL "I2S")
    target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq)
    target_compile_definitions(audio_driver PRIVATE FEATURE_AUDIO_I2S=1 SOUND_FREQUENCY=44100)
elseif(AUDIO_TYPE STREQUAL "PWM")
    target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq hardware_pwm)
    target_compile_definitions(audio_driver PRIVATE FEATURE_AUDIO_PWM=1 SOUND_FREQUENCY=44100)
endif()

#=============================================================================
# USB HID Keyboard Driver (optional)
#=============================================================================
add_subdirectory(drivers/usbhid)

if(EMM)
    SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-1.6V-P${PSRAM_SPEED}-${AUDIO_TYPE}-${FIRMWARE_VERSION}-emm")
else ()
    SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-1.6V-P${PSRAM_SPEED}-${AUDIO_TYPE}-${FIRMWARE_VERSION}")
endif()

#=============================================================================
# Core 386 Emulator Sources
#=============================================================================
set(EMU_CORE_SOURCES
    # CPU and core
    src/i386.c
    src/i386_arm.S   # ARM assembly optimizations for hot paths
    src/pc.c

    src/286/cpu.c
    src/286/fpu.c

    # RP2350 SeaBIOS BIOS port
    src/bios/bios_00h.c
    src/bios/bios_05h.c
    src/bios/bios_08h.c
    src/bios/bios_09h.c
    src/bios/bios_10h.c
    src/bios/bios_11h.c
    src/bios/bios_12h.c
    src/bios/bios_13h.c
    src/bios/bios_14h.c
    src/bios/bios_15h.c
    src/bios/bios_16h.c
    src/bios/bios_17h.c
    src/bios/bios_18h.c
    src/bios/bios_19h.c
    src/bios/bios_1Ah.c
    src/bios/bios_33h.c
    src/bios/bios_FFh.c
    src/bios/bios_intcall.c
    src/bios/bios_printf.c
    drivers/vga/font8x8.c
    drivers/vga/font8x14.c

    # FreeDOS kernel
    src/fdos/boot.c
    src/fdos/intr.c
    src/fdos/initclk.c
    src/fdos/initdisk.c
    src/fdos/dyninit.c
    src/fdos/config.cpp
    src/fdos/memmgr.cpp
    src/fdos/inithma.c
    src/fdos/dsk.cpp
    src/fdos/dosfns.cpp
    src/fdos/fcbfns.c
    src/fdos/lfnapi.c
    src/fdos/lfn_guest_proxy.cpp
    src/fdos/fatfs.cpp
    src/fdos/fattab.cpp
    src/fdos/fatdir.cpp
    src/fdos/blockio.cpp
    src/fdos/chario.c
    src/fdos/kernel.c
    src/fdos/native_devices.cpp
    src/fdos/kernel_guest_proxy.cpp
    src/fdos/mcb_proxy.cpp
    src/fdos/task.c
    src/fdos/fcom/fcom.c
    src/fdos/fcom/fcom_guest.cpp
    src/fdos/nls.c
    src/fdos/ioctl.c
    src/fdos/ioctl_guest_proxy.cpp
    src/fdos/fdos_21h.cpp
    src/fdos/fdos_2528.c
    src/fdos/fdos_29h.c
    src/fdos/fdos_2fh.c

    src/dos_api.c

    # Peripheral chips
    src/i8042.c      # Keyboard controller
    src/i8254.c      # Timer
    src/i8257.c      # DMA
    src/i8259.c      # PIC

    # Devices
    src/vga.c        # VGA emulation
    src/disk.c       # INT 13h disk handler (from pico-286)
    src/ide.c
    src/fdd.c
    src/pci.c        # PCI bus
    src/misc.c       # Serial, RTC, CMOS

    # Sound devices
    src/adlib.c      # Adlib/OPL2
    src/sb16.c       # Sound Blaster 16
    src/pcspk.c      # PC Speaker
    src/sn76489.c    # Tandy 3-Voice Sound (SN76489)
    src/dss.c        # Disney Sound Source
    # FM OPL synthesis engine
    src/emu8950/emu8950.c
    src/emu8950/emuadpcm.c
    src/emu8950/slot_render.cpp

    # Optional devices (can be disabled for size)
    src/fpu.c      # FPU emulation - disabled for initial port
    # src/ne2000.c   # Network - disabled for initial port

    # Configuration
    src/ini.c
    src/profile_subsys.c
    src/codeprofile.c
    src/gameport.c
    $<$<STREQUAL:${BOARD},C2>:${CMAKE_CURRENT_LIST_DIR}/src/diskcache.c>
    src/pcsample.c
    src/bbprofile.c
    src/autotype.c
    $<$<STREQUAL:${BOARD},C2>:${CMAKE_CURRENT_LIST_DIR}/src/remote_mem.c>
    $<$<STREQUAL:${BOARD},C2>:${CMAKE_CURRENT_LIST_DIR}/link/link_fast.c>

    # access to sd-card using W/A with network-like driver (mapdrive.com)
    src/netredirect.c
)

#=============================================================================
# Main Executable
#=============================================================================
add_executable(${BUILD_NAME}
    src/main.c
    src/platform_rp2350.c
    src/stubs_rp2350.c
    src/diskui.c
    src/settingsui.c
    src/config_save.c
    src/ps2kbd_mrmltr.cpp
    src/ps2kbd_wrapper.cpp
    ${EMU_CORE_SOURCES}
)

if(NOT VIDEO_MODE STREQUAL "VGA256")
    target_sources(${BUILD_NAME} PRIVATE src/ega128_paging.c)
    if(BOARD STREQUAL "M1")
        target_include_directories(${BUILD_NAME} PRIVATE drivers/psram_spi)
        target_sources(${BUILD_NAME} PRIVATE drivers/psram_spi/psram_spi.c)
    endif()
endif()

set(GFX_BUFFER_EXPECTED_SIZE 262144)
configure_file(memmap.ld.in memmap.ld @ONLY)
pico_set_linker_script(${BUILD_NAME} ${CMAKE_CURRENT_BINARY_DIR}/memmap.ld)

target_include_directories(${BUILD_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2
    ${CMAKE_CURRENT_LIST_DIR}/drivers/psram
    ${CMAKE_CURRENT_LIST_DIR}/drivers/vga
    ${CMAKE_CURRENT_LIST_DIR}/drivers/hdmi
    ${CMAKE_CURRENT_LIST_DIR}/drivers/audio
    ${CMAKE_CURRENT_LIST_DIR}/drivers/nespad
    ${CMAKE_CURRENT_LIST_DIR}/link
)
if(BOARD STREQUAL "C2")
    pico_generate_pio_header(${BUILD_NAME} ${CMAKE_CURRENT_LIST_DIR}/link/link_bus.pio)
endif()
if((NOT VIDEO_MODE STREQUAL "VGA256") AND BOARD STREQUAL "M1")
    target_include_directories(${BUILD_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/drivers/psram_spi)
    pico_generate_pio_header(${BUILD_NAME} ${CMAKE_CURRENT_LIST_DIR}/drivers/psram_spi/psram_spi.pio)
    target_compile_definitions(${BUILD_NAME} PRIVATE
        PSRAM_PIN_CS=18 PSRAM_PIN_SCK=19 PSRAM_PIN_MOSI=20 PSRAM_PIN_MISO=21
    )
endif()
target_include_directories(${BUILD_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/drivers/usbhid
)

if(AUDIO_TYPE STREQUAL "I2S")
    target_compile_definitions(${BUILD_NAME} PRIVATE FEATURE_AUDIO_I2S=1)
elseif(AUDIO_TYPE STREQUAL "PWM")
    target_compile_definitions(${BUILD_NAME} PRIVATE FEATURE_AUDIO_PWM=1)
endif()

if(EMM)
    target_compile_definitions(${BUILD_NAME} PRIVATE
        EMULATE_LTEMS=1
        EMS_START=0xD0000ul
        EMS_END=0xE0000ul
    )
endif()
if(CPU_TARGET STREQUAL "386")
    target_compile_definitions(${BUILD_NAME} PRIVATE I386_MODE=1)
endif()

# --- Нативный стек core0 -----------------------------------------------
target_compile_definitions(${BUILD_NAME} PRIVATE
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0xBE0
)

# Точная карта кадров для дальнейшей разгрузки стека: родные .su-файлы
# ARM-тулчейна (build/**/*.su после сборки).
target_compile_options(${BUILD_NAME} PRIVATE -fstack-usage)

# SD-card data directory. It holds the BIOS images, config.ini and disk
# images, which DIFFER between a 286 and a 386 build - so it is tied to
# CPU_TARGET and can never disagree with the binary. Previously "386" was
# hardcoded in ~19 places and had to be hand-edited per build.
# SD_DATA_DIR is the bare name ("386"); SD_DATA_DIR_SLASH adds the "/".
target_compile_definitions(${BUILD_NAME} PRIVATE
    SD_DATA_DIR="${CPU_TARGET}"
    SD_DATA_DIR_SLASH="${CPU_TARGET}/")
target_compile_definitions(${BUILD_NAME} PRIVATE
    # Version information
    FRANK_386_VERSION_MAJOR=${FRANK_386_VERSION_MAJOR}
    FRANK_386_VERSION_MINOR=${FRANK_386_VERSION_MINOR}
    #WELCOME_SCREEN=1

    # no FreeDOS redundant messages
    QUIET=1
    #PDB_DEBUG=1
    #INT21_DIAG=1
    #DIAG=1
    #XMS_DEBUG=1
    # FreeDOS FAT32 support
    WITHFAT32=1
    WITHLFNAPI=1
    #FDOS_BUFFER_NOCACHE=1
    #FDOS_BUFFER_NOCACHE_UNIT=2
    #FDOS_FAT_DIAG=1
    #FDOS_RAWSD_DIAG=1

    # Board configuration
    BOARD_${BOARD}
    BOARD_VARIANT_STR="${BOARD}"
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}

    # Platform identification
    PICO_ON_DEVICE=1
    RP2350_BUILD=1

    # Emulator configuration (8MB PSRAM)
    EMU_VGA_MEM_SIZE_KB=${EMU_VGA_MEM_SIZE_KB}
    EMU_CPU_GEN=4

    # Disable features for initial port
#    NO_FPU=1
    # NO_SOUND=1  # Sound enabled with I2S output
    NO_NETWORK=1

    # Performance optimizations
    I386_ENABLE_INLINE=1

    SOUND_FREQUENCY=44100

    # OPL2
    USE_EMU8950_OPL
    EMU8950_SLOT_RENDER=1
    EMU8950_NO_RATECONV=1
    EMU8950_NO_WAVE_TABLE_MAP=1
    EMU8950_NO_TLL=1
    EMU8950_NO_FLOAT=1
    EMU8950_NO_TIMER=1
    EMU8950_NO_TEST_FLAG=1
    EMU8950_SIMPLER_NOISE=1
    EMU8950_SHORT_NOISE_UPDATE_CHECK=1
    EMU8950_LINEAR_SKIP=1
    EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION
    EMU8950_NO_PERCUSSION_MODE=1
    EMU8950_LINEAR=1
)

# Add DEBUG_ENABLED if enabled
if(DEBUG_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE DEBUG_ENABLED=1)
endif()

# Force HDMI output
if(FORCE_HDMI)
    target_compile_definitions(${BUILD_NAME} PRIVATE FORCE_HDMI=1)
    target_compile_definitions(video_driver PRIVATE FORCE_HDMI=1)
endif()

# Force VGA output (wins over FORCE_HDMI in vga_hw_init)
if(FORCE_VGA)
    target_compile_definitions(${BUILD_NAME} PRIVATE FORCE_VGA=1)
    target_compile_definitions(video_driver PRIVATE FORCE_VGA=1)
endif()

# Add I386_PROFILE if profiling enabled
if(PROFILE_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE I386_PROFILE=1)
endif()

# On-device diagnostics: core0 hardfault/stack-overflow/hang catcher.
# Off by default (zero cost in release). When on, compile diag.c and
# define DIAG_ENABLED so the three hooks in main.c are activated.
if(DIAG_ENABLED)
    target_sources(${BUILD_NAME} PRIVATE src/diag.c)
    target_compile_definitions(${BUILD_NAME} PRIVATE DIAG_ENABLED=1)
endif()

if(CONTROL_STACK)
    target_compile_definitions(${BUILD_NAME} PRIVATE CONTROL_STACK=1)
endif()

# Ignore cpu_freq/psram_freq from the SD card's config.ini. Needed for any
# clock-scaling measurement, since config.ini otherwise pins this board to
# 504 MHz regardless of what the build asked for.
option(PIN_CLOCKS "Ignore config.ini clock settings" OFF)

# Guest code footprint measurement, for JIT cache sizing. See codeprofile.h.
option(CODE_PROFILE "Measure the guest code working set" OFF)

# Statistical PC sampling of core 0 via SysTick. See src/pcsample.c.
# Scripted keystrokes for unattended benchmarking. See src/autotype.h.
set(AUTOTYPE "" CACHE STRING "Keys to type into the guest after boot")
if(NOT AUTOTYPE STREQUAL "")
    target_compile_definitions(${BUILD_NAME} PRIVATE AUTOTYPE_STR="${AUTOTYPE}")
endif()

option(PC_SAMPLE "Sample core-0 PC to find interpreter hot spots" OFF)

# Basic-block discovery and reuse profiling. Stage 1 of the JIT.
# SD block cache in the slave RP2350's PSRAM (C2 only). See src/diskcache.h.
# Inline the instruction-fetch fast path. Worth +5.6% but costs ~12 KB of
# RAM, so it is on only for the board it was measured and boot-tested on.
if(BOARD STREQUAL "C2")
    option(FAST_FETCH "Inline the instruction-fetch fast path" ON)
else()
    option(FAST_FETCH "Inline the instruction-fetch fast path" OFF)
endif()
if(FAST_FETCH)
    target_compile_definitions(${BUILD_NAME} PRIVATE FAST_FETCH=1)
endif()

option(DISK_CACHE "C2: cache SD blocks in the slave's PSRAM" OFF)
if(DISK_CACHE AND BOARD STREQUAL "C2")
    target_compile_definitions(${BUILD_NAME} PRIVATE DISK_CACHE=1)
    target_compile_definitions(sdcard PRIVATE DISK_CACHE=1)
endif()

option(BB_PROFILE "Profile guest basic blocks" OFF)
if(BB_PROFILE)
    target_compile_definitions(${BUILD_NAME} PRIVATE BB_PROFILE=1)
endif()
if(PC_SAMPLE)
    target_compile_definitions(${BUILD_NAME} PRIVATE PC_SAMPLE=1)
endif()
if(CODE_PROFILE)
    target_compile_definitions(${BUILD_NAME} PRIVATE CODE_PROFILE=1)
endif()
if(PIN_CLOCKS)
    target_compile_definitions(${BUILD_NAME} PRIVATE PIN_CLOCKS=1)
endif()

# Subsystem cycle profiling. sdcard needs it too: the timed disk_read /
# disk_write wrappers live there.
if(SUBSYS_PROFILE)
    target_compile_definitions(${BUILD_NAME} PRIVATE SUBSYS_PROFILE=1)
    target_compile_definitions(sdcard PRIVATE SUBSYS_PROFILE=1)
endif()

# Compiler optimizations for performance
target_compile_options(${BUILD_NAME} PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
)

# --- FreeDOS kernel port (src/fdos): stricter diagnostics -------------
#
# The port models guest far pointers as `dos_far_ptr`, a *struct*
# { uint16_t offset; uint16_t segment; }.  Two GCC warnings are
# load-bearing for that design and are BOTH off by default:
#
#   -Wmissing-braces   `.some_far_ptr = 0` is brace elision: it
#                      initialises only the first member (.offset) and
#                      zero-fills the rest.  That happens to be correct
#                      for a 0000:0000 sentinel, but the identical
#                      spelling `.some_far_ptr = 0xFFFF` would silently
#                      yield 0000:FFFF instead of FFFF:FFFF.
#   -Wunused-function  catches helpers that were ported over but never
#                      wired up to a call site.
#
# Scoped to src/fdos (+ fcom) only: the emulator/BIOS/peripheral sources
# are third-party derived and would bury the signal.
#
# -Wno-unused-parameter / -Wno-sign-compare: inherited wholesale from
# upstream FreeDOS' own style; they are pure noise here and hide nothing
# relevant to the guest-pointer model.  Re-enable once the port is clean.
set(FDOS_WARN_SOURCES
    src/fdos/boot.c
    src/fdos/intr.c
    src/fdos/initclk.c
    src/fdos/initdisk.c
    src/fdos/dyninit.c
    src/fdos/config.cpp
    src/fdos/memmgr.cpp
    src/fdos/inithma.c
    src/fdos/dsk.cpp
    src/fdos/dosfns.cpp
    src/fdos/fcbfns.c
    src/fdos/lfnapi.c
    src/fdos/lfn_guest_proxy.cpp
    src/fdos/fatfs.cpp
    src/fdos/fattab.cpp
    src/fdos/fatdir.cpp
    src/fdos/blockio.cpp
    src/fdos/chario.c
    src/fdos/kernel.c
    src/fdos/native_devices.cpp
    src/fdos/kernel_guest_proxy.cpp
    src/fdos/mcb_proxy.cpp
    src/fdos/task.c
    src/fdos/fcom/fcom.c
    src/fdos/fcom/fcom_guest.cpp
    src/fdos/nls.c
    src/fdos/ioctl.c
    src/fdos/ioctl_guest_proxy.cpp
    src/fdos/fdos_21h.cpp
    src/fdos/fdos_2528.c
    src/fdos/fdos_29h.c
    src/fdos/fdos_2fh.c
)
set_source_files_properties(${FDOS_WARN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-sign-compare"
)

# ARM assembly flags for Cortex-M33
set_source_files_properties(src/i386_arm.S PROPERTIES
    COMPILE_FLAGS "-mcpu=cortex-m33 -mthumb"
)

target_link_libraries(${BUILD_NAME}
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_flash
    hardware_watchdog
    hardware_irq
    hardware_interp
    fatfs
    sdcard
    ps2_driver
    psram
    video_driver
    audio_driver
    nespad
    usbhid
)

# The native USB controller is owned by TinyUSB in either runtime role,
# therefore USB CDC stdio cannot share it.
pico_enable_stdio_usb(${BUILD_NAME} 0)
if(BOARD STREQUAL "C2")
    # C2 has a dedicated debug UART header (J2: GPIO0 TX, GPIO1 RX).
    pico_enable_stdio_uart(${BUILD_NAME} 1)
else()
    # Other boards route GPIO0/1 to PS/2 or a gamepad.
    pico_enable_stdio_uart(${BUILD_NAME} 0)
endif()

#=============================================================================
# Print configuration summary
#=============================================================================
message(STATUS "")
message(STATUS "frank-386 Build Configuration:")
message(STATUS "  Board Variant:  ${BOARD}")
message(STATUS "  EMM:            ${EMM}")
message(STATUS "  CPU Speed:      ${CPU_SPEED} MHz")
message(STATUS "  CPU Voltage:    ${CPU_VOLTAGE}")
message(STATUS "  PSRAM Speed:    ${PSRAM_SPEED} MHz")
message(STATUS "  Audio Type:     ${AUDIO_TYPE}")
message(STATUS "  USB:            runtime HOST/DEVICE")
message(STATUS "  Debug Output:   ${DEBUG_ENABLED}")
message(STATUS "  Video Mode:     ${VIDEO_MODE}")
message(STATUS "  Video VRAM:     ${EMU_VGA_MEM_SIZE_KB} KiB")
message(STATUS "  Build Name:     ${BUILD_NAME}")
message(STATUS "")

# Create UF2 output
pico_add_extra_outputs(${BUILD_NAME})

target_link_options(${BUILD_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections -Wl,--gc-sections)
