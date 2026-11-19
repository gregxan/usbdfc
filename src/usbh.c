/*
 * Core 1: PIO-USB host, record streaming (USBH-*, C1-*, section 7.1).
 */
#include "usbh.h"

#include <string.h>

#include "hardware/irq.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/time.h"

#include "host/hcd.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "tusb.h"

#include "config.h"
#include "diag.h"
#include "mailbox.h"
#include "record.h"
#include "tick.h"

volatile usbh_stats_t g_usbh;

/* Counter helpers: core 1 is the only writer (IC-3). */
#define INC(f) (g_usbh.f = g_usbh.f + 1u)

/* ================================================================ SOF === */

static uint s_sof_alarm;
static uint32_t s_sof_bit;
static uint64_t s_sof_t0;   /* SOF of tick 0 (never used itself)            */
static uint64_t s_sof_next; /* target time of the next SOF                   */
static volatile uint64_t s_frame; /* tick index of the frame now running   */

/* USBH-S4, C1-2: SOF from the tick grid plus SOF_OFFSET_US, each target
 * scheduled from the previous one. Core 1's only interrupt (C1-1). */
static void __not_in_flash_func(sof_isr)(void) {
    uint32_t c0 = diag_cycles();
    timer_hw->intr = s_sof_bit;
    uint64_t target = s_sof_next;
    uint32_t late_us = timer_hw->timerawl - (uint32_t)target;

    s_sof_next = target + TICK_PERIOD_US;
    timer_hw->alarm[s_sof_alarm] = (uint32_t)s_sof_next;
    while ((int32_t)(timer_hw->timerawl - (uint32_t)s_sof_next) >= 0) {
        timer_hw->armed = s_sof_bit;
        timer_hw->intr = s_sof_bit;
        s_sof_next += TICK_PERIOD_US;
        INC(sof_overruns);
        timer_hw->alarm[s_sof_alarm] = (uint32_t)s_sof_next;
    }

    s_frame = (target - s_sof_t0) / TICK_PERIOD_US;
    pio_usb_host_frame();
    INC(sof_count);

    if (late_us > g_usbh.sof_latency_max_us) {
        g_usbh.sof_latency_max_us = late_us;
    }
    uint32_t d = diag_cycles() - c0;
    if (d > g_usbh.frame_cycles_max) {
        g_usbh.frame_cycles_max = d;
    }
}

static void sof_start(void) {
    s_sof_alarm = (uint)timer_hardware_alarm_claim_unused(timer_hw, true);
    s_sof_bit = 1u << s_sof_alarm;
    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, s_sof_alarm);
    irq_set_exclusive_handler(irq, sof_isr);
    irq_set_priority(irq, IRQ_PRIO_SOF);

    s_sof_t0 = tick_time_us(0) + SOF_OFFSET_US;
    uint64_t now = time_us_64() + 50u;
    uint64_t f = (now > s_sof_t0) ? (now - s_sof_t0) / TICK_PERIOD_US + 1u : 0u;
    s_sof_next = s_sof_t0 + f * TICK_PERIOD_US;

    hw_set_bits(&timer_hw->inte, s_sof_bit);
    timer_hw->alarm[s_sof_alarm] = (uint32_t)s_sof_next;
    irq_set_enabled(irq, true); /* this core's NVIC only */
}

/* ===================================================== TinyUSB hooks === */

/* Section 8a L9, USBH-S1a rule 5: time from the timer, no alarm pool. */
uint32_t tusb_time_millis_api(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

void tusb_time_delay_ms_api(uint32_t ms) {
    busy_wait_us_32(ms * 1000u);
}

/* Streaming state (thread context). */
typedef enum { ST_NONE, ST_CHECKING, ST_STREAMING, ST_UNSUPPORTED } stream_state_t;

static volatile stream_state_t s_st = ST_NONE;
static uint8_t s_cdc_idx;
static volatile uint8_t s_daddr;  /* 0 = none */
static volatile uint8_t s_ep_out; /* bulk OUT address */
static volatile uint8_t s_ep_in;  /* bulk IN address  */
static uint8_t s_comm_itf;
static bool s_stream_start;

/* Set in the frame handler (hcd event, core 1 ISR), consumed by the thread. */
static volatile bool s_attach_ev;
static volatile bool s_detach_ev;
static volatile bool s_out_done;
static volatile uint8_t s_out_result;
static volatile uint64_t s_out_frame;
static volatile bool s_in_stall_ev;
static volatile bool s_in_err_ev;

/* USBH-T6 / section 7.1: transfer results are not reported to the
 * application by TinyUSB's CDC host (cdch_xfer_cb asserts on failure), so
 * the events are observed here, through -Wl,--wrap=hcd_event_handler. */
extern void __real_hcd_event_handler(hcd_event_t const *event, bool in_isr);

void __not_in_flash_func(__wrap_hcd_event_handler)(hcd_event_t const *event, bool in_isr) {
    switch (event->event_id) {
    case HCD_EVENT_DEVICE_ATTACH:
        s_attach_ev = true;
        break;
    case HCD_EVENT_DEVICE_REMOVE:
        s_detach_ev = true;
        break;
    case HCD_EVENT_XFER_COMPLETE:
        if (s_daddr != 0u && event->dev_addr == s_daddr) {
            uint8_t ep = event->xfer_complete.ep_addr;
            uint8_t res = event->xfer_complete.result;
            if (ep == s_ep_out) {
                s_out_result = res;
                s_out_frame = s_frame;
                s_out_done = true;
            } else if (ep == s_ep_in && res == XFER_RESULT_STALLED) {
                s_in_stall_ev = true;
            } else if (ep == s_ep_in && res != XFER_RESULT_SUCCESS) {
                s_in_err_ev = true;
            }
        }
        break;
    default:
        break;
    }
    __real_hcd_event_handler(event, in_isr);
}

/* ---------------------------------------- USBH-5: endpoint check ---- */

static uint8_t s_cfg_buf[CFG_TUH_ENUMERATION_BUFSIZE] __attribute__((aligned(4)));

/* Find the data interface that follows the CDC communication interface and
 * its bulk endpoints. Returns false if not found or not 64 bytes. */
static bool parse_endpoints(const uint8_t *d, uint32_t len) {
    uint32_t i = 0;
    bool after_comm = false;
    bool in_data = false;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint16_t mps_in = 0;
    uint16_t mps_out = 0;
    while (i + 2u <= len && d[i] >= 2u && i + d[i] <= len) {
        uint8_t blen = d[i];
        uint8_t type = d[i + 1u];
        if (type == TUSB_DESC_INTERFACE && blen >= 9u) {
            uint8_t num = d[i + 2u];
            uint8_t cls = d[i + 5u];
            if (in_data) {
                break; /* end of the data interface */
            }
            if (num == s_comm_itf) {
                after_comm = true;
            } else if (after_comm && cls == TUSB_CLASS_CDC_DATA) {
                in_data = true;
            }
        } else if (type == TUSB_DESC_ENDPOINT && blen >= 7u && in_data) {
            uint8_t addr = d[i + 2u];
            uint8_t attr = d[i + 3u] & 3u;
            uint16_t mps = (uint16_t)(d[i + 4u] | (d[i + 5u] << 8)) & 0x7ffu;
            if (attr == TUSB_XFER_BULK) {
                if (addr & 0x80u) {
                    ep_in = addr;
                    mps_in = mps;
                } else {
                    ep_out = addr;
                    mps_out = mps;
                }
            }
        }
        i += blen;
    }
    if (ep_in == 0u || ep_out == 0u || mps_in < 64u || mps_out < 64u) {
        return false;
    }
    s_ep_in = ep_in;
    s_ep_out = ep_out;
    return true;
}

static void reset_stream(void) {
    s_stream_start = true;
}

static bool s_inflight;
static bool s_pending;

static void cfg_desc_cb(tuh_xfer_t *xfer) {
    if (s_st != ST_CHECKING) {
        return;
    }
    if (xfer->result != XFER_RESULT_SUCCESS) {
        INC(enum_failures);
        s_st = ST_UNSUPPORTED;
        return;
    }
    if (parse_endpoints(s_cfg_buf, xfer->actual_len)) {
        s_inflight = false;
        s_pending = false;
        reset_stream();
        __dmb();
        s_st = ST_STREAMING;
    } else {
        INC(unsupported); /* USBH-5: bulk endpoints under 64 bytes */
        s_st = ST_UNSUPPORTED;
    }
}

void tuh_cdc_mount_cb(uint8_t idx) {
    tuh_itf_info_t info;
    if (!tuh_cdc_itf_get_info(idx, &info)) {
        return;
    }
    if (tuh_speed_get(info.daddr) != TUSB_SPEED_FULL) {
        return; /* USBH-1a: counted as unsupported in tuh_mount_cb */
    }
    s_cdc_idx = idx;
    s_comm_itf = info.desc.bInterfaceNumber;
    s_ep_in = 0u;
    s_ep_out = 0u;
    s_daddr = info.daddr;
    s_st = ST_CHECKING;
    if (!tuh_descriptor_get_configuration(info.daddr, 0, s_cfg_buf, sizeof s_cfg_buf, cfg_desc_cb, 0)) {
        INC(enum_failures);
        s_st = ST_UNSUPPORTED;
    }
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (idx == s_cdc_idx) {
        s_st = ST_NONE;
        s_daddr = 0u;
        s_inflight = false;
        s_pending = false;
    }
}

/* Every device, after configuration (USBH-9): anything that did not mount
 * as a full-speed CDC-ACM function is unsupported. */
void tuh_mount_cb(uint8_t daddr) {
    if (s_st == ST_NONE || s_daddr != daddr) {
        INC(unsupported);
    }
}

void tuh_umount_cb(uint8_t daddr) {
    (void)daddr;
}

/* ============================================ Halt clear (sec. 7.1) === */

static tusb_control_request_t s_clear_req;
static volatile bool s_ctrl_busy;
static bool s_clear_req_out;
static bool s_clear_req_in;
static bool s_clear_deferred_out;
static bool s_clear_deferred_in;
static uint64_t s_last_clear_out;
static uint64_t s_last_clear_in;
static bool s_have_clear_out;
static bool s_have_clear_in;

/* Pico-PIO-USB's hcd_edpt_clear_stall() is a stub, so reset the data
 * toggle of that endpoint in its public pool (relies on fe9133f). */
static void reset_toggle(uint8_t daddr, uint8_t ep_addr) {
    for (unsigned i = 0; i < PIO_USB_EP_POOL_CNT; i++) {
        endpoint_t *ep = PIO_USB_ENDPOINT(i);
        if (ep->size != 0u && ep->dev_addr == daddr && ep->ep_num == ep_addr) {
            ep->data_id = 0u; /* DATA0 */
        }
    }
}

static void clear_cb(tuh_xfer_t *xfer) {
    uint8_t ep = (uint8_t)xfer->user_data;
    if (xfer->result == XFER_RESULT_SUCCESS && s_daddr == xfer->daddr) {
        reset_toggle(xfer->daddr, ep);
        INC(halt_clears);
        if (ep == s_ep_in) {
            uint8_t dummy;
            (void)tuh_cdc_read(s_cdc_idx, &dummy, 0u); /* re-arm bulk IN */
        }
    } else {
        INC(xfer_errors);
    }
    s_ctrl_busy = false;
}

static bool send_clear(uint8_t ep) {
    s_clear_req.bmRequestType_bit.recipient = TUSB_REQ_RCPT_ENDPOINT;
    s_clear_req.bmRequestType_bit.type = TUSB_REQ_TYPE_STANDARD;
    s_clear_req.bmRequestType_bit.direction = TUSB_DIR_OUT;
    s_clear_req.bRequest = TUSB_REQ_CLEAR_FEATURE;
    s_clear_req.wValue = TUSB_REQ_FEATURE_EDPT_HALT;
    s_clear_req.wIndex = ep;
    s_clear_req.wLength = 0u;
    tuh_xfer_t x;
    memset(&x, 0, sizeof x);
    x.daddr = s_daddr;
    x.ep_addr = 0u;
    x.setup = &s_clear_req;
    x.buffer = NULL;
    x.complete_cb = clear_cb; /* L9: asynchronous only */
    x.user_data = ep;
    s_ctrl_busy = true;
    if (!tuh_control_xfer(&x)) {
        s_ctrl_busy = false;
        return false;
    }
    return true;
}

static void halt_poll(uint64_t now) {
    if (s_ctrl_busy) {
        return;
    }
    if (s_clear_req_out) {
        if (!s_have_clear_out || now - s_last_clear_out >= USBH_HALT_CLEAR_MIN_US) {
            if (send_clear(s_ep_out)) {
                s_clear_req_out = false;
                s_clear_deferred_out = false;
                s_have_clear_out = true;
                s_last_clear_out = now;
            }
        } else if (!s_clear_deferred_out) {
            s_clear_deferred_out = true;
            INC(halt_clears_deferred);
        }
        return;
    }
    if (s_clear_req_in) {
        if (!s_have_clear_in || now - s_last_clear_in >= USBH_HALT_CLEAR_MIN_US) {
            if (send_clear(s_ep_in)) {
                s_clear_req_in = false;
                s_clear_deferred_in = false;
                s_have_clear_in = true;
                s_last_clear_in = now;
            }
        } else if (!s_clear_deferred_in) {
            s_clear_deferred_in = true;
            INC(halt_clears_deferred);
        }
    }
}

/* ================================================= Streaming (T1-T8) === */

static mb_rec_t s_pend_rec;
static uint64_t s_inflight_t;
static uint64_t s_inflight_frame;
static bool s_have_seq;
static uint64_t s_last_seq;

/* Designated frame for record cycle c: tick 0 of cycle c + 1 (section 8a H1). */
static inline uint64_t designated_frame(uint64_t cycle) {
    return (cycle + 1u) * MAG_CYCLE_TICKS + MAG_S_TICK;
}

static void queue_record(mb_rec_t *r, uint64_t now) {
    uint8_t frame[REC_FRAME_LEN];
    size_t n = rec_finalize(frame, r->payload, s_stream_start);
    if (tuh_cdc_write(s_cdc_idx, frame, (uint32_t)n) != n) {
        (void)tuh_cdc_write_clear(s_cdc_idx);
        INC(xfer_errors);
        return;
    }
    (void)tuh_cdc_write_flush(s_cdc_idx);
    s_stream_start = false;
    s_inflight = true;
    s_inflight_t = now;
    s_inflight_frame = designated_frame(r->cycle);
    uint32_t q = (uint32_t)(now - r->t_pub_us);
    if (q > g_usbh.pub_to_queued_max_us) {
        g_usbh.pub_to_queued_max_us = q;
    }
}

static void stream_poll(uint64_t now) {
    /* Bulk IN: count and discard (USBH-4a). Reading also re-arms IN. */
    uint8_t junk[64];
    uint32_t avail = tuh_cdc_read_available(s_cdc_idx);
    if (avail != 0u) {
        uint32_t n = tuh_cdc_read(s_cdc_idx, junk, avail < sizeof junk ? avail : sizeof junk);
        g_usbh.in_bytes_discarded += n;
    }
    if (s_in_stall_ev) {
        s_in_stall_ev = false;
        INC(stall_in);
        s_clear_req_in = true;
    }
    if (s_in_err_ev) {
        s_in_err_ev = false;
        INC(xfer_errors);
        uint8_t dummy;
        (void)tuh_cdc_read(s_cdc_idx, &dummy, 0u);
    }

    /* Completion of the record in flight. */
    if (s_out_done) {
        s_out_done = false;
        s_inflight = false;
        uint8_t res = s_out_result;
        if (res == XFER_RESULT_SUCCESS) {
            INC(sent);
            if (s_out_frame > s_inflight_frame) {
                INC(late); /* H4: includes frames where IN returned data */
            }
        } else if (res == XFER_RESULT_STALLED) {
            INC(stall_out);
            (void)tuh_cdc_write_clear(s_cdc_idx);
            s_clear_req_out = true;
        } else {
            INC(xfer_errors);
            (void)tuh_cdc_write_clear(s_cdc_idx);
        }
    }

    /* USBH-T6: abort a record not accepted within one period. */
    if (s_inflight && now - s_inflight_t >= USBH_STUCK_RECORD_US) {
        if (tuh_edpt_abort_xfer(s_daddr, s_ep_out)) {
            s_inflight = false;
            (void)tuh_cdc_write_clear(s_cdc_idx); /* M5: no partial frame */
            INC(aborted);
        }
    }

    halt_poll(now);

    /* New record from core 0. */
    mb_rec_t r;
    if (mb_take(&r)) {
        if (s_have_seq && r.cycle > s_last_seq + 1u) {
            g_usbh.overwritten += (uint32_t)(r.cycle - s_last_seq - 1u);
        }
        s_have_seq = true;
        s_last_seq = r.cycle;

        uint64_t sof_us = s_sof_t0 + designated_frame(r.cycle) * TICK_PERIOD_US;
        if (sof_us > r.t_pub_us) {
            uint32_t margin = (uint32_t)(sof_us - r.t_pub_us);
            if (g_usbh.pub_margin_min_us == 0u || margin < g_usbh.pub_margin_min_us) {
                g_usbh.pub_margin_min_us = margin;
            }
        }
        if (now >= sof_us) {
            INC(missing); /* USBH-T5 / H1: too late for its frame; never sent */
        } else if (s_inflight || s_clear_req_out || s_ctrl_busy) {
            if (s_pending) {
                INC(overwritten); /* newest wins */
            }
            s_pend_rec = r;
            s_pending = true;
        } else {
            queue_record(&r, now);
        }
    }

    if (s_pending && !s_inflight && !s_clear_req_out && !s_ctrl_busy) {
        s_pending = false;
        queue_record(&s_pend_rec, now); /* goes out late (USBH-T6) */
    }
}

/* ======================================================== Thread ===== */

static bool s_attached;
static bool s_enum_timeout_counted;
static uint64_t s_attach_us;

static void events_poll(uint64_t now) {
    if (s_attach_ev) {
        s_attach_ev = false;
        INC(attaches);
        s_attached = true;
        s_attach_us = now;
        s_enum_timeout_counted = false;
    }
    if (s_detach_ev) {
        s_detach_ev = false;
        INC(detaches);
        s_attached = false;
    }
    /* USBH-3: not streaming within 2 s of attach (L8: may still stream later). */
    if (s_attached && !s_enum_timeout_counted && s_st != ST_STREAMING && s_st != ST_UNSUPPORTED &&
        now - s_attach_us > USBH_ENUM_TIMEOUT_US) {
        s_enum_timeout_counted = true;
        INC(enum_timeouts);
    }
}

#if APP_STALL_HOOK
extern volatile uint32_t g_diag_stall_core;
#endif

void usbh_core1_main(void) {
    diag_cycles_enable(); /* core 1's own DWT */

    /* USBH-S3 */
    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
    cfg.pin_dp = USB_HOST_DP_PIN;
    cfg.pinout = PIO_USB_PINOUT_DPDM;
    cfg.pio_tx_num = 0u;
    cfg.pio_rx_num = 0u;
    cfg.tx_ch = USB_HOST_TX_DMA_CH;
    cfg.skip_alarm_pool = true;
    (void)tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
    (void)tuh_init(BOARD_TUH_RHPORT);

    sof_start();
    multicore_fifo_push_blocking(USBH_FIFO_INIT_DONE); /* L4: core 0 enables VBUS */

    for (;;) {
        g_usbh.heartbeat = g_usbh.heartbeat + 1u;
        tuh_task();
        uint64_t now = time_us_64();
        events_poll(now);
        if (s_st == ST_STREAMING) {
            stream_poll(now);
        } else {
            mb_rec_t r;
            (void)mb_take(&r); /* M4: drained, uncounted, while not streaming */
            s_have_seq = false;
        }
#if APP_STALL_HOOK
        while (g_diag_stall_core == 2u) {
            tight_loop_contents(); /* V-14: core 1 stalled */
        }
#endif
    }
}
