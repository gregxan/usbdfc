/*
 * Output record (section 5.2, REC-1 .. REC-6).
 *
 * Core 0 builds the 46-byte payload (rec_build). Core 1 sets the
 * stream-start flag, computes the CRC and COBS-encodes it into the 48-byte
 * frame sent in one bulk OUT transfer (rec_finalize). Section 8a M2.
 */
#ifndef RECORD_H
#define RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mag.h"

#define REC_VERSION 1u
#define REC_PAYLOAD_LEN 46u
#define REC_CRC_OFFSET 44u
#define REC_FRAME_LEN 48u /* COBS(46) = 47 bytes, plus the 0x00 delimiter */

/* REC-2 type */
#define REC_TYPE_MAG 1u
#define REC_TYPE_MAG_SINGLE 2u /* section 8a H2, diag-single only */

/* REC-2 flags */
#define REC_FLAG_VALID (1u << 0)
#define REC_FLAG_STALE (1u << 1)
#define REC_FLAG_STREAM_START (1u << 2)
#define REC_FLAG_SATURATED (1u << 3)
#define REC_FLAG_CHIP_UNSUPPORTED (1u << 4)

/* REC-2 reason (M-9 amended; section 8a M3) */
typedef enum {
    REC_REASON_OK = 0,
    REC_REASON_NO_PRIOR = 1,
    REC_REASON_NOT_READY = 2,
    REC_REASON_PAIR_INCOMPLETE = 3,
    REC_REASON_SENSOR_FAULT = 4,
} rec_reason_t;

/* Build the payload for one cycle (crc and stream-start left 0).
 * p is the published pair for that cycle, or NULL if there is none.
 * Returns the reason code written. */
rec_reason_t rec_build(uint8_t out[REC_PAYLOAD_LEN], const mag_pair_t *p, uint64_t cycle, bool sensor_fault,
                       bool chip_unsupported, uint32_t boot_id, uint64_t now_us);

/* Set stream-start if requested, write the CRC, COBS-encode, append the
 * delimiter. Returns REC_FRAME_LEN. */
size_t rec_finalize(uint8_t frame[REC_FRAME_LEN], uint8_t payload[REC_PAYLOAD_LEN], bool stream_start);

/* REC-3: CRC-16/CCITT-FALSE. */
uint16_t rec_crc16(const uint8_t *data, size_t len);

#endif /* RECORD_H */
