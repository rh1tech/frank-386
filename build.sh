#!/bin/bash
# Build frank-386 - 386 Emulator for RP2350
#
# Usage: ./build.sh [OPTIONS]
#   -b, --board      Board variant: M1, M2, PC, Z2, C2 (default: M2)
#   -a, --audio      Audio output: I2S, PWM (default: PWM; PC is always PWM)
#   -p, --psram      PSRAM speed in MHz (default: 133)
#   -c, --cpu        CPU speed in MHz: 378 (default), 504
#   --vga            Force VGA output (instead of HDMI)
#   --usb-hid        Enable USB HID keyboard (disables USB CDC)
#   --debug          Enable debug output
#   -clean           Clean build directory first
#   -h, --help       Show this help
#
# Short options:
#   -M1, -M2, -PC, -Z2, -C2   Board variant
#   -378, -504            CPU speed in MHz
#   -i2s, -pwm            Audio output type

# Defaults (378/133 for stable overclocked operation)
BOARD="M2"
AUDIO="PWM"
PSRAM="133"
CPU="378"
USB_HID="OFF"
HDMI="OFF"
VGA="OFF"
DEBUG="ON"
PROFILE="OFF"
SUBSYS="OFF"
REMOTE="OFF"
PINCLK="OFF"
CODEPROF="OFF"
PCSAMPLE="OFF"
BBPROF="OFF"
AUTOTYPE=""
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
        -PC)
            BOARD="PC"
            shift
            ;;
        -Z2)
            BOARD="Z2"
            shift
            ;;
        -C2)
            BOARD="C2"
            shift
            ;;
        -a|--audio)
            AUDIO=$(echo "$2" | tr '[:lower:]' '[:upper:]')
            shift 2
            ;;
        -i2s)
            AUDIO="I2S"
            shift
            ;;
        -pwm)
            AUDIO="PWM"
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
        --usb-hid)
            USB_HID="ON"
            shift
            ;;
        --hdmi)
            HDMI="ON"
            VGA="OFF"
            shift
            ;;
        --vga)
            VGA="ON"
            HDMI="OFF"
            shift
            ;;
        --debug)
            DEBUG="ON"
            shift
            ;;
        --profile)
            PROFILE="ON"
            shift
            ;;
        --subsys-profile)
            SUBSYS="ON"
            shift
            ;;
        --remote-mem)
            REMOTE="ON"
            shift
            ;;
        --pin-clocks)
            PINCLK="ON"
            shift
            ;;
        --code-profile)
            CODEPROF="ON"
            shift
            ;;
        --pc-sample)
            PCSAMPLE="ON"
            shift
            ;;
        --bb-profile)
            BBPROF="ON"
            shift
            ;;
        --autotype)
            AUTOTYPE="$2"
            shift 2
            ;;
        -clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            head -15 "$0" | tail -13
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Olimex PC has no I2S hardware - force PWM
if [[ "$BOARD" == "PC" && "$AUDIO" != "PWM" ]]; then
    echo "Warning: Olimex PC does not support I2S, forcing PWM audio"
    AUDIO="PWM"
fi

# FRANK Core 2: TDA1387T I2S DAC, HDMI only, USB HID is the only input.
# CMakeLists forces these too; set them here so the echoed summary is honest.
if [[ "$BOARD" == "C2" ]]; then
    AUDIO="I2S"
    USB_HID="ON"
    [[ "$VGA" != "ON" ]] && HDMI="ON"
fi

# Build cmake arguments
CMAKE_ARGS=(-DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=MinSizeRel)
CMAKE_ARGS+=("-DBOARD=${BOARD}")
CMAKE_ARGS+=("-DCPU_SPEED=$CPU")
CMAKE_ARGS+=("-DPSRAM_SPEED=$PSRAM")
CMAKE_ARGS+=("-DAUDIO_TYPE=$AUDIO")

CMAKE_ARGS+=("-DUSB_HID_ENABLED=$USB_HID")

# NOTE: every optional feature below is passed explicitly as ON or OFF.
# Omitting the flag leaves whatever CMake cached from the previous
# configure, so `./build.sh -C2` after `./build.sh -C2 --remote-mem`
# would quietly still have remote memory compiled in — which invalidates
# any before/after measurement taken with it.
CMAKE_ARGS+=("-DDEBUG_ENABLED=$DEBUG")

CMAKE_ARGS+=("-DPROFILE_ENABLED=$PROFILE")

CMAKE_ARGS+=("-DSUBSYS_PROFILE=$SUBSYS")

CMAKE_ARGS+=("-DREMOTE_MEM=$REMOTE")
CMAKE_ARGS+=("-DPIN_CLOCKS=$PINCLK")
CMAKE_ARGS+=("-DCODE_PROFILE=$CODEPROF")
CMAKE_ARGS+=("-DPC_SAMPLE=$PCSAMPLE")
CMAKE_ARGS+=("-DBB_PROFILE=$BBPROF")
CMAKE_ARGS+=("-DAUTOTYPE=$AUTOTYPE")

CMAKE_ARGS+=("-DFORCE_HDMI=$HDMI")

CMAKE_ARGS+=("-DFORCE_VGA=$VGA")

echo "Building frank-386:"
echo "  Board: $BOARD"
echo "  Audio: $AUDIO"
echo "  CPU: $CPU MHz"
echo "  PSRAM: $PSRAM MHz"
echo "  USB HID: $USB_HID"
echo "  HDMI: $HDMI"
echo "  VGA: $VGA"
echo "  Debug: $DEBUG"
echo "  Profile: $PROFILE"
echo "  Subsys profile: $SUBSYS"
echo "  Remote mem: $REMOTE"
echo ""

if [[ $CLEAN -eq 1 ]] || [[ ! -d ./build ]]; then
    rm -rf ./build
    mkdir build
fi

cd build
cmake "${CMAKE_ARGS[@]}" ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
