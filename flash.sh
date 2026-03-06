#!/bin/bash
# Flash murm386 to connected Pico device

# Default to ELF file from build directory
FIRMWARE="${1:-./build/m2p2-386-378MHz-P133-PWM-v1.01.elf}"

# Check if firmware file exists
if [ ! -f "$FIRMWARE" ]; then
    # Try .uf2 if .elf not found
    FIRMWARE="${FIRMWARE%.elf}.uf2"
    if [ ! -f "$FIRMWARE" ]; then
        echo "Error: Firmware file not found"
        echo "Usage: $0 [firmware.elf|firmware.uf2]"
        echo "Default: ./build/murm386.elf"
        exit 1
    fi
fi

echo "Flashing: $FIRMWARE"
picotool load -f "$FIRMWARE" && picotool reboot -f
