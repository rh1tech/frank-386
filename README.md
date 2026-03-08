# murm386

i386 PC Emulator for RP2350 (Raspberry Pi Pico 2) with VGA output, SD card storage, PS/2 and USB keyboard, and audio output.

Based on [Tiny386](https://github.com/hchunhui/tiny386) by Chunhui He.

## Features

- Full i386 (and partially i486/i586) CPU emulation with optional x87 FPU
- Up to 7MB RAM (using 8MB PSRAM)
- VGA graphics output (text modes and graphics up to 640x480)
- Sound: AdLib OPL2, Sound Blaster 16, PC Speaker
- SD card support for floppy, hard disk, and CD-ROM images
- Runtime disk manager (Win+F12) for hot-swapping disk images
- Settings menu (Win+F11) for changing emulator configuration
- PS/2 keyboard input
- USB keyboard and mouse input (via native USB Host)
- Boots DOS, Windows 3.x/95, Linux, and more

## Supported Boards

This firmware is designed for RP2350-based boards with integrated VGA, SD card, and keyboard input:

- **[Murmulator](https://murmulator.ru)** - A compact retro-computing platform based on RP Pico 2
- **[FRANK](https://rh1.tech/projects/frank?area=about)** - A versatile development board with VGA output

Both boards provide all necessary peripherals out of the box.

## Hardware Requirements

- **Raspberry Pi Pico 2** (RP2350) or compatible board
- **8MB PSRAM** (required for extended memory)
- **VGA connector** (accent resistor DAC)
- **SD card module** (SPI mode)
- **PS/2 keyboard** (directly connected) - OR - **USB keyboard** (via native USB port)
- **Audio output** (optional):
  - **I2S DAC module** (e.g., TDA1387 or PCM5102) - recommended for best quality

> **Note:** When USB HID is enabled, the native USB port is used for keyboard/mouse input. USB serial console (CDC) is disabled in this mode; use UART for debug output.

## Board Configurations

Two GPIO layouts are supported: **M1** and **M2**.

### VGA (accent resistor DAC)
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| HSYNC  | 6       | 12      |
| VSYNC  | 7       | 13      |
| R0     | 8       | 14      |
| R1     | 9       | 15      |
| G0     | 10      | 16      |
| G1     | 11      | 17      |
| B0     | 12      | 18      |
| B1     | 13      | 19      |

### SD Card (SPI mode)
| Signal  | M1 GPIO | M2 GPIO |
|---------|---------|---------|
| CLK     | 2       | 6       |
| CMD     | 3       | 7       |
| DAT0    | 4       | 4       |
| DAT3/CS | 5       | 5       |

### PS/2 Keyboard
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| CLK    | 0       | 2       |
| DATA   | 1       | 3       |

### I2S Audio
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| DATA   | 26      | 9       |
| BCLK   | 27      | 10      |
| LRCLK  | 28      | 11      |

## SD Card Setup

### Directory Structure

Create a `386/` directory on your SD card:

```
SD Card Root/
└── 386/
    ├── config.ini      # Configuration file
    ├── bios.bin        # SeaBIOS ROM (required)
    ├── vgabios.bin     # VGA BIOS ROM (required)
    ├── dos622.img      # Hard disk image
    ├── boot.img        # Floppy image
    └── ...             # Other disk images
```

### BIOS Files

Download SeaBIOS and VGA BIOS from the [SeaBIOS releases](https://www.seabios.org/downloads/) or use bios.bin/vgabios.bin from `sdcard/386`.

### Configuration File (config.ini)

Create `386/config.ini`:

```ini
[pc]
mem=2M
bios=bios.bin
vga_bios=vgabios.bin

[murm386]
cpu_freq=504
psram_freq=166
```

### Preparing Disk Images

**Floppy Images (.img):**
- Standard 1.44MB floppy images (1474560 bytes)
- Create with: `dd if=/dev/zero of=floppy.img bs=512 count=2880`
- Format with DOS or use pre-made DOS boot disks

**Hard Disk Images (.img):**
- Raw disk images up to 2GB
- Create with: `dd if=/dev/zero of=hdd.img bs=1M count=512`
- Use FDISK and FORMAT from DOS to partition and format

**CD-ROM Images (.iso):**
- Standard ISO 9660 images
- Use CD burning software to create ISOs from CDs

### Loading Disk Images

**At Boot:**
Configure disk images in `config.ini` as shown above.

**At Runtime (Disk Manager):**
1. Press **Win+F12** to open the Disk Manager
2. Use arrow keys to select a drive (A:, B:, C:, D:, E:)
3. Press **Enter** to browse disk images in the `386/` directory
4. Select an image file to insert, or eject the current disk
5. Press **Escape** to close the Disk Manager

Changes made via Disk Manager are saved to `config.ini` automatically.

## Controls

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Win+F12  | Open Disk Manager |
| Win+F11  | Open Settings Menu |
| Ctrl+Alt+Delete | System reset (sent to guest OS) |

### Settings Menu (Win+F11)

Configure emulator settings at runtime:
- Memory size (1-7 MB)
- CPU generation (386/486/586)
- FPU emulation on/off
- Sound devices on/off
- Mouse support on/off
- RP2350 CPU frequency (378/504 MHz)
- PSRAM frequency (133/166 MHz)

Settings are saved to `config.ini` and take effect after restart.

### Disk Manager (Win+F12)

Manage disk images without restarting:
- Insert/eject floppy images (A:, B:)
- Insert/eject hard disk images (C:, D:)
- Insert/eject CD-ROM images (E:)

## Building

### Prerequisites

1. Install the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (version 2.0+)
2. Set environment variable: `export PICO_SDK_PATH=/path/to/pico-sdk`
3. Install ARM GCC toolchain

### Build Steps

```bash
# Clone the repository
git clone https://github.com/user/murm386.git
cd murm386

# Build with default settings (M1 board, 378MHz, PS/2 keyboard)
./build.sh

# Build for M2 board
./build.sh -M2

# Build with USB keyboard support
./build.sh --usb-hid

# Build for Murmulator OS
./build.sh --mos2

# Custom build
./build.sh -b M1 -c 504 -p 166 --debug
```

### Build Options (build.sh)

| Option | Description |
|--------|-------------|
| `-b, --board <M1\|M2>` | Board variant (default: M1) |
| `-c, --cpu <MHz>` | CPU speed: 378 (default), 504 |
| `-p, --psram <MHz>` | PSRAM speed: 133 (default), 166 |
| `--usb-hid` | Enable USB keyboard (disables USB serial) |
| `--debug` | Enable debug output |
| `--mos2` | Build for Murmulator OS (m1p2/m2p2 format) |
| `-clean` | Clean build directory first |

### Build Options (CMake)

| Option | Description |
|--------|-------------|
| `-DPICO_BOARD=pico2` | Build for RP2350 (default) |
| `-DBOARD=M1` | Use M1 GPIO layout (default) |
| `-DBOARD=M2` | Use M2 GPIO layout |
| `-DCPU_SPEED=378` | CPU clock in MHz (378, 504) |
| `-DPSRAM_SPEED=133` | PSRAM clock in MHz (133, 166) |
| `-DUSB_HID_ENABLED=ON` | Enable USB keyboard (disables USB serial) |
| `-DDEBUG_ENABLED=ON` | Enable verbose debug logging |
| `-DMOS2=ON` | Build for Murmulator OS |

### Release Builds

To build all firmware variants:

```bash
./release.sh
```

This creates firmware files in the `release/` directory:
- `murm386_m1_*.uf2` - M1 board, standard UF2 format
- `murm386_m2_*.uf2` - M2 board, standard UF2 format
- `murm386_m1_*.m1p2` - M1 board, Murmulator OS 2 format
- `murm386_m2_*.m2p2` - M2 board, Murmulator OS 2 format

### Flashing

```bash
# With device in BOOTSEL mode:
picotool load build/murm386.uf2

# Or use the flash script:
./flash.sh
```

## Troubleshooting

### "0 bytes of memory" during Windows 95 setup
Use `setup /im` to bypass memory check.

### "Protection error" during Windows 95 startup
Use [patcher9x](https://github.com/JHRobotics/patcher9x).

### Freeze during Windows NT4/2000/XP startup
Set `fill_cmos = 0` in config.ini.

### No keyboard input
- For PS/2: Check keyboard connection and GPIO pins
- For USB: Ensure firmware was built with `--usb-hid` option

### SD card not detected
- Ensure SD card is formatted as FAT32
- Check SD card module connections
- Verify `386/` directory exists on SD card

## License

MIT License. See [LICENSE](LICENSE) for details.

## Authors & Contributors

**Mikhail Matveev** <<xtreme@rh1.tech>>
- murm386 port and development (2026)
- Website: [https://rh1.tech](https://rh1.tech)

## Acknowledgments

This project is based on the following open-source projects:

### Tiny386
- **Project:** [Tiny386 - x86 PC Emulator](https://github.com/hchunhui/tiny386)
- **Author:** Chunhui He
- **License:** BSD 3-Clause
- **Description:** The core i386 CPU emulator and PC peripheral emulation (8259 PIC, 8254 PIT, 8042 keyboard controller, VGA, sound devices).

### Pico-286
- **Project:** [Pico-286](https://github.com/xrip/pico-286)
- **Author:** xrip
- **License:** MIT
- **Description:** RP2350 platform integration, disk management, VGA driver concepts.

### QuakeGeneric
- **Project:** [QuakeGeneric](https://github.com/DnCraptor/quakegeneric)
- **Author:** DnCraptor
- **License:** GPL v2
- **Description:** RP2350 hardware integration patterns, Murmulator platform support, and PS/2 mouse driver implementation.

### QEMU
- **Project:** [QEMU](https://www.qemu.org/)
- **Authors:** Fabrice Bellard (2003-2017), Vassili Karpov "malc" (2003-2005), Joachim Henke (2006)
- **License:** MIT
- **Description:** PC peripheral emulation code including 8259 PIC, 8254 PIT, 8257 DMA, 8042 keyboard controller, PCI bus, PC speaker, VGA, and AdLib OPL2 proxy.

### MAME FM Sound Generator
- **Project:** [MAME](https://www.mamedev.org/)
- **Author:** Tatsuyuki Satoh (1999-2000)
- **License:** LGPL 2.1+
- **Description:** FM OPL sound generator (fmopl) for AdLib emulation, forked from MAME and relicensed under LGPL.

### inih
- **Project:** [inih](https://github.com/benhoyt/inih)
- **Author:** Ben Hoyt (2009-2020)
- **License:** BSD 3-Clause
- **Description:** Simple INI file parser for configuration file handling.

### SeaBIOS
- **Project:** [SeaBIOS](https://www.seabios.org/)
- **Authors:** Kevin O'Connor and contributors
- **License:** GNU LGPL v3
- **Description:** x86 BIOS and VGA BIOS firmware.

### FatFs
- **Project:** [FatFs](http://elm-chan.org/fsw/ff/)
- **Author:** ChaN (2014, 2021)
- **License:** FatFs License (BSD-style)
- **Description:** Generic FAT filesystem module for SD card access.

### FatFs Utilities
- **Author:** Carl John Kugler III (2021)
- **License:** Apache 2.0
- **Description:** FatFs utility functions for error handling and result string conversion.

### Raspberry Pi Pico SDK
- **Project:** [Pico SDK](https://github.com/raspberrypi/pico-sdk)
- **Author:** Raspberry Pi (Trading) Ltd. (2020)
- **License:** BSD 3-Clause
- **Description:** PIO SPI driver for SD card communication.
