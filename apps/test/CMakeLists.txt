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

project(test C CXX ASM)
pico_sdk_init()

SET(BUILD_NAME "${PROJECT_NAME}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../../bin/${CMAKE_BUILD_TYPE}")
set(COMPILED_DIR "${CMAKE_SOURCE_DIR}/../../apps/compiled")
if(WIN32)
    set(ELF2EZ_TOOL "${CMAKE_SOURCE_DIR}/../../tools/elf2ez.exe")
else()
    set(ELF2EZ_TOOL "${CMAKE_SOURCE_DIR}/../../tools/elf2ez")
endif()
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

# INCLUDE FILES THAT SHOULD BE COMPILED:
file(GLOB_RECURSE SRC "*.cpp" "*.c" "${CMAKE_CURRENT_SOURCE_DIR}/../../api/*.c")

message(STATUS "Add source files:")
foreach (SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach ()
message(STATUS "")

add_executable(${PROJECT_NAME}
    test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../api/crt0.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../api/crt0.S
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../api/native-dos-runtime.cmake)
native_dos_runtime_attach(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../api
)

configure_file(memmap.ld.in memmap.ld @ONLY)
pico_set_linker_script(${PROJECT_NAME} ${CMAKE_CURRENT_BINARY_DIR}/memmap.ld)

pico_set_program_name(${PROJECT_NAME} "ARM DOS Applications: test")
pico_set_program_version(${PROJECT_NAME} "test")

target_link_libraries(${PROJECT_NAME} PRIVATE
#    pico_runtime
#    pico_stdlib
)

#family_configure_device_example(${PROJECT_NAME} noos)
# -fPIC -shared
target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage -r)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
target_compile_options(${PROJECT_NAME} PUBLIC
        -fno-jump-tables  # suppress __gnu_thumb1_case_uni symbol generation
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
endif ()

target_compile_definitions(${PROJECT_NAME} PRIVATE ELF_MODE)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    # Convert the relocatable ARM ELF with the repository host tool.
    # elf2ez derives the .exe name from its input, so stage the ET_REL
    # temporarily in apps/compiled and remove it after conversion.
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        ${COMPILED_DIR}/${BUILD_NAME}.crt
    COMMAND ${ELF2EZ_TOOL}
        /y ${COMPILED_DIR}/${BUILD_NAME}.crt
    COMMAND ${CMAKE_COMMAND} -E rm -f
        ${COMPILED_DIR}/${BUILD_NAME}.crt
)
