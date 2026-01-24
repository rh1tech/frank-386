cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board configuration variant (M1, M2)
set(BOARD_VARIANT "M1" CACHE STRING "Board variant: M1 or M2")

# CPU overclock configuration (252, 378, 504 MHz)
# 252 = no overclock, 378 = medium, 504 = high (default)
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM speed configuration (84, 100, 133, 166 MHz target)
set(PSRAM_SPEED "166" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "murm386: Board=${BOARD_VARIANT}, CPU=${CPU_SPEED}MHz, PSRAM=${PSRAM_SPEED}MHz, Voltage=${CPU_VOLTAGE}")

include(pico_sdk_import.cmake)

project(murm386 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

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
    BOARD_${BOARD_VARIANT}
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
    BOARD_${BOARD_VARIANT}
)
target_link_libraries(ps2kbd pico_stdlib hardware_pio)
# Generate PIO headers for both M1 and M2 board variants
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd/ps2kbd_mrmltr.pio)
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd/ps2kbd_mrmltr2.pio)

#=============================================================================
# PSRAM Driver
#=============================================================================
add_library(psram
    drivers/psram/psram_init.c
)
target_include_directories(psram PUBLIC drivers/psram src)
target_compile_definitions(psram PRIVATE
    BOARD_${BOARD_VARIANT}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
target_link_libraries(psram pico_stdlib hardware_clocks)

#=============================================================================
# VGA Video Driver
#=============================================================================
add_library(video_driver
    drivers/vga/vga_hw.c
)
target_include_directories(video_driver PUBLIC drivers/vga src)
target_compile_definitions(video_driver PRIVATE
    BOARD_${BOARD_VARIANT}
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
    BOARD_${BOARD_VARIANT}
)
target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq)
pico_generate_pio_header(audio_driver ${CMAKE_CURRENT_LIST_DIR}/drivers/audio/audio_i2s.pio)

#=============================================================================
# Core 386 Emulator Sources
#=============================================================================
set(EMU_CORE_SOURCES
    # CPU and core
    i386.c
    i386_arm.S   # ARM assembly optimizations for hot paths
    pc.c

    # Peripheral chips
    i8042.c      # Keyboard controller
    i8254.c      # Timer
    i8257.c      # DMA
    i8259.c      # PIC

    # Devices
    vga.c        # VGA emulation
    disk.c       # INT 13h disk handler (from pico-286)
    pci.c        # PCI bus
    misc.c       # Serial, RTC, CMOS

    # Sound devices
    adlib.c      # Adlib/OPL2
    sb16.c       # Sound Blaster 16
    pcspk.c      # PC Speaker
    fmopl.c      # FM OPL synthesis engine

    # Optional devices (can be disabled for size)
    # fpu.c      # FPU emulation - disabled for initial port
    # ne2000.c   # Network - disabled for initial port

    # Configuration
    ini.c
)

#=============================================================================
# Main Executable
#=============================================================================
add_executable(murm386
    src/main.c
    src/platform_rp2350.c
    src/stubs_rp2350.c
    ${EMU_CORE_SOURCES}
)

target_include_directories(murm386 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/drivers/psram
    ${CMAKE_CURRENT_LIST_DIR}/drivers/vga
    ${CMAKE_CURRENT_LIST_DIR}/drivers/audio
)

target_compile_definitions(murm386 PRIVATE
    # Board configuration
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}

    # Platform identification
    PICO_ON_DEVICE=1
    RP2350_BUILD=1

    # Emulator configuration (7MB max to fit in 8MB PSRAM with VGA memory)
    EMU_MEM_SIZE_MB=7
    EMU_VGA_MEM_SIZE_KB=256
    EMU_CPU_GEN=4

    # Disable features for initial port
    NO_FPU=1
    # NO_SOUND=1  # Sound enabled with I2S output
    NO_NETWORK=1

    # Performance optimizations
    I386_ENABLE_INLINE=1
)

# Compiler optimizations for performance
target_compile_options(murm386 PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
)

# ARM assembly flags for Cortex-M33
set_source_files_properties(i386_arm.S PROPERTIES
    COMPILE_FLAGS "-mcpu=cortex-m33 -mthumb"
)

target_link_options(murm386 PRIVATE
    -Wl,--gc-sections
)

target_link_libraries(murm386
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_flash
    hardware_irq
    fatfs
    sdcard
    ps2kbd
    psram
    video_driver
    audio_driver
)

# USB Serial console (enabled)
pico_enable_stdio_usb(murm386 1)
pico_enable_stdio_uart(murm386 0)

# Generate UF2 and other output formats
pico_add_extra_outputs(murm386)

#=============================================================================
# Print configuration summary
#=============================================================================
message(STATUS "")
message(STATUS "murm386 Build Configuration:")
message(STATUS "  Board Variant:  ${BOARD_VARIANT}")
message(STATUS "  CPU Speed:      ${CPU_SPEED} MHz")
message(STATUS "  CPU Voltage:    ${CPU_VOLTAGE}")
message(STATUS "  PSRAM Speed:    ${PSRAM_SPEED} MHz")
message(STATUS "")
