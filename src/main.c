/*
 * Initialisation and the core 0 main loop (HW-8, C1-8, EX-10).
 *
 * Core 0: the magnetometer (tick + I2C interrupts), and 1 ms epochs at
 * tick + EPOCH_OFFSET_US. The epoch at tick 0 of each cycle builds the
 * record for the previous cycle and publishes it to core 1 (IC-1). Every
 * epoch also polls the VBUS overcurrent flag (HW-12) and feeds the
 * watchdog while both cores make progress (C1-7).
 *
 * Core 1: the PIO-USB host (usbh.c).
 */
#include <stdbool.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "config.h"
#include "diag.h"
#include "mag.h"
#include "mailbox.h"
#include "prof.h"
#include "record.h"
#include "tick.h"
#include "usbh.h"

/* Section 7.2 persistent and static values (read by the debugger or the
 * diagnostic status reporter). */
volatile uint32_t g_watchdog_resets;
volatile uint32_t g_chip_version;
volatile uint32_t g_boot_id;
static bool s_chip_unsupported;

#if APP_STALL_HOOK
volatile uint32_t g_diag_stall_core; /* V-14: 1 = stall core 0, 2 = core 1 */
#endif

/* C1-7 / section 8a L3: scratch[0] = magic, scratch[1] = count.
 * scratch[4..7] belong to the SDK's reboot functions. */
#define WD_MAGIC 0x55534443u /* "USDC" */

static void watchdog_boot_check(void) {
    if (watchdog_hw->scratch[0] != WD_MAGIC) {
        watchdog_hw->scratch[0] = WD_MAGIC;
        watchdog_hw->scratch[1] = 0u;
    }
    if (watchdog_enable_caused_reboot()) {
        watchdog_hw->scratch[1] = watchdog_hw->scratch[1] + 1u;
    }
    g_watchdog_resets = watchdog_hw->scratch[1];
}

#if APP_TEST_PINS
static void test_pins_init(void) {
    gpio_init(TEST_PIN_EPOCH);
    gpio_set_dir(TEST_PIN_EPOCH, GPIO_OUT);
    gpio_put(TEST_PIN_EPOCH, false);
    gpio_init(TEST_PIN_RTRIG);
    gpio_set_dir(TEST_PIN_RTRIG, GPIO_OUT);
    gpio_put(TEST_PIN_RTRIG, false);
}
#define EPOCH_PIN_HIGH() (sio_hw->gpio_set = 1u << TEST_PIN_EPOCH)
#define EPOCH_PIN_LOW() (sio_hw->gpio_clr = 1u << TEST_PIN_EPOCH)
#else
static void test_pins_init(void) {}
#define EPOCH_PIN_HIGH() ((void)0)
#define EPOCH_PIN_LOW() ((void)0)
#endif

/* HW-12: VBUS off until core 1's host is initialised (L4). */
static void vbus_init(void) {
    gpio_init(VBUS_EN_PIN);
    gpio_put(VBUS_EN_PIN, false);
    gpio_set_dir(VBUS_EN_PIN, GPIO_OUT);
    gpio_init(VBUS_OC_PIN);
    gpio_set_dir(VBUS_OC_PIN, GPIO_IN);
    gpio_pull_up(VBUS_OC_PIN);
}

/* ------------------------------------------------------------ epoch ---- */

static bool s_oc_last;
static bool s_wd_enabled;
static uint32_t s_wd_sof_last;
static uint64_t s_wd_sof_seen_us;
static uint32_t s_wd_hb_last;
static uint64_t s_wd_hb_seen_us;

/* Record for cycle `cycle`, published in the epoch of tick 0 of the next
 * cycle (C1-5). */
static void __not_in_flash_func(build_and_publish)(uint64_t cycle) {
    PROF_BEGIN(PROF_RECORD);
    mag_pair_t p;
    bool have = mag_read_pair(&p) && p.cycle == cycle;
    mb_rec_t r;
    rec_reason_t reason = rec_build(r.payload, have ? &p : NULL, cycle, mag_fault(), s_chip_unsupported,
                                    g_boot_id, time_us_64());
    if (reason == REC_REASON_PAIR_INCOMPLETE) {
        diag_inc(DIAG_MAG_PAIR_INCOMPLETE);
    }
    r.cycle = cycle;
    r.t_pub_us = time_us_64();
    mb_publish(&r);
    PROF_END(PROF_RECORD);
}

/* C1-7: feed only while core 0 runs (this epoch), core 1's SOF counter
 * advances, and core 1's loop heartbeat advanced within the last 1 s. */
static void __not_in_flash_func(watchdog_poll)(uint64_t now) {
    uint32_t sof = g_usbh.sof_count;
    uint32_t hb = g_usbh.heartbeat;
    if (sof != s_wd_sof_last) {
        s_wd_sof_last = sof;
        s_wd_sof_seen_us = now;
    }
    if (hb != s_wd_hb_last) {
        s_wd_hb_last = hb;
        s_wd_hb_seen_us = now;
    }
    bool sof_ok = (now - s_wd_sof_seen_us) < 10u * TICK_PERIOD_US;
    bool hb_ok = (now - s_wd_hb_seen_us) < WATCHDOG_HEARTBEAT_WINDOW_US;
    if (s_wd_enabled && sof_ok && hb_ok) {
        watchdog_update();
    }
}

static void __not_in_flash_func(epoch_work)(uint64_t e, uint64_t now) {
    if (e % MAG_CYCLE_TICKS == MAG_S_TICK && e >= MAG_CYCLE_TICKS) {
        build_and_publish(e / MAG_CYCLE_TICKS - 1u);
    }

    /* HW-12: count each assertion (active low); no action (D-5). */
    bool oc = !gpio_get(VBUS_OC_PIN);
    if (oc && !s_oc_last) {
        diag_inc(DIAG_VBUS_OVERCURRENT);
    }
    s_oc_last = oc;

    watchdog_poll(now);

#if APP_STALL_HOOK
    while (g_diag_stall_core == 1u) {
        tight_loop_contents(); /* V-14: core 0 stalled */
    }
#endif
}

/* -------------------------------------------------------------- main --- */

int main(void) {
    /* C1-8 step 1. R-3: 120 MHz; clk_peri follows clk_sys. */
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    vbus_init();
    test_pins_init();
    diag_init();
    watchdog_boot_check();
    g_boot_id = get_rand_32(); /* REC-4 */
    g_chip_version = rp2350_chip_version();
    s_chip_unsupported = g_chip_version < 4u; /* HW-10: A4 and later */
    tick_init();

    /* Step 2: core 1 initialises the host, claims its DMA channel, starts
     * SOF, then signals. VBUS on only after that (L4). */
    multicore_launch_core1(usbh_core1_main);
    uint32_t msg = 0u;
    if (multicore_fifo_pop_timeout_us(1000000u, &msg) && msg == USBH_FIFO_INIT_DONE) {
        gpio_put(VBUS_EN_PIN, true);
    }

    /* Step 3: magnetometer (blocking start-up, M-2). DMA claims now. */
    (void)mag_init();

    /* Step 4: watchdog (paused while a debugger halts a core), main loop. */
    uint64_t now0 = time_us_64();
    s_wd_sof_seen_us = now0;
    s_wd_hb_seen_us = now0;
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
    s_wd_enabled = true;

    uint64_t e = tick_count(); /* next epoch: the next tick */
    for (;;) {
        uint64_t target = tick_time_us(e) + EPOCH_OFFSET_US;
        /* EX-2: waiting for the epoch with interrupts enabled. */
        while (time_us_64() < target) {
            tight_loop_contents();
        }
        uint64_t now = time_us_64();
        uint64_t late = now - target;
        if (late > g_diag_epoch_late_max_us) {
            g_diag_epoch_late_max_us = (uint32_t)(late > UINT32_MAX ? UINT32_MAX : late);
        }
        if (late >= TICK_PERIOD_US) {
            /* EX-8: the previous epoch ran past this one; skip ahead. */
            diag_inc(DIAG_EPOCH_OVERRUN);
            e += late / TICK_PERIOD_US;
        }

        EPOCH_PIN_HIGH();
        PROF_BEGIN(PROF_EPOCH);
        epoch_work(e, now);
        PROF_END(PROF_EPOCH);
        EPOCH_PIN_LOW();
        e++;
    }
}
