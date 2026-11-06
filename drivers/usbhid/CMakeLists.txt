# USB HID Driver for frank-386
# Provides keyboard input via USB Host
#
# SPDX-License-Identifier: MIT

add_library(usbhid INTERFACE)

# Check if USB HID is enabled
if(USB_HID_ENABLED)
    message(STATUS "USB HID Host enabled")
    message(STATUS "  NOTE: USB CDC stdio will be DISABLED when USB HID is enabled!")

    # Add the source files
    target_sources(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
        ${CMAKE_CURRENT_LIST_DIR}/usbgamepad.c
        ${CMAKE_CURRENT_LIST_DIR}/usbkbd_wrapper.c
        ${CMAKE_CURRENT_LIST_DIR}/usbmouse_wrapper.c
    )

    # Include directory for headers and tusb_config.h
    target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
    )

    target_compile_definitions(usbhid INTERFACE
        USB_HID_ENABLED=1
    )

    if(PIO_USB_ENABLED)
        message(STATUS "  USB host on PIO-USB (root port 1)")
        # SRAM is tight (the heap feeds pc_new()). The stock pools are sized
        # for 2 root ports and 32 endpoints, ~6 KB; a hub with a keyboard
        # and a mouse needs one root port and about 8 endpoints.
        target_compile_definitions(usbhid INTERFACE
            CFG_TUH_RPI_PIO_USB=1
            PIO_USB_ROOT_PORT_CNT=1
            PIO_USB_EP_POOL_CNT=12
        )
        target_link_libraries(usbhid INTERFACE tinyusb_pico_pio_usb)
    endif()

    # Link TinyUSB Host
    target_link_libraries(usbhid INTERFACE
        tinyusb_host
        tinyusb_board
    )
else()
    message(STATUS "USB HID Host disabled (use -DUSB_HID_ENABLED=1 to enable)")
    message(STATUS "  USB CDC stdio remains available for debug output")

    # When disabled: only provide stub header, no source files
    # The usbkbd_wrapper.h has inline stubs when USB_HID_ENABLED is not defined
endif()
