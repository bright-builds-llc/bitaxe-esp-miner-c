#ifndef BWG_CADENCE_H
#define BWG_CADENCE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "bwg_json_writer.h"

#define BWG_CADENCE_PHASES 3u
#define BWG_CADENCE_BUCKETS 4u
/** The largest review a full recorder can write. */
#define BWG_CADENCE_REVIEW_BYTES 4096u
/** The arm reply, with the longest values each field can hold. */
#define BWG_CADENCE_ARM_REPLY_BYTES 192u

typedef enum {
    BWG_CADENCE_IDLE = 0,
    BWG_CADENCE_USB,
    BWG_CADENCE_MINING,
} bwg_cadence_phase_t;

typedef enum {
    BWG_CADENCE_EMPTY = 0,
    BWG_CADENCE_ARMED,
    BWG_CADENCE_CAPTURING,
    BWG_CADENCE_COMPLETE,
} bwg_cadence_state_t;

typedef enum {
    BWG_CADENCE_PROJECTED = 0,
    BWG_CADENCE_UNCHANGED,
    BWG_CADENCE_NO_SUBSCRIBERS,
    BWG_CADENCE_PROJECTION_FAILED,
    BWG_CADENCE_SERIALIZATION_FAILED,
} bwg_cadence_publication_t;

/** One pass of the telemetry loop. */
typedef struct {
    uint64_t started_at_us;
    uint64_t finished_at_us;
    uint32_t subscribers;
    uint32_t cpu;
    uint32_t priority;
} bwg_cadence_iteration_t;

typedef struct {
    bwg_cadence_state_t state;
    uint32_t generation;
    uint32_t max_probe_count;
    uint32_t interval_count;
    uint32_t cpu_mismatch_count;
    uint32_t priority_mismatch_count;
    uint32_t subscriber_mismatch_count;
    uint32_t projection_count;
    uint32_t unchanged_count;
    uint32_t no_subscriber_count;
    uint32_t projection_failures;
    uint32_t serialization_failures;
    uint32_t send_failures;
    uint32_t sends;
    uint32_t clock_failures;
    uint32_t buckets[BWG_CADENCE_BUCKETS];
    uint64_t first_max_probe_at_us;
    uint64_t last_max_probe_at_us;
    uint64_t armed_at_us;
    uint64_t started_at_us;
    uint64_t ended_at_us;
    uint64_t maximum_interval_us;
    uint64_t maximum_execution_us;
    bool overflow;
} bwg_cadence_summary_t;

/**
 * What the Gate's telemetry cadence qualification reads: each phase is armed once per boot, in order, and nothing can
 * erase what it captured. Zeroed storage is ready to use, because the telemetry loop can start before the Gate does.
 * A note that finds it busy is counted as lost rather than waited for.
 */
typedef struct {
    atomic_flag busy;
    _Atomic bool enabled;
    _Atomic uint32_t dropped;
    bool has_previous;
    bwg_cadence_iteration_t previous;
    bwg_cadence_summary_t phases[BWG_CADENCE_PHASES];
} bwg_cadence_t;

/** True and writes the arm reply when `phase` is next and the loop has run. `generation` is the mining work's. */
bool bwg_cadence_arm(bwg_cadence_t *cadence, bwg_cadence_phase_t phase, uint64_t at_us, uint32_t generation,
                     uint32_t subscribers, bwg_jw_t *writer);

/** Whether the mining phase is waiting for this generation's first work. */
bool bwg_cadence_awaits_dispatch(bwg_cadence_t *cadence, uint32_t *generation);

/** Begins the mining capture at the armed generation's first work. */
void bwg_cadence_note_dispatch(bwg_cadence_t *cadence, uint32_t generation, uint64_t at_us, uint32_t subscribers);

void bwg_cadence_note_iteration(bwg_cadence_t *cadence, const bwg_cadence_iteration_t *iteration);
void bwg_cadence_note_publication(bwg_cadence_t *cadence, bwg_cadence_publication_t publication);

/** The board's sends finish before they return, so one note covers queueing and completion. */
void bwg_cadence_note_send(bwg_cadence_t *cadence, bool succeeded);

/** Counts a transport probe of the largest size, inside the USB phase's capture. */
void bwg_cadence_note_max_probe(bwg_cadence_t *cadence, uint64_t at_us);

/** Writes the worker-telemetry-cadence-v1 review. A note in progress makes it the snapshot-unavailable form. */
void bwg_cadence_write_review(bwg_cadence_t *cadence, bwg_jw_t *writer);

#endif // BWG_CADENCE_H
