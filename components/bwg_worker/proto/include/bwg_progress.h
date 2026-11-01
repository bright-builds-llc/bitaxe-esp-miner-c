#ifndef BWG_PROGRESS_H
#define BWG_PROGRESS_H

#include <stdint.h>

#include "bwg_json_writer.h"

/**
 * What the ASIC path observed during the current or last lease, for diagnosing a lease with no shares.
 * Observations only: nothing here authorizes work. A category the C driver has no check for stays zero.
 */
typedef enum {
    BWG_PROGRESS_POLL_REQUESTED = 0,
    BWG_PROGRESS_POLL_IDLE,
    BWG_PROGRESS_POLL_NONCE,
    BWG_PROGRESS_POLL_REGISTER,
    BWG_PROGRESS_POLL_EMPTY,
    BWG_PROGRESS_QUALIFIED_CANDIDATE,
    BWG_PROGRESS_BELOW_POOL_TARGET,
    BWG_PROGRESS_DISCARD_INVALID_LENGTH,
    BWG_PROGRESS_DISCARD_INVALID_PREAMBLE,
    BWG_PROGRESS_DISCARD_INVALID_CRC,
    BWG_PROGRESS_DISCARD_JOB_LOOKUP,
    BWG_PROGRESS_DISCARD_PARSER_INVARIANT,
    BWG_PROGRESS_BLOCKED_OTHER,
    BWG_PROGRESS_EXPECTED_FILTER_MATCH,
    BWG_PROGRESS_EXPECTED_FILTER_MISS,
    BWG_PROGRESS_EVENT_COUNT,
} bwg_progress_event_t;

/** Starts counting from zero. A lease's start calls this. */
void bwg_progress_begin(void);

/** Freezes the counts, so local mining adds nothing to a lease's evidence. */
void bwg_progress_freeze(void);

void bwg_progress_note(bwg_progress_event_t event);

/**
 * Counts a chip result against the one filter the Gate names, a software expectation and not a reading of the chip's mask.
 * `sha256d` is the double hash's raw output, a little-endian integer.
 */
void bwg_progress_note_expected_filter(const uint8_t sha256d[32]);

/** Writes the `worker-mining-progress-v2` object at the writer's position. */
void bwg_progress_write(bwg_jw_t *json, uint32_t generation, uint64_t observed_at_ms);

#endif // BWG_PROGRESS_H
