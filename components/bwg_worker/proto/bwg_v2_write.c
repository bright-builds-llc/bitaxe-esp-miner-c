#include <stdio.h>
#include <string.h>

#include "bwg_v2_record.h"

static const char *const STAGE_TEXT[] = {
    "admitted", "preparing", "connected", "authenticated", "setup", "channel", "job", "target", "work_ready",
    "socket_closed", "worker_quiescent", "asic_dispatch", "nonce", "submission", "accepted", "revoked", "shutdown", "cooled",
};
static const char *const FAILURE_TEXT[] = {
    NULL, "admission", "clock", "allocation", "authority", "timeout", "eof", "extra", "authentication", "protocol",
    "channel_mismatch", "job_mismatch", "invalid_nonce", "rejected_share", "safety", "cleanup", "evidence",
};
static const char *const OPERATION_TEXT[BWG_V2_OP_COUNT] = {
    "initiator_construction", "act_one_construction", "connect", "act_one_write", "act_two_read", "act_two_authentication",
    "frame_encrypt", "frame_write", "frame_read", "header_decrypt", "payload_decrypt", "socket_close", "worker_join",
};
static const char *const OUTCOME_TEXT[] = {NULL, "accepted", "rejected", "expired", "cancelled", "incomplete"};
static const char *const STATE_TEXT[] = {"idle", "admitted", "running", "terminal"};

static const char *scope_text(bwg_v2_scope_t scope)
{
    return scope == BWG_V2_SCOPE_CHANNEL ? "channel" : "share";
}

static void key_value(bwg_jw_t *writer, const char *key, uint64_t maybe_value)
{
    bwg_jw_key(writer, key);
    if (maybe_value == BWG_V2_NONE) {
        bwg_jw_null(writer);
    } else {
        bwg_jw_u64(writer, maybe_value);
    }
}

static void key_digest(bwg_jw_t *writer, const char *key, bool present, const uint8_t digest[BWG_V2_DIGEST_BYTES])
{
    bwg_jw_key(writer, key);
    if (!present) {
        bwg_jw_null(writer);
        return;
    }
    char text[BWG_V2_DIGEST_BYTES * 2 + 1];
    for (size_t i = 0; i < BWG_V2_DIGEST_BYTES; i++) {
        snprintf(text + i * 2, 3, "%02x", digest[i]);
    }
    bwg_jw_string(writer, text, BWG_V2_DIGEST_BYTES * 2);
}

static void write_event(bwg_jw_t *writer, const bwg_v2_event_t *event)
{
    bwg_jw_object_begin(writer);
    key_value(writer, "sequence", event->sequence);
    key_value(writer, "atDeviceUs", event->at_us);
    bwg_jw_key(writer, "kind");
    bwg_jw_cstr(writer, STAGE_TEXT[event->kind]);
    key_value(writer, "channelId", event->channel_id);
    key_value(writer, "jobId", event->job_id);
    key_value(writer, "submissionSequence", event->submission_sequence);
    key_digest(writer, "payloadSha256", event->has_digest, event->digest);
    bwg_jw_object_end(writer);
}

static void write_timing(bwg_jw_t *writer, bwg_v2_operation_t operation, const bwg_v2_timing_t *timing)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "operation");
    bwg_jw_cstr(writer, OPERATION_TEXT[operation]);
    key_value(writer, "count", timing->count);
    key_value(writer, "failedCount", timing->failed_count);
    key_value(writer, "maxDurationUs", timing->max_duration_us);
    key_value(writer, "totalDurationUs", timing->total_duration_us);
    key_value(writer, "firstStartedAtDeviceUs", timing->first_started_at_us);
    key_value(writer, "lastFinishedAtDeviceUs", timing->last_finished_at_us);
    key_value(writer, "inFlightStartedAtDeviceUs", timing->in_flight_started_at_us);
    bwg_jw_object_end(writer);
}

static void write_share(bwg_jw_t *writer, const bwg_v2_share_t *share)
{
    const bool acked = share->ack_at_us != BWG_V2_NONE;
    bwg_jw_object_begin(writer);
    key_value(writer, "dispatchSequence", share->dispatch_sequence);
    key_value(writer, "asicJobId", share->asic_job_id);
    key_digest(writer, "workFieldsSha256", true, share->work_fields_sha256);
    key_value(writer, "dispatchedAtDeviceUs", share->dispatched_at_us);
    key_value(writer, "nonceAtDeviceUs", share->nonce_at_us);
    key_value(writer, "writeStartedAtDeviceUs", share->write_started_at_us);
    key_value(writer, "writeCompletedAtDeviceUs", share->write_completed_at_us);
    key_value(writer, "nonce", share->nonce);
    key_value(writer, "versionBits", share->version_bits);
    key_value(writer, "asicIndex", share->asic_index);
    key_value(writer, "coreId", share->core_id);
    key_value(writer, "smallCoreId", share->small_core_id);
    key_value(writer, "channelId", share->channel_id);
    key_value(writer, "jobId", share->job_id);
    key_value(writer, "submissionSequence", share->submission_sequence);
    key_value(writer, "ntime", share->ntime);
    key_value(writer, "version", share->version);
    key_value(writer, "ackAtDeviceUs", share->ack_at_us);
    key_value(writer, "ackLastSequence", acked ? share->ack_last_sequence : BWG_V2_NONE);
    key_value(writer, "ackAcceptedCount", acked ? share->ack_accepted_count : BWG_V2_NONE);
    key_value(writer, "ackSharesSum", acked ? share->ack_shares_sum : BWG_V2_NONE);
    key_value(writer, "matchedSubmitCount", acked ? 1u : BWG_V2_NONE);
    bwg_jw_object_end(writer);
}

static void write_failure(bwg_jw_t *writer, const bwg_v2_failure_t *failure)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "stage");
    bwg_jw_cstr(writer, STAGE_TEXT[failure->stage]);
    bwg_jw_key(writer, "category");
    bwg_jw_cstr(writer, FAILURE_TEXT[failure->category]);
    key_value(writer, "atDeviceUs", failure->at_us);
    bwg_jw_object_end(writer);
}

static void write_resources(bwg_jw_t *writer, const bwg_v2_record_t *record)
{
    bwg_jw_key(writer, "resources");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "socketClosed");
    bwg_jw_bool(writer, record->socket_closed);
    bwg_jw_key(writer, "workerQuiescent");
    bwg_jw_bool(writer, record->worker_quiescent);
    bwg_jw_key(writer, "fenceRetained");
    bwg_jw_bool(writer, record->fence_retained);
    key_value(writer, "socketClosedAtUs", record->socket_closed_at_us);
    key_value(writer, "workerQuiescentAtUs", record->worker_quiescent_at_us);
    bwg_jw_object_end(writer);
}

void bwg_v2_write_record(bwg_jw_t *writer, const bwg_v2_record_t *record)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-v2-serial-evidence-v1");
    bwg_jw_key(writer, "scope");
    bwg_jw_cstr(writer, scope_text(record->scope));
    bwg_jw_key(writer, "attemptId");
    bwg_jw_cstr(writer, record->attempt_id);
    key_value(writer, "bootOrdinal", record->boot_ordinal);
    key_value(writer, "workerGeneration", record->worker_generation);
    key_value(writer, "poolSessionGeneration", record->pool_session_generation);
    key_value(writer, "poolTransportEpoch", record->pool_transport_epoch);
    key_value(writer, "serialTransportEpoch", record->serial_transport_epoch);
    key_digest(writer, "jobCommitment", bwg_v2_record_committed(record), record->job_commitment);
    key_value(writer, "observedAtUs", record->observed_at_us);
    bwg_jw_key(writer, "state");
    bwg_jw_cstr(writer, STATE_TEXT[record->state]);
    key_value(writer, "admittedAtDeviceUs", record->admitted_at_us);
    key_value(writer, "authorityDeadlineDeviceUs", record->authority_deadline_us);
    key_value(writer, "observationDeadlineDeviceUs", record->observation_deadline_us);
    key_value(writer, "terminalAtDeviceUs", record->terminal_at_us);
    bwg_jw_key(writer, "outcome");
    if (record->outcome == BWG_V2_OUTCOME_NONE) {
        bwg_jw_null(writer);
    } else {
        bwg_jw_cstr(writer, OUTCOME_TEXT[record->outcome]);
    }
    bwg_jw_key(writer, "events");
    bwg_jw_array_begin(writer);
    for (uint32_t i = 0; i < record->event_count; i++) {
        write_event(writer, &record->events[i]);
    }
    bwg_jw_array_end(writer);
    bwg_jw_key(writer, "timings");
    bwg_jw_array_begin(writer);
    for (int i = 0; i < BWG_V2_OP_COUNT; i++) {
        write_timing(writer, (bwg_v2_operation_t)i, &record->timings[i]);
    }
    bwg_jw_array_end(writer);
    bwg_jw_key(writer, "shareFacts");
    bwg_jw_array_begin(writer);
    for (uint32_t i = 0; i < record->share_count; i++) {
        write_share(writer, &record->shares[i]);
    }
    bwg_jw_array_end(writer);
    bwg_jw_key(writer, "firstFailure");
    if (bwg_v2_record_failed(record)) {
        write_failure(writer, &record->first_failure);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_key(writer, "secondaryFailures");
    bwg_jw_array_begin(writer);
    for (uint32_t i = 0; i < record->secondary_count; i++) {
        write_failure(writer, &record->secondary[i]);
    }
    bwg_jw_array_end(writer);
    write_resources(writer, record);
    bwg_jw_object_end(writer);
}

static void write_address(bwg_jw_t *writer, uint32_t ipv4)
{
    char text[BWG_V2_IPV4_TEXT_SIZE];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", (unsigned)(ipv4 >> 24), (unsigned)((ipv4 >> 16) & 0xffu), (unsigned)((ipv4 >> 8) & 0xffu),
             (unsigned)(ipv4 & 0xffu));
    bwg_jw_cstr(writer, text);
}

static void write_socket(bwg_jw_t *writer, const bwg_v2_socket_t *socket)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "localIpv4");
    write_address(writer, socket->local_ipv4);
    key_value(writer, "localPort", socket->local_port);
    bwg_jw_key(writer, "remoteIpv4");
    write_address(writer, socket->remote_ipv4);
    key_value(writer, "remotePort", socket->remote_port);
    bwg_jw_object_end(writer);
}

/** Another task can add to the record after the observation is taken, and a status is never older than what it reports. */
static uint64_t observed_at(const bwg_v2_observation_t *observation, const bwg_v2_record_t *maybe_record)
{
    if (maybe_record == NULL || maybe_record->observed_at_us == BWG_V2_NONE) {
        return observation->observed_at_us;
    }
    return maybe_record->observed_at_us > observation->observed_at_us ? maybe_record->observed_at_us : observation->observed_at_us;
}

static void write_observation(bwg_jw_t *writer, const bwg_v2_observation_t *observation, const bwg_v2_record_t *maybe_record)
{
    bwg_jw_key(writer, "observation");
    bwg_jw_object_begin(writer);
    key_value(writer, "bootOrdinal", observation->boot_ordinal);
    key_value(writer, "workerGeneration", observation->worker_generation);
    key_value(writer, "serialTransportEpoch", observation->serial_transport_epoch);
    key_value(writer, "observedAtUs", observation->clock_valid ? observed_at(observation, maybe_record) : BWG_V2_NONE);
    bwg_jw_key(writer, "clockValid");
    bwg_jw_bool(writer, observation->clock_valid);
    bwg_jw_key(writer, "stationIpv4");
    if (observation->wifi_connected && observation->station_ipv4[0] != '\0') {
        bwg_jw_cstr(writer, observation->station_ipv4);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_key(writer, "wifiConnected");
    bwg_jw_bool(writer, observation->wifi_connected);
    bwg_jw_key(writer, "socket");
    if (maybe_record != NULL && maybe_record->connection_at_us != BWG_V2_NONE && !maybe_record->socket_closed) {
        write_socket(writer, &maybe_record->socket);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_object_end(writer);
}

static void write_connection(bwg_jw_t *writer, const bwg_v2_record_t *maybe_record)
{
    bwg_jw_key(writer, "connection");
    if (maybe_record == NULL || maybe_record->connection_at_us == BWG_V2_NONE) {
        bwg_jw_null(writer);
        return;
    }
    bwg_jw_object_begin(writer);
    key_value(writer, "observedAtUs", maybe_record->connection_at_us);
    key_value(writer, "bootOrdinal", maybe_record->boot_ordinal);
    key_value(writer, "workerGeneration", maybe_record->worker_generation);
    key_value(writer, "serialTransportEpoch", maybe_record->serial_transport_epoch);
    key_value(writer, "poolSessionGeneration", maybe_record->pool_session_generation);
    key_value(writer, "poolTransportEpoch", maybe_record->pool_transport_epoch);
    bwg_jw_key(writer, "socket");
    write_socket(writer, &maybe_record->socket);
    bwg_jw_object_end(writer);
}

void bwg_v2_write_status(bwg_jw_t *writer, bwg_v2_scope_t scope, const bwg_v2_observation_t *observation,
                         const bwg_v2_record_t *maybe_record)
{
    const bwg_v2_record_t *record = maybe_record != NULL && maybe_record->scope == scope ? maybe_record : NULL;
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-stratum-v2-status-v1");
    bwg_jw_key(writer, "scope");
    bwg_jw_cstr(writer, scope_text(scope));
    bwg_jw_key(writer, "state");
    bwg_jw_cstr(writer, STATE_TEXT[record != NULL ? record->state : BWG_V2_STATE_IDLE]);
    write_observation(writer, observation, record);
    write_connection(writer, record);
    bwg_jw_key(writer, "record");
    if (record != NULL) {
        bwg_v2_write_record(writer, record);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_object_end(writer);
}
