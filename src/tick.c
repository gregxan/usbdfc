#include "tick.h"

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include "config.h"
#include "diag.h"
#include "mag.h"

static uint s_alarm;
static uint32_t s_alarm_bit;
static uint64_t s_t0;          /* time of tick 0                     */
static uint64_t s_next_target; /* time of the tick now being awaited */
static uint64_t s_next_index;
static volatile uint32_t s_count_lo; /* tick_count(), split for 32-bit access */
static volatile uint32_t s_count_hi;

uint64_t __not_in_flash_func(tick_count)(void) {
    /* Single writer (tick ISR); re-read hi to detect a carry in between. */
    uint32_t hi;
    uint32_t lo;
    do {
        hi = s_count_hi;
        lo = s_count_lo;
    } while (hi != s_count_hi);
    return ((uint64_t)hi << 32) | lo;
}

uint64_t __not_in_flash_func(tick_time_us)(uint64_t idx) {
    return s_t0 + idx * TICK_PERIOD_US;
}

static void __not_in_flash_func(tick_isr)(void) {
    uint32_t t0 = diag_cycles();
    timer_hw->intr = s_alarm_bit;

    uint64_t idx = s_next_index;

    /* M-6a: schedule from the previous target, never from "now". */
    s_next_target += TICK_PERIOD_US;
    s_next_index = idx + 1u;
    timer_hw->alarm[s_alarm] = (uint32_t)s_next_target;

    /* Alarms match on equality of the low 32 bits. If the target is
     * already in the past (only if this ISR was held off for a whole
     * period, e.g. by a debugger), skip forward on the same grid. */
    while ((int32_t)(timer_hw->timerawl - (uint32_t)s_next_target) >= 0) {
        timer_hw->armed = s_alarm_bit;
        timer_hw->intr = s_alarm_bit;
        s_next_target += TICK_PERIOD_US;
        s_next_index++;
        diag_inc(DIAG_TICK_OVERRUN);
        timer_hw->alarm[s_alarm] = (uint32_t)s_next_target;
    }

    s_count_lo = (uint32_t)s_next_index;
    s_count_hi = (uint32_t)(s_next_index >> 32);

    mag_tick_isr(idx); /* M-6, M-16, M-20 */

    diag_max(DIAG_T_ISR_TICK, diag_cycles() - t0);
}

void tick_init(void) {
    s_alarm = (uint)timer_hardware_alarm_claim_unused(timer_hw, true);
    s_alarm_bit = 1u << s_alarm;
    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, s_alarm);
    irq_set_exclusive_handler(irq, tick_isr);
    irq_set_priority(irq, IRQ_PRIO_TICK);

    s_t0 = time_us_64() + TICK_PERIOD_US;
    s_next_target = s_t0;
    s_next_index = 0u;
    __dmb(); /* s_t0 is read by core 1, which is launched after this */

    hw_set_bits(&timer_hw->inte, s_alarm_bit);
    timer_hw->alarm[s_alarm] = (uint32_t)s_next_target;
    irq_set_enabled(irq, true);
}
