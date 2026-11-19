#include "diag.h"

#include "hardware/clocks.h"

volatile uint32_t g_diag_ctr[DIAG_COUNT];
volatile uint32_t g_diag_max_cycles[DIAG_T_COUNT];
volatile uint32_t g_diag_epoch_late_max_us;

static uint32_t s_cycles_per_us;

void diag_cycles_enable(void) {
#if DIAG_TIMING
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0u;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
#endif
}

void diag_init(void) {
    diag_cycles_enable();
    s_cycles_per_us = clock_get_hz(clk_sys) / 1000000u;
    if (s_cycles_per_us == 0u) {
        s_cycles_per_us = 1u;
    }
}

uint32_t diag_take(diag_ctr_t c) {
    cs_t cs = cs_enter();
    uint32_t v = g_diag_ctr[c];
    g_diag_ctr[c] = 0u;
    cs_exit(cs);
    return v;
}

void diag_take_all(uint32_t out[DIAG_COUNT]) {
    /* DIAG_COUNT load/store pairs: well under 1 us at 120 MHz (EX-3). */
    cs_t cs = cs_enter();
    for (unsigned i = 0; i < DIAG_COUNT; i++) {
        out[i] = g_diag_ctr[i];
        g_diag_ctr[i] = 0u;
    }
    cs_exit(cs);
}

uint32_t diag_cycles_to_us(uint32_t cycles) {
    return (cycles + s_cycles_per_us - 1u) / s_cycles_per_us;
}
