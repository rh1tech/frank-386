#!/usr/bin/env bash
set -euo pipefail

# Build one supported murm386/FRANK RP2350 firmware variant.
# The production build scripts intentionally build the 286 core only.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BOARD="M1"
VIDEO_MODE="EGA128"
AUDIO="PWM"
CPU_SPEED="504"
PSRAM_SPEED="66"
BUILD_TYPE="Release"
BUILD_DIR="$SCRIPT_DIR/build"
JOBS=""
CLEAN=0
FORCE_HDMI="OFF"
FORCE_VGA="OFF"
DEBUG="OFF"
DIAG="OFF"
EMM="OFF"

usage() {
    cat <<'USAGE'
Usage: ./build.sh [options]

Supported production CPU target: 286

Main options:
  -b, --board M1|M2|PC|Z2|C2       Board variant (default: M1)
  -v, --video MCGA|EGA128|VGA128|VGA256
                                      Video/VRAM profile (default: EGA128)
  -a, --audio I2S|PWM               Audio backend (default: PWM)
  -c, --clock MHz                   RP2350 clock (default: 504)
  -p, --psram MHz                   QSPI PSRAM max clock (default: 66)
      --hdmi                        Force HDMI output
      --vga                         Force VGA output
      --debug                       Enable DEBUG_ENABLED
      --diag                        Enable DIAG_ENABLED
      --emm                         Enable EMM
      --build-type TYPE             CMake build type (default: Release)
      --build-dir DIR               Build directory (default: ./build)
  -j, --jobs N                      Parallel build jobs
      --clean                       Remove the selected build directory first
  -h, --help                        Show this help

Short forms:
  -M1 -M2 -PC -Z2 -C2
  -MCGA -EGA128 -VGA128 -VGA256
  -i2s -pwm
  -252 -378 -504

Examples:
  ./build.sh -M1 -VGA256 -i2s -504 -p 66 --clean
  ./build.sh -M2 -VGA128 -pwm --hdmi
USAGE
}

need_arg() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        echo "Missing argument for $1" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--board) need_arg "$@"; BOARD="${2^^}"; shift 2 ;;
        -v|--video) need_arg "$@"; VIDEO_MODE="${2^^}"; shift 2 ;;
        -a|--audio) need_arg "$@"; AUDIO="${2^^}"; shift 2 ;;
        -c|--clock) need_arg "$@"; CPU_SPEED="$2"; shift 2 ;;
        -p|--psram) need_arg "$@"; PSRAM_SPEED="$2"; shift 2 ;;
        --build-type) need_arg "$@"; BUILD_TYPE="$2"; shift 2 ;;
        --build-dir) need_arg "$@"; BUILD_DIR="$2"; shift 2 ;;
        -j|--jobs) need_arg "$@"; JOBS="$2"; shift 2 ;;

        -M1|-M2|-PC|-Z2|-C2) BOARD="${1#-}"; shift ;;
        -MCGA|-EGA128|-VGA128|-VGA256) VIDEO_MODE="${1#-}"; shift ;;
        -i2s) AUDIO="I2S"; shift ;;
        -pwm) AUDIO="PWM"; shift ;;
        -252|-378|-504) CPU_SPEED="${1#-}"; shift ;;

        --hdmi) FORCE_HDMI="ON"; FORCE_VGA="OFF"; shift ;;
        --vga) FORCE_VGA="ON"; FORCE_HDMI="OFF"; shift ;;
        --debug) DEBUG="ON"; shift ;;
        --diag) DIAG="ON"; shift ;;
        --emm) EMM="ON"; shift ;;
        --clean|-clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$BOARD" in M1|M2|PC|Z2|C2) ;; *) echo "Invalid board: $BOARD" >&2; exit 2 ;; esac
case "$VIDEO_MODE" in MCGA|EGA128|VGA128|VGA256) ;; *) echo "Invalid video mode: $VIDEO_MODE" >&2; exit 2 ;; esac
case "$AUDIO" in I2S|PWM) ;; *) echo "Invalid audio type: $AUDIO" >&2; exit 2 ;; esac

# Hardware constraints mirrored from CMakeLists.txt.
if [[ "$BOARD" == "PC" ]]; then
    AUDIO="PWM"
elif [[ "$BOARD" == "C2" ]]; then
    AUDIO="I2S"
fi

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

CMAKE_ARGS=(
    -S "$SCRIPT_DIR"
    -B "$BUILD_DIR"
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DCPU_TARGET=286"
    "-DBOARD=$BOARD"
    "-DVIDEO_MODE=$VIDEO_MODE"
    "-DAUDIO_TYPE=$AUDIO"
    "-DCPU_SPEED=$CPU_SPEED"
    "-DPSRAM_SPEED=$PSRAM_SPEED"
    "-DFORCE_HDMI=$FORCE_HDMI"
    "-DFORCE_VGA=$FORCE_VGA"
    "-DDEBUG_ENABLED=$DEBUG"
    "-DDIAG_ENABLED=$DIAG"
    "-DEMM=$EMM"
)

printf 'murm386 build\n'
printf '  CPU target : 286\n'
printf '  Board      : %s\n' "$BOARD"
printf '  Video mode : %s\n' "$VIDEO_MODE"
printf '  Audio      : %s\n' "$AUDIO"
printf '  RP2350     : %s MHz\n' "$CPU_SPEED"
printf '  PSRAM max  : %s MHz\n' "$PSRAM_SPEED"
printf '  Build type : %s\n' "$BUILD_TYPE"
printf '  Build dir  : %s\n\n' "$BUILD_DIR"

cmake "${CMAKE_ARGS[@]}"
if [[ -n "$JOBS" ]]; then
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS"
else
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel
fi
