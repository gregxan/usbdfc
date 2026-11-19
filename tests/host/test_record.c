/* Host test for src/record.c: CRC check value, COBS framing, field layout.
 * Prints one hex frame per line for tools/rx_decode.py --selftest. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "record.h"
#include "tick.h"

uint64_t tick_time_us(uint64_t idx) { return 1000u + idx * 1000u; }

static void set_raw(uint8_t raw[MMC_READ_BYTES], uint32_t x, uint32_t y, uint32_t z) {
    uint32_t v[3] = {x, y, z};
    memset(raw, 0, MMC_READ_BYTES);
    for (unsigned a = 0; a < 3; a++) {
        raw[2 * a] = (uint8_t)(v[a] >> 10);
        raw[2 * a + 1] = (uint8_t)(v[a] >> 2);
        raw[6] |= (uint8_t)((v[a] & 3u) << (6 - 2 * a));
    }
    raw[8] = MMC_STATUS_MEAS_M_DONE;
}

static void emit(const mag_pair_t *p, uint64_t cycle, bool fault, bool start) {
    uint8_t pl[REC_PAYLOAD_LEN], fr[REC_FRAME_LEN];
    rec_build(pl, p, cycle, fault, false, 0xA1B2C3D4u, 25000u + cycle * 20000u);
    size_t n = rec_finalize(fr, pl, start);
    assert(n == REC_FRAME_LEN);
    for (size_t i = 0; i + 1 < n; i++) assert(fr[i] != 0);
    assert(fr[n - 1] == 0);
    for (size_t i = 0; i < n; i++) printf("%02x", fr[i]);
    printf("\n");
}

int main(void) {
    assert(rec_crc16((const uint8_t *)"123456789", 9) == 0x29B1u);
    mag_pair_t p;
    memset(&p, 0, sizeof p);
    /* S = null + 1000/-2000/+5, R = null - 1000/+2000/+5 (offset 5 counts). */
    set_raw(p.s_raw, 131072u + 1000u + 5u, 131072u - 2000u + 5u, 131072u + 5u);
    set_raw(p.r_raw, 131072u - 1000u + 5u, 131072u + 2000u + 5u, 131072u + 5u);
    p.cycle = 7; p.t_us = 7 * 20000u + 1400u;
    emit(&p, 7, false, true);           /* valid, stream start          */
    p.r_half = MAG_HALF_NOT_READY;
    emit(&p, 8, false, false);          /* NOT_READY                    */
    emit(NULL, 9, false, false);        /* PAIR_INCOMPLETE, no pair     */
    emit(NULL, 10, true, false);        /* SENSOR_FAULT                 */
    set_raw(p.s_raw, 50u, 131072u, 131072u); p.r_half = MAG_HALF_OK;
    emit(&p, 11, false, false);         /* saturated                    */
    return 0;
}
