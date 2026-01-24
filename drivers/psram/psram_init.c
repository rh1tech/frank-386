/**
 * PSRAM Driver for RP2350
 *
 * Initializes APS6404L-3SQR QSPI PSRAM (8MB) on CS1.
 * Memory is mapped at 0x11000000 (XIP_SRAM_BASE).
 */

#include "psram_init.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>

// PSRAM max frequency from build config (default 133 MHz)
#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

/**
 * Initialize PSRAM hardware with specified frequency.
 * This function MUST run from RAM, not flash, as it reconfigures the XIP controller.
 */
void __no_inline_not_in_flash_func(psram_init_with_freq)(uint cs_pin, int freq_mhz) {
    const int clock_hz = clock_get_hz(clk_sys);

    // Configure GPIO for XIP CS1 function
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    // Enter direct mode with slow clock for initialization
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB |
                        QMI_DIRECT_CSR_EN_BITS |
                        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    // Send QPI enable command (0x35) to PSRAM
    const uint CMD_QPI_EN = 0x35;
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_QPI_EN;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    // Calculate optimal clock divisor for target PSRAM frequency
    const int max_psram_freq = freq_mhz * 1000000;

    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;  // Minimum divisor of 2 at high system clocks
    }

    // RX delay compensation for high-speed operation
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1;
    }

    // Calculate timing parameters
    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select_val = (125 * 1000000) / clock_period_fs;
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    // Configure M1 (PSRAM) timing
    qmi_hw->m[1].timing =
        1 << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        max_select_val << QMI_M1_TIMING_MAX_SELECT_LSB |
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
        divisor << QMI_M1_TIMING_CLKDIV_LSB;

    // Configure read format: Quad mode, 0xEB fast read command, 6 dummy cycles
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;

    qmi_hw->m[1].rcmd = 0xEB;  // Fast Read Quad I/O

    // Configure write format: Quad mode, 0x38 write command
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;

    qmi_hw->m[1].wcmd = 0x38;  // Quad Write

    // Exit direct mode
    qmi_hw->direct_csr = 0;

    // Enable writes to M1 (PSRAM) region
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

/**
 * Initialize PSRAM hardware with default frequency.
 * This function MUST run from RAM, not flash, as it reconfigures the XIP controller.
 */
void __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    psram_init_with_freq(cs_pin, PSRAM_MAX_FREQ_MHZ);
}

/**
 * Test PSRAM by writing and reading back test patterns.
 */
bool psram_test(void) {
    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_BASE_ADDR;

    // Test pattern at various locations
    const uint32_t test_offsets[] = {
        0,                      // Start
        1024,                   // 4KB
        256 * 1024,             // 1MB
        1024 * 1024,            // 4MB
        2 * 1024 * 1024 - 4,    // Near end of 8MB
    };

    const uint32_t test_pattern = 0xDEADBEEF;
    const uint32_t test_pattern2 = 0x12345678;

    for (int i = 0; i < (int)(sizeof(test_offsets) / sizeof(test_offsets[0])); i++) {
        uint32_t offset = test_offsets[i] / 4;  // Convert to word offset

        // Write pattern
        psram[offset] = test_pattern;

        // Read back and verify
        if (psram[offset] != test_pattern) {
            return false;
        }

        // Write second pattern
        psram[offset] = test_pattern2;

        // Read back and verify
        if (psram[offset] != test_pattern2) {
            return false;
        }

        // Clear
        psram[offset] = 0;
    }

    return true;
}
