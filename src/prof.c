#include "prof.h"

prof_t g_prof[PROF_COUNT];

/* Names for the diagnostic status lines; keep in step with prof_id_t. */
const char *const g_prof_name[PROF_COUNT] = {
    [PROF_EPOCH] = "epoch",
    [PROF_RECORD] = "record",
};

uint32_t prof_avg(prof_id_t id) {
    const prof_t *p = &g_prof[id];
    return p->count != 0u ? (uint32_t)(p->sum / p->count) : 0u;
}
