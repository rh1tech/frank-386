#!/bin/bash
set -e

BUILD_DIR="build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DPICO_BOARD=pico2 ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo "Build complete: $BUILD_DIR/bouncing_box.uf2"
