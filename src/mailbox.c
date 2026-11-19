#include "mailbox.h"

#include "hardware/sync.h"
#include "pico.h"

#define MB_NONE 3u

static mb_rec_t s_slot[3];
static volatile uint32_t s_newest = MB_NONE;  /* written by core 0 only */
static volatile uint32_t s_reading = MB_NONE; /* written by core 1 only */

/* Core 1 private. */
static bool s_have_last;
static uint64_t s_last_cycle;

void __not_in_flash_func(mb_publish)(const mb_rec_t *r) {
    uint32_t newest = s_newest;
    uint32_t reading = s_reading;
    uint32_t i = 0;
    while (i == newest || i == reading) {
        i++; /* three slots, at most two excluded */
    }
    s_slot[i] = *r;
    __dmb();
    s_newest = i;
    __dmb();
}

bool __not_in_flash_func(mb_take)(mb_rec_t *r) {
    for (;;) {
        uint32_t n = s_newest;
        if (n == MB_NONE) {
            return false;
        }
        s_reading = n;
        __dmb();
        if (s_newest != n) {
            continue; /* core 0 published meanwhile: take the newer one */
        }
        mb_rec_t copy = s_slot[n];
        __dmb();
        s_reading = MB_NONE;
        if (s_have_last && copy.cycle == s_last_cycle) {
            return false;
        }
        s_have_last = true;
        s_last_cycle = copy.cycle;
        *r = copy;
        return true;
    }
}
