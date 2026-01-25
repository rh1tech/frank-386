#!/bin/bash
# Build murm386 - 386 Emulator for RP2350
#
# Usage: ./build.sh [OPTIONS]
#   -b, --board      Board variant: M1 or M2 (default: M2)
#   -p, --psram      PSRAM speed in MHz (default: 133)
#   -c, --cpu        CPU speed in MHz: 378 (default), 504
#   --mos2           Build for Murmulator OS (m1p2/m2p2 format)
#   --usb-hid        Enable USB HID keyboard (disables USB CDC)
#   --debug          Enable debug output
#   -clean           Clean build directory first
#   -h, --help       Show this help
#
# Short options:
#   -M1, -M2         Board variant
#   -378, -504       CPU speed in MHz

# Defaults (378/133 for stable overclocked operation)
BOARD="M1"
PSRAM="133"
CPU="378"
MOS2="OFF"
USB_HID="OFF"
DEBUG="ON"
CLEAN=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--board)
            BOARD="$2"
            shift 2
            ;;
        -M1)
            BOARD="M1"
            shift
            ;;
        -M2)
            BOARD="M2"
            shift
            ;;
        -p|--psram)
            PSRAM="$2"
            shift 2
            ;;
        -c|--cpu)
            CPU="$2"
            shift 2
            ;;
        -378)
            CPU="378"
            PSRAM="133"
            shift
            ;;
        -504)
            CPU="504"
            PSRAM="166"
            shift
            ;;
        --mos2)
            MOS2="ON"
            shift
            ;;
        --usb-hid)
            USB_HID="ON"
            shift
            ;;
        --debug)
            DEBUG="ON"
            shift
            ;;
        -clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            head -16 "$0" | tail -14
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Build cmake arguments
CMAKE_ARGS="-DPICO_BOARD=pico2"
CMAKE_ARGS="$CMAKE_ARGS -DBOARD=${BOARD}"
CMAKE_ARGS="$CMAKE_ARGS -DCPU_SPEED=$CPU"
CMAKE_ARGS="$CMAKE_ARGS -DPSRAM_SPEED=$PSRAM"

if [[ "$USB_HID" == "ON" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DUSB_HID_ENABLED=ON"
fi

if [[ "$DEBUG" == "ON" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DDEBUG_ENABLED=ON"
fi

if [[ "$MOS2" == "ON" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DMOS2=ON"
fi

echo "Building murm386:"
echo "  Board: $BOARD"
echo "  CPU: $CPU MHz"
echo "  PSRAM: $PSRAM MHz"
echo "  MOS2: $MOS2"
echo "  USB HID: $USB_HID"
echo "  Debug: $DEBUG"
echo ""

if [[ $CLEAN -eq 1 ]] || [[ ! -d ./build ]]; then
    rm -rf ./build
    mkdir build
fi

cd build
cmake $CMAKE_ARGS ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
