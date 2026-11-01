#include "bwg_noise_record.h"

#include <string.h>

#define MAX_SAFE_INTEGER 9007199254740991ull

static const char *const STATE_TEXT[] = {"idle", "admitted", "running", "cancelling", "terminal"};
static const char *const STAGE_TEXT[] = {
    "noise_prepared", "tcp_connected", "act_one_written", "act_two_received", "authority_verified", "proof_written",
    "socket_closed", "worker_quiescent", "admission", "fixture_ready", "candidate_inventory", "proof_received", "cleanup", "evidence",
};
static const char *const CATEGORY_TEXT[] = {
    "preparation", "connect", "write", "read", "authentication", "proof", "authority_lost", "clock_invalid", "cleanup", "evidence_incomplete",
};
static const char *const DETAIL_TEXT[] = {
    "timeout", "eof", "extra", "malformed", "io", "wrong_authority", "certificate_time", "session_replaced", "cancel_requested",
    "clock_discontinuity", "resource_unreleased", "missing", "duplicate", "rng", "allocation",
};
static const char *const OUTCOME_TEXT[] = {"accepted", "rejected", "cancelled", "expired", "incomplete"};
static const char *const SOCKET_TEXT[] = {"not_created", "open", "closed"};

static uint64_t maybe_now(const bwg_noise_record_t *record, uint64_t now_us)
{
    return record->clock_valid ? now_us : BWG_NOISE_NONE;
}

static void set_failure(bwg_noise_failure_t *failure, bwg_noise_stage_t stage, bwg_noise_category_t category, bwg_noise_detail_t detail,
                        uint64_t at_us)
{
    *failure = (bwg_noise_failure_t){.stage = stage, .category = category, .detail = detail, .at_us = at_us};
}

bool bwg_noise_record_admit(bwg_noise_record_t *record, const char *attempt_id, const char *input_sha256,
                            const bwg_v2_observation_t *observation)
{
    const uint64_t now_us = observation->observed_at_us;
    if (now_us > MAX_SAFE_INTEGER - BWG_NOISE_AUTHORITY_US - BWG_NOISE_TAIL_US) {
        return false;
    }
    memset(record, 0, sizeof(*record));
    record->state = BWG_NOISE_STATE_ADMITTED;
    strlcpy(record->attempt_id, attempt_id, sizeof(record->attempt_id));
    strlcpy(record->input_sha256, input_sha256, sizeof(record->input_sha256));
    record->boot_ordinal = observation->boot_ordinal;
    record->worker_generation = observation->worker_generation;
    record->transport_epoch = observation->serial_transport_epoch;
    record->admitted_at_us = now_us;
    record->authority_deadline_us = now_us + BWG_NOISE_AUTHORITY_US;
    record->local_socket_port = BWG_NOISE_NONE;
    record->decided_at_us = BWG_NOISE_NONE;
    record->cleanup_started_at_us = BWG_NOISE_NONE;
    record->cleanup_deadline_at_us = BWG_NOISE_NONE;
    record->released_at_us = BWG_NOISE_NONE;
    record->last_us = now_us;
    record->clock_valid = true;
    return true;
}

bool bwg_noise_record_permitted(const bwg_noise_record_t *record)
{
    return record->clock_valid && !record->has_first_failure && !record->has_terminal;
}

bool bwg_noise_record_dispatch(bwg_noise_record_t *record, uint64_t now_us)
{
    bwg_noise_record_tick(record, now_us);
    if (record->state != BWG_NOISE_STATE_ADMITTED || !bwg_noise_record_permitted(record)) {
        return false;
    }
    record->state = BWG_NOISE_STATE_RUNNING;
    return true;
}

void bwg_noise_record_cleanup(bwg_noise_record_t *record, uint64_t now_us)
{
    if (record->cleanup_deadline_at_us != BWG_NOISE_NONE) {
        return;
    }
    record->cleanup_started_at_us = now_us;
    record->cleanup_deadline_at_us = record->authority_deadline_us + BWG_NOISE_TAIL_US;
}

void bwg_noise_record_fail(bwg_noise_record_t *record, bwg_noise_stage_t stage, bwg_noise_category_t category,
                           bwg_noise_detail_t detail, uint64_t at_us)
{
    if (detail == BWG_NOISE_DETAIL_CLOCK_DISCONTINUITY) {
        record->clock_valid = false;
    }
    if (record->has_terminal) {
        return;
    }
    if (detail == BWG_NOISE_DETAIL_CLOCK_DISCONTINUITY && !record->has_resource_failure) {
        record->has_resource_failure = true;
        set_failure(&record->resource_failure, BWG_NOISE_AT_CLEANUP, BWG_NOISE_CATEGORY_CLOCK_INVALID, detail, BWG_NOISE_NONE);
    }
    if (!record->has_first_failure) {
        record->has_first_failure = true;
        set_failure(&record->first_failure, stage, category, detail, at_us);
    }
    record->state = BWG_NOISE_STATE_CANCELLING;
    bwg_noise_record_cleanup(record, at_us);
}

static void fail_clock(bwg_noise_record_t *record)
{
    bwg_noise_record_fail(record, BWG_NOISE_AT_EVIDENCE, BWG_NOISE_CATEGORY_CLOCK_INVALID, BWG_NOISE_DETAIL_CLOCK_DISCONTINUITY, BWG_NOISE_NONE);
}

static void end_incomplete(bwg_noise_record_t *record, uint64_t now_us)
{
    const uint64_t at_us = maybe_now(record, now_us);
    record->has_deadline_met = true;
    record->deadline_met = false;
    if (!record->has_resource_failure) {
        record->has_resource_failure = true;
        set_failure(&record->resource_failure, BWG_NOISE_AT_CLEANUP, BWG_NOISE_CATEGORY_CLEANUP, BWG_NOISE_DETAIL_RESOURCE_UNRELEASED, at_us);
    }
    record->has_terminal = true;
    record->outcome = BWG_NOISE_OUTCOME_INCOMPLETE;
    record->decided_at_us = at_us;
    record->state = BWG_NOISE_STATE_TERMINAL;
}

void bwg_noise_record_tick(bwg_noise_record_t *record, uint64_t now_us)
{
    if (record->has_terminal) {
        return;
    }
    if (now_us < record->last_us || now_us > MAX_SAFE_INTEGER) {
        fail_clock(record);
    } else {
        record->last_us = now_us;
    }
    if (now_us >= record->authority_deadline_us) {
        bwg_noise_record_fail(record, BWG_NOISE_AT_CLEANUP, BWG_NOISE_CATEGORY_AUTHORITY_LOST, BWG_NOISE_DETAIL_TIMEOUT, maybe_now(record, now_us));
    }
    if (now_us >= record->authority_deadline_us + BWG_NOISE_TAIL_US) {
        end_incomplete(record, now_us);
    }
}

static bool stage_seen(const bwg_noise_record_t *record, bwg_noise_stage_t stage)
{
    for (uint32_t i = 0; i < record->stage_count; i++) {
        if (record->stages[i].stage == stage) {
            return true;
        }
    }
    return false;
}

static bool is_cleanup_stage(bwg_noise_stage_t stage)
{
    return stage == BWG_NOISE_STAGE_SOCKET_CLOSED || stage == BWG_NOISE_STAGE_WORKER_QUIESCENT;
}

bool bwg_noise_record_stage(bwg_noise_record_t *record, bwg_noise_stage_t stage, uint64_t now_us, uint64_t duration_us, uint16_t bytes)
{
    // A completion can precede a concurrent tick's clock sample, so this compares event order alone.
    const uint64_t previous = record->stage_count > 0 && record->stages[record->stage_count - 1].at_us != BWG_NOISE_NONE
                                  ? record->stages[record->stage_count - 1].at_us
                                  : record->admitted_at_us;
    if (now_us < previous || now_us > MAX_SAFE_INTEGER) {
        fail_clock(record);
    }
    if (!is_cleanup_stage(stage) && !bwg_noise_record_permitted(record)) {
        return false;
    }
    if (stage_seen(record, stage)) {
        bwg_noise_record_fail(record, BWG_NOISE_AT_EVIDENCE, BWG_NOISE_CATEGORY_EVIDENCE_INCOMPLETE, BWG_NOISE_DETAIL_DUPLICATE, maybe_now(record, now_us));
        return false;
    }
    if (!is_cleanup_stage(stage) && (uint32_t)stage != record->stage_count) {
        bwg_noise_record_fail(record, BWG_NOISE_AT_EVIDENCE, BWG_NOISE_CATEGORY_EVIDENCE_INCOMPLETE, BWG_NOISE_DETAIL_MALFORMED, maybe_now(record, now_us));
        return false;
    }
    record->stages[record->stage_count++] = (bwg_noise_stage_row_t){
        .stage = stage,
        .at_us = maybe_now(record, now_us),
        .duration_us = maybe_now(record, duration_us),
        .bytes = bytes,
    };
    return true;
}

void bwg_noise_record_socket_opened(bwg_noise_record_t *record, uint16_t port)
{
    record->local_socket_port = port;
    record->socket = BWG_NOISE_SOCKET_OPEN;
}

void bwg_noise_record_socket_closed(bwg_noise_record_t *record, uint64_t now_us, uint64_t duration_us)
{
    record->socket = BWG_NOISE_SOCKET_CLOSED;
    bwg_noise_record_stage(record, BWG_NOISE_STAGE_SOCKET_CLOSED, now_us, duration_us, 0);
}

static bwg_noise_outcome_t outcome_of(bwg_noise_record_t *record, uint64_t now_us)
{
    if (record->has_first_failure) {
        switch (record->first_failure.detail) {
        case BWG_NOISE_DETAIL_TIMEOUT:
            return BWG_NOISE_OUTCOME_EXPIRED;
        case BWG_NOISE_DETAIL_CANCEL_REQUESTED:
        case BWG_NOISE_DETAIL_SESSION_REPLACED:
            return BWG_NOISE_OUTCOME_CANCELLED;
        default:
            return BWG_NOISE_OUTCOME_REJECTED;
        }
    }
    if (record->stage_count == BWG_NOISE_STAGE_COUNT && record->socket == BWG_NOISE_SOCKET_CLOSED && now_us < record->authority_deadline_us) {
        return BWG_NOISE_OUTCOME_ACCEPTED;
    }
    record->has_first_failure = true;
    set_failure(&record->first_failure, BWG_NOISE_AT_EVIDENCE, BWG_NOISE_CATEGORY_EVIDENCE_INCOMPLETE, BWG_NOISE_DETAIL_MISSING, now_us);
    return BWG_NOISE_OUTCOME_REJECTED;
}

void bwg_noise_record_joined(bwg_noise_record_t *record, uint64_t now_us)
{
    bwg_noise_record_tick(record, now_us);
    bwg_noise_record_cleanup(record, maybe_now(record, now_us));
    const uint64_t started = record->cleanup_started_at_us;
    bwg_noise_record_stage(record, BWG_NOISE_STAGE_WORKER_QUIESCENT, now_us, started == BWG_NOISE_NONE || now_us < started ? 0 : now_us - started, 0);
    record->worker_quiescent = true;
    record->volatile_inputs_disposed = true;
    record->released_at_us = maybe_now(record, now_us);
    if (!record->has_deadline_met) {
        record->has_deadline_met = true;
        record->deadline_met = record->clock_valid && now_us <= record->authority_deadline_us + BWG_NOISE_TAIL_US;
    }
    if (record->has_terminal) {
        return;
    }
    if (!record->clock_valid || record->socket == BWG_NOISE_SOCKET_OPEN) {
        end_incomplete(record, now_us);
        return;
    }
    record->outcome = outcome_of(record, now_us);
    record->has_terminal = true;
    record->decided_at_us = now_us;
    record->state = BWG_NOISE_STATE_TERMINAL;
}

static void key_value(bwg_jw_t *writer, const char *key, uint64_t maybe_value)
{
    bwg_jw_key(writer, key);
    if (maybe_value == BWG_NOISE_NONE) {
        bwg_jw_null(writer);
    } else {
        bwg_jw_u64(writer, maybe_value);
    }
}

static void key_text(bwg_jw_t *writer, const char *key, const char *text)
{
    bwg_jw_key(writer, key);
    bwg_jw_cstr(writer, text);
}

static void key_failure(bwg_jw_t *writer, const char *key, bool present, const bwg_noise_failure_t *failure)
{
    bwg_jw_key(writer, key);
    if (!present) {
        bwg_jw_null(writer);
        return;
    }
    bwg_jw_object_begin(writer);
    key_text(writer, "stage", STAGE_TEXT[failure->stage]);
    key_text(writer, "category", CATEGORY_TEXT[failure->category]);
    key_text(writer, "detail", DETAIL_TEXT[failure->detail]);
    key_value(writer, "atUs", failure->at_us);
    bwg_jw_object_end(writer);
}

static void write_stages(bwg_jw_t *writer, const bwg_noise_record_t *record)
{
    bwg_jw_key(writer, "stages");
    bwg_jw_array_begin(writer);
    for (uint32_t i = 0; i < record->stage_count; i++) {
        const bwg_noise_stage_row_t *row = &record->stages[i];
        bwg_jw_object_begin(writer);
        key_text(writer, "stage", STAGE_TEXT[row->stage]);
        key_value(writer, "sequence", i + 1u);
        key_value(writer, "atUs", row->at_us);
        key_value(writer, "durationUs", row->duration_us);
        key_value(writer, "bytes", row->bytes == 0 ? BWG_NOISE_NONE : row->bytes);
        bwg_jw_object_end(writer);
    }
    bwg_jw_array_end(writer);
}

static void write_resources(bwg_jw_t *writer, const bwg_noise_record_t *record)
{
    bwg_jw_key(writer, "resources");
    bwg_jw_object_begin(writer);
    key_text(writer, "socketState", SOCKET_TEXT[record->socket]);
    key_text(writer, "workerState", record->worker_quiescent ? "quiescent" : "running");
    bwg_jw_key(writer, "volatileInputsDisposed");
    bwg_jw_bool(writer, record->volatile_inputs_disposed);
    key_value(writer, "startedAtUs", record->cleanup_started_at_us);
    key_value(writer, "deadlineAtUs", record->cleanup_deadline_at_us);
    key_value(writer, "releasedAtUs", record->released_at_us);
    bwg_jw_key(writer, "deadlineMet");
    if (record->has_deadline_met) {
        bwg_jw_bool(writer, record->deadline_met);
    } else {
        bwg_jw_null(writer);
    }
    key_failure(writer, "failure", record->has_resource_failure, &record->resource_failure);
    bwg_jw_object_end(writer);
}

static void write_job(bwg_jw_t *writer, const bwg_noise_record_t *record)
{
    bwg_jw_object_begin(writer);
    key_text(writer, "attemptId", record->attempt_id);
    key_text(writer, "inputSha256", record->input_sha256);
    key_value(writer, "bootOrdinal", record->boot_ordinal);
    key_value(writer, "workerGeneration", record->worker_generation);
    key_value(writer, "transportEpoch", record->transport_epoch);
    key_value(writer, "admittedAtUs", record->admitted_at_us);
    key_value(writer, "authorityDeadlineUs", record->authority_deadline_us);
    key_value(writer, "localSocketPort", record->local_socket_port);
    write_stages(writer, record);
    key_failure(writer, "firstFailure", record->has_first_failure, &record->first_failure);
    bwg_jw_key(writer, "terminal");
    if (record->has_terminal) {
        bwg_jw_object_begin(writer);
        key_text(writer, "outcome", OUTCOME_TEXT[record->outcome]);
        key_value(writer, "decidedAtUs", record->decided_at_us);
        bwg_jw_object_end(writer);
    } else {
        bwg_jw_null(writer);
    }
    write_resources(writer, record);
    bwg_jw_object_end(writer);
}

static uint64_t later(uint64_t current, uint64_t maybe_time)
{
    return maybe_time != BWG_NOISE_NONE && maybe_time > current ? maybe_time : current;
}

/** The reader takes a status older than its own record as a broken clock, and a record moves between the two reads. */
static uint64_t observed_at(uint64_t observed_at_us, const bwg_noise_record_t *record)
{
    uint64_t latest = later(observed_at_us, record->last_us);
    for (uint32_t i = 0; i < record->stage_count; i++) {
        latest = later(latest, record->stages[i].at_us);
    }
    latest = later(latest, record->decided_at_us);
    latest = later(latest, record->released_at_us);
    latest = later(latest, record->cleanup_started_at_us);
    if (record->has_first_failure) {
        latest = later(latest, record->first_failure.at_us);
    }
    if (record->has_resource_failure) {
        latest = later(latest, record->resource_failure.at_us);
    }
    return latest;
}

void bwg_noise_write_status(bwg_jw_t *writer, const bwg_v2_observation_t *observation, const bwg_noise_record_t *maybe_record)
{
    bwg_jw_object_begin(writer);
    key_text(writer, "schema", "worker-noise-diagnostic-status-v2");
    key_text(writer, "state", STATE_TEXT[maybe_record != NULL ? maybe_record->state : BWG_NOISE_STATE_IDLE]);
    bwg_jw_key(writer, "observation");
    bwg_jw_object_begin(writer);
    key_value(writer, "bootOrdinal", observation->boot_ordinal);
    key_value(writer, "workerGeneration", observation->worker_generation);
    key_value(writer, "transportEpoch", observation->serial_transport_epoch);
    key_value(writer, "observedAtUs", maybe_record != NULL ? observed_at(observation->observed_at_us, maybe_record) : observation->observed_at_us);
    bwg_jw_key(writer, "stationIpv4");
    if (observation->wifi_connected && observation->station_ipv4[0] != '\0') {
        bwg_jw_cstr(writer, observation->station_ipv4);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_key(writer, "wifiConnected");
    bwg_jw_bool(writer, observation->wifi_connected);
    bwg_jw_object_end(writer);
    bwg_jw_key(writer, "job");
    if (maybe_record != NULL) {
        write_job(writer, maybe_record);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_object_end(writer);
}
