# murm386 / FRANK PC firmware

RP2350-based PC/AT-compatible firmware with a 286-class x86 core, native FreeDOS kernel integration, VGA/HDMI output, SD-card storage, PS/2/USB input and multiple audio backends.

The current production target is **286**. A 386 core still exists in the tree, but it has not been regression-tested for a long time and is intentionally excluded from the normal build scripts.

## Current status

The firmware is actively developed around the RP2350 and currently targets these board profiles:

- **M1** — Murmulator/FRANK M1 pinout
- **M2** — Murmulator/FRANK M2 pinout
- **PC** — Olimex PICO-PC profile
- **Z2** — Waveshare RP2350-PiZero profile
- **C2** — FRANK Core 2 profile

The authoritative GPIO map is `src/board_config.h`. Board-specific Pico SDK headers are in `boards/`.

## CPU and DOS

- Production CPU target: **286** (`CPU_TARGET=286`).
- The emulator core also contains 386 support, but this branch is currently considered experimental/untested.
- The DOS environment uses the RP2350 port of the FreeDOS kernel in `src/fdos/`.
- The native command shell is FCOM (`src/fdos/fcom/`).
- BIOS and DOS services are integrated into the firmware; the project is no longer the old “SeaBIOS + external VGA BIOS” layout described by earlier versions of this README.

## Video profiles

Exactly one video/VRAM profile is selected at build time:

| `VIDEO_MODE` | Guest video RAM | Notes |
|---|---:|---|
| `MCGA` | 64 KiB | Reduced-VRAM build |
| `EGA128` | 128 KiB | Reduced-VRAM build |
| `VGA128` | 128 KiB | Reduced-VRAM build |
| `VGA256` | 256 KiB | Full 256 KiB VGA RAM |

Physical output is VGA or HDMI depending on the board and runtime/forced output selection. `--vga` and `--hdmi` in the build scripts force one path where the board supports it.

### Guest RAM backends

`VGA256` expects direct QSPI PSRAM for guest RAM.

Reduced-VRAM builds (`MCGA`, `EGA128`, `VGA128`) can use either direct QSPI PSRAM or the paging backend:

- on **M1**, paging can use the external SPI PSRAM backend;
- otherwise paging falls back to `286/pagefile.sys` on the SD card;
- paging exposes an 8 MiB guest address space with a cache in RP2350 SRAM;
- current cache size is 192 KiB for `MCGA` and 128 KiB for `EGA128`/`VGA128`.

The paging implementation lives in `src/ega128_paging.c`.

When reduced-VRAM builds use direct QSPI PSRAM, core0 can move its stack into the unused tail of `GFX_BUFFER`. The released stack SRAM is then available to the FatFs write-through sector cache.

## Audio

Supported firmware audio backends are:

- **I2S**
- **PWM**

The emulated PC audio devices include PC speaker, AdLib/OPL, Sound Blaster-compatible paths, Tandy, Covox and Disney Sound Source support.

Board restrictions are enforced by CMake/build scripts:

- `PC` uses PWM;
- `C2` uses I2S.

## Input

Depending on board capabilities:

- PS/2 keyboard and mouse;
- USB HID host keyboard/mouse;
- NES/SNES-style gamepad support.

USB host/device role is a runtime configuration; there is no longer a build-time `USB_HID_ENABLED` switch in the normal build scripts.

## SD card layout

Production builds use the CPU target as their data directory. For the currently supported 286 build this is:

```text
SD root/
└── 286/
    ├── config.ini
    ├── disk images ...
    └── pagefile.sys      # created/used by SD paging when needed
```

`config.ini` is optional; the firmware can start with defaults and the runtime settings/disk manager can create or update configuration.

## Building

The recommended entry points are:

- Linux/macOS/WSL: `./build.sh`
- Windows: `build.bat`
- all supported 286 board/video/audio combinations: `build_all.sh` / `build_all.bat`

Examples:

```sh
./build.sh -M1 -VGA256 -i2s -504 -p 66 --clean
./build.sh -M2 -VGA128 -pwm --hdmi
./build_all.sh 286
```

The single-build scripts always pass `CPU_TARGET=286` intentionally. The all-build scripts accept `286` as an explicit CPU-target argument for forward compatibility, but currently reject `386` because that branch is not considered tested.

See [README-host-build.md](README-host-build.md) for toolchain setup, script options, build matrix and output locations.

## Output names

CMake encodes the important build parameters in the firmware name. Typical output looks like:

```text
m1p2-286-VGA128-504MHz-1.6V-P66-I2S-v1.14.uf2
```

Outputs are written under:

```text
bin/<CMAKE_BUILD_TYPE>/
```

## Runtime controls

The project contains the on-screen settings and disk-management UI. Current key bindings are implemented in `src/main.c`; consult that source when changing input mappings so documentation does not drift from code again.

## Source layout

```text
src/                 emulator, BIOS, platform and DOS integration
src/fdos/           native FreeDOS kernel port and FCOM
apps/                native applications and API examples
drivers/             video, audio, SD, PSRAM, PS/2, USB HID, gamepad
boards/              RP2350 board definitions used by Pico SDK
slave/               FRANK Core 2 slave firmware sources
```

## Upstream

The x86 emulator is derived from Tiny386 by Chunhui He and has since accumulated substantial RP2350-specific CPU, memory, BIOS, DOS, video and peripheral work.
