#!/bin/bash
# Flash frank-386 to a connected board.
#
# Two transports:
#
#   ./flash.sh [firmware]                 USB BOOTSEL via picotool (default)
#   ./flash.sh --swd master [firmware]    SWD via a Raspberry Pi Debug Probe
#   ./flash.sh --swd slave  [firmware]
#   ./flash.sh --swd-id                   identify the attached chip, flash nothing
#
# SWD is strongly preferred on FRANK Core 2. USB-BOOTSEL needs a button
# press whenever the firmware wedges — exactly when you are iterating
# fastest — and `picotool reboot -u` will not recover a target that has
# faulted into lockup. SWD does not care what the target is doing.
#
# Wiring: probe SWD to J1 (master, U3) or J3 (slave, U6). Both headers
# are pin 1 = SWDIO, pin 2 = GND, pin 3 = SWCLK.
#
# The master (U3) is an RP2350B in QFN-80 and the slave (U6) an RP2350A
# in QFN-60. Only one probe is attached at a time and the two headers sit
# next to each other, so --swd reads SYSINFO.PACKAGE_SEL and refuses to
# program a target whose package does not match the half you asked for.
# Writing the master image to the slave is not fatal — SWD recovers it —
# but the resulting "it flashed fine and does nothing" is a bad hour.

set -uo pipefail

cd "$(dirname "$0")"

# SYSINFO on RP2350 is at 0x40000000 (NOT the RP2040 address 0x40108000).
# PACKAGE_SEL bit 0: 1 = RP2350A/QFN-60, 0 = RP2350B/QFN-80.
SYSINFO_PACKAGE_SEL=0x40000004

# Fill the global OOCD array with openocd arguments for probe serial $1
# (empty = whichever probe openocd picks). A global rather than a
# printf/read pair because macOS ships bash 3.2, which has no mapfile.
set_openocd_args() {
    OOCD=(-f interface/cmsis-dap.cfg)
    [ -n "${1:-}" ] && OOCD+=(-c "adapter serial $1")
    OOCD+=(-c "adapter speed 5000" -f target/rp2350.cfg)
}

# List the serial number of every attached CMSIS-DAP probe.
swd_probe_serials() {
    ioreg -p IOUSB -l -w 0 2>/dev/null \
        | grep -A25 "CMSIS_DAP" \
        | sed -n 's/.*"USB Serial Number" = "\([0-9A-Fa-f]*\)".*/\1/p' \
        | sort -u
}

# Print "master", "slave", or return 1. $1 = probe serial (may be empty).
swd_identify() {
    local out pkg
    set_openocd_args "${1:-}"
    out=$(timeout 20 openocd "${OOCD[@]}" \
              -c "init" -c "mdw ${SYSINFO_PACKAGE_SEL} 1" -c "exit" 2>&1)
    pkg=$(echo "$out" | sed -n "s/^0x40000004: *\([0-9a-f]*\).*/\1/p" | tail -1)
    [ -z "$pkg" ] && return 1
    if (( 0x$pkg & 1 )); then echo "slave"; else echo "master"; fi
}

# Print the serial of the probe attached to half $1 ("master"/"slave").
swd_probe_for() {
    local want="$1" s who
    for s in $(swd_probe_serials); do
        who=$(swd_identify "$s") || continue
        if [ "$who" = "$want" ]; then echo "$s"; return 0; fi
    done
    return 1
}

if [ "${1:-}" = "--swd-id" ]; then
    found=0
    for s in $(swd_probe_serials); do
        who=$(swd_identify "$s")
        if [ -z "$who" ]; then
            echo "probe $s -> no RP2350 target responded"
        elif [ "$who" = "slave" ]; then
            echo "probe $s -> SLAVE  (U6, RP2350A/QFN-60, header J3)"
        else
            echo "probe $s -> MASTER (U3, RP2350B/QFN-80, header J1)"
        fi
        found=1
    done
    [ $found -eq 0 ] && { echo "ERROR: no CMSIS-DAP probe found." >&2; exit 1; }
    exit 0
fi

if [ "${1:-}" = "--swd" ]; then
    TARGET="${2:-}"
    case "$TARGET" in
        master) DEFAULT_GLOB="./build/c2p2-386*.elf" ;;
        slave)  DEFAULT_GLOB="./slave/build/*.elf" ;;
        *) echo "usage: $0 --swd master|slave [firmware.elf]" >&2; exit 1 ;;
    esac

    FIRMWARE="${3:-}"
    if [ -z "$FIRMWARE" ]; then
        FIRMWARE=$(ls -t $DEFAULT_GLOB 2>/dev/null | head -1)
    fi
    if [ -z "$FIRMWARE" ] || [ ! -f "$FIRMWARE" ]; then
        echo "ERROR: no $TARGET firmware found (looked for $DEFAULT_GLOB)." >&2
        if [ "$TARGET" = "master" ]; then
            echo "       Build it with: ./build.sh -C2" >&2
        else
            echo "       The murm386 slave firmware does not exist yet (plan phase 1+)." >&2
        fi
        exit 1
    fi

    echo "Locating the probe attached to the $TARGET..."
    SERIAL=$(swd_probe_for "$TARGET")
    if [ -z "$SERIAL" ]; then
        echo "" >&2
        echo "ERROR: no attached probe is on the $TARGET." >&2
        echo "       Probes seen: $(swd_probe_serials | tr '\n' ' ')" >&2
        echo "       Run '$0 --swd-id' to see what each one is connected to." >&2
        exit 1
    fi

    echo "Flashing $TARGET (probe $SERIAL) over SWD: $FIRMWARE"
    set_openocd_args "$SERIAL"
    exec openocd "${OOCD[@]}" -c "program $FIRMWARE verify reset exit"
fi

#-----------------------------------------------------------------------
# USB BOOTSEL (picotool)
#-----------------------------------------------------------------------
if [ -n "${1:-}" ]; then
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
    echo "       $0 --swd master|slave [firmware.elf]"
    exit 1
fi

echo "Flashing: $FIRMWARE"
picotool load -f "$FIRMWARE" && picotool reboot -f
