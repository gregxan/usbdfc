/*
 * Core 0 counters and timing instrumentation (section 7.2).
 * Adapted from DFC Rev 2.16 src/diag.h (B-26): UART counters, high-water
 * marks and offset calibrations removed; pair, scheduling and hardware
 * counters added. Core 1's counters live in usbh.h (IC-3).
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/structs/m33.h"
#include "hardware/sync.h"

#include "config.h"

/* Section 7.2 core 0 counters. Each has exactly one incrementing context,
 * so a plain read-modify-write is safe; the main loop clears with
 * diag_take() (D-2). */
typedef enum {
    DIAG_MAG_I2C_ERROR,
    DIAG_MAG_MISSED_TICK, /* a scheduled transaction could not start (M-16) */
    DIAG_MAG_TIMEOUT,
    DIAG_MAG_PRODUCT_ID_FAULT,
    DIAG_MAG_BUS_CLEAR,
    DIAG_MAG_RECOVERED,
    DIAG_MAG_NOT_READY,    /* measurements read with Meas_M_Done clear (M-9) */
    DIAG_MAG_SATURATED,    /* raw S or R measurements near full scale        */
    DIAG_MAG_RUNTIME_FAULT,/* M-2b                                           */
    DIAG_MAG_PAIR_INCOMPLETE, /* records missing one half of their pair      */
    DIAG_TICK_OVERRUN,     /* M-6a                                           */
    DIAG_EPOCH_OVERRUN,    /* EX-8                                           */
    DIAG_VBUS_OVERCURRENT, /* HW-12: flag assertions (edges)                 */
    DIAG_COUNT
} diag_ctr_t;

/* Timing slots, maximum cycles (V-4). */
typedef enum {
    DIAG_T_ISR_I2C,
    DIAG_T_ISR_TICK,
    DIAG_T_CRITICAL, /* longest critical section in the main-loop API (EX-3) */
    DIAG_T_API_MAG,  /* mag_read_pair() (EX-7)                               */
    DIAG_T_STATUS,   /* diagnostic-build status reporter (D-4, NU-3)         */
    DIAG_T_COUNT
} diag_timing_t;

extern volatile uint32_t g_diag_ctr[DIAG_COUNT];
extern volatile uint32_t g_diag_max_cycles[DIAG_T_COUNT];

/* EX-10: worst epoch start latency after its scheduled time, in us. */
extern volatile uint32_t g_diag_epoch_late_max_us;

void diag_init(void);

/* Enable the DWT cycle counter on the calling core (each core has its own). */
void diag_cycles_enable(void);

static inline void diag_inc(diag_ctr_t c) {
    g_diag_ctr[c] = g_diag_ctr[c] + 1u;
}

/* D-2: atomic read-and-clear of one counter. Main loop only. */
uint32_t diag_take(diag_ctr_t c);

/* Read-and-clear every counter in one critical section. Main loop only. */
void diag_take_all(uint32_t out[DIAG_COUNT]);

/* ---- cycle counter --------------------------------------------------- */

static inline uint32_t diag_cycles(void) {
#if DIAG_TIMING
    return m33_hw->dwt_cyccnt;
#else
    return 0u;
#endif
}

static inline void diag_max(diag_timing_t t, uint32_t cycles) {
#if DIAG_TIMING
    if (cycles > g_diag_max_cycles[t]) {
        g_diag_max_cycles[t] = cycles;
    }
#else
    (void)t;
    (void)cycles;
#endif
}

/* ---- critical sections (EX-3) ---------------------------------------- */

typedef struct {
    uint32_t irq;
    uint32_t t0;
} cs_t;

static inline cs_t cs_enter(void) {
    cs_t c;
    c.irq = save_and_disable_interrupts();
    c.t0 = diag_cycles();
    return c;
}

/* Only used from the core 0 main loop, so DIAG_T_CRITICAL has one writer. */
static inline void cs_exit(cs_t c) {
    uint32_t d = diag_cycles() - c.t0;
    restore_interrupts(c.irq);
    diag_max(DIAG_T_CRITICAL, d);
}

/* Convert cycles to microseconds (rounded up) at the current clk_sys. */
uint32_t diag_cycles_to_us(uint32_t cycles);

#endif /* DIAG_H */
