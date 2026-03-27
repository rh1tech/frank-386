#!/bin/bash
# Flash frank-386 to connected Pico device

if [ -n "$1" ]; then
    FIRMWARE="$1"
else
    # Auto-detect: newest .uf2 file in build/
    FIRMWARE=$(ls -t ./build/*.uf2 2>/dev/null | head -1)
    if [ -z "$FIRMWARE" ]; then
        FIRMWARE=$(ls -t ./build/*.elf 2>/dev/null | head -1)
    fi
fi

if [ -z "$FIRMWARE" ] || [ ! -f "$FIRMWARE" ]; then
    echo "Error: No firmware file found in build/"
    echo "Usage: $0 [firmware.elf|firmware.uf2]"
    exit 1
fi

echo "Flashing: $FIRMWARE"
picotool load -f "$FIRMWARE" && picotool reboot -f
