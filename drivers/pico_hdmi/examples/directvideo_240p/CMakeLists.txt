cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(directvideo_240p C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Use runtime video mode switching API
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)

# Add pico_hdmi library
add_subdirectory(../.. pico_hdmi)

add_executable(directvideo_240p
    main.c
)

target_link_libraries(directvideo_240p
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    pico_hdmi
)

# Enable USB output, disable UART
pico_enable_stdio_usb(directvideo_240p 1)
pico_enable_stdio_uart(directvideo_240p 0)

pico_add_extra_outputs(directvideo_240p)
