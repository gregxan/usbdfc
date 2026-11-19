/*
 * 1 kHz hardware-timer tick (M-6, M-6a) on TIMER0. The schedule grid for
 * the magnetometer cycle (M-20), the core 0 epochs (EX-10) and the core 1
 * SOF alarm (USBH-S4). Adapted from DFC Rev 2.16 (B-26): the UART
 * bookkeeping (U-8, U-15a) is removed and the index is 64-bit, so cycle
 * numbers never wrap within the life of a unit.
 */
#ifndef TICK_H
#define TICK_H

#include <stdint.h>

/* Start the tick. */
void tick_init(void);

/* Scheduled (target) time of tick idx, in us since boot. Fixed grid:
 * tick_time_us(idx) = t0 + idx * TICK_PERIOD_US. Valid on both cores once
 * tick_init() has returned (t0 is written once, before core 1 starts). */
uint64_t tick_time_us(uint64_t idx);

/* Number of grid ticks whose handler has run (or been skipped, M-6a). */
uint64_t tick_count(void);

#endif /* TICK_H */
