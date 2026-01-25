#!/bin/bash
# Copyright (c) 2025-2026 Mikhail Matveev <xtreme@rh1.tech>
#
# release.sh - Build all release variants of murm386
#
# Creates firmware files for each combination:
#
# RP2350 variants (M1, M2):
#   - UF2 format for direct flashing
#
# MOS2 variants (M1, M2) - Murmulator OS:
#   - m1p2/m2p2 format for MOS2 bootloader
#
# Output format: murm386_<board>_<version>.{uf2,m1p2,m2p2}
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

# Interactive version input
echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                    murm386 Release Builder                      │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}

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
BOARDS=("M1" "M2")
CPU_SPEED="378"    # Stable overclock for releases
PSRAM_SPEED="133"  # Stable PSRAM speed for releases

# Count total builds: 2 boards * 2 formats (UF2 + MOS2) = 4
TOTAL_BUILDS=4
BUILD_COUNT=0

echo ""
echo -e "${YELLOW}Building $TOTAL_BUILDS firmware variants...${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Function to build a single variant
build_variant() {
    local BOARD=$1
    local MOS2=$2       # "ON" or "OFF"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Determine board number for filename
    local BOARD_LC=$(echo "$BOARD" | tr '[:upper:]' '[:lower:]')

    # Determine file extension
    local EXT="uf2"
    if [[ "$MOS2" == "ON" ]]; then
        [[ "$BOARD" == "M1" ]] && EXT="m1p2"
        [[ "$BOARD" == "M2" ]] && EXT="m2p2"
    fi

    local OUTPUT_NAME="murm386_${BOARD_LC}_${VERSION}.${EXT}"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | CPU: ${CPU_SPEED}MHz | PSRAM: ${PSRAM_SPEED}MHz | MOS2: $MOS2"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Build cmake arguments - release configuration
    local CMAKE_ARGS="-DPICO_BOARD=pico2"
    CMAKE_ARGS="$CMAKE_ARGS -DBOARD=$BOARD"
    CMAKE_ARGS="$CMAKE_ARGS -DCPU_SPEED=$CPU_SPEED"
    CMAKE_ARGS="$CMAKE_ARGS -DPSRAM_SPEED=$PSRAM_SPEED"
    CMAKE_ARGS="$CMAKE_ARGS -DUSB_HID_ENABLED=ON"      # Enable USB keyboard for releases
    CMAKE_ARGS="$CMAKE_ARGS -DDEBUG_ENABLED=OFF"       # Disable debug logs for releases

    [[ "$MOS2" == "ON" ]] && CMAKE_ARGS="$CMAKE_ARGS -DMOS2=ON"

    # Configure with CMake
    if cmake $CMAKE_ARGS .. > /dev/null 2>&1; then
        # Build
        if make -j8 > /dev/null 2>&1; then
            # Find and copy output file
            local SRC_FILE="$SCRIPT_DIR/build/murm386.${EXT}"

            if [[ -f "$SRC_FILE" ]]; then
                cp "$SRC_FILE" "$RELEASE_DIR/$OUTPUT_NAME"
                echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
            else
                echo -e "  ${RED}✗ Output file not found: $SRC_FILE${NC}"
                ls -la "$SCRIPT_DIR/build/"*.{uf2,m1p2,m2p2} 2>/dev/null || true
            fi
        else
            echo -e "  ${RED}✗ Build failed${NC}"
            # Show last few lines of make output for debugging
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
    build_variant "$BOARD" "OFF"
done

# ============================================================================
# MOS2 (Murmulator OS) builds
# ============================================================================
echo ""
echo -e "${CYAN}=== Building MOS2 firmware (Murmulator OS) ===${NC}"

for BOARD in "${BOARDS[@]}"; do
    build_variant "$BOARD" "ON"
done

# ============================================================================
# Clean up and create ZIP archives
# ============================================================================
rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Release build complete!${NC}"
echo ""

echo -e "${CYAN}=== Creating ZIP archives ===${NC}"
echo ""

cd "$RELEASE_DIR"

# UF2 archive
ZIP_UF2="murm386_${VERSION}.zip"
zip -q "$ZIP_UF2" murm386_*_${VERSION}.uf2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_UF2" || echo -e "  ${YELLOW}⚠ No UF2 files${NC}"

# MOS2 archive
ZIP_MOS2="murm386_mos2_${VERSION}.zip"
zip -q "$ZIP_MOS2" murm386_*_${VERSION}.m?p2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_MOS2" || echo -e "  ${YELLOW}⚠ No MOS2 files${NC}"

# Remove individual files after zipping (keep only ZIPs)
rm -f murm386_*.uf2 murm386_*.m?p2 2>/dev/null

cd "$SCRIPT_DIR"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Release archives in: $RELEASE_DIR/"
echo ""
ls -la "$RELEASE_DIR"/*.zip 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
