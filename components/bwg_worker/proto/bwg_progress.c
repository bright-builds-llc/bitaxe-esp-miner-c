#include "bwg_progress.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static _Atomic bool s_counting;
static _Atomic uint32_t s_counts[BWG_PROGRESS_EVENT_COUNT];

void bwg_progress_begin(void)
{
    for (size_t i = 0; i < BWG_PROGRESS_EVENT_COUNT; i++) {
        atomic_store(&s_counts[i], 0);
    }
    atomic_store(&s_counting, true);
}

void bwg_progress_freeze(void)
{
    atomic_store(&s_counting, false);
}

void bwg_progress_note(bwg_progress_event_t event)
{
    if (atomic_load(&s_counting)) {
        atomic_fetch_add(&s_counts[event], 1);
    }
}

void bwg_progress_note_expected_filter(const uint8_t sha256d[32])
{
    // Under 2^216: the top five bytes of the little-endian integer are zero.
    const bool matches = (sha256d[27] | sha256d[28] | sha256d[29] | sha256d[30] | sha256d[31]) == 0;
    bwg_progress_note(matches ? BWG_PROGRESS_EXPECTED_FILTER_MATCH : BWG_PROGRESS_EXPECTED_FILTER_MISS);
}

// The Gate reads counts as decimal strings, since they may exceed what JSON numbers hold exactly.
static void write_decimal(bwg_jw_t *json, const char *key, uint64_t value)
{
    char text[24];
    int len = snprintf(text, sizeof(text), "%" PRIu64, value);
    bwg_jw_key(json, key);
    bwg_jw_string(json, text, (size_t) len);
}

static void write_count(bwg_jw_t *json, const char *key, bwg_progress_event_t event)
{
    write_decimal(json, key, atomic_load(&s_counts[event]));
}

/** An empty poll that nothing else explains: the chip files return empty for an unknown register. */
static uint32_t register_response_discards(void)
{
    static const bwg_progress_event_t explained[] = {
        BWG_PROGRESS_POLL_IDLE,
        BWG_PROGRESS_DISCARD_INVALID_LENGTH,
        BWG_PROGRESS_DISCARD_INVALID_PREAMBLE,
        BWG_PROGRESS_DISCARD_INVALID_CRC,
        BWG_PROGRESS_DISCARD_JOB_LOOKUP,
        BWG_PROGRESS_DISCARD_PARSER_INVARIANT,
    };
    uint32_t empty = atomic_load(&s_counts[BWG_PROGRESS_POLL_EMPTY]);
    for (size_t i = 0; i < sizeof(explained) / sizeof(explained[0]); i++) {
        uint32_t count = atomic_load(&s_counts[explained[i]]);
        empty = empty > count ? empty - count : 0;
    }
    return empty;
}

void bwg_progress_write(bwg_jw_t *json, uint32_t generation, uint64_t observed_at_ms)
{
    bwg_jw_object_begin(json);
    bwg_jw_key(json, "schema");
    bwg_jw_cstr(json, "worker-mining-progress-v2");
    bwg_jw_key(json, "generation");
    bwg_jw_u64(json, generation);
    write_decimal(json, "observed_at_ms", observed_at_ms);
    write_count(json, "poll_requested", BWG_PROGRESS_POLL_REQUESTED);
    write_count(json, "poll_idle", BWG_PROGRESS_POLL_IDLE);
    write_count(json, "poll_nonce", BWG_PROGRESS_POLL_NONCE);
    write_count(json, "poll_register", BWG_PROGRESS_POLL_REGISTER);
    write_decimal(json, "stale_completion", 0);
    write_count(json, "qualified_candidates", BWG_PROGRESS_QUALIFIED_CANDIDATE);
    write_count(json, "below_pool_target", BWG_PROGRESS_BELOW_POOL_TARGET);
    write_decimal(json, "duplicate_candidates", 0);
    bwg_jw_key(json, "discards");
    bwg_jw_object_begin(json);
    write_count(json, "invalid_length", BWG_PROGRESS_DISCARD_INVALID_LENGTH);
    write_count(json, "invalid_preamble", BWG_PROGRESS_DISCARD_INVALID_PREAMBLE);
    write_count(json, "invalid_crc", BWG_PROGRESS_DISCARD_INVALID_CRC);
    write_count(json, "job_lookup", BWG_PROGRESS_DISCARD_JOB_LOOKUP);
    write_decimal(json, "core", 0);
    write_decimal(json, "address_interval", 0);
    write_decimal(json, "register_response", register_response_discards());
    write_count(json, "parser_invariant", BWG_PROGRESS_DISCARD_PARSER_INVARIANT);
    bwg_jw_object_end(json);
    bwg_jw_key(json, "blocked");
    bwg_jw_object_begin(json);
    write_decimal(json, "wrong_session", 0);
    write_decimal(json, "job_lookup", 0);
    write_decimal(json, "work_stale", 0);
    write_decimal(json, "target_mismatch", 0);
    write_count(json, "other", BWG_PROGRESS_BLOCKED_OTHER);
    bwg_jw_object_end(json);
    bwg_jw_key(json, "expected_filter");
    bwg_jw_cstr(json, "bm1366-ticket-256-leading-zero-40-v1");
    write_count(json, "expected_filter_matches", BWG_PROGRESS_EXPECTED_FILTER_MATCH);
    write_count(json, "expected_filter_misses", BWG_PROGRESS_EXPECTED_FILTER_MISS);
    bwg_jw_object_end(json);
}
