#include "bwg_v2_record.h"

#include <sodium.h>
#include <string.h>

#define MAX_SAFE_INTEGER 9007199254740991ull
#define MSG_SETUP_SUCCESS 0x01u
#define MSG_CHANNEL_SUCCESS 0x11u
#define MSG_NEW_MINING_JOB 0x15u
#define MSG_SET_NEW_PREV_HASH 0x20u
#define MSG_SET_TARGET 0x21u
#define MSG_SUBMIT_SUCCESS 0x1cu
#define MSG_SUBMIT_ERROR 0x1du
#define ACK_PAYLOAD_BYTES 20u

_Static_assert(sizeof(crypto_hash_sha256_state) <= sizeof(((bwg_v2_record_t *)0)->commitment_state), "commitment state");

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const uint8_t *bytes)
{
    return (uint64_t)read_u32(bytes) | ((uint64_t)read_u32(bytes + 4) << 32);
}

static bool record_time(bwg_v2_record_t *record, uint64_t now_us, uint64_t *time_us)
{
    if (now_us != BWG_V2_NONE && now_us >= record->last_time_us && now_us <= MAX_SAFE_INTEGER) {
        record->last_time_us = now_us;
        record->observed_at_us = now_us;
        *time_us = now_us;
        return true;
    }
    record->observed_at_us = BWG_V2_NONE;
    *time_us = BWG_V2_NONE;
    bwg_v2_record_fail(record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_CLOCK, BWG_V2_NONE);
    return false;
}

bool bwg_v2_record_admit(bwg_v2_record_t *record, bwg_v2_scope_t scope, const char *attempt_id,
                         const bwg_v2_observation_t *observation)
{
    if (!observation->clock_valid || observation->observed_at_us == BWG_V2_NONE ||
        strlen(attempt_id) >= BWG_V2_ATTEMPT_ID_SIZE) {
        return false;
    }
    const uint64_t now_us = observation->observed_at_us;
    memset(record, 0, sizeof(*record));
    record->scope = scope;
    strcpy(record->attempt_id, attempt_id);
    record->boot_ordinal = observation->boot_ordinal;
    record->worker_generation = observation->worker_generation;
    record->serial_transport_epoch = observation->serial_transport_epoch;
    record->pool_session_generation = BWG_V2_NONE;
    record->pool_transport_epoch = BWG_V2_NONE;
    record->observed_at_us = now_us;
    record->last_time_us = now_us;
    record->state = BWG_V2_STATE_ADMITTED;
    record->admitted_at_us = now_us;
    record->authority_deadline_us = scope == BWG_V2_SCOPE_CHANNEL ? now_us + BWG_V2_AUTHORITY_US : BWG_V2_NONE;
    record->observation_deadline_us = scope == BWG_V2_SCOPE_CHANNEL ? now_us + BWG_V2_OBSERVATION_US : BWG_V2_NONE;
    record->terminal_at_us = BWG_V2_NONE;
    record->fence_retained = true;
    record->socket_closed_at_us = BWG_V2_NONE;
    record->worker_quiescent_at_us = BWG_V2_NONE;
    for (size_t i = 0; i < BWG_V2_OP_COUNT; i++) {
        bwg_v2_timing_t *timing = &record->timings[i];
        timing->max_duration_us = BWG_V2_NONE;
        timing->total_duration_us = BWG_V2_NONE;
        timing->first_started_at_us = BWG_V2_NONE;
        timing->last_finished_at_us = BWG_V2_NONE;
        timing->in_flight_started_at_us = BWG_V2_NONE;
    }
    crypto_hash_sha256_init((crypto_hash_sha256_state *)record->commitment_state);
    bwg_v2_record_event(record, BWG_V2_STAGE_ADMITTED, now_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    return true;
}

bool bwg_v2_record_budget_armed(bwg_v2_record_t *record, uint64_t arming_epoch_ms, uint32_t limit_ms, uint64_t now_us)
{
    uint64_t time_us;
    const bool has_time = record_time(record, now_us, &time_us);
    const uint64_t deadline_us = (arming_epoch_ms + limit_ms) * 1000u;
    if (record->scope != BWG_V2_SCOPE_SHARE || !has_time || arming_epoch_ms > time_us / 1000u ||
        arming_epoch_ms < record->admitted_at_us / 1000u || limit_ms != BWG_V2_SHARE_LIMIT_MS ||
        deadline_us > MAX_SAFE_INTEGER || deadline_us <= record->admitted_at_us) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_ASIC_DISPATCH, BWG_V2_FAILURE_CLOCK, BWG_V2_NONE);
        return false;
    }
    if (record->authority_deadline_us == BWG_V2_NONE) {
        record->authority_deadline_us = deadline_us;
        return true;
    }
    if (record->authority_deadline_us == deadline_us) {
        return true;
    }
    bwg_v2_record_fail(record, BWG_V2_STAGE_ASIC_DISPATCH, BWG_V2_FAILURE_EVIDENCE, record->last_time_us);
    return false;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_reversed_words(uint8_t *out, const uint8_t words[32])
{
    for (size_t i = 0; i < 8; i++) {
        memcpy(out + i * 4, words + (7 - i) * 4, 4);
    }
}

void bwg_v2_work_fields_digest(const bwg_v2_work_t *work, uint16_t slot, uint8_t digest[BWG_V2_DIGEST_BYTES])
{
    uint8_t payload[82];
    payload[0] = (uint8_t)(slot * BWG_V2_ASIC_JOB_ID_STEP);
    payload[1] = 1;
    put_le32(payload + 2, work->starting_nonce);
    put_le32(payload + 6, work->nbits);
    put_le32(payload + 10, work->ntime);
    put_reversed_words(payload + 14, work->merkle_root);
    put_reversed_words(payload + 46, work->prev_hash);
    put_le32(payload + 78, work->version);
    crypto_hash_sha256(digest, payload, sizeof(payload));
}

bool bwg_v2_record_bind_pool(bwg_v2_record_t *record, uint64_t generation, uint64_t epoch)
{
    if (record->pool_session_generation != BWG_V2_NONE || record->outcome != BWG_V2_OUTCOME_NONE) {
        return false;
    }
    record->pool_session_generation = generation;
    record->pool_transport_epoch = epoch;
    return true;
}

static bool same_failure(const bwg_v2_failure_t *failure, bwg_v2_stage_t stage, bwg_v2_failure_category_t category, uint64_t at_us)
{
    return failure->stage == stage && failure->category == category && failure->at_us == at_us;
}

void bwg_v2_record_fail(bwg_v2_record_t *record, bwg_v2_stage_t stage, bwg_v2_failure_category_t category, uint64_t at_us)
{
    if (!record->has_first_failure) {
        record->has_first_failure = true;
        record->first_failure = (bwg_v2_failure_t){.stage = stage, .category = category, .at_us = at_us};
        return;
    }
    if ((category != BWG_V2_FAILURE_CLOCK && category != BWG_V2_FAILURE_CLEANUP) ||
        same_failure(&record->first_failure, stage, category, at_us) || record->secondary_count >= BWG_V2_MAX_FAILURES) {
        return;
    }
    for (uint32_t i = 0; i < record->secondary_count; i++) {
        if (record->secondary[i].stage == stage && record->secondary[i].category == category) {
            return;
        }
    }
    record->secondary[record->secondary_count++] = (bwg_v2_failure_t){.stage = stage, .category = category, .at_us = at_us};
}

static bool stage_outlives_outcome(bwg_v2_stage_t kind)
{
    return kind == BWG_V2_STAGE_SOCKET_CLOSED || kind == BWG_V2_STAGE_WORKER_QUIESCENT || kind == BWG_V2_STAGE_REVOKED ||
           kind == BWG_V2_STAGE_SHUTDOWN || kind == BWG_V2_STAGE_COOLED;
}

static bool stage_keeps_digest(bwg_v2_stage_t kind)
{
    return kind != BWG_V2_STAGE_SETUP && kind != BWG_V2_STAGE_CHANNEL && kind != BWG_V2_STAGE_ADMITTED &&
           kind != BWG_V2_STAGE_PREPARING && kind != BWG_V2_STAGE_CONNECTED && kind != BWG_V2_STAGE_AUTHENTICATED;
}

void bwg_v2_record_event(bwg_v2_record_t *record, bwg_v2_stage_t kind, uint64_t now_us, uint64_t channel_id, uint64_t job_id,
                         uint64_t submission_sequence, const uint8_t *maybe_digest)
{
    uint64_t time_us;
    record_time(record, now_us, &time_us);
    if (record->event_count >= BWG_V2_MAX_EVENTS) {
        bwg_v2_record_fail(record, kind, BWG_V2_FAILURE_EVIDENCE, time_us);
        return;
    }
    if (record->outcome != BWG_V2_OUTCOME_NONE && !stage_outlives_outcome(kind)) {
        return;
    }
    bwg_v2_event_t *event = &record->events[record->event_count++];
    *event = (bwg_v2_event_t){
        .sequence = record->event_count,
        .at_us = time_us,
        .kind = kind,
        .channel_id = channel_id,
        .job_id = job_id,
        .submission_sequence = submission_sequence,
    };
    if (maybe_digest != NULL && stage_keeps_digest(kind)) {
        event->has_digest = true;
        memcpy(event->digest, maybe_digest, BWG_V2_DIGEST_BYTES);
    }
    if (kind == BWG_V2_STAGE_PREPARING) {
        record->state = BWG_V2_STATE_RUNNING;
    }
}

void bwg_v2_record_begin(bwg_v2_record_t *record, bwg_v2_operation_t operation, uint64_t now_us)
{
    uint64_t time_us;
    record_time(record, now_us, &time_us);
    bwg_v2_timing_t *timing = &record->timings[operation];
    if (timing->in_flight_started_at_us != BWG_V2_NONE) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_EVIDENCE, time_us);
        return;
    }
    timing->in_flight_started_at_us = time_us;
}

void bwg_v2_record_end(bwg_v2_record_t *record, bwg_v2_operation_t operation, uint64_t now_us, bool failed)
{
    uint64_t time_us;
    const bool has_time = record_time(record, now_us, &time_us);
    bwg_v2_timing_t *timing = &record->timings[operation];
    const uint64_t started_us = timing->in_flight_started_at_us;
    timing->in_flight_started_at_us = BWG_V2_NONE;
    if (timing->count == 0) {
        timing->first_started_at_us = started_us;
    }
    timing->count++;
    timing->failed_count += failed ? 1u : 0u;
    timing->last_finished_at_us = time_us;
    const bool had_totals = timing->count == 1 || timing->total_duration_us != BWG_V2_NONE;
    if (!has_time || started_us == BWG_V2_NONE || !had_totals) {
        timing->max_duration_us = BWG_V2_NONE;
        timing->total_duration_us = BWG_V2_NONE;
        bwg_v2_record_fail(record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_CLOCK, BWG_V2_NONE);
        return;
    }
    const uint64_t duration_us = time_us - started_us;
    const uint64_t previous_max = timing->count == 1 ? 0 : timing->max_duration_us;
    const uint64_t previous_total = timing->count == 1 ? 0 : timing->total_duration_us;
    timing->max_duration_us = duration_us > previous_max ? duration_us : previous_max;
    timing->total_duration_us = previous_total + duration_us;
}

bool bwg_v2_record_connected(bwg_v2_record_t *record, const bwg_v2_socket_t *socket, uint64_t now_us)
{
    uint64_t time_us;
    if (!record_time(record, now_us, &time_us)) {
        return false;
    }
    if (record->has_connection) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_CONNECTED, BWG_V2_FAILURE_EXTRA, time_us);
        return false;
    }
    record->has_connection = true;
    record->connection_at_us = time_us;
    record->socket = *socket;
    bwg_v2_record_event(record, BWG_V2_STAGE_CONNECTED, time_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    return true;
}

static void commit_frame(bwg_v2_record_t *record, const uint8_t header[6], const uint8_t *payload, size_t payload_len)
{
    crypto_hash_sha256_state *state = (crypto_hash_sha256_state *)record->commitment_state;
    const uint32_t length = (uint32_t)payload_len + 6u;
    const uint8_t prefix[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    crypto_hash_sha256_update(state, prefix, sizeof(prefix));
    crypto_hash_sha256_update(state, header, 6);
    crypto_hash_sha256_update(state, payload, payload_len);
}

static void frame_digest(const uint8_t header[6], const uint8_t *payload, size_t payload_len, uint8_t digest[BWG_V2_DIGEST_BYTES])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, header, 6);
    crypto_hash_sha256_update(&state, payload, payload_len);
    crypto_hash_sha256_final(&state, digest);
}

/** The four job frames in their one order. A share scope can't bind its work to anything else. */
static bool job_frame(bwg_v2_record_t *record, uint8_t type, const uint8_t header[6], const uint8_t *payload, size_t payload_len,
                      uint64_t time_us)
{
    static const uint8_t ORDER[4] = {MSG_CHANNEL_SUCCESS, MSG_NEW_MINING_JOB, MSG_SET_TARGET, MSG_SET_NEW_PREV_HASH};
    if (record->frame_phase >= 4) {
        return true;
    }
    if (type != ORDER[record->frame_phase] || payload_len < 8) {
        bwg_v2_record_fail(record, record->last_frame_stage, BWG_V2_FAILURE_PROTOCOL, time_us);
        return false;
    }
    uint8_t digest[BWG_V2_DIGEST_BYTES];
    frame_digest(header, payload, payload_len, digest);
    commit_frame(record, header, payload, payload_len);
    record->frame_phase++;
    const uint32_t first = read_u32(payload);
    const uint32_t second = read_u32(payload + 4);
    switch (type) {
    case MSG_CHANNEL_SUCCESS:
        record->last_frame_stage = BWG_V2_STAGE_CHANNEL;
        bwg_v2_record_event(record, BWG_V2_STAGE_CHANNEL, time_us, second, BWG_V2_NONE, BWG_V2_NONE, digest);
        break;
    case MSG_NEW_MINING_JOB:
        record->last_frame_stage = BWG_V2_STAGE_JOB;
        bwg_v2_record_event(record, BWG_V2_STAGE_JOB, time_us, first, second, BWG_V2_NONE, digest);
        break;
    case MSG_SET_TARGET:
        record->last_frame_stage = BWG_V2_STAGE_TARGET;
        bwg_v2_record_event(record, BWG_V2_STAGE_TARGET, time_us, first, BWG_V2_NONE, BWG_V2_NONE, digest);
        break;
    default:
        record->last_frame_stage = BWG_V2_STAGE_WORK_READY;
        crypto_hash_sha256_final((crypto_hash_sha256_state *)record->commitment_state, record->job_commitment);
        record->has_job_commitment = true;
        bwg_v2_record_event(record, BWG_V2_STAGE_WORK_READY, time_us, first, second, BWG_V2_NONE, digest);
        break;
    }
    return true;
}

static uint32_t unsettled_through(const bwg_v2_record_t *record, uint32_t last_sequence)
{
    uint32_t pending = 0;
    for (uint32_t i = 0; i < record->share_count; i++) {
        const bwg_v2_share_t *share = &record->shares[i];
        pending += share->submission_sequence <= last_sequence && share->ack_at_us == BWG_V2_NONE ? 1u : 0u;
    }
    return pending;
}

static bwg_v2_share_t *share_with_sequence(bwg_v2_record_t *record, uint32_t sequence)
{
    for (uint32_t i = 0; i < record->share_count; i++) {
        if (record->shares[i].submission_sequence == sequence) {
            return &record->shares[i];
        }
    }
    return NULL;
}

/** Joins an ACK to one actual completed write. Any ACK that can't be matched exactly ends the attempt. */
static bool acknowledgement(bwg_v2_record_t *record, const uint8_t header[6], const uint8_t *payload, size_t payload_len, uint64_t time_us)
{
    const uint16_t extension = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
    if (extension != BWG_V2_EXTENSION_CHANNEL || payload_len != ACK_PAYLOAD_BYTES) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_ACCEPTED, BWG_V2_FAILURE_PROTOCOL, time_us);
        return false;
    }
    const uint32_t channel_id = read_u32(payload);
    const uint32_t last_sequence = read_u32(payload + 4);
    const uint32_t accepted_count = read_u32(payload + 8);
    const uint64_t shares_sum = read_u64(payload + 12);
    bwg_v2_share_t *maybe_share = share_with_sequence(record, last_sequence);
    if (maybe_share == NULL || record->scope != BWG_V2_SCOPE_SHARE || record->socket_closed || record->worker_quiescent ||
        record->outcome != BWG_V2_OUTCOME_NONE || accepted_count != 1 || shares_sum != BWG_V2_ACK_SHARES_SUM ||
        unsettled_through(record, last_sequence) != 1 || maybe_share->channel_id != channel_id ||
        maybe_share->write_completed_at_us == BWG_V2_NONE || maybe_share->ack_at_us != BWG_V2_NONE) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_ACCEPTED, BWG_V2_FAILURE_EVIDENCE, time_us);
        return false;
    }
    maybe_share->ack_at_us = time_us;
    maybe_share->ack_last_sequence = last_sequence;
    maybe_share->ack_accepted_count = accepted_count;
    maybe_share->ack_shares_sum = shares_sum;
    uint8_t digest[BWG_V2_DIGEST_BYTES];
    frame_digest(header, payload, payload_len, digest);
    bwg_v2_record_event(record, BWG_V2_STAGE_ACCEPTED, time_us, channel_id, maybe_share->job_id, last_sequence, digest);
    return true;
}

bool bwg_v2_record_frame(bwg_v2_record_t *record, const uint8_t header[6], const uint8_t *payload, size_t payload_len, uint64_t now_us)
{
    uint64_t time_us;
    if (!record_time(record, now_us, &time_us)) {
        return false;
    }
    const uint8_t type = header[2];
    switch (type) {
    case MSG_SETUP_SUCCESS:
        record->last_frame_stage = BWG_V2_STAGE_SETUP;
        bwg_v2_record_event(record, BWG_V2_STAGE_SETUP, time_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
        return true;
    case MSG_SUBMIT_SUCCESS:
        return acknowledgement(record, header, payload, payload_len, time_us);
    case MSG_SUBMIT_ERROR:
        bwg_v2_record_fail(record, BWG_V2_STAGE_ACCEPTED, BWG_V2_FAILURE_REJECTED_SHARE, time_us);
        return false;
    default:
        return job_frame(record, type, header, payload, payload_len, time_us);
    }
}

void bwg_v2_record_dispatch(bwg_v2_record_t *record, uint16_t slot, const uint8_t digest[BWG_V2_DIGEST_BYTES], uint64_t now_us)
{
    uint64_t time_us;
    if (!record_time(record, now_us, &time_us) || record->scope != BWG_V2_SCOPE_SHARE) {
        return;
    }
    if (slot >= BWG_V2_MAX_DISPATCHES || record->authority_deadline_us == BWG_V2_NONE) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_ASIC_DISPATCH, BWG_V2_FAILURE_EVIDENCE, time_us);
        return;
    }
    record->dispatch_count++;
    record->dispatches[slot] = (bwg_v2_dispatch_t){
        .valid = true,
        .sequence = record->dispatch_count,
        .at_us = time_us,
        .asic_job_id = (uint8_t)(slot * BWG_V2_ASIC_JOB_ID_STEP),
    };
    memcpy(record->dispatches[slot].digest, digest, BWG_V2_DIGEST_BYTES);
    // Only the first of a run becomes an event: a long lease could dispatch more than the record holds.
    if (record->dispatch_count == 1) {
        bwg_v2_record_event(record, BWG_V2_STAGE_ASIC_DISPATCH, time_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    }
}

void bwg_v2_record_nonce(bwg_v2_record_t *record, uint16_t slot, const uint8_t digest[BWG_V2_DIGEST_BYTES], uint32_t nonce,
                         uint32_t rolled_version, uint32_t version_bits, uint8_t asic_index, uint8_t core_id,
                         uint8_t small_core_id, uint64_t now_us)
{
    uint64_t time_us;
    if (!record_time(record, now_us, &time_us) || record->scope != BWG_V2_SCOPE_SHARE || slot >= BWG_V2_MAX_DISPATCHES) {
        return;
    }
    const bwg_v2_dispatch_t *dispatch = &record->dispatches[slot];
    if (!dispatch->valid || memcmp(dispatch->digest, digest, BWG_V2_DIGEST_BYTES) != 0) {
        return;
    }
    record->pending_nonce = (bwg_v2_pending_nonce_t){
        .valid = true,
        .at_us = time_us,
        .dispatch = dispatch,
        .nonce = nonce,
        .rolled_version = rolled_version,
        .version_bits = version_bits,
        .asic_index = asic_index,
        .core_id = core_id,
        .small_core_id = small_core_id,
    };
    bwg_v2_record_event(record, BWG_V2_STAGE_NONCE, time_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
}

bool bwg_v2_record_submission_begin(bwg_v2_record_t *record, uint32_t channel_id, uint32_t job_id, uint32_t sequence, uint32_t nonce,
                                    uint32_t ntime, uint32_t version, uint64_t now_us)
{
    uint64_t time_us;
    if (!record_time(record, now_us, &time_us)) {
        return false;
    }
    const bwg_v2_pending_nonce_t pending = record->pending_nonce;
    record->pending_nonce.valid = false;
    if (record->scope != BWG_V2_SCOPE_SHARE || !pending.valid || pending.nonce != nonce || pending.rolled_version != version ||
        record->share_count >= BWG_V2_MAX_SHARES || share_with_sequence(record, sequence) != NULL ||
        record->authority_deadline_us == BWG_V2_NONE) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_NONCE, BWG_V2_FAILURE_EVIDENCE, time_us);
        return false;
    }
    record->shares[record->share_count++] = (bwg_v2_share_t){
        .dispatch_sequence = pending.dispatch->sequence,
        .asic_job_id = pending.dispatch->asic_job_id,
        .dispatched_at_us = pending.dispatch->at_us,
        .nonce_at_us = pending.at_us,
        .write_started_at_us = time_us,
        .write_completed_at_us = BWG_V2_NONE,
        .nonce = nonce,
        .version_bits = pending.version_bits,
        .asic_index = pending.asic_index,
        .core_id = pending.core_id,
        .small_core_id = pending.small_core_id,
        .channel_id = channel_id,
        .job_id = job_id,
        .submission_sequence = sequence,
        .ntime = ntime,
        .version = version,
        .ack_at_us = BWG_V2_NONE,
    };
    memcpy(record->shares[record->share_count - 1].work_fields_sha256, pending.dispatch->digest, BWG_V2_DIGEST_BYTES);
    bwg_v2_record_event(record, BWG_V2_STAGE_SUBMISSION, time_us, channel_id, job_id, sequence, NULL);
    return true;
}

void bwg_v2_record_submission_end(bwg_v2_record_t *record, uint32_t sequence, bool written, uint64_t now_us)
{
    uint64_t time_us;
    record_time(record, now_us, &time_us);
    bwg_v2_share_t *maybe_share = share_with_sequence(record, sequence);
    if (maybe_share != NULL && written) {
        maybe_share->write_completed_at_us = time_us;
    }
}

void bwg_v2_record_socket_closed(bwg_v2_record_t *record, uint64_t now_us)
{
    uint64_t time_us;
    record_time(record, now_us, &time_us);
    record->socket_closed = true;
    record->socket_closed_at_us = time_us;
    bwg_v2_record_event(record, BWG_V2_STAGE_SOCKET_CLOSED, time_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
}

static void terminal(bwg_v2_record_t *record, bwg_v2_outcome_t outcome, uint64_t time_us)
{
    record->state = BWG_V2_STATE_TERMINAL;
    record->outcome = outcome;
    record->terminal_at_us = time_us;
}

static bool share_succeeded(const bwg_v2_record_t *record)
{
    if (record->authority_deadline_us == BWG_V2_NONE) {
        return false;
    }
    for (uint32_t i = 0; i < record->share_count; i++) {
        const bwg_v2_share_t *share = &record->shares[i];
        if (share->write_completed_at_us != BWG_V2_NONE && share->ack_at_us != BWG_V2_NONE && share->ack_accepted_count == 1) {
            return true;
        }
    }
    return false;
}

static void finish(bwg_v2_record_t *record, uint64_t time_us)
{
    if (record->outcome != BWG_V2_OUTCOME_NONE) {
        return;
    }
    const bool complete = record->socket_closed && time_us != BWG_V2_NONE && !record->fence_retained;
    const bool success = !record->has_first_failure && complete && record->has_job_commitment &&
                         (record->scope == BWG_V2_SCOPE_SHARE ? share_succeeded(record) : time_us <= record->authority_deadline_us);
    if (!success && !record->has_first_failure) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_EVIDENCE, time_us);
    }
    terminal(record, success ? BWG_V2_OUTCOME_ACCEPTED : (complete ? BWG_V2_OUTCOME_REJECTED : BWG_V2_OUTCOME_INCOMPLETE), time_us);
}

void bwg_v2_record_joined(bwg_v2_record_t *record, uint64_t now_us, bool fence_released)
{
    uint64_t time_us;
    record_time(record, now_us, &time_us);
    record->worker_quiescent = true;
    record->worker_quiescent_at_us = time_us;
    record->fence_retained = !fence_released;
    bwg_v2_record_event(record, BWG_V2_STAGE_WORKER_QUIESCENT, time_us, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    if (fence_released) {
        finish(record, time_us);
    }
}

bool bwg_v2_record_release_fence(bwg_v2_record_t *record, uint64_t now_us)
{
    if (!record->worker_quiescent) {
        return false;
    }
    record->fence_retained = false;
    uint64_t time_us;
    record_time(record, now_us, &time_us);
    finish(record, time_us);
    return true;
}

void bwg_v2_record_tick(bwg_v2_record_t *record, uint64_t now_us)
{
    uint64_t time_us;
    const bool has_time = record_time(record, now_us, &time_us);
    if (!has_time || record->outcome != BWG_V2_OUTCOME_NONE) {
        return;
    }
    if (record->scope == BWG_V2_SCOPE_CHANNEL && time_us >= record->authority_deadline_us) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_TIMEOUT, time_us);
    }
    if (record->observation_deadline_us != BWG_V2_NONE && time_us >= record->observation_deadline_us) {
        bwg_v2_record_fail(record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_CLEANUP, time_us);
        terminal(record, BWG_V2_OUTCOME_INCOMPLETE, time_us);
    }
}

bool bwg_v2_record_has_stage(const bwg_v2_record_t *record, bwg_v2_stage_t stage)
{
    for (uint32_t i = 0; i < record->event_count; i++) {
        if (record->events[i].kind == stage) {
            return true;
        }
    }
    return false;
}

bool bwg_v2_record_has_accepted_share(const bwg_v2_record_t *record)
{
    for (uint32_t i = 0; i < record->share_count; i++) {
        if (record->shares[i].ack_at_us != BWG_V2_NONE && record->shares[i].ack_accepted_count == 1) {
            return true;
        }
    }
    return false;
}
