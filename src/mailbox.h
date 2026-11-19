/*
 * Core 0 -> core 1 triple-buffer mailbox (IC-1, section 8a M1).
 *
 * Three slots, a `newest` index written only by core 0 and a `reading`
 * index written only by core 1, each an aligned 32-bit word. No hardware
 * spinlocks and no read-modify-write across cores.
 *   core 0: pick a slot that is neither `newest` nor `reading`, fill it,
 *           DMB, store `newest`, DMB.
 *   core 1: load `newest`, store it to `reading`, DMB, re-load `newest`;
 *           if it changed, repeat; otherwise copy the slot.
 * The DMBs on both sides mean core 0 either sees core 1's `reading` or
 * core 1's re-check sees core 0's new `newest`, so a slot is never written
 * while it is being copied.
 */
#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

typedef struct {
    uint8_t payload[REC_PAYLOAD_LEN];
    uint64_t cycle;    /* record cycle (its seq)               */
    uint64_t t_pub_us; /* when core 0 published it (USBH-T3)   */
} mb_rec_t;

/* Core 0 only. Never blocks. */
void mb_publish(const mb_rec_t *r);

/* Core 1 only. Never blocks. Returns true and copies the newest record if
 * it is newer than the last one taken. */
bool mb_take(mb_rec_t *r);

#endif /* MAILBOX_H */
