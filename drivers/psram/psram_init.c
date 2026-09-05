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
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <string.h>

// PSRAM max frequency from build config (default 133 MHz)
#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

/**
 * Compute and apply the QMI M1 timing for a system clock of clock_hz.
 * This function MUST run from RAM, not flash, as it retimes the XIP controller.
 */
static void __no_inline_not_in_flash_func(psram_write_timing)(int clock_hz, int freq_mhz) {
    // Calculate optimal clock divisor for target PSRAM frequency
    const int max_psram_freq = freq_mhz * 1000000;

    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;  // Minimum divisor of 2 at high system clocks
    }
    if (divisor > 255) divisor = 255;   /* CLKDIV is 8 bits */

    const int clock_period_fs = 1000000000000000ll / clock_hz;

    // RX delay compensation for high-speed operation
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1;
    }

    /*
     * RXDELAY is counted in units of *half* a system clock cycle (see
     * QMI_M1_TIMING_RXDELAY), but what it has to cover - pad output delay, the
     * PSRAM's own access time, pad input delay - is a fixed physical time.
     * Deriving it from the divisor therefore moves the sampling instant
     * earlier as the system clock rises, and it can walk out of the
     * data-valid window without the divisor changing at all.
     *
     * Measured on Waveshare RP2350-PiZero (Z2), 8 MB APS6404:
     *
     *   504 MHz, psram_freq=84   divisor 6  rxdelay 6  5.95 ns   works
     *   378 MHz, psram_freq=133  divisor 3  rxdelay 4  5.29 ns   works
     *   504 MHz, psram_freq=133  divisor 4  rxdelay 5  4.96 ns   hangs at boot
     *
     * The last two run the PSRAM at the same 126 MHz and differ only in when
     * the data is sampled, so the requirement is a time, and it lies between
     * 4.96 and 5.29 ns.  Floor the delay at 5.25 ns and leave the divisor
     * heuristic alone wherever it already clears that.  This raises only the
     * failing case: both working configurations above compute what they did
     * before, so a board that boots today still boots.
     */
    const int rx_floor = (2 * 5250000 + clock_period_fs - 1) / clock_period_fs;
    if (rxdelay < rx_floor) rxdelay = rx_floor;
    if (rxdelay > 7) rxdelay = 7;   /* QMI_M1_TIMING_RXDELAY is 3 bits */

    // Calculate timing parameters
    int max_select_val = (125 * 1000000) / clock_period_fs;
    if (max_select_val > 63) max_select_val = 63;   /* MAX_SELECT is 6 bits */
    int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;
    if (min_deselect < 0) min_deselect = 0;
    if (min_deselect > 31) min_deselect = 31;       /* MIN_DESELECT is 5 bits */

    // Configure M1 (PSRAM) timing
    qmi_hw->m[1].timing =
        1 << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        max_select_val << QMI_M1_TIMING_MAX_SELECT_LSB |
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
        divisor << QMI_M1_TIMING_CLKDIV_LSB;
}

/**
 * Re-time the PSRAM for a system clock of cpu_mhz that has not been applied
 * yet.
 *
 * psram_init_with_freq() derives the divisor from the *live* clk_sys, so it
 * can only ever be called once the PLL has already moved.  Every caller that
 * raises the system clock therefore left the PSRAM running with the divisor
 * computed for the old, lower clock - on the HDMI boost path that is 150 MHz
 * of divisor against a 504 MHz clock, and the part is driven far out of spec
 * until the re-init lands.  The QMI documents the cure directly: raise CLKDIV
 * first, force a dummy access so it takes effect, and only then move the
 * clock.  This is the PSRAM twin of set_flash_timings().
 *
 * Call it *before* set_sys_clock_khz() when raising the clock; when lowering,
 * the old divisor is merely conservative, so re-time afterwards instead.
 */
void __no_inline_not_in_flash_func(psram_set_timings)(int cpu_mhz, int psram_mhz) {
    psram_write_timing(cpu_mhz * 1000000, psram_mhz);

    /* "If software is increasing CLKDIV in anticipation of an increase in the
     * system clock frequency, a dummy access to either memory window (and
     * appropriate processor barriers/fences) must be inserted after the
     * Mx_TIMING write to ensure the SCK divisor change is in effect _before_
     * the system clock is changed." - RP2350 datasheet, QMI_M1_TIMING_CLKDIV. */
    __compiler_memory_barrier();
    (void)*(volatile uint32_t *)PSRAM_BASE_ADDR;
    __dmb();
}

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

    psram_write_timing(clock_hz, freq_mhz);

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
        0,                                      // Start
        4 * 1024,                               // 4 KiB
        1 * 1024 * 1024,                        // 1 MiB
        4 * 1024 * 1024,                        // 4 MiB
        PSRAM_SIZE_BYTES - sizeof(uint32_t),    // Last word of PSRAM
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
