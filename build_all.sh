#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CPU_TARGET="286"
if [[ $# -gt 0 && "$1" != -* ]]; then
    CPU_TARGET="$1"
    shift
fi

if [[ "$CPU_TARGET" != "286" ]]; then
    echo "CPU target '$CPU_TARGET' is not enabled in build_all: the 386 branch is currently untested." >&2
    exit 2
fi

BOARDS=(M1 M2 PC Z2 C2)
VIDEOS=(MCGA EGA128 VGA128 VGA256)
EXTRA_ARGS=("$@")
COUNT=0
TOTAL=32

build_one() {
    local board="$1" video="$2" audio="$3"
    local tag="${board}-${CPU_TARGET}-${video}-${audio}"
    COUNT=$((COUNT + 1))
    printf '\n[%d/%d] %s\n' "$COUNT" "$TOTAL" "$tag"
    "$SCRIPT_DIR/build.sh" \
        --board "$board" --video "$video" --audio "$audio" \
        --build-dir "$SCRIPT_DIR/build/all/$tag" \
        "${EXTRA_ARGS[@]}"
}

for board in "${BOARDS[@]}"; do
    for video in "${VIDEOS[@]}"; do
        case "$board" in
            PC) build_one "$board" "$video" PWM ;;
            C2) build_one "$board" "$video" I2S ;;
            *)
                build_one "$board" "$video" I2S
                build_one "$board" "$video" PWM
                ;;
        esac
    done
done

printf '\nAll %d supported 286 variants built. UF2 files are under bin/<build-type>/.\n' "$TOTAL"
