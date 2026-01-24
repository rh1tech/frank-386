/**
 * Configuration Save for murm386
 *
 * Writes configuration to INI file on SD card.
 */

#include "config_save.h"
#include "board_config.h"
#include "disk.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Current configuration values (minimal storage)
static int cfg_mem_mb = 4;
static int cfg_vga_kb = 128;
static int cfg_cpu_gen = 4;
static int cfg_fpu = 0;
static int cfg_fill_cmos = 1;
static bool cfg_changed = false;

// Hardware settings (use build-time defaults)
static int cfg_pcspeaker = 1;
static int cfg_adlib = 1;
static int cfg_soundblaster = 1;
static int cfg_mouse = 1;
static int cfg_cpu_freq = CPU_CLOCK_MHZ;
static int cfg_psram_freq = PSRAM_MAX_FREQ_MHZ;
static int cfg_vga_hshift = 138;  // Default VGA horizontal shift
static bool cfg_hw_changed = false;

// INI file path
#define CONFIG_PATH "386/config.ini"

void config_init_from_current(void) {
    // These will be set from PCConfig in main.c
    cfg_changed = false;
}

int config_get_mem_size_mb(void) { return cfg_mem_mb; }
void config_set_mem_size_mb(int mb) {
    if (cfg_mem_mb != mb) {
        cfg_mem_mb = mb;
        cfg_changed = true;
    }
}

int config_get_vga_mem_kb(void) { return cfg_vga_kb; }
void config_set_vga_mem_kb(int kb) {
    if (cfg_vga_kb != kb) {
        cfg_vga_kb = kb;
        cfg_changed = true;
    }
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

int config_get_fill_cmos(void) { return cfg_fill_cmos; }
void config_set_fill_cmos(int enabled) {
    if (cfg_fill_cmos != enabled) {
        cfg_fill_cmos = enabled;
        cfg_changed = true;
    }
}

// Hardware settings
int config_get_pcspeaker(void) { return cfg_pcspeaker; }
void config_set_pcspeaker(int enabled) {
    if (cfg_pcspeaker != enabled) {
        cfg_pcspeaker = enabled;
        cfg_changed = true;
    }
}

int config_get_adlib(void) { return cfg_adlib; }
void config_set_adlib(int enabled) {
    if (cfg_adlib != enabled) {
        cfg_adlib = enabled;
        cfg_changed = true;
    }
}

int config_get_soundblaster(void) { return cfg_soundblaster; }
void config_set_soundblaster(int enabled) {
    if (cfg_soundblaster != enabled) {
        cfg_soundblaster = enabled;
        cfg_changed = true;
    }
}

int config_get_mouse(void) { return cfg_mouse; }
void config_set_mouse(int enabled) {
    if (cfg_mouse != enabled) {
        cfg_mouse = enabled;
        cfg_changed = true;
    }
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
void config_set_psram_freq(int mhz) {
    if (cfg_psram_freq != mhz) {
        cfg_psram_freq = mhz;
        cfg_changed = true;
        cfg_hw_changed = true;
    }
}

int config_get_vga_hshift(void) { return cfg_vga_hshift; }
void config_set_vga_hshift(int pixels) {
    if (cfg_vga_hshift != pixels) {
        cfg_vga_hshift = pixels;
        cfg_changed = true;
    }
}

bool config_hw_changed(void) { return cfg_hw_changed; }
bool config_has_changes(void) { return cfg_changed; }
void config_clear_changes(void) { cfg_changed = false; cfg_hw_changed = false; }

// Write a line to file
static bool write_line(FIL *fp, const char *line) {
    UINT bw;
    FRESULT res = f_write(fp, line, strlen(line), &bw);
    return (res == FR_OK && bw == strlen(line));
}

bool config_save_all(void) {
    FIL fp;
    FRESULT res;
    char line[80];

    res = f_open(&fp, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return false;

    // Write [pc] section
    write_line(&fp, "[pc]\n");

    // Memory
    snprintf(line, sizeof(line), "mem=%dM\n", cfg_mem_mb);
    write_line(&fp, line);

    snprintf(line, sizeof(line), "vga_mem=%dK\n", cfg_vga_kb);
    write_line(&fp, line);

    // CPU
    snprintf(line, sizeof(line), "cpu=%d\n", cfg_cpu_gen);
    write_line(&fp, line);

    // BIOS files
    write_line(&fp, "bios=bios.bin\n");
    write_line(&fp, "vga_bios=vgabios.bin\n");

    // Fill CMOS
    snprintf(line, sizeof(line), "fill_cmos=%d\n", cfg_fill_cmos);
    write_line(&fp, line);

    // Disks (must be in [pc] section)
    write_line(&fp, "\n; Disk images\n");
    for (int i = 0; i < 2; i++) {
        const char *fname = disk_get_filename(i);
        if (fname && fname[0]) {
            snprintf(line, sizeof(line), "fd%c=%s\n", 'a' + i, fname);
            write_line(&fp, line);
        }
    }
    for (int i = 2; i < 5; i++) {
        const char *fname = disk_get_filename(i);
        if (fname && fname[0]) {
            if (disk_is_cdrom(i)) {
                snprintf(line, sizeof(line), "cd%c=%s\n", 'a' + (i - 2), fname);
            } else {
                snprintf(line, sizeof(line), "hd%c=%s\n", 'a' + (i - 2), fname);
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

    // Hardware settings (murm386-specific)
    write_line(&fp, "\n[murm386]\n");
    snprintf(line, sizeof(line), "pcspeaker=%d\n", cfg_pcspeaker);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "adlib=%d\n", cfg_adlib);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "soundblaster=%d\n", cfg_soundblaster);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "mouse=%d\n", cfg_mouse);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "cpu_freq=%d\n", cfg_cpu_freq);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "psram_freq=%d\n", cfg_psram_freq);
    write_line(&fp, line);
    snprintf(line, sizeof(line), "vga_hshift=%d\n", cfg_vga_hshift);
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

// INI parser callback for [murm386] section
int parse_murm386_ini(void* user, const char* section,
                      const char* name, const char* value) {
    (void)user;

    if (strcmp(section, "murm386") != 0) return 1;  // Not our section

    if (strcmp(name, "pcspeaker") == 0) {
        cfg_pcspeaker = atoi(value);
    } else if (strcmp(name, "adlib") == 0) {
        cfg_adlib = atoi(value);
    } else if (strcmp(name, "soundblaster") == 0) {
        cfg_soundblaster = atoi(value);
    } else if (strcmp(name, "mouse") == 0) {
        cfg_mouse = atoi(value);
    } else if (strcmp(name, "cpu_freq") == 0) {
        cfg_cpu_freq = atoi(value);
    } else if (strcmp(name, "psram_freq") == 0) {
        cfg_psram_freq = atoi(value);
    } else if (strcmp(name, "vga_hshift") == 0) {
        cfg_vga_hshift = atoi(value);
    }

    return 1;  // Success
}
