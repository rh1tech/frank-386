cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(bouncing_box C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Add pico_hdmi library
add_subdirectory(../.. pico_hdmi)

add_executable(bouncing_box
    main.c
)

target_link_libraries(bouncing_box
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    pico_hdmi
)

# Enable USB output, disable UART
pico_enable_stdio_usb(bouncing_box 1)
pico_enable_stdio_uart(bouncing_box 0)

pico_add_extra_outputs(bouncing_box)
