/*
 * Diagnostic status reporter and command input on the native-USB CDC port
 * (NU-2, section 7.3). Diagnostic builds only. Core 0 epoch only; never
 * blocks: a line is written only when it fits whole in the CDC buffer.
 *
 * Output, once per second, one line each (all values totals since boot):
 *   S <n> MAG  ...    magnetometer counters and state
 *   S <n> SCHED ...   scheduling counters and core 0 timing maxima
 *   S <n> USBH ...    core 1 device and transfer counters
 *   S <n> REC ...     core 1 record counters and timing
 *   S <n> SYS ...     boot ID, watchdog resets, chip, modes, last record
 * Commands, one per line:
 *   mode pair | mode single   section 8a H2 (V-18)
 *   rtick <1..19>             R-trigger tick (V-19 reference: 5)
 *   stall <0|1|2>             V-14 hook (builds with APP_STALL_HOOK)
 *   status                    report now
 */
#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>

#include "config.h"

/* Latest record built by core 0 (for the SYS line). */
extern volatile uint32_t g_last_reason;
extern volatile uint32_t g_last_flags;

#if APP_USB_STATUS
void status_poll(uint64_t now_us);
#else
static inline void status_poll(uint64_t now_us) {
    (void)now_us;
}
#endif

#endif /* STATUS_H */
