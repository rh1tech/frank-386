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

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

set(BUILD_NAME "386")

# Firmware version string (set by release.sh or manually)
set(FIRMWARE_VERSION "v1.01" CACHE STRING "Firmware version string")

# Board configuration variant (M1, M2)
set(BOARD "M1" CACHE STRING "Board variant: M1 or M2")
# Alias for backwards compatibility
if(DEFINED BOARD_VARIANT AND NOT DEFINED BOARD)
    set(BOARD ${BOARD_VARIANT})
endif()

# CPU overclock configuration (378, 504 MHz)
# 378 = stable overclock (default), 504 = high overclock
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 378, 504")

# PSRAM speed configuration (133, 166 MHz target)
set(PSRAM_SPEED "166" CACHE STRING "PSRAM max frequency in MHz: 133, 166")

# Audio output configuration (I2S or PWM)
set(AUDIO_TYPE "PWM" CACHE STRING "Audio output: I2S or PWM")

# USB HID keyboard support (disabled by default to keep USB CDC for debug)
option(USB_HID_ENABLED "Enable USB HID keyboard (disables USB CDC stdio)" ON)

# Debug output (disabled by default for cleaner console)
option(DEBUG_ENABLED "Enable verbose debug output" OFF)

# Profiling (for performance analysis)
option(PROFILE_ENABLED "Enable i386 instruction profiling" OFF)

if(BOARD STREQUAL "M1")
    SET(BUILD_NAME "m1p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "PC")
    SET(BUILD_NAME "PCp2-${BUILD_NAME}")
elseif(BOARD STREQUAL "Z0")
    SET(BUILD_NAME "z0p2-${BUILD_NAME}")
else()  # M2
    SET(BUILD_NAME "m2p2-${BUILD_NAME}")
endif()

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "murm386: Board=${BOARD}, CPU=${CPU_SPEED}MHz, PSRAM=${PSRAM_SPEED}MHz, Voltage=${CPU_VOLTAGE} ${AUDIO_TYPE}")

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
list(GET VERSION_LIST 0 MURM386_VERSION_MAJOR)
list(GET VERSION_LIST 1 MURM386_VERSION_MINOR)
message(STATUS "murm386 version: ${MURM386_VERSION_MAJOR}.${MURM386_VERSION_MINOR}")

pico_sdk_init()

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
)
target_link_libraries(sdcard pico_stdlib hardware_spi hardware_pio hardware_dma)
pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard/spi.pio)

#=============================================================================
# PS/2 Keyboard Driver
#=============================================================================
add_library(ps2kbd
    drivers/ps2kbd/ps2kbd_mrmltr.cpp
    drivers/ps2kbd/ps2kbd_wrapper.cpp
)
target_include_directories(ps2kbd PUBLIC drivers/ps2kbd src)
target_compile_definitions(ps2kbd PRIVATE
    BOARD_${BOARD}
)
target_link_libraries(ps2kbd pico_stdlib hardware_pio)
# Generate PIO headers for both M1 and M2 board variants
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd/ps2kbd_mrmltr.pio)
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd/ps2kbd_mrmltr2.pio)

#=============================================================================
# PS/2 Mouse Driver
#=============================================================================
add_library(ps2mouse
    drivers/ps2mouse/ps2mouse.c
)
target_include_directories(ps2mouse PUBLIC drivers/ps2mouse src)
target_compile_definitions(ps2mouse PRIVATE
    BOARD_${BOARD}
)
target_link_libraries(ps2mouse pico_stdlib hardware_gpio)

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
target_link_libraries(psram pico_stdlib hardware_clocks)

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
    target_compile_definitions(audio_driver PRIVATE FEATURE_AUDIO_I2S=1)
elseif(AUDIO_TYPE STREQUAL "PWM")
    target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq hardware_pwm)
    target_compile_definitions(audio_driver PRIVATE FEATURE_AUDIO_PWM=1)
endif()

#=============================================================================
# USB HID Keyboard Driver (optional)
#=============================================================================
add_subdirectory(drivers/usbhid)

SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-P${PSRAM_SPEED}-${AUDIO_TYPE}-${FIRMWARE_VERSION}")

#=============================================================================
# Core 386 Emulator Sources
#=============================================================================
set(EMU_CORE_SOURCES
    # CPU and core
    src/i386.c
    src/i386_arm.S   # ARM assembly optimizations for hot paths
    src/pc.c

    # Peripheral chips
    src/i8042.c      # Keyboard controller
    src/i8254.c      # Timer
    src/i8257.c      # DMA
    src/i8259.c      # PIC

    # Devices
    src/vga.c        # VGA emulation
    src/disk.c       # INT 13h disk handler (from pico-286)
    src/pci.c        # PCI bus
    src/misc.c       # Serial, RTC, CMOS

    # Sound devices
    src/adlib.c      # Adlib/OPL2
    src/sb16.c       # Sound Blaster 16
    src/pcspk.c      # PC Speaker
    src/fmopl.c      # FM OPL synthesis engine
    src/sn76489.c    # Tandy 3-Voice Sound (SN76489)
    src/dss.c        # Disney Sound Source

    # Optional devices (can be disabled for size)
    # src/fpu.c      # FPU emulation - disabled for initial port
    # src/ne2000.c   # Network - disabled for initial port

    # Configuration
    src/ini.c
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
    ${EMU_CORE_SOURCES}
)

target_include_directories(${BUILD_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2mouse
    ${CMAKE_CURRENT_LIST_DIR}/drivers/psram
    ${CMAKE_CURRENT_LIST_DIR}/drivers/vga
    ${CMAKE_CURRENT_LIST_DIR}/drivers/hdmi
    ${CMAKE_CURRENT_LIST_DIR}/drivers/audio
)
# Only include usbhid directory when USB HID is enabled (to avoid tusb_config.h conflicts)
if(USB_HID_ENABLED)
    target_include_directories(${BUILD_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/drivers/usbhid
    )
endif()

target_compile_definitions(${BUILD_NAME} PRIVATE
    # Version information
    MURM386_VERSION_MAJOR=${MURM386_VERSION_MAJOR}
    MURM386_VERSION_MINOR=${MURM386_VERSION_MINOR}

    # Board configuration
    BOARD_${BOARD}
    BOARD_VARIANT_STR="${BOARD}"
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}

    # Platform identification
    PICO_ON_DEVICE=1
    RP2350_BUILD=1

    # Emulator configuration (7MB max to fit in 8MB PSRAM with VGA memory)
    EMU_MEM_SIZE_MB=6
    EMU_VGA_MEM_SIZE_KB=256
    EMU_CPU_GEN=4

    EMULATE_LTEMS=0

    # Disable features for initial port
    NO_FPU=1
    # NO_SOUND=1  # Sound enabled with I2S output
    NO_NETWORK=1

    # Performance optimizations
    I386_ENABLE_INLINE=1
)

# Add DEBUG_ENABLED if enabled
if(DEBUG_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE DEBUG_ENABLED=1)
endif()

# Add I386_PROFILE if profiling enabled
if(PROFILE_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE I386_PROFILE=1)
endif()

# Compiler optimizations for performance
target_compile_options(${BUILD_NAME} PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
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
    fatfs
    sdcard
    ps2kbd
    ps2mouse
    psram
    video_driver
    audio_driver
    usbhid
)

# USB configuration: USB HID uses Host mode which disables CDC
if(USB_HID_ENABLED)
    message(STATUS "USB HID enabled: Using UART for stdio (USB is Host mode for keyboard)")
    pico_enable_stdio_usb(${BUILD_NAME} 0)
    pico_enable_stdio_uart(${BUILD_NAME} 0)
else()
    # USB Serial console (when USB HID is disabled)
    pico_enable_stdio_usb(${BUILD_NAME} 1)
    pico_enable_stdio_uart(${BUILD_NAME} 0)
endif()

#=============================================================================
# Print configuration summary
#=============================================================================
message(STATUS "")
message(STATUS "murm386 Build Configuration:")
message(STATUS "  Board Variant:  ${BOARD}")
message(STATUS "  CPU Speed:      ${CPU_SPEED} MHz")
message(STATUS "  CPU Voltage:    ${CPU_VOLTAGE}")
message(STATUS "  PSRAM Speed:    ${PSRAM_SPEED} MHz")
message(STATUS "  Audio Type:     ${AUDIO_TYPE}")
message(STATUS "  USB HID:        ${USB_HID_ENABLED}")
message(STATUS "  Debug Output:   ${DEBUG_ENABLED}")
message(STATUS "  Build Name:     ${BUILD_NAME}")
message(STATUS "")

# Create UF2 output
pico_add_extra_outputs(${BUILD_NAME})

target_link_options(${BUILD_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections -Wl,--gc-sections)
