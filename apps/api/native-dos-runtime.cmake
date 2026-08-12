# Common native DOS userspace runtime.
#
# Any relocatable ARM ELF application running under the native DOS loader
# should attach this module instead of selecting individual API implementation
# files in the application CMakeLists.txt.
#
# Usage:
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../api/native-dos-runtime.cmake)
#   native_dos_runtime_attach(my_target)

function(native_dos_runtime_attach target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "native_dos_runtime_attach: target '${target}' does not exist")
    endif()

    set(runtime_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")

    target_sources(${target} PRIVATE
        "${runtime_dir}/dos-api-sdtfn.c"
        "${runtime_dir}/dos-api-math.c"
        "${runtime_dir}/dos-api-divmod.S"
    )

    target_include_directories(${target} PRIVATE
        "${runtime_dir}"
    )

    # These files intentionally provide libc/compiler-runtime entry points.
    # Prevent GCC from recognizing their bodies as calls back into the same
    # builtin and from silently substituting a toolchain libc implementation.
    set_source_files_properties(
        "${runtime_dir}/dos-api-sdtfn.c"
        "${runtime_dir}/dos-api-math.c"
        PROPERTIES COMPILE_OPTIONS "-fno-builtin"
    )
endfunction()
