option(PICO_HDMI_RUNTIME_MODES "Use runtime video mode switching (video_output_rt)" OFF)

if(PICO_HDMI_RUNTIME_MODES)
    set(PICO_HDMI_VIDEO_OUTPUT_SRC src/video_output_rt.c)
else()
    set(PICO_HDMI_VIDEO_OUTPUT_SRC src/video_output.c)
endif()

add_library(pico_hdmi
    ${PICO_HDMI_VIDEO_OUTPUT_SRC}
    src/hstx_data_island_queue.c
    src/hstx_packet.c
)

target_include_directories(pico_hdmi PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(pico_hdmi
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
)
