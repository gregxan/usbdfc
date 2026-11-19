/*
 * Section profiling (D-4, D-6). Copied from DFC Rev 2.16 (B-26) with the
 * section list changed. Measures elapsed time with the DWT cycle counter
 * (120 cycles = 1 us). Core 0 main loop only (single writer).
 *
 *     PROF_BEGIN(PROF_RECORD);
 *     build_record();
 *     PROF_END(PROF_RECORD);
 */
#ifndef PROF_H
#define PROF_H

#include <stdint.h>

#include "diag.h"

typedef enum {
    PROF_EPOCH,  /* whole epoch work (EX-8)                 */
    PROF_RECORD, /* building and publishing one record      */
    PROF_COUNT
} prof_id_t;

typedef struct {
    uint32_t last;  /* cycles, most recent run */
    uint32_t max;   /* cycles, worst case since boot */
    uint64_t sum;   /* cycles, for the average */
    uint64_t count; /* runs */
} prof_t;

extern prof_t g_prof[PROF_COUNT];
extern const char *const g_prof_name[PROF_COUNT];

static inline void prof_record(prof_id_t id, uint32_t cycles) {
    prof_t *p = &g_prof[id];
    p->last = cycles;
    if (cycles > p->max) {
        p->max = cycles;
    }
    p->sum += cycles;
    p->count++;
}

/* Average in cycles (0 if never run). */
uint32_t prof_avg(prof_id_t id);

#define PROF_BEGIN(id) const uint32_t prof_t0_##id = diag_cycles()
#define PROF_END(id) prof_record((id), diag_cycles() - prof_t0_##id)

#endif /* PROF_H */
