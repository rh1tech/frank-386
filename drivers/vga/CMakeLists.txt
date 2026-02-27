add_library(vga INTERFACE)

target_sources(vga INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/vga_hw.c
        ${CMAKE_CURRENT_LIST_DIR}/font8x16.c
        ${CMAKE_CURRENT_LIST_DIR}/test_pins.c
)

target_link_libraries(vga INTERFACE 
    hardware_pio 
    hardware_dma 
    hardware_gpio 
    hardware_clocks
)

target_include_directories(vga INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
