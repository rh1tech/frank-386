#!/bin/sh
# Build the host (Linux) console version of elf2ez into ../../tools/elf2ez.
#
# Mirrors build-host.bat. Uses CMake's default generator (Unix Makefiles) and
# the system compiler - nothing Pico/ARM is pulled in because -DELF2EZ_HOST=ON
# makes CMakeLists.txt take the host branch and return() before pico_sdk_init().
#
# Run from this directory (apps/elf2ez):
#     ./build-host.sh
# Extra CMake args are forwarded, e.g.:
#     ./build-host.sh -DCMAKE_C_COMPILER=clang
set -eu

cmake -S . -B build-host \
  -DELF2EZ_HOST=ON \
  -DCMAKE_BUILD_TYPE=Release \
  "$@"

cmake --build build-host

echo
echo "Built: $(cd ../../tools && pwd)/elf2ez"
