/*
 * MMC5983MA on I2C1, run entirely by DMA (M-7), on the 20 ms SET/RESET
 * pair cycle (M-20 .. M-22).
 *
 * Adapted from DFC Rev 2.16 src/mag.h (B-26). DFC ran one transaction per
 * 1 ms tick; here a cycle is 20 ticks with two transactions:
 *   tick 0  (MAG_S_TICK): read R of the previous cycle, SET, trigger S
 *   tick 10 (MAG_R_TICK): read S of this cycle, RESET, trigger R
 * Each transaction is
 *   START W(0x00) RESTART R x9 RESTART W(0x09, SET|RESET) RESTART W(0x09, TM_M) STOP
 * The I2C STOP_DET interrupt marks completion (M-12). When tick 0's
 * transaction completes, the pair for the previous cycle is complete and is
 * published through a double buffer (M-11).
 */
#ifndef MAG_H
#define MAG_H

#include <stdbool.h>
#include <stdint.h>

#include "mmc5983ma.h"

#include "config.h"

/* Status of one half (S or R) of a pair (M-9, amended). */
typedef enum {
    MAG_HALF_OK = 0,
    MAG_HALF_NO_PRIOR = 1,  /* nothing was triggered for it (start-up, abort) */
    MAG_HALF_NOT_READY = 2, /* read with Meas_M_Done clear                    */
    MAG_HALF_MISSING = 3,   /* its transaction was aborted or timed out       */
} mag_half_t;

/* One published pair. raw arrays hold registers 0x00..0x08 as read by DMA:
 * the 7 field bytes, Tout (not updated) and Status. */
typedef struct {
    uint8_t s_raw[MMC_READ_BYTES];
    uint8_t r_raw[MMC_READ_BYTES];
    uint8_t s_half; /* mag_half_t */
    uint8_t r_half; /* mag_half_t; MAG_HALF_OK and unused in single mode */
    bool single;    /* section 8a H2: single-measurement mode            */
    uint64_t cycle; /* cycle in which S was triggered (section 8a L1)    */
    uint64_t t_us;  /* S trigger time (M-10), or the nominal tick time
                     * of the S tick if S had no trigger                 */
} mag_pair_t;

/* Blocking start-up (before the main loop): bus clear, software reset,
 * product-ID and OTP check (M-2), configuration (M-3), and one
 * RESET -> SET conditioning (M-4 start-up). On failure the fault flag is
 * set; the tick then re-probes (M-2a). Returns false on fault. */
bool mag_init(void);

/* True while the sensor is not confirmed and configured. */
bool mag_fault(void);

/* Copy the latest published pair in a short critical section (M-13).
 * Core 0 main loop only. Returns false if no pair has been published. */
bool mag_read_pair(mag_pair_t *p);

/* Unsigned 18-bit count of one axis (0 = X, 1 = Y, 2 = Z), p. 13. */
static inline uint32_t mag_raw18(const uint8_t raw[MMC_SAMPLE_BYTES], unsigned axis) {
    unsigned shift = 6u - 2u * axis;
    return ((uint32_t)raw[2u * axis] << 10) | ((uint32_t)raw[2u * axis + 1u] << 2) |
           (((uint32_t)raw[6] >> shift) & 3u);
}

/* Section 8a H2 / M10 run-time test modes, applied at the next cycle
 * boundary: g_mag_single = 1 selects single-measurement mode (one SET,
 * then one plain measurement per cycle at tick 0, V-18); g_mag_r_tick
 * moves the R trigger (5 for the V-19 reference; default MAG_R_TICK).
 * Set by diagnostic commands (status.c) or from the debugger. */
extern volatile uint32_t g_mag_single;
extern volatile uint32_t g_mag_r_tick;

/* Called from the 1 ms tick ISR only (core 0). */
void mag_tick_isr(uint64_t tick_index);

#endif /* MAG_H */
