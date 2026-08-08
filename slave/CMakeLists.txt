#
# frank-386 — C2 slave firmware (RP2350A, U6)
#
# Phase 1: a memory server. See slave/src/main.c.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# == Same Raspberry Pi Pico VS Code shim the master build uses. It also
# repairs a stale exported PICO_SDK_PATH, which is the common way to get
# a confusing "Directory not found" from pico_sdk_import.cmake. ==
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
# ====================================================================
cmake_minimum_required(VERSION 3.13)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# The slave is the RP2350A (QFN-60): 30 GPIOs, PSRAM CS on GPIO0, blue
# LED on GPIO26, UART1 on the J4 header.
set(PICO_BOARD_HEADER_DIRS ${REPO_ROOT}/boards CACHE PATH "" FORCE)
set(PICO_BOARD "frank_core2_slave" CACHE STRING "Pico board type" FORCE)

# Both halves must be built at the same CPU_SPEED. The receiving PIO
# program has to finish its loop inside the transmitter's byte period,
# and each side derives that from its own clk_sys. Mismatched clocks give
# a link that works one way and drops bytes the other.
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz — must match the master")

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "frank-386 C2 slave: CPU ${CPU_SPEED} MHz, vreg ${CPU_VOLTAGE}")

include(${REPO_ROOT}/pico_sdk_import.cmake)
project(frank-386-slave C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(frank-386-slave
    src/main.c
    ${REPO_ROOT}/link/link_fast.c
    ${REPO_ROOT}/drivers/psram/psram_init.c
)

target_include_directories(frank-386-slave PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${REPO_ROOT}/link
    ${REPO_ROOT}/drivers/psram
    ${REPO_ROOT}/src
)

pico_generate_pio_header(frank-386-slave ${REPO_ROOT}/link/link_bus.pio)

target_compile_definitions(frank-386-slave PRIVATE
    CPU_CLOCK_MHZ=${CPU_SPEED}
    PSRAM_MAX_FREQ_MHZ=133
    BOARD_C2=1
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PICO_ON_DEVICE=1
)

target_compile_options(frank-386-slave PRIVATE -O3)

target_link_libraries(frank-386-slave
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_vreg
    hardware_gpio
)

# Console on UART1 (J4 header, GPIO24 TX / GPIO25 RX, 115200). The slave
# USB port is left alone so it stays available for BOOTSEL recovery.
pico_enable_stdio_usb(frank-386-slave 0)
pico_enable_stdio_uart(frank-386-slave 1)

pico_add_extra_outputs(frank-386-slave)
