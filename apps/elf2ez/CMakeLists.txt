# ==========================================================================
# Optional host (Win32 / Linux) console build of elf2ez.
#
#   Default (OFF): the original Raspberry Pi Pico / native-DOS build below is
#   used, completely unchanged - it still produces the RP2040 `.com` tool.
#
#   -DELF2EZ_HOST=ON: build elf2ez as an ordinary native console program with
#   the host's default compiler (GCC, Clang, MinGW or MSVC). Only ISO C
#   <stdio.h>/<stdlib.h>/<string.h> are used - no Pico SDK, no cross toolchain,
#   no POSIX-only or experimental APIs. Example:
#
#       cmake -S . -B build-host -DELF2EZ_HOST=ON -DCMAKE_BUILD_TYPE=Release
#       cmake --build build-host
#
#   The executable is written straight into the repository tools/ directory
#   (../../tools), next to the prebuilt elf2ez.exe - no per-config subfolder.
# ==========================================================================
cmake_minimum_required(VERSION 3.13)

option(ELF2EZ_HOST "Build elf2ez as a native Win32/Linux console tool" OFF)

if(ELF2EZ_HOST)
    project(elf2ez C)

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)

    # Default to a Release single-config build when the user did not choose.
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    add_executable(elf2ez elf2ez.c)

    target_compile_definitions(elf2ez PRIVATE ELF2EZ_HOST)
    target_include_directories(elf2ez PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../api   # for ez.h
    )

    if(MSVC)
        # fopen() etc. are standard C; silence MSVC's non-standard deprecation.
        target_compile_definitions(elf2ez PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    # Emit the host tool straight into the repository tools/ directory, with no
    # per-config subfolder (elf2ez on Linux, elf2ez.exe on Windows). The *_DEBUG
    # / *_RELEASE overrides pin the same path for multi-config generators (MSVC).
    set(_elf2ez_tools "${CMAKE_CURRENT_SOURCE_DIR}/../../tools")
    set_target_properties(elf2ez PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${_elf2ez_tools}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${_elf2ez_tools}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_elf2ez_tools}")

    return()
endif()

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
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.22)
include(pico_sdk_import.cmake)

project(elf2ez C CXX ASM)
pico_sdk_init()

set(BUILD_NAME "${PROJECT_NAME}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../../bin/${CMAKE_BUILD_TYPE}")
set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../apps/compiled")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

add_executable(${PROJECT_NAME}
    elf2ez.c
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../api/native-dos-runtime.cmake)
native_dos_runtime_attach(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

configure_file(memmap.ld.in memmap.ld @ONLY)
pico_set_linker_script(${PROJECT_NAME} ${CMAKE_CURRENT_BINARY_DIR}/memmap.ld)

pico_set_program_name(${PROJECT_NAME} "ARM DOS Applications: elf2ez")
pico_set_program_version(${PROJECT_NAME} "elf2ez")

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage -r)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${PROJECT_NAME} PUBLIC
        -fno-jump-tables
        -Wno-error=suggest-attribute=format
        -Wno-error=cast-qual
        -Wno-error=unused-parameter
        -Wno-error=conversion
        -Wno-error=format=
        -Wno-error=sign-compare
        -Wno-error=missing-field-initializers
        -Wno-error=switch
        -Wno-error=implicit-fallthrough=
        -Wno-error=stringop-truncation
        -Wno-error=restrict
        -w
    )
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE ELF_MODE)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DINPUT=$<TARGET_FILE:${PROJECT_NAME}>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_unresolved.cmake
    COMMAND ${CMAKE_COMMAND} -E rename
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/${BUILD_NAME}.com
)
