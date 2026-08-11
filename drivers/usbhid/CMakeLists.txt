# USB driver for frank-386
# Builds both TinyUSB Host HID and Device MSC; runtime config selects the role.
#
# SPDX-License-Identifier: MIT

add_library(usbhid INTERFACE)

target_sources(usbhid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
    ${CMAKE_CURRENT_LIST_DIR}/usbgamepad.c
    ${CMAKE_CURRENT_LIST_DIR}/usbkbd_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/usbmouse_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/usbmsc_device.c
)

target_include_directories(usbhid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(usbhid INTERFACE
    USB_HID_ENABLED=1
)

target_link_libraries(usbhid INTERFACE
    tinyusb_host
    tinyusb_device
    tinyusb_board
)

message(STATUS "USB runtime roles enabled: HOST HID / DEVICE MSC")
