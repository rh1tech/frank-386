/**
 * Configuration Save for murm386
 *
 * Writes configuration to INI file on SD card.
 */

#ifndef CONFIG_SAVE_H
#define CONFIG_SAVE_H

#include <stdint.h>
#include <stdbool.h>

// Save all configuration to INI file
// Returns true on success
bool config_save_all(void);

// Save just disk configuration
bool config_save_disks(void);

// Get/set configuration values (stored in memory until saved)
int config_get_mem_size_mb(void);
void config_set_mem_size_mb(int mb);

int config_get_vga_mem_kb(void);
void config_set_vga_mem_kb(int kb);

int config_get_cpu_gen(void);
void config_set_cpu_gen(int gen);

int config_get_fpu(void);
void config_set_fpu(int enabled);

int config_get_fill_cmos(void);
void config_set_fill_cmos(int enabled);

// Hardware settings (saved in [murm386] section)
int config_get_pcspeaker(void);
void config_set_pcspeaker(int enabled);

int config_get_adlib(void);
void config_set_adlib(int enabled);

int config_get_soundblaster(void);
void config_set_soundblaster(int enabled);

int config_get_mouse(void);
void config_set_mouse(int enabled);

int config_get_cpu_freq(void);
void config_set_cpu_freq(int mhz);

int config_get_psram_freq(void);
void config_set_psram_freq(int mhz);

int config_get_vga_hshift(void);
void config_set_vga_hshift(int pixels);

// Check if hardware settings changed (requires reboot)
bool config_hw_changed(void);

// Check if config has unsaved changes
bool config_has_changes(void);

// Reset change tracking (call after save)
void config_clear_changes(void);

// Initialize config from current PCConfig
void config_init_from_current(void);

// INI parser callback for [murm386] section
int parse_murm386_ini(void* user, const char* section,
                      const char* name, const char* value);

#endif // CONFIG_SAVE_H
