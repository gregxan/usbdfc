/*
 * Core 1: PIO-USB host (section 4.1, section 5).
 *
 * Core 1 runs only the SOF alarm interrupt (pio_usb_host_frame(), C1-1)
 * and the IC-2 thread loop: take records from the mailbox, queue them with
 * TinyUSB's CDC host, run tuh_task(), update the heartbeat.
 */
#ifndef USBH_H
#define USBH_H

#include <stdbool.h>
#include <stdint.h>

/* Core 1 counters (section 7.2). Cumulative, never cleared, written only
 * by core 1 (IC-3). Core 0 reads them as aligned 32-bit words. */
typedef struct {
    /* device */
    uint32_t attaches;
    uint32_t detaches;
    uint32_t enum_failures;
    uint32_t enum_timeouts;
    uint32_t unsupported;
    /* transfers */
    uint32_t xfer_errors;
    uint32_t stall_out;
    uint32_t stall_in;
    uint32_t halt_clears;
    uint32_t halt_clears_deferred;
    /* records */
    uint32_t sent;
    uint32_t late;
    uint32_t aborted;
    uint32_t overwritten;
    uint32_t missing;
    /* other */
    uint32_t in_bytes_discarded;
    uint32_t sof_count;  /* written by the SOF ISR (core 1) */
    uint32_t heartbeat;  /* written by the thread loop (core 1) */
    uint32_t sof_overruns;
    /* timing maxima / minima (section 7.2) */
    uint32_t sof_latency_max_us;    /* SOF ISR entry after its target (C1-2) */
    uint32_t frame_cycles_max;      /* frame handler duration (C1-3)         */
    uint32_t pub_to_queued_max_us;  /* C1-4                                  */
    uint32_t pub_margin_min_us;     /* USBH-T3: publish to designated SOF    */
} usbh_stats_t;

extern volatile usbh_stats_t g_usbh;

/* Core 1 entry point (multicore_launch_core1). After tuh_init() it pushes
 * USBH_FIFO_INIT_DONE to the SIO FIFO (section 8a L4). */
void usbh_core1_main(void);

#define USBH_FIFO_INIT_DONE 0x55534248u /* "USBH" */

#endif /* USBH_H */
