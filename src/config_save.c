/**
 * frank-386 - i386 PC Emulator for RP2350
 *
 * Configuration Save - writes configuration to INI file on SD card.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "config_save.h"
#include "board_config.h"
#include "disk.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pc.h"

// Current configuration values (minimal storage)
static int cfg_cpu_gen = 4;
static int cfg_fpu = 0;
static int cfg_redirector = 1;
static char cfg_bios[32] = "";  /* empty = Native BIOS */
static int cfg_raw_sd_hdd = 0;
static int cfg_usb_mode = USB_MODE_HOST;
static bool cfg_changed = false;

// Hardware settings (use build-time defaults)
static int cfg_pcspeaker = 1;
static int cfg_adlib = 1;
static int cfg_soundblaster = 1;
static int cfg_tandy = 0;
static int cfg_covox = 1;
static int cfg_mpu401 = 1;
static int cfg_dss = 0;
static int cfg_mouse = 1;
static int cfg_nes_mouse = 0;
static int cfg_nes_joystick = 0;
static int cfg_usb_joystick = 0;
static int cfg_cpu_freq = CPU_CLOCK_MHZ;
static int cfg_psram_freq = PSRAM_MAX_FREQ_MHZ;
static int cfg_psram_size_mb = 0;
static int cfg_psram_test_freq = 0;
static int cfg_flash_freq = FLASH_MAX_FREQ_MHZ;
static int cfg_volume = 15;
static int cfg_voltage = -1;  /* -1 = auto (by cpu_freq) */
static int cfg_mouse_invert_y = 0;
static bool cfg_hw_changed = false;

extern PC *pc;

// INI file path
#define CONFIG_PATH SD_DATA_DIR_SLASH "config.ini"

bool config_ensure_data_dir(void) {
    FILINFO info;
    FRESULT res = f_stat(SD_DATA_DIR, &info);

    if (res == FR_OK)
        return (info.fattrib & AM_DIR) != 0;

    if (res != FR_NO_FILE && res != FR_NO_PATH)
        return false;

    res = f_mkdir(SD_DATA_DIR);
    if (res == FR_OK)
        return true;

    /*
     * Another path may have created it between f_stat() and f_mkdir().
     * Accept FR_EXIST only when the existing object really is a directory.
     */
    if (res == FR_EXIST) {
        res = f_stat(SD_DATA_DIR, &info);
        return res == FR_OK && (info.fattrib & AM_DIR) != 0;
    }

    return false;
}

void config_init_from_current(void) {
    // These will be set from PCConfig in main.c
    cfg_changed = false;
}

int config_get_cpu_gen(void) { return cfg_cpu_gen; }
void config_set_cpu_gen(int gen) {
    if (cfg_cpu_gen != gen) {
        cfg_cpu_gen = gen;
        cfg_changed = true;
    }
}

int config_get_fpu(void) { return cfg_fpu; }
void config_set_fpu(int enabled) {
    if (cfg_fpu != enabled) {
        cfg_fpu = enabled;
        cfg_changed = true;
    }
}

int config_get_redirector(void) { return cfg_redirector; }
void config_set_redirector(int enabled) {
    if (cfg_redirector != enabled) {
        cfg_redirector = enabled;
        cfg_changed = true;
    }
}

const char *config_get_bios_file(void) {
    return cfg_bios[0] ? cfg_bios : NULL;
}

int config_get_raw_sd_hdd(void) { return cfg_raw_sd_hdd; }
void config_set_raw_sd_hdd(int enabled) {
    enabled = !!enabled;
    if (cfg_raw_sd_hdd != enabled) {
        cfg_raw_sd_hdd = enabled;
        cfg_changed = true;
    }
}

int config_get_usb_mode(void) { return cfg_usb_mode; }
void config_set_usb_mode(int mode) {
    mode = (mode == USB_MODE_DEVICE) ? USB_MODE_DEVICE : USB_MODE_HOST;
    if (cfg_usb_mode != mode) {
        cfg_usb_mode = mode;
        cfg_changed = true;
        cfg_hw_changed = true;
    }
}

void config_set_bios_file(const char *filename) {
    char normalized[sizeof(cfg_bios)];

    if (!filename || filename[0] == '\0' || strcasecmp(filename, "native") == 0) {
        normalized[0] = '\0';
    } else {
        strncpy(normalized, filename, sizeof(normalized) - 1);
        normalized[sizeof(normalized) - 1] = '\0';
    }

    if (strcmp(cfg_bios, normalized) != 0) {
        strcpy(cfg_bios, normalized);
        cfg_changed = true;
    }

    if (pc)
        pc->bios = cfg_bios[0] ? cfg_bios : NULL;
}

// Hardware settings
int config_get_pcspeaker(void) { return cfg_pcspeaker; }
void config_set_pcspeaker(int enabled) {
    pc->pcspk_enabled = enabled;
    if (cfg_pcspeaker != enabled) {
        cfg_pcspeaker = enabled;
        cfg_changed = true;
    }
}

int config_get_adlib(void) { return cfg_adlib; }
void config_set_adlib(int enabled) {
    pc->adlib_enabled = enabled;
    if (cfg_adlib != enabled) {
        cfg_adlib = enabled;
        cfg_changed = true;
    }
}

int config_get_soundblaster(void) { return cfg_soundblaster; }
void config_set_soundblaster(int enabled) {
    pc->sb16_enabled = enabled;
    if (cfg_soundblaster != enabled) {
        cfg_soundblaster = enabled;
        cfg_changed = true;
    }
}

int config_get_tandy(void) { return cfg_tandy; }
void config_set_tandy(int enabled) {
    pc->tandy_enabled = enabled;
    if (cfg_tandy != enabled) {
        cfg_tandy = enabled;
        cfg_changed = true;
    }
}

int config_get_covox(void) { return cfg_covox; }
void config_set_covox(int enabled) {
    pc->covox_enabled = enabled;
    if (cfg_covox != enabled) {
        cfg_covox = enabled;
        cfg_changed = true;
    }
}

int config_get_mpu401(void) { return cfg_mpu401; }
void config_set_mpu401(int enabled) {
    pc->mpu401_enabled = enabled;
    if (cfg_mpu401 != enabled) {
        cfg_mpu401 = enabled;
        cfg_changed = true;
    }
}

int config_get_dss(void) { return cfg_dss; }
void config_set_dss(int enabled) {
    pc->dss_enabled = enabled;
    if (cfg_dss != enabled) {
        cfg_dss = enabled;
        cfg_changed = true;
    }
}

int config_get_mouse(void) { return cfg_mouse; }
void config_set_mouse(int enabled) {
    pc->mouse_enabled = enabled;
    if (cfg_mouse != enabled) {
        cfg_mouse = enabled;
        cfg_changed = true;
    }
}

int config_get_usb_joystick(void) { return cfg_usb_joystick; }
void config_set_usb_joystick(int enabled) {
    pc->joystick_enabled = enabled || cfg_nes_joystick;
    if (cfg_usb_joystick != enabled) {
        cfg_usb_joystick = enabled;
        cfg_changed = true;
    }
}

int config_get_nes_joystick(void) { return cfg_nes_joystick; }
void config_set_nes_joystick(int enabled) {
    /* Applied live, like the other device toggles: the game port appears
     * or disappears without a restart. With it off, reads of 0x201 fall
     * back to 0xF0, which is what an empty adapter returns. */
    pc->joystick_enabled = enabled || cfg_usb_joystick;
    if (cfg_nes_joystick != enabled) {
        cfg_nes_joystick = enabled;
        cfg_changed = true;
    }
}

int config_get_nes_mouse(void) { return cfg_nes_mouse; }
void config_set_nes_mouse(int enabled) {
    if (cfg_nes_mouse != enabled) {
        cfg_nes_mouse = enabled;
        cfg_changed = true;
    }
    /* NES mouse still needs the emulated i8042 mouse port active */
    if (enabled) pc->mouse_enabled = 1;
}

int config_get_cpu_freq(void) { return cfg_cpu_freq; }
void config_set_cpu_freq(int mhz) {
    if (cfg_cpu_freq != mhz) {
        cfg_cpu_freq = mhz;
        cfg_changed = true;
        cfg_hw_changed = true;
    }
}

int config_get_psram_freq(void) { return cfg_psram_freq; }
int config_get_psram_size_mb(void) { return cfg_psram_size_mb; }
int config_get_psram_test_freq(void) { return cfg_psram_test_freq; }
void config_set_psram_test_cache(int size_mb, int test_freq_mhz) {
    if (size_mb != 1 && size_mb != 2 && size_mb != 4 &&
        size_mb != 8 && size_mb != 16)
        return;
    cfg_psram_size_mb = size_mb;
    cfg_psram_test_freq = test_freq_mhz;
    cfg_changed = true;
}
void config_invalidate_psram_test_cache_runtime(void) {
    cfg_psram_size_mb = 0;
    cfg_psram_test_freq = 0;
}
void config_set_psram_freq(int mhz) {
    if (cfg_psram_freq != mhz) {
        cfg_psram_freq = mhz;
        cfg_changed = true;
        cfg_hw_changed = true;
    }
}
int config_get_flash_freq(void) { return cfg_flash_freq; }
void config_set_flash_freq(int mhz) {
    if (cfg_flash_freq != mhz) {
        cfg_flash_freq = mhz;
        cfg_changed = true;
        cfg_hw_changed = true;
    }
}
int config_get_volume(void) { return cfg_volume; }
void config_set_volume(int vol) {
    if (cfg_volume != vol) {
        cfg_volume = vol;
        cfg_changed = true;
    }
}
int config_get_voltage(void) { return cfg_voltage; }
void config_set_voltage(int v) {
    if (cfg_voltage != v) {
        cfg_voltage = v;
        cfg_changed = true;
        cfg_hw_changed = true;
    }
}
int config_get_mouse_invert_y(void) { return cfg_mouse_invert_y; }
void config_set_mouse_invert_y(int enabled) {
    if (cfg_mouse_invert_y != enabled) {
        cfg_mouse_invert_y = enabled;
        cfg_changed = true;
    }
}

bool config_hw_changed(void) { return cfg_hw_changed; }
bool config_has_changes(void) { return cfg_changed; }
void config_clear_changes(void) { cfg_changed = false; cfg_hw_changed = false; }

// Write text using DOS-compatible CRLF line endings.
static bool write_line(FIL *fp, const char *line) {
    const char *p = line;

    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        UINT len = (UINT)(end - p);
        UINT bw;

        if (len) {
            FRESULT res = f_write(fp, p, len, &bw);
            if (res != FR_OK || bw != len)
                return false;
        }

        if (!nl)
            break;

        // Preserve an existing CRLF; otherwise convert LF to CRLF.
        if (nl == p || nl[-1] != '\r') {
            static const char cr = '\r';
            FRESULT res = f_write(fp, &cr, 1, &bw);
            if (res != FR_OK || bw != 1)
                return false;
        }

        {
            static const char lf = '\n';
            FRESULT res = f_write(fp, &lf, 1, &bw);
            if (res != FR_OK || bw != 1)
                return false;
        }

        p = nl + 1;
    }

    return true;
}

bool config_save_all(void) {
    FIL fp;
    FRESULT res;
    char line[80];

    /*
     * The directory may legitimately disappear while the raw SD card is
     * exported over USB MSC, or may not exist yet on a fresh card.
     * Recreate it before trying to persist the configuration.
     */
    if (!config_ensure_data_dir())
        return false;

    res = f_open(&fp, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return false;

    // Write [pc] section
    write_line(&fp, "[pc]\n");

//    snprintf(line, sizeof(line), "vga_mem=%dK\n", cfg_vga_kb);
//    write_line(&fp, line);

    // CPU
    snprintf(line, sizeof(line), "cpu=%d\n", cfg_cpu_gen);
    write_line(&fp, line);

    // BIOS files
    if (cfg_bios[0]) {
        snprintf(line, sizeof(line), "bios=%s\n", cfg_bios);
        write_line(&fp, line);
    } else {
        write_line(&fp, "bios=native\n");
    }
    write_line(&fp, "vga_bios=vgabios.bin\n");

    // Fill CMOS
    snprintf(line, sizeof(line), "redirector=%d\n", cfg_redirector);
    write_line(&fp, line);

    // Disks (must be in [pc] section)
    write_line(&fp, "\n; Disk images\n");
    snprintf(line, sizeof(line), "raw_sd_hdd=%d\n", cfg_raw_sd_hdd);
    write_line(&fp, line);
    for (int i = 0; i < 2; i++) {
        const char *fname = fdd_get_filename(i);
        if (fname && fname[0]) {
            snprintf(line, sizeof(line), "fd%c=%s\n", 'a' + i, fname);
            write_line(&fp, line);
        }
    }
    for (int i = 0; i < 4; i++) {
        const char *fname = ata_get_filename(i);
        if (fname && fname[0]) {
            if (ata_is_cdrom(i)) {
                snprintf(line, sizeof(line), "cd%c=%s\n", 'a' + i, fname);
            } else {
                snprintf(line, sizeof(line), "hd%c=%s\n", 'a' + i, fname);
            }
            write_line(&fp, line);
        }
    }

    // FPU (separate section)
    write_line(&fp, "\n[cpu]\n");
    snprintf(line, sizeof(line), "gen=%d\n", cfg_cpu_gen);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "fpu=%d\n", cfg_fpu);
    write_line(&fp, line);

    // Hardware settings (frank-386-specific)
    write_line(&fp, "\n[frank-386]\n");
    snprintf(line, sizeof(line), "pcspeaker=%d\n", cfg_pcspeaker);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "adlib=%d\n", cfg_adlib);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "soundblaster=%d\n", cfg_soundblaster);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "tandy=%d\n", cfg_tandy);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "covox=%d\n", cfg_covox);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "mpu401=%d\n", cfg_mpu401);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "dss=%d\n", cfg_dss);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "mouse=%d\n", cfg_mouse);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "nes_mouse=%d\n", cfg_nes_mouse);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "nes_joystick=%d\n", cfg_nes_joystick);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "usb_joystick=%d\n", cfg_usb_joystick);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "usb=%s\n",
             cfg_usb_mode == USB_MODE_DEVICE ? "DEVICE" : "HOST");
    write_line(&fp, line);
    snprintf(line, sizeof(line), "cpu_freq=%d\n", cfg_cpu_freq);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "psram_freq=%d\n", cfg_psram_freq);
    write_line(&fp, line);
    if (cfg_psram_size_mb) {
        snprintf(line, sizeof(line), "psram_size=%d\n", cfg_psram_size_mb);
        write_line(&fp, line);
        snprintf(line, sizeof(line), "psram_test_freq=%d\n", cfg_psram_test_freq);
        write_line(&fp, line);
    }
    snprintf(line, sizeof(line), "flash_freq=%d\n", cfg_flash_freq);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "volume=%d\n", cfg_volume);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "voltage=%d\n", cfg_voltage);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "mouse_invert_y=%d\n", cfg_mouse_invert_y);
    write_line(&fp, line);

    f_close(&fp);
    cfg_changed = false;
    cfg_hw_changed = false;
    return true;
}

bool config_save_disks(void) {
    // For now, save everything (simpler implementation)
    return config_save_all();
}

// INI parser callback for [frank-386] section
int parse_frank_386_ini(void* user, const char* section,
                      const char* name, const char* value) {
    (void)user;

    // Accept both new and legacy section names
    if (strcmp(section, "frank-386") != 0 && strcmp(section, "murm386") != 0) return 1;

    if (strcmp(name, "pcspeaker") == 0) {
        cfg_pcspeaker = atoi(value);
    } else if (strcmp(name, "adlib") == 0) {
        cfg_adlib = atoi(value);
    } else if (strcmp(name, "soundblaster") == 0) {
        cfg_soundblaster = atoi(value);
    } else if (strcmp(name, "tandy") == 0) {
        cfg_tandy = atoi(value);
    } else if (strcmp(name, "covox") == 0) {
        cfg_covox = atoi(value);
    } else if (strcmp(name, "mpu401") == 0) {
        cfg_mpu401 = atoi(value);
    } else if (strcmp(name, "dss") == 0) {
        cfg_dss = atoi(value);
    } else if (strcmp(name, "mouse") == 0) {
        cfg_mouse = atoi(value);
    } else if (strcmp(name, "usb_joystick") == 0) {
        cfg_usb_joystick = atoi(value);
    } else if (strcmp(name, "usb") == 0) {
        cfg_usb_mode = (strcasecmp(value, "DEVICE") == 0)
                     ? USB_MODE_DEVICE : USB_MODE_HOST;
    } else if (strcmp(name, "nes_joystick") == 0) {
        cfg_nes_joystick = atoi(value);
    } else if (strcmp(name, "nes_mouse") == 0) {
        cfg_nes_mouse = atoi(value);
    } else if (strcmp(name, "cpu_freq") == 0) {
        cfg_cpu_freq = atoi(value);
    } else if (strcmp(name, "psram_freq") == 0) {
        cfg_psram_freq = atoi(value);
    } else if (strcmp(name, "psram_size") == 0) {
        int mb = atoi(value);
        if (mb == 1 || mb == 2 || mb == 4 || mb == 8 || mb == 16)
            cfg_psram_size_mb = mb;
    } else if (strcmp(name, "psram_test_freq") == 0) {
        cfg_psram_test_freq = atoi(value);
    } else if (strcmp(name, "flash_freq") == 0) {
        cfg_flash_freq = atoi(value);
    } else if (strcmp(name, "volume") == 0) {
        cfg_volume = atoi(value);
    } else if (strcmp(name, "voltage") == 0) {
        cfg_voltage = atoi(value);
    } else if (strcmp(name, "mouse_invert_y") == 0) {
        cfg_mouse_invert_y = atoi(value);
    }

    return 1;  // Success
}
