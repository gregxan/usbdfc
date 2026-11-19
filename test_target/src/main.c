/*
 * usbdfc test target (VE-1, VE-2): a Raspberry Pi Pico 2 that acts as the
 * downstream CDC-ACM device for the XIAO's PIO-USB host.
 *
 * It timestamps, with the DWT cycle counter (clk_sys, 150 MHz):
 *   - each SOF and each completed bulk OUT packet, in the USB interrupt
 *     (through -Wl,--wrap=dcd_event_handler);
 *   - each edge on PIN_MARKER (the XIAO's R-trigger marker, V-3).
 * and forwards text lines to the PC on UART0 (921600 8N1):
 *   H <cyc_hz> <mode> <variant>    header (at boot and every 5 s)
 *   S <cyc>                        SOF
 *   O <cyc> <hex bytes>            one received bulk OUT packet
 *   M <cyc> <level>                marker edge
 *   E <cyc> <text>                 event (bus reset, mode change, ...)
 * <cyc> is 8 hex digits and wraps every 2^32 cycles (28.6 s); SOF lines
 * every 1 ms let the PC unwrap it.
 *
 * Commands from the PC, one per line (tools/target_ctl.py):
 *   mode normal | nak <ms> | neverack | stall_out_once | stall_out_rep |
 *        stall_in | in_data | ep32 | vendor | nolinereq |
 *        stall_linecoding | enumhang
 *   coil 50 | coil 60 | coil off
 *   hello
 */
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

#include "target.h"

#define EP_OUT 0x02u
#define EP_IN 0x82u

static inline uint32_t cyc(void) {
    return m33_hw->dwt_cyccnt;
}

/* ============================================================ modes ===== */

typedef enum {
    M_NORMAL,
    M_NAK,
    M_NEVERACK,
    M_STALL_OUT_ONCE,
    M_STALL_OUT_REP,
    M_IN_DATA,
    M_EP32,
    M_VENDOR,
    M_NOLINEREQ,
    M_STALL_LC,
    M_ENUMHANG,
} target_mode_t;

static const char *const s_mode_name[] = {
    "normal", "nak", "neverack", "stall_out_once", "stall_out_rep", "in_data",
    "ep32", "vendor", "nolinereq", "stall_linecoding", "enumhang",
};

static volatile target_mode_t s_mode = M_NORMAL;
static volatile bool s_hang;  /* M_ENUMHANG: stop running the device task */
static uint64_t s_nak_end_us;

/* ================================================== event rings ========= */

typedef struct {
    uint8_t type; /* 'S', 'M', 'R' (bus reset), 'L' (line coding stalled) */
    uint8_t arg;
    uint32_t cyc;
} ev_t;

#define EV_N 1024u /* power of two */
static ev_t s_ev[EV_N];
static volatile uint32_t s_ev_head;
static volatile uint32_t s_ev_tail;
static volatile uint32_t s_ev_drop;

static void ev_push(uint8_t type, uint8_t arg, uint32_t c) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t h = s_ev_head;
    if (h - s_ev_tail < EV_N) {
        s_ev[h & (EV_N - 1u)] = (ev_t){type, arg, c};
        s_ev_head = h + 1u;
    } else {
        s_ev_drop++;
    }
    restore_interrupts(irq);
}

typedef struct {
    uint32_t cyc;
    uint16_t len;
} pkt_t;

#define PKT_N 64u
static pkt_t s_pkt[PKT_N];
static volatile uint32_t s_pkt_head; /* USB ISR */
static volatile uint32_t s_pkt_tail; /* main loop */

/* ================================================= TinyUSB hooks ======== */

extern void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);

void __not_in_flash_func(__wrap_dcd_event_handler)(dcd_event_t const *event, bool in_isr) {
    uint32_t c = cyc();
    switch (event->event_id) {
    case DCD_EVENT_SOF:
        ev_push('S', 0, c);
        break;
    case DCD_EVENT_BUS_RESET:
        ev_push('R', 0, c);
        if (s_mode == M_ENUMHANG) {
            s_hang = true;
        }
        break;
    case DCD_EVENT_XFER_COMPLETE:
        if (event->xfer_complete.ep_addr == EP_OUT && event->xfer_complete.result == XFER_RESULT_SUCCESS) {
            uint32_t h = s_pkt_head;
            if (h - s_pkt_tail < PKT_N) {
                s_pkt[h % PKT_N] = (pkt_t){c, (uint16_t)event->xfer_complete.len};
                s_pkt_head = h + 1u;
            }
        }
        break;
    default:
        break;
    }
    __real_dcd_event_handler(event, in_isr);
}

/* V-11: STALL SET_LINE_CODING in M_STALL_LC. */
extern bool __real_cdcd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request);

bool __wrap_cdcd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (s_mode == M_STALL_LC && stage == CONTROL_STAGE_SETUP &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
        request->bRequest == CDC_REQUEST_SET_LINE_CODING) {
        ev_push('L', 0, cyc());
        return false; /* usbd stalls the control transfer */
    }
    return __real_cdcd_control_xfer_cb(rhport, stage, request);
}

void tud_sof_cb(uint32_t frame_count) {
    (void)frame_count; /* SOF is timestamped in the wrapper */
}

/* ========================================================= UART out ===== */

#define TX_N 16384u
static char s_tx[TX_N];
static uint32_t s_tx_head;
static uint32_t s_tx_tail;
static uint32_t s_tx_drop;

static void out(const char *s, size_t n) {
    if (TX_N - (s_tx_head - s_tx_tail) < n) {
        s_tx_drop++;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_tx[(s_tx_head + i) % TX_N] = s[i];
    }
    s_tx_head += (uint32_t)n;
}

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) {
        out(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1u);
    }
}

static void tx_drain(void) {
    while (s_tx_tail != s_tx_head && uart_is_writable(uart0)) {
        uart_putc_raw(uart0, s_tx[s_tx_tail % TX_N]);
        s_tx_tail++;
    }
}

static void header(void) {
    outf("H %" PRIu32 " %s %" PRIu32 "\n", clock_get_hz(clk_sys), s_mode_name[s_mode], g_desc_variant);
}

static void event(const char *text) {
    outf("E %08" PRIx32 " %s\n", cyc(), text);
}

/* ============================================================= coil ===== */

#define COIL_CARRIER_HZ 20000u
static uint s_coil_slice;
static uint32_t s_coil_wrap;
static uint16_t s_sine[256];
static volatile uint32_t s_coil_step;
static uint32_t s_coil_phase;

static void coil_isr(void) {
    pwm_clear_irq(s_coil_slice);
    s_coil_phase += s_coil_step;
    pwm_set_chan_level(s_coil_slice, PWM_CHAN_B, s_sine[s_coil_phase >> 24]);
}

static void coil_init(void) {
    gpio_set_function(PIN_COIL, GPIO_FUNC_PWM);
    s_coil_slice = pwm_gpio_to_slice_num(PIN_COIL);
    s_coil_wrap = clock_get_hz(clk_sys) / COIL_CARRIER_HZ - 1u;
    for (unsigned i = 0; i < 256u; i++) {
        float v = 0.5f + 0.45f * sinf(6.2831853f * (float)i / 256.0f);
        s_sine[i] = (uint16_t)(v * (float)s_coil_wrap);
    }
    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, (uint16_t)s_coil_wrap);
    pwm_init(s_coil_slice, &c, true);
    pwm_set_chan_level(s_coil_slice, PWM_CHAN_B, 0);
    irq_set_exclusive_handler(PWM_DEFAULT_IRQ_NUM(), coil_isr);
    irq_set_priority(PWM_DEFAULT_IRQ_NUM(), 0xC0); /* below USB and the marker */
    irq_set_enabled(PWM_DEFAULT_IRQ_NUM(), true);
}

static void coil_set(uint32_t hz) {
    if (hz == 0u) {
        pwm_set_irq_enabled(s_coil_slice, false);
        pwm_set_chan_level(s_coil_slice, PWM_CHAN_B, 0);
        return;
    }
    s_coil_step = (uint32_t)(((uint64_t)hz << 32) / COIL_CARRIER_HZ);
    pwm_clear_irq(s_coil_slice);
    pwm_set_irq_enabled(s_coil_slice, true);
}

/* =========================================================== marker ===== */

static void marker_cb(uint gpio, uint32_t events) {
    uint32_t c = cyc();
    if (gpio == PIN_MARKER) {
        ev_push('M', (events & GPIO_IRQ_EDGE_RISE) ? 1u : 0u, c);
    }
}

/* ======================================================== USB control === */

static void wait_us_serviced(uint32_t us) {
    uint64_t end = time_us_64() + us;
    while (time_us_64() < end) {
        tx_drain();
    }
}

static void reconnect(uint32_t variant) {
    (void)tud_disconnect();
    wait_us_serviced(300000u);
    g_desc_variant = variant;
    s_hang = false;
    s_pkt_tail = s_pkt_head;
    (void)tud_connect();
    event("reconnect");
}

static bool reading_allowed(void) {
    return s_mode != M_NAK && s_mode != M_NEVERACK;
}

static void data_poll(void) {
    if (!reading_allowed()) {
        return;
    }
    while (s_pkt_tail != s_pkt_head) {
        pkt_t p = s_pkt[s_pkt_tail % PKT_N];
        if (tud_cdc_available() < p.len) {
            break; /* the device task has not moved it into the FIFO yet */
        }
        uint8_t b[64];
        uint32_t n = tud_cdc_read(b, p.len < sizeof b ? p.len : sizeof b);
        s_pkt_tail++;
        char line[16 + 2 * 64 + 2];
        int k = snprintf(line, sizeof line, "O %08" PRIx32 " ", p.cyc);
        for (uint32_t i = 0; i < n && k > 0 && (size_t)k + 3u < sizeof line; i++) {
            k += snprintf(line + k, sizeof line - (size_t)k, "%02x", b[i]);
        }
        if (k > 0) {
            line[k++] = '\n';
            out(line, (size_t)k);
        }
        if (s_mode == M_STALL_OUT_ONCE || s_mode == M_STALL_OUT_REP) {
            usbd_edpt_stall(BOARD_TUD_RHPORT, EP_OUT);
            event("stall_out");
            if (s_mode == M_STALL_OUT_ONCE) {
                s_mode = M_NORMAL;
            }
        }
    }
    uint8_t dummy;
    (void)tud_cdc_read(&dummy, 0); /* re-arm OUT after a halt clear */
}

/* ========================================================= commands ===== */

static void set_mode(const char *name, const char *arg) {
    uint32_t variant = DESC_CDC;
    target_mode_t m;
    if (strcmp(name, "normal") == 0) {
        m = M_NORMAL;
    } else if (strcmp(name, "nak") == 0) {
        m = M_NAK;
        s_nak_end_us = time_us_64() + 1000u * (uint64_t)(arg ? strtoul(arg, NULL, 10) : 50u);
    } else if (strcmp(name, "neverack") == 0) {
        m = M_NEVERACK;
    } else if (strcmp(name, "stall_out_once") == 0) {
        m = M_STALL_OUT_ONCE;
    } else if (strcmp(name, "stall_out_rep") == 0) {
        m = M_STALL_OUT_REP;
    } else if (strcmp(name, "stall_in") == 0) {
        usbd_edpt_stall(BOARD_TUD_RHPORT, EP_IN); /* one-shot */
        event("stall_in");
        return;
    } else if (strcmp(name, "in_data") == 0) {
        m = M_IN_DATA;
    } else if (strcmp(name, "ep32") == 0) {
        m = M_EP32;
        variant = DESC_CDC_EP32;
    } else if (strcmp(name, "vendor") == 0) {
        m = M_VENDOR;
        variant = DESC_VENDOR;
    } else if (strcmp(name, "nolinereq") == 0) {
        m = M_NOLINEREQ;
        variant = DESC_CDC_NOLINEREQ;
    } else if (strcmp(name, "stall_linecoding") == 0) {
        m = M_STALL_LC;
    } else if (strcmp(name, "enumhang") == 0) {
        m = M_ENUMHANG;
    } else {
        event("err unknown mode");
        return;
    }
    target_mode_t old = s_mode;
    bool reenum = (variant != g_desc_variant) || m == M_STALL_LC || m == M_ENUMHANG || old == M_ENUMHANG ||
                  old == M_STALL_LC;
    s_mode = m;
    outf("E %08" PRIx32 " mode %s\n", cyc(), s_mode_name[m]);
    if (reenum) {
        reconnect(variant);
    }
}

static char s_cmd[64];
static unsigned s_cmd_len;

static void command(char *c) {
    char *name = strtok(c, " ");
    char *a1 = strtok(NULL, " ");
    char *a2 = strtok(NULL, " ");
    if (name == NULL) {
        return;
    }
    if (strcmp(name, "mode") == 0 && a1 != NULL) {
        set_mode(a1, a2);
    } else if (strcmp(name, "coil") == 0 && a1 != NULL) {
        uint32_t hz = (strcmp(a1, "off") == 0) ? 0u : (uint32_t)strtoul(a1, NULL, 10);
        coil_set(hz);
        outf("E %08" PRIx32 " coil %" PRIu32 "\n", cyc(), hz);
    } else if (strcmp(name, "hello") == 0) {
        header();
    } else {
        event("err unknown command");
    }
}

static void rx_poll(void) {
    while (uart_is_readable(uart0)) {
        char ch = (char)uart_getc(uart0);
        if (ch == '\r' || ch == '\n') {
            if (s_cmd_len != 0u) {
                s_cmd[s_cmd_len] = '\0';
                command(s_cmd);
                s_cmd_len = 0u;
            }
        } else if (s_cmd_len + 1u < sizeof s_cmd) {
            s_cmd[s_cmd_len++] = ch;
        }
    }
}

/* ============================================================== main ==== */

static void events_drain(void) {
    while (s_ev_tail != s_ev_head) {
        ev_t e = s_ev[s_ev_tail & (EV_N - 1u)];
        s_ev_tail++;
        switch (e.type) {
        case 'S':
            outf("S %08" PRIx32 "\n", e.cyc);
            break;
        case 'M':
            outf("M %08" PRIx32 " %u\n", e.cyc, (unsigned)e.arg);
            break;
        case 'R':
            outf("E %08" PRIx32 " bus_reset\n", e.cyc);
            break;
        case 'L':
            outf("E %08" PRIx32 " stall_line_coding\n", e.cyc);
            break;
        default:
            break;
        }
    }
}

int main(void) {
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0u;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;

    uart_init(uart0, UART_BAUD);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);

    gpio_init(PIN_MARKER);
    gpio_set_dir(PIN_MARKER, GPIO_IN);
    gpio_set_irq_enabled_with_callback(PIN_MARKER, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, marker_cb);
    irq_set_priority(IO_IRQ_BANK0, 0x40);

    coil_init();

    (void)tud_init(BOARD_TUD_RHPORT);
    irq_set_priority(USBCTRL_IRQ, 0x40);
    tud_sof_cb_enable(true);

    header();
    uint64_t next_header = time_us_64() + 5000000u;
    uint64_t next_in = 0u;
    uint32_t drops_reported = 0u;

    for (;;) {
        if (!s_hang) {
            tud_task();
        }
        uint64_t now = time_us_64();
        if (s_mode == M_NAK && now >= s_nak_end_us) {
            s_mode = M_NORMAL;
            event("nak_end");
        }
        if (s_mode == M_IN_DATA && now >= next_in && tud_cdc_connected()) {
            next_in = now + 20000u;
            (void)tud_cdc_write_str("TT-IN-DATA-0123\n");
            (void)tud_cdc_write_flush();
        }
        data_poll();
        events_drain();
        rx_poll();
        if (now >= next_header) {
            next_header = now + 5000000u;
            header();
            uint32_t drops = s_ev_drop + s_tx_drop;
            if (drops != drops_reported) {
                drops_reported = drops;
                outf("E %08" PRIx32 " drops ev=%" PRIu32 " tx=%" PRIu32 "\n", cyc(), s_ev_drop, s_tx_drop);
            }
        }
        tx_drain();
    }
}
