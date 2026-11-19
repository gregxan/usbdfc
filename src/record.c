#include "record.h"

#include <string.h>

#include "pico.h"

#include "config.h"
#include "tick.h"

static inline void put_u32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static inline void put_u64(uint8_t *b, uint64_t v) {
    put_u32(b, (uint32_t)v);
    put_u32(b + 4, (uint32_t)(v >> 32));
}

static bool near_full_scale(const uint8_t *raw) {
    for (unsigned a = 0; a < 3u; a++) {
        uint32_t v = mag_raw18(raw, a);
        if (v <= MAG_SAT_MARGIN || v >= MMC_FULL_SCALE_COUNTS - 1u - MAG_SAT_MARGIN) {
            return true;
        }
    }
    return false;
}

static rec_reason_t pair_reason(const mag_pair_t *p) {
    mag_half_t s = (mag_half_t)p->s_half;
    mag_half_t r = p->single ? MAG_HALF_OK : (mag_half_t)p->r_half;
    /* M-9 order: NO_PRIOR, then NOT_READY, then PAIR_INCOMPLETE. */
    if (s == MAG_HALF_NO_PRIOR || r == MAG_HALF_NO_PRIOR) {
        return REC_REASON_NO_PRIOR;
    }
    if (s == MAG_HALF_NOT_READY || r == MAG_HALF_NOT_READY) {
        return REC_REASON_NOT_READY;
    }
    if (s == MAG_HALF_MISSING || r == MAG_HALF_MISSING) {
        return REC_REASON_PAIR_INCOMPLETE;
    }
    return REC_REASON_OK;
}

rec_reason_t __not_in_flash_func(rec_build)(uint8_t out[REC_PAYLOAD_LEN], const mag_pair_t *p, uint64_t cycle,
                                            bool sensor_fault, bool chip_unsupported, uint32_t boot_id,
                                            uint64_t now_us) {
    memset(out, 0, REC_PAYLOAD_LEN);

    rec_reason_t reason;
    uint64_t t_us;
    if (sensor_fault) {
        reason = REC_REASON_SENSOR_FAULT; /* section 8a M3: takes precedence */
    } else if (p == NULL) {
        reason = REC_REASON_PAIR_INCOMPLETE;
    } else {
        reason = pair_reason(p);
    }
    t_us = (p != NULL) ? p->t_us : tick_time_us(cycle * MAG_CYCLE_TICKS + MAG_S_TICK);

    uint8_t flags = 0u;
    if (reason == REC_REASON_OK) {
        flags |= REC_FLAG_VALID;
    }
    if (now_us > t_us && now_us - t_us > MAG_STALE_US) { /* section 8a L2 */
        flags |= REC_FLAG_STALE;
    }
    if (chip_unsupported) {
        flags |= REC_FLAG_CHIP_UNSUPPORTED;
    }

    out[0] = (uint8_t)REC_VERSION;
    out[1] = (uint8_t)((p != NULL && p->single) ? REC_TYPE_MAG_SINGLE : REC_TYPE_MAG);
    out[3] = (uint8_t)reason;
    put_u32(&out[4], boot_id);
    put_u32(&out[8], (uint32_t)cycle);
    put_u64(&out[12], t_us);

    if (p != NULL) {
        bool sat = false;
        for (unsigned a = 0; a < 3u; a++) {
            int32_t sv = (int32_t)mag_raw18(p->s_raw, a);
            int32_t d;
            int32_t s;
            if (p->single) {
                d = sv - (int32_t)MMC_NULL_COUNTS; /* section 8a H2 */
                s = 0;
            } else {
                int32_t rv = (int32_t)mag_raw18(p->r_raw, a);
                d = sv - rv;
                s = sv + rv - (int32_t)MMC_FULL_SCALE_COUNTS;
            }
            put_u32(&out[20u + 4u * a], (uint32_t)d);
            put_u32(&out[32u + 4u * a], (uint32_t)s);
        }
        if (p->s_half == MAG_HALF_OK && near_full_scale(p->s_raw)) {
            sat = true;
        }
        if (!p->single && p->r_half == MAG_HALF_OK && near_full_scale(p->r_raw)) {
            sat = true;
        }
        if (sat) {
            flags |= REC_FLAG_SATURATED;
        }
    }
    out[2] = flags;
    return reason;
}

uint16_t __not_in_flash_func(rec_crc16)(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* COBS encode len (< 254) bytes; returns the encoded length (len + 1). */
static size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t code_pos = 0;
    size_t o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0u) {
            out[code_pos] = code;
            code_pos = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            code++;
        }
    }
    out[code_pos] = code;
    return o;
}

size_t __not_in_flash_func(rec_finalize)(uint8_t frame[REC_FRAME_LEN], uint8_t payload[REC_PAYLOAD_LEN],
                                         bool stream_start) {
    if (stream_start) {
        payload[2] = (uint8_t)(payload[2] | REC_FLAG_STREAM_START);
    }
    uint16_t crc = rec_crc16(payload, REC_CRC_OFFSET);
    payload[REC_CRC_OFFSET] = (uint8_t)crc;
    payload[REC_CRC_OFFSET + 1u] = (uint8_t)(crc >> 8);
    size_t n = cobs_encode(payload, REC_PAYLOAD_LEN, frame);
    frame[n++] = 0x00u;
    return n;
}
