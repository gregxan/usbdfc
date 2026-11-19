#include "status.h"

volatile uint32_t g_last_reason;
volatile uint32_t g_last_flags;

#if APP_USB_STATUS

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tusb.h"

#include "diag.h"
#include "mag.h"
#include "prof.h"
#include "usbh.h"

extern volatile uint32_t g_watchdog_resets;
extern volatile uint32_t g_chip_version;
extern volatile uint32_t g_boot_id;
#if APP_STALL_HOOK
extern volatile uint32_t g_diag_stall_core;
#endif

#define STATUS_PERIOD_US 1000000u
#define N_LINES 5u
#define LINE_MAX 240u

static uint32_t s_total[DIAG_COUNT]; /* core 0 counters, totals since boot */
static uint64_t s_next_us;
static uint32_t s_seq;
static char s_lines[N_LINES][LINE_MAX];
static int s_len[N_LINES];
static unsigned s_next_line = N_LINES; /* N_LINES = nothing pending */

static char s_cmd[48];
static unsigned s_cmd_len;

static uint32_t ns(diag_timing_t t) {
    return (uint32_t)(((uint64_t)g_diag_max_cycles[t] * 1000u) / (SYS_CLOCK_KHZ / 1000u));
}

static uint32_t us_of(uint32_t cycles) {
    return diag_cycles_to_us(cycles);
}

static void build_lines(void) {
    uint32_t d[DIAG_COUNT];
    diag_take_all(d); /* D-2 read-and-clear, then accumulate */
    for (unsigned i = 0; i < DIAG_COUNT; i++) {
        s_total[i] += d[i];
    }
    const uint32_t *t = s_total;
    s_seq++;
    s_len[0] = snprintf(s_lines[0], LINE_MAX,
                        "S %" PRIu32 " MAG i2c=%" PRIu32 " missed=%" PRIu32 " timeout=%" PRIu32 " pid=%" PRIu32
                        " bclr=%" PRIu32 " recov=%" PRIu32 " notready=%" PRIu32 " sat=%" PRIu32 " rtfault=%" PRIu32
                        " pairinc=%" PRIu32 " fault=%d\r\n",
                        s_seq, t[DIAG_MAG_I2C_ERROR], t[DIAG_MAG_MISSED_TICK], t[DIAG_MAG_TIMEOUT],
                        t[DIAG_MAG_PRODUCT_ID_FAULT], t[DIAG_MAG_BUS_CLEAR], t[DIAG_MAG_RECOVERED],
                        t[DIAG_MAG_NOT_READY], t[DIAG_MAG_SATURATED], t[DIAG_MAG_RUNTIME_FAULT],
                        t[DIAG_MAG_PAIR_INCOMPLETE], mag_fault() ? 1 : 0);
    s_len[1] = snprintf(s_lines[1], LINE_MAX,
                        "S %" PRIu32 " SCHED tickovr=%" PRIu32 " epochovr=%" PRIu32 " epoch_us=%" PRIu32 "/%" PRIu32
                        "/%" PRIu32 " record_us_max=%" PRIu32 " epoch_late_max_us=%" PRIu32 " isr_i2c_ns=%" PRIu32
                        " isr_tick_ns=%" PRIu32 " crit_ns=%" PRIu32 " api_mag_ns=%" PRIu32 " status_us_max=%" PRIu32
                        "\r\n",
                        s_seq, t[DIAG_TICK_OVERRUN], t[DIAG_EPOCH_OVERRUN], us_of(g_prof[PROF_EPOCH].last),
                        us_of(g_prof[PROF_EPOCH].max), us_of(prof_avg(PROF_EPOCH)), us_of(g_prof[PROF_RECORD].max),
                        g_diag_epoch_late_max_us, ns(DIAG_T_ISR_I2C), ns(DIAG_T_ISR_TICK), ns(DIAG_T_CRITICAL),
                        ns(DIAG_T_API_MAG), us_of(g_diag_max_cycles[DIAG_T_STATUS]));
    s_len[2] = snprintf(s_lines[2], LINE_MAX,
                        "S %" PRIu32 " USBH att=%" PRIu32 " det=%" PRIu32 " enumfail=%" PRIu32 " enumto=%" PRIu32
                        " unsup=%" PRIu32 " xerr=%" PRIu32 " stall_out=%" PRIu32 " stall_in=%" PRIu32
                        " hclr=%" PRIu32 " hclr_def=%" PRIu32 " sofovr=%" PRIu32 " vbus_oc=%" PRIu32 "\r\n",
                        s_seq, g_usbh.attaches, g_usbh.detaches, g_usbh.enum_failures, g_usbh.enum_timeouts,
                        g_usbh.unsupported, g_usbh.xfer_errors, g_usbh.stall_out, g_usbh.stall_in,
                        g_usbh.halt_clears, g_usbh.halt_clears_deferred, g_usbh.sof_overruns,
                        t[DIAG_VBUS_OVERCURRENT]);
    s_len[3] = snprintf(s_lines[3], LINE_MAX,
                        "S %" PRIu32 " REC sent=%" PRIu32 " late=%" PRIu32 " aborted=%" PRIu32 " over=%" PRIu32
                        " missing=%" PRIu32 " in_bytes=%" PRIu32 " sof=%" PRIu32 " hb=%" PRIu32
                        " sof_lat_max_us=%" PRIu32 " frame_us_max=%" PRIu32 " queue_us_max=%" PRIu32
                        " margin_us_min=%" PRIu32 "\r\n",
                        s_seq, g_usbh.sent, g_usbh.late, g_usbh.aborted, g_usbh.overwritten, g_usbh.missing,
                        g_usbh.in_bytes_discarded, g_usbh.sof_count, g_usbh.heartbeat, g_usbh.sof_latency_max_us,
                        us_of(g_usbh.frame_cycles_max), g_usbh.pub_to_queued_max_us, g_usbh.pub_margin_min_us);
    s_len[4] = snprintf(s_lines[4], LINE_MAX,
                        "S %" PRIu32 " SYS boot_id=%08" PRIx32 " wdres=%" PRIu32 " chip=%" PRIu32 " single=%" PRIu32
                        " rtick=%" PRIu32 " last_reason=%" PRIu32 " last_flags=%02" PRIx32 "\r\n",
                        s_seq, g_boot_id, g_watchdog_resets, g_chip_version, g_mag_single, g_mag_r_tick,
                        g_last_reason, g_last_flags);
    for (unsigned i = 0; i < N_LINES; i++) {
        if (s_len[i] < 0) {
            s_len[i] = 0;
        } else if (s_len[i] >= (int)LINE_MAX) {
            s_len[i] = (int)LINE_MAX - 1;
        }
    }
    s_next_line = 0;
}

static void reply(const char *msg) {
    size_t n = strlen(msg);
    if (tud_cdc_write_available() >= n) {
        (void)tud_cdc_write(msg, (uint32_t)n);
        (void)tud_cdc_write_flush();
    }
}

static void handle_command(const char *c) {
    if (strcmp(c, "mode pair") == 0) {
        g_mag_single = 0u;
        reply("OK mode pair\r\n");
    } else if (strcmp(c, "mode single") == 0) {
        g_mag_single = 1u;
        reply("OK mode single\r\n");
    } else if (strncmp(c, "rtick ", 6) == 0) {
        long v = strtol(c + 6, NULL, 10);
        if (v >= 1 && v < (long)MAG_CYCLE_TICKS) {
            g_mag_r_tick = (uint32_t)v;
            reply("OK rtick\r\n");
        } else {
            reply("ERR rtick\r\n");
        }
#if APP_STALL_HOOK
    } else if (strncmp(c, "stall ", 6) == 0) {
        long v = strtol(c + 6, NULL, 10);
        if (v >= 0 && v <= 2) {
            reply("OK stall\r\n");
            g_diag_stall_core = (uint32_t)v;
        } else {
            reply("ERR stall\r\n");
        }
#endif
    } else if (strcmp(c, "status") == 0) {
        s_next_us = 0u;
    } else {
        reply("ERR unknown\r\n");
    }
}

static void poll_commands(void) {
    while (tud_cdc_available() != 0u) {
        char ch;
        if (tud_cdc_read(&ch, 1) != 1u) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            if (s_cmd_len != 0u) {
                s_cmd[s_cmd_len] = '\0';
                handle_command(s_cmd);
                s_cmd_len = 0u;
            }
        } else if (s_cmd_len + 1u < sizeof s_cmd) {
            s_cmd[s_cmd_len++] = ch;
        }
    }
}

void status_poll(uint64_t now_us) {
    if (!tud_cdc_connected()) {
        s_next_line = N_LINES; /* drop a half-written report */
        return;
    }
    poll_commands();
    if (s_next_line == N_LINES && now_us >= s_next_us) {
        s_next_us = now_us + STATUS_PERIOD_US;
        build_lines();
    }
    /* At most one line per epoch, and only if it fits whole (NU-2). */
    if (s_next_line < N_LINES) {
        uint32_t n = (uint32_t)s_len[s_next_line];
        if (tud_cdc_write_available() >= n) {
            (void)tud_cdc_write(s_lines[s_next_line], n);
            (void)tud_cdc_write_flush();
            s_next_line++;
        }
    }
}

#endif /* APP_USB_STATUS */
