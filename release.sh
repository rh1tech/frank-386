#!/bin/bash
# Copyright (c) 2025-2026 Mikhail Matveev <xtreme@rh1.tech>
#
# release.sh - Build all release variants of frank-386
#
# Creates firmware files for each board variant (M1, M2, PC, Z2):
#   - UF2 format for direct flashing
#   - I2S and PWM audio variants (PC is PWM only)
#
# Output format: frank-386_<board>_<audio>_<version>.uf2
#
# Release configuration:
#   - CPU: 378 MHz (stable overclock)
#   - PSRAM: 133 MHz
#   - USB HID: Enabled (keyboard support)
#   - Debug: Disabled (no logs)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Version file
VERSION_FILE="version.txt"

# Read last version or initialize
if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi

# Calculate next version (for default suggestion)
NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                    frank-386 Release Builder                      │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo ""

# Accept version from command line or prompt interactively
if [[ -n "$1" ]]; then
    INPUT_VERSION="$1"
else
    DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
    read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
    INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}
fi

# Parse version (handle both "1.00" and "1 00" formats)
if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

# Remove leading zeros for arithmetic, then re-pad
MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))

# Validate
if [[ $MAJOR -lt 1 ]]; then
    echo -e "${RED}Error: Major version must be >= 1${NC}"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: Minor version must be 0-99${NC}"
    exit 1
fi

# Format version string
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${MAJOR}.$(printf '%02d' $MINOR)${NC}"

# Save new version
echo "$MAJOR $MINOR" > "$VERSION_FILE"

# Create release directory
RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

# Configuration
BOARDS=("M1" "M2" "PC" "Z2")
AUDIO_TYPES=("I2S" "PWM")
CPU_SPEED="378"    # Stable overclock for releases
PSRAM_SPEED="133"  # Stable PSRAM speed for releases

# Count total builds: M1/M2/Z2 get I2S+PWM (6), PC gets PWM only (1),
# C2 gets one build (its audio, video and USB configuration are all
# forced by the board block in CMakeLists) = 8
TOTAL_BUILDS=8
BUILD_COUNT=0

echo ""
echo -e "${YELLOW}Building $TOTAL_BUILDS firmware variants...${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Function to build a single variant
build_variant() {
    local BOARD=$1
    local AUDIO=$2

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Determine board number for filename
    local BOARD_LC=$(echo "$BOARD" | tr '[:upper:]' '[:lower:]')
    local AUDIO_LC=$(echo "$AUDIO" | tr '[:upper:]' '[:lower:]')

    local OUTPUT_NAME="${3:-frank-386_${BOARD_LC}_${AUDIO_LC}_${VERSION}.uf2}"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | Audio: $AUDIO | CPU: ${CPU_SPEED}MHz | PSRAM: ${PSRAM_SPEED}MHz"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Build cmake arguments - release configuration
    local CMAKE_ARGS="-DPICO_BOARD=pico2"
    CMAKE_ARGS="$CMAKE_ARGS -DBOARD=$BOARD"
    CMAKE_ARGS="$CMAKE_ARGS -DAUDIO_TYPE=$AUDIO"
    CMAKE_ARGS="$CMAKE_ARGS -DCPU_SPEED=$CPU_SPEED"
    CMAKE_ARGS="$CMAKE_ARGS -DPSRAM_SPEED=$PSRAM_SPEED"
    CMAKE_ARGS="$CMAKE_ARGS -DUSB_HID_ENABLED=ON"      # Enable USB keyboard for releases
    CMAKE_ARGS="$CMAKE_ARGS -DDEBUG_ENABLED=OFF"       # Disable debug logs for releases

    # Configure with CMake
    if cmake $CMAKE_ARGS .. > /dev/null 2>&1; then
        # Build
        if make -j8 > /dev/null 2>&1; then
            # Find and copy output file
            local SRC_FILE=$(find "$SCRIPT_DIR" -name "*.uf2" -newer "$SCRIPT_DIR/build/CMakeCache.txt" 2>/dev/null | head -1)

            if [[ -f "$SRC_FILE" ]]; then
                cp "$SRC_FILE" "$RELEASE_DIR/$OUTPUT_NAME"
                echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
            else
                echo -e "  ${RED}✗ Output file not found${NC}"
            fi
        else
            echo -e "  ${RED}✗ Build failed${NC}"
            make -j8 2>&1 | tail -20
        fi
    else
        echo -e "  ${RED}✗ CMake failed${NC}"
        cmake $CMAKE_ARGS .. 2>&1 | tail -20
    fi

    cd "$SCRIPT_DIR"
}

# ============================================================================
# RP2350 UF2 builds
# ============================================================================
echo ""
echo -e "${CYAN}=== Building RP2350 UF2 firmware ===${NC}"

for BOARD in "${BOARDS[@]}"; do
    for AUDIO in "${AUDIO_TYPES[@]}"; do
        # Olimex PC has no I2S hardware - PWM only
        if [[ "$BOARD" == "PC" && "$AUDIO" == "I2S" ]]; then
            continue
        fi
        build_variant "$BOARD" "$AUDIO"
    done
done

# FRANK Core 2 (dual RP2350). Only one variant exists: CMakeLists forces
# I2S audio (the TDA1387T sits on the pins M2 would use for PWM), HDMI
# output, and USB HID as the sole input path. Passing an explicit output
# name keeps it from being labelled with an audio choice it does not have.
build_variant "C2" "I2S" "frank-386_c2_${VERSION}.uf2"

# ============================================================================
# Clean up
# ============================================================================
rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Release build complete!${NC}"
echo ""
echo "Release files in: $RELEASE_DIR/"
echo ""
ls -la "$RELEASE_DIR"/frank-386_*${VERSION}.uf2 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
