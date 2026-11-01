#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#include "bwg_test.h"
#include "bwg_v2_record.h"
#include "test_suites.h"

#define ADMIT_US 2000000ull
#define ARMED_MS 2100ull
#define ATTEMPT "AAAAAAAAAAAAAAAAAAAAAA"
#define MSG_SETUP 0x01
#define MSG_CHANNEL 0x11
#define MSG_JOB 0x15
#define MSG_PREV_HASH 0x20
#define MSG_TARGET 0x21
#define MSG_ACK 0x1c
#define CHANNEL_ID 9u
#define JOB_ID 7u

static bwg_v2_record_t s_record;
static uint8_t s_status[BWG_V2_STATUS_BYTES];

static bwg_v2_observation_t observation_at(uint64_t now_us)
{
    bwg_v2_observation_t observation = {
        .boot_ordinal = 2,
        .worker_generation = 7,
        .serial_transport_epoch = 4,
        .observed_at_us = now_us,
        .clock_valid = true,
        .wifi_connected = true,
    };
    strcpy(observation.station_ipv4, "192.168.1.2");
    return observation;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void header_of(uint8_t header[6], uint16_t extension, uint8_t type, size_t length)
{
    header[0] = (uint8_t)extension;
    header[1] = (uint8_t)(extension >> 8);
    header[2] = type;
    header[3] = (uint8_t)length;
    header[4] = (uint8_t)(length >> 8);
    header[5] = (uint8_t)(length >> 16);
}

static bool feed(uint16_t extension, uint8_t type, uint32_t first, uint32_t second, uint64_t now_us)
{
    uint8_t header[6];
    uint8_t payload[8];
    put_u32(payload, first);
    put_u32(payload + 4, second);
    header_of(header, extension, type, sizeof(payload));
    return bwg_v2_record_frame(&s_record, header, payload, sizeof(payload), now_us);
}

static void feed_job_frames(void)
{
    feed(0, MSG_SETUP, 0, 0, ADMIT_US + 100);
    feed(0, MSG_CHANNEL, 1, CHANNEL_ID, ADMIT_US + 200);
    feed(BWG_V2_EXTENSION_CHANNEL, MSG_JOB, CHANNEL_ID, JOB_ID, ADMIT_US + 300);
    feed(BWG_V2_EXTENSION_CHANNEL, MSG_TARGET, CHANNEL_ID, 0, ADMIT_US + 400);
    feed(BWG_V2_EXTENSION_CHANNEL, MSG_PREV_HASH, CHANNEL_ID, JOB_ID, ADMIT_US + 500);
}

static bool feed_ack(uint32_t channel_id, uint32_t sequence, uint32_t accepted, uint64_t sum, uint64_t now_us)
{
    uint8_t header[6];
    uint8_t payload[20];
    put_u32(payload, channel_id);
    put_u32(payload + 4, sequence);
    put_u32(payload + 8, accepted);
    put_u32(payload + 12, (uint32_t)sum);
    put_u32(payload + 16, (uint32_t)(sum >> 32));
    header_of(header, BWG_V2_EXTENSION_CHANNEL, MSG_ACK, sizeof(payload));
    return bwg_v2_record_frame(&s_record, header, payload, sizeof(payload), now_us);
}

static void admit(bwg_v2_scope_t scope)
{
    const bwg_v2_observation_t observation = observation_at(ADMIT_US);
    bwg_v2_record_admit(&s_record, scope, ATTEMPT, &observation);
}

static void share_with_write(uint32_t sequence, uint32_t nonce, uint64_t now_us)
{
    const uint8_t digest[BWG_V2_DIGEST_BYTES] = {1};
    bwg_v2_record_nonce(&s_record, 0, digest, nonce, 0x20002000u, 0x2000u, 0, 3, 1, now_us);
    bwg_v2_record_submission_begin(&s_record, CHANNEL_ID, JOB_ID, sequence, nonce, 1000, 0x20002000u, now_us + 1);
    bwg_v2_record_submission_end(&s_record, sequence, true, now_us + 2);
}

static void running_share(void)
{
    const uint8_t digest[BWG_V2_DIGEST_BYTES] = {1};
    admit(BWG_V2_SCOPE_SHARE);
    bwg_v2_record_bind_pool(&s_record, 1, 1);
    bwg_v2_record_event(&s_record, BWG_V2_STAGE_PREPARING, ADMIT_US + 50, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    feed_job_frames();
    bwg_v2_record_budget_armed(&s_record, ARMED_MS, BWG_V2_SHARE_LIMIT_MS, ARMED_MS * 1000 + 1);
    bwg_v2_record_dispatch(&s_record, 0, digest, ARMED_MS * 1000 + 2);
}

static yyjson_doc *status_doc(bwg_v2_scope_t scope, const bwg_v2_record_t *maybe_record, const bwg_v2_observation_t *observation)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, s_status, sizeof(s_status));
    bwg_v2_write_status(&writer, scope, observation, maybe_record);
    return bwg_jw_ok(&writer) ? yyjson_read((const char *)s_status, writer.len, 0) : NULL;
}

static void admission_starts_a_channel_with_both_deadlines(void)
{
    // Act
    admit(BWG_V2_SCOPE_CHANNEL);

    // Assert
    ASSERT_EQ(s_record.state, BWG_V2_STATE_ADMITTED);
    ASSERT_EQ(s_record.authority_deadline_us, ADMIT_US + BWG_V2_AUTHORITY_US);
    ASSERT_EQ(s_record.observation_deadline_us, ADMIT_US + BWG_V2_OBSERVATION_US);
    ASSERT_TRUE(bwg_v2_record_has_stage(&s_record, BWG_V2_STAGE_ADMITTED));
}

static void a_share_has_no_deadline_until_its_budget_arms(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_SHARE);
    ASSERT_EQ(s_record.authority_deadline_us, BWG_V2_NONE);

    // Act
    bool armed = bwg_v2_record_budget_armed(&s_record, ARMED_MS, BWG_V2_SHARE_LIMIT_MS, ARMED_MS * 1000 + 1);

    // Assert
    ASSERT_TRUE(armed);
    ASSERT_EQ(s_record.authority_deadline_us, (ARMED_MS + BWG_V2_SHARE_LIMIT_MS) * 1000);
}

static void a_budget_clock_from_before_admission_is_refused(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_SHARE);

    // Act
    bool armed = bwg_v2_record_budget_armed(&s_record, ADMIT_US / 1000 - 1, BWG_V2_SHARE_LIMIT_MS, ADMIT_US + 1);

    // Assert
    ASSERT_FALSE(armed);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_CLOCK);
}

static void the_first_failure_stays_and_only_cleanup_or_clock_add(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    bwg_v2_record_fail(&s_record, BWG_V2_STAGE_SETUP, BWG_V2_FAILURE_PROTOCOL, 1);
    bwg_v2_record_fail(&s_record, BWG_V2_STAGE_JOB, BWG_V2_FAILURE_EOF, 2);
    bwg_v2_record_fail(&s_record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_CLEANUP, 3);
    bwg_v2_record_fail(&s_record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_CLEANUP, 4);

    // Assert
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_PROTOCOL);
    ASSERT_EQ(s_record.secondary_count, 1);
    ASSERT_EQ(s_record.secondary[0].category, BWG_V2_FAILURE_CLEANUP);
}

static void events_past_the_bound_become_an_evidence_failure(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    for (uint32_t i = 0; i < BWG_V2_MAX_EVENTS + 1; i++) {
        bwg_v2_record_event(&s_record, BWG_V2_STAGE_NONCE, ADMIT_US + i, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    }

    // Assert
    ASSERT_EQ(s_record.event_count, BWG_V2_MAX_EVENTS);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_EVIDENCE);
}

static void an_operation_timing_sums_every_run(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    bwg_v2_record_begin(&s_record, BWG_V2_OP_FRAME_READ, ADMIT_US + 10);
    bwg_v2_record_end(&s_record, BWG_V2_OP_FRAME_READ, ADMIT_US + 30, false);
    bwg_v2_record_begin(&s_record, BWG_V2_OP_FRAME_READ, ADMIT_US + 40);
    bwg_v2_record_end(&s_record, BWG_V2_OP_FRAME_READ, ADMIT_US + 45, true);

    // Assert
    const bwg_v2_timing_t *timing = &s_record.timings[BWG_V2_OP_FRAME_READ];
    ASSERT_EQ(timing->count, 2);
    ASSERT_EQ(timing->failed_count, 1);
    ASSERT_EQ(timing->max_duration_us, 20);
    ASSERT_EQ(timing->total_duration_us, 25);
    ASSERT_EQ(timing->first_started_at_us, ADMIT_US + 10);
    ASSERT_EQ(timing->last_finished_at_us, ADMIT_US + 45);
    ASSERT_EQ(timing->in_flight_started_at_us, BWG_V2_NONE);
}

static void a_second_begin_in_flight_is_an_evidence_failure(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);
    bwg_v2_record_begin(&s_record, BWG_V2_OP_CONNECT, ADMIT_US + 1);

    // Act
    bwg_v2_record_begin(&s_record, BWG_V2_OP_CONNECT, ADMIT_US + 2);

    // Assert
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_EVIDENCE);
}

static void a_clock_going_back_nulls_the_observed_time(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    bwg_v2_record_event(&s_record, BWG_V2_STAGE_PREPARING, ADMIT_US - 1, BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);

    // Assert
    ASSERT_EQ(s_record.observed_at_us, BWG_V2_NONE);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_CLOCK);
}

static void a_second_connection_is_refused_as_extra(void)
{
    // Arrange
    const bwg_v2_socket_t socket = {.local_ipv4 = 0xc0a80102u, .local_port = 1, .remote_ipv4 = 0xc0a80103u, .remote_port = 2};
    admit(BWG_V2_SCOPE_SHARE);
    ASSERT_TRUE(bwg_v2_record_connected(&s_record, &socket, ADMIT_US + 1));

    // Act
    bool second = bwg_v2_record_connected(&s_record, &socket, ADMIT_US + 2);

    // Assert
    ASSERT_FALSE(second);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_EXTRA);
}

static void the_job_commitment_covers_the_four_frames_length_prefixed(void)
{
    // Arrange
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    const struct {
        uint16_t extension;
        uint8_t type;
        uint32_t first;
        uint32_t second;
    } frames[] = {
        {0, MSG_CHANNEL, 1, CHANNEL_ID},
        {BWG_V2_EXTENSION_CHANNEL, MSG_JOB, CHANNEL_ID, JOB_ID},
        {BWG_V2_EXTENSION_CHANNEL, MSG_TARGET, CHANNEL_ID, 0},
        {BWG_V2_EXTENSION_CHANNEL, MSG_PREV_HASH, CHANNEL_ID, JOB_ID},
    };
    for (size_t i = 0; i < 4; i++) {
        uint8_t length[4];
        uint8_t encoded[14];
        put_u32(length, sizeof(encoded));
        header_of(encoded, frames[i].extension, frames[i].type, 8);
        put_u32(encoded + 6, frames[i].first);
        put_u32(encoded + 10, frames[i].second);
        crypto_hash_sha256_update(&state, length, sizeof(length));
        crypto_hash_sha256_update(&state, encoded, sizeof(encoded));
    }
    uint8_t expected[BWG_V2_DIGEST_BYTES];
    crypto_hash_sha256_final(&state, expected);
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    feed_job_frames();

    // Assert
    ASSERT_TRUE(s_record.has_job_commitment);
    ASSERT_EQ(memcmp(s_record.job_commitment, expected, sizeof(expected)), 0);
    ASSERT_TRUE(bwg_v2_record_has_stage(&s_record, BWG_V2_STAGE_WORK_READY));
}

static void a_job_frame_out_of_order_ends_the_attempt(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    bool accepted = feed(BWG_V2_EXTENSION_CHANNEL, MSG_JOB, CHANNEL_ID, JOB_ID, ADMIT_US + 1);

    // Assert
    ASSERT_FALSE(accepted);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_PROTOCOL);
}

static void a_share_joins_dispatch_nonce_write_and_ack(void)
{
    // Arrange
    running_share();
    const uint64_t at = ARMED_MS * 1000 + 10;
    share_with_write(5, 0x1234, at);

    // Act
    bool settled = feed_ack(CHANNEL_ID, 5, 1, BWG_V2_ACK_SHARES_SUM, at + 10);

    // Assert
    const bwg_v2_share_t *share = &s_record.shares[0];
    ASSERT_TRUE(settled);
    ASSERT_EQ(s_record.share_count, 1);
    ASSERT_EQ(share->asic_job_id, 0);
    ASSERT_EQ(share->dispatch_sequence, 1);
    ASSERT_EQ(share->nonce_at_us, at);
    ASSERT_EQ(share->write_started_at_us, at + 1);
    ASSERT_EQ(share->write_completed_at_us, at + 2);
    ASSERT_EQ(share->ack_at_us, at + 10);
    ASSERT_TRUE(bwg_v2_record_has_accepted_share(&s_record));
    ASSERT_TRUE(bwg_v2_record_has_stage(&s_record, BWG_V2_STAGE_ACCEPTED));
    ASSERT_FALSE(s_record.has_first_failure);
}

static void a_submit_without_its_nonce_must_not_be_sent(void)
{
    // Arrange
    running_share();

    // Act
    bool allowed = bwg_v2_record_submission_begin(&s_record, CHANNEL_ID, JOB_ID, 0, 1, 1000, 0x20000000u, ARMED_MS * 1000 + 10);

    // Assert
    ASSERT_FALSE(allowed);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_EVIDENCE);
}

static void a_nonce_of_unknown_work_is_ignored(void)
{
    // Arrange
    const uint8_t other[BWG_V2_DIGEST_BYTES] = {2};
    running_share();
    const uint32_t events = s_record.event_count;

    // Act
    bwg_v2_record_nonce(&s_record, 0, other, 1, 0x20000000u, 0, 0, 0, 0, ARMED_MS * 1000 + 10);

    // Assert
    ASSERT_EQ(s_record.event_count, events);
    ASSERT_FALSE(s_record.pending_nonce.valid);
}

static void a_repeated_ack_gains_no_second_credit(void)
{
    // Arrange
    running_share();
    const uint64_t at = ARMED_MS * 1000 + 10;
    share_with_write(0, 1, at);
    ASSERT_TRUE(feed_ack(CHANNEL_ID, 0, 1, BWG_V2_ACK_SHARES_SUM, at + 10));

    // Act
    bool second = feed_ack(CHANNEL_ID, 0, 1, BWG_V2_ACK_SHARES_SUM, at + 11);

    // Assert
    ASSERT_FALSE(second);
    ASSERT_EQ(s_record.shares[0].ack_at_us, at + 10);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_EVIDENCE);
}

static void an_unfinished_write_cannot_be_settled(void)
{
    // Arrange
    const uint8_t digest[BWG_V2_DIGEST_BYTES] = {1};
    const uint64_t at = ARMED_MS * 1000 + 10;
    running_share();
    bwg_v2_record_nonce(&s_record, 0, digest, 1, 0x20002000u, 0x2000u, 0, 3, 1, at);
    bwg_v2_record_submission_begin(&s_record, CHANNEL_ID, JOB_ID, 0, 1, 1000, 0x20002000u, at + 1);

    // Act
    bool settled = feed_ack(CHANNEL_ID, 0, 1, BWG_V2_ACK_SHARES_SUM, at + 2);

    // Assert
    ASSERT_FALSE(settled);
    ASSERT_EQ(s_record.shares[0].ack_at_us, BWG_V2_NONE);
}

static void a_later_ack_cannot_skip_an_unsettled_submission(void)
{
    // Arrange
    running_share();
    const uint64_t at = ARMED_MS * 1000 + 10;
    share_with_write(0, 1, at);
    share_with_write(1, 2, at + 5);

    // Act
    bool settled = feed_ack(CHANNEL_ID, 1, 1, BWG_V2_ACK_SHARES_SUM, at + 20);

    // Assert
    ASSERT_FALSE(settled);
    ASSERT_EQ(s_record.shares[0].ack_at_us, BWG_V2_NONE);
    ASSERT_EQ(s_record.shares[1].ack_at_us, BWG_V2_NONE);
}

static void an_ack_must_match_channel_count_and_sum(void)
{
    // Arrange
    const uint64_t at = ARMED_MS * 1000 + 10;
    const struct {
        uint32_t channel;
        uint32_t accepted;
        uint64_t sum;
    } refused[] = {{CHANNEL_ID + 1, 1, BWG_V2_ACK_SHARES_SUM}, {CHANNEL_ID, 2, BWG_V2_ACK_SHARES_SUM}, {CHANNEL_ID, 1, 1}};

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        running_share();
        share_with_write(0, 1, at);

        // Act
        bool settled = feed_ack(refused[i].channel, 0, refused[i].accepted, refused[i].sum, at + 10);

        // Assert
        ASSERT_FALSE(settled);
        ASSERT_EQ(s_record.shares[0].ack_at_us, BWG_V2_NONE);
    }
}

static void a_share_finishes_accepted_only_once_network_and_fence_release(void)
{
    // Arrange
    running_share();
    const uint64_t at = ARMED_MS * 1000 + 10;
    share_with_write(0, 1, at);
    feed_ack(CHANNEL_ID, 0, 1, BWG_V2_ACK_SHARES_SUM, at + 10);

    // Act / Assert
    bwg_v2_record_socket_closed(&s_record, at + 20);
    bwg_v2_record_joined(&s_record, at + 30, false);
    ASSERT_EQ(s_record.outcome, BWG_V2_OUTCOME_NONE);
    ASSERT_TRUE(s_record.fence_retained);
    ASSERT_TRUE(bwg_v2_record_release_fence(&s_record, at + 40));
    ASSERT_EQ(s_record.outcome, BWG_V2_OUTCOME_ACCEPTED);
    ASSERT_EQ(s_record.state, BWG_V2_STATE_TERMINAL);
    ASSERT_EQ(s_record.terminal_at_us, at + 40);
}

static void a_share_without_an_acked_share_is_rejected(void)
{
    // Arrange
    running_share();
    const uint64_t at = ARMED_MS * 1000 + 10;

    // Act
    bwg_v2_record_socket_closed(&s_record, at);
    bwg_v2_record_joined(&s_record, at + 1, true);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_V2_OUTCOME_REJECTED);
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_EVIDENCE);
}

static void a_channel_finishes_accepted_after_work_ready(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);
    feed_job_frames();

    // Act
    bwg_v2_record_socket_closed(&s_record, ADMIT_US + 600);
    bwg_v2_record_joined(&s_record, ADMIT_US + 700, true);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_V2_OUTCOME_ACCEPTED);
}

static void a_channel_past_its_authority_deadline_times_out(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    bwg_v2_record_tick(&s_record, ADMIT_US + BWG_V2_AUTHORITY_US);

    // Assert
    ASSERT_EQ(s_record.first_failure.category, BWG_V2_FAILURE_TIMEOUT);
    ASSERT_EQ(s_record.outcome, BWG_V2_OUTCOME_NONE);
}

static void a_channel_past_its_observation_deadline_is_incomplete(void)
{
    // Arrange
    admit(BWG_V2_SCOPE_CHANNEL);

    // Act
    bwg_v2_record_tick(&s_record, ADMIT_US + BWG_V2_OBSERVATION_US);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_V2_OUTCOME_INCOMPLETE);
    ASSERT_EQ(s_record.state, BWG_V2_STATE_TERMINAL);
}

static void a_full_record_fits_its_status_bound(void)
{
    // Arrange
    running_share();
    const uint64_t at = ARMED_MS * 1000 + 10;
    for (uint32_t i = 0; i < BWG_V2_MAX_SHARES; i++) {
        share_with_write(i, i + 1, at + i * 10);
    }
    while (s_record.event_count < BWG_V2_MAX_EVENTS) {
        const uint8_t digest[BWG_V2_DIGEST_BYTES] = {3};
        bwg_v2_record_event(&s_record, BWG_V2_STAGE_JOB, at + 1000, 1, 1, 1, digest);
    }
    const bwg_v2_observation_t observation = observation_at(at + 2000);

    // Act
    yyjson_doc *doc = status_doc(BWG_V2_SCOPE_SHARE, &s_record, &observation);

    // Assert
    ASSERT_TRUE(doc != NULL);
    yyjson_val *record = yyjson_obj_get(yyjson_doc_get_root(doc), "record");
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(record, "events")), BWG_V2_MAX_EVENTS);
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(record, "timings")), BWG_V2_OP_COUNT);
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(record, "shareFacts")), BWG_V2_MAX_SHARES);
    yyjson_doc_free(doc);
}

static void a_nonce_of_a_slot_holding_other_work_is_ignored(void)
{
    // Arrange
    const uint8_t digest[BWG_V2_DIGEST_BYTES] = {1};
    running_share();
    const uint64_t events = s_record.event_count;

    // Act
    bwg_v2_record_nonce(&s_record, 5, digest, 1, 0x20000000u, 0, 0, 0, 0, ARMED_MS * 1000 + 10);

    // Assert
    ASSERT_EQ(s_record.event_count, events);
    ASSERT_TRUE(!s_record.pending_nonce.valid);
}

void bwg_tests_v2_record(void)
{
    BWG_RUN(admission_starts_a_channel_with_both_deadlines);
    BWG_RUN(a_share_has_no_deadline_until_its_budget_arms);
    BWG_RUN(a_budget_clock_from_before_admission_is_refused);
    BWG_RUN(the_first_failure_stays_and_only_cleanup_or_clock_add);
    BWG_RUN(events_past_the_bound_become_an_evidence_failure);
    BWG_RUN(an_operation_timing_sums_every_run);
    BWG_RUN(a_second_begin_in_flight_is_an_evidence_failure);
    BWG_RUN(a_clock_going_back_nulls_the_observed_time);
    BWG_RUN(a_second_connection_is_refused_as_extra);
    BWG_RUN(the_job_commitment_covers_the_four_frames_length_prefixed);
    BWG_RUN(a_job_frame_out_of_order_ends_the_attempt);
    BWG_RUN(a_share_joins_dispatch_nonce_write_and_ack);
    BWG_RUN(a_submit_without_its_nonce_must_not_be_sent);
    BWG_RUN(a_nonce_of_unknown_work_is_ignored);
    BWG_RUN(a_repeated_ack_gains_no_second_credit);
    BWG_RUN(an_unfinished_write_cannot_be_settled);
    BWG_RUN(a_later_ack_cannot_skip_an_unsettled_submission);
    BWG_RUN(an_ack_must_match_channel_count_and_sum);
    BWG_RUN(a_share_finishes_accepted_only_once_network_and_fence_release);
    BWG_RUN(a_share_without_an_acked_share_is_rejected);
    BWG_RUN(a_channel_finishes_accepted_after_work_ready);
    BWG_RUN(a_channel_past_its_authority_deadline_times_out);
    BWG_RUN(a_channel_past_its_observation_deadline_is_incomplete);
    BWG_RUN(a_nonce_of_a_slot_holding_other_work_is_ignored);
    BWG_RUN(a_full_record_fits_its_status_bound);
}
