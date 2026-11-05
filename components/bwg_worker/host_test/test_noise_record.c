#include <string.h>
#include <yyjson.h>

#include "bwg_noise_record.h"
#include "bwg_test.h"
#include "test_suites.h"

#define ADMIT_US 2000000ull
#define ATTEMPT "AAAAAAAAAAAAAAAAAAAAAA"
#define DIGEST "0000000000000000000000000000000000000000000000000000000000000000"
#define PORT 50000u
#define FINISHED_US (ADMIT_US + 80000ull)

static bwg_noise_record_t s_record;
static uint8_t s_status[BWG_NOISE_STATUS_BYTES];

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

static void admit_running(void)
{
    const bwg_v2_observation_t observation = observation_at(ADMIT_US);
    ASSERT_TRUE(bwg_noise_record_admit(&s_record, ATTEMPT, DIGEST, &observation));
    ASSERT_TRUE(bwg_noise_record_dispatch(&s_record, ADMIT_US + 1000));
}

/** Everything up to the point the worker closes its socket. */
static void run_to_proof(void)
{
    admit_running();
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_NOISE_PREPARED, ADMIT_US + 10000, 9000, 0));
    bwg_noise_record_socket_opened(&s_record, PORT);
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_TCP_CONNECTED, ADMIT_US + 20000, 9000, 0));
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_ACT_ONE_WRITTEN, ADMIT_US + 30000, 1000, 64));
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_ACT_TWO_RECEIVED, ADMIT_US + 40000, 9000, 234));
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_AUTHORITY_VERIFIED, ADMIT_US + 50000, 9000, 0));
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_PROOF_WRITTEN, ADMIT_US + 60000, 1000, 22));
}

static void run_accepted(void)
{
    run_to_proof();
    bwg_noise_record_cleanup(&s_record, ADMIT_US + 65000);
    bwg_noise_record_socket_closed(&s_record, ADMIT_US + 70000, 5000);
    bwg_noise_record_joined(&s_record, FINISHED_US);
}

static void fail_at_read(bwg_noise_detail_t detail)
{
    admit_running();
    bwg_noise_record_fail(&s_record, BWG_NOISE_STAGE_ACT_TWO_RECEIVED, BWG_NOISE_CATEGORY_READ, detail, ADMIT_US + 30000);
}

static yyjson_doc *status_doc(const bwg_noise_record_t *maybe_record, uint64_t observed_at_us)
{
    const bwg_v2_observation_t observation = observation_at(observed_at_us);
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, s_status, sizeof(s_status));
    bwg_noise_write_status(&writer, &observation, maybe_record);
    return bwg_jw_ok(&writer) ? yyjson_read((const char *)s_status, writer.len, 0) : NULL;
}

static yyjson_val *at(yyjson_doc *doc, const char *pointer)
{
    return yyjson_doc_ptr_get(doc, pointer);
}

static void a_full_exchange_is_accepted(void)
{
    // Arrange / Act
    run_accepted();

    // Assert
    ASSERT_EQ(s_record.state, BWG_NOISE_STATE_TERMINAL);
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_ACCEPTED);
    ASSERT_EQ(s_record.first_failure.category, BWG_NOISE_CATEGORY_NONE);
    ASSERT_EQ(s_record.deadline_met, BWG_NOISE_DEADLINE_MET);
}

static void a_stage_out_of_order_fails_the_evidence(void)
{
    // Arrange
    admit_running();

    // Act
    const bool added = bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_TCP_CONNECTED, ADMIT_US + 5000, 100, 0);

    // Assert
    ASSERT_FALSE(added);
    ASSERT_EQ(s_record.first_failure.stage, BWG_NOISE_AT_EVIDENCE);
    ASSERT_EQ(s_record.first_failure.detail, BWG_NOISE_DETAIL_MALFORMED);
}

static void a_repeated_stage_fails_the_evidence(void)
{
    // Arrange
    admit_running();
    ASSERT_TRUE(bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_NOISE_PREPARED, ADMIT_US + 5000, 100, 0));

    // Act
    const bool added = bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_NOISE_PREPARED, ADMIT_US + 6000, 100, 0);

    // Assert
    ASSERT_FALSE(added);
    ASSERT_EQ(s_record.first_failure.detail, BWG_NOISE_DETAIL_DUPLICATE);
}

static void the_first_failure_stays(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_EOF);

    // Act
    bwg_noise_record_fail(&s_record, BWG_NOISE_AT_CLEANUP, BWG_NOISE_CATEGORY_CLEANUP, BWG_NOISE_DETAIL_IO, ADMIT_US + 40000);

    // Assert
    ASSERT_EQ(s_record.first_failure.detail, BWG_NOISE_DETAIL_EOF);
}

static void a_failure_closes_permission_and_starts_cleanup(void)
{
    // Arrange / Act
    fail_at_read(BWG_NOISE_DETAIL_EOF);

    // Assert
    ASSERT_FALSE(bwg_noise_record_permitted(&s_record));
    ASSERT_EQ(s_record.state, BWG_NOISE_STATE_CANCELLING);
    ASSERT_EQ(s_record.cleanup_started_at_us, ADMIT_US + 30000);
    ASSERT_EQ(s_record.cleanup_deadline_at_us, ADMIT_US + BWG_NOISE_AUTHORITY_US + BWG_NOISE_TAIL_US);
}

static void a_failed_attempt_can_still_record_its_cleanup(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_EOF);

    // Act
    bwg_noise_record_socket_closed(&s_record, ADMIT_US + 40000, 100);

    // Assert
    ASSERT_EQ(s_record.stage_count, 1);
    ASSERT_EQ(s_record.stages[0].stage, BWG_NOISE_STAGE_SOCKET_CLOSED);
}

static void a_failed_attempt_adds_no_protocol_stage(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_EOF);

    // Act
    const bool added = bwg_noise_record_stage(&s_record, BWG_NOISE_STAGE_NOISE_PREPARED, ADMIT_US + 40000, 100, 0);

    // Assert
    ASSERT_FALSE(added);
}

static void a_rejected_exchange_ends_rejected(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_WRONG_AUTHORITY);

    // Act
    bwg_noise_record_joined(&s_record, ADMIT_US + 40000);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_REJECTED);
}

static void a_cancelled_exchange_ends_cancelled(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_CANCEL_REQUESTED);

    // Act
    bwg_noise_record_joined(&s_record, ADMIT_US + 40000);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_CANCELLED);
}

static void a_replaced_session_ends_cancelled(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_SESSION_REPLACED);

    // Act
    bwg_noise_record_joined(&s_record, ADMIT_US + 40000);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_CANCELLED);
}

static void the_authority_deadline_expires_the_attempt(void)
{
    // Arrange
    admit_running();

    // Act
    bwg_noise_record_tick(&s_record, ADMIT_US + BWG_NOISE_AUTHORITY_US);
    bwg_noise_record_joined(&s_record, ADMIT_US + BWG_NOISE_AUTHORITY_US + 1000);

    // Assert
    ASSERT_EQ(s_record.first_failure.category, BWG_NOISE_CATEGORY_AUTHORITY_LOST);
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_EXPIRED);
    ASSERT_EQ(s_record.deadline_met, BWG_NOISE_DEADLINE_MET);
}

static void a_worker_past_the_tail_leaves_the_attempt_incomplete(void)
{
    // Arrange
    admit_running();

    // Act
    bwg_noise_record_tick(&s_record, ADMIT_US + BWG_NOISE_AUTHORITY_US + BWG_NOISE_TAIL_US);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_INCOMPLETE);
    ASSERT_EQ(s_record.deadline_met, BWG_NOISE_DEADLINE_MISSED);
    ASSERT_EQ(s_record.resource_failure.detail, BWG_NOISE_DETAIL_RESOURCE_UNRELEASED);
}

static void a_late_join_keeps_an_incomplete_outcome(void)
{
    // Arrange
    admit_running();
    bwg_noise_record_tick(&s_record, ADMIT_US + BWG_NOISE_AUTHORITY_US + BWG_NOISE_TAIL_US);

    // Act
    bwg_noise_record_joined(&s_record, ADMIT_US + BWG_NOISE_AUTHORITY_US + BWG_NOISE_TAIL_US + 1000);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_INCOMPLETE);
    ASSERT_EQ(s_record.deadline_met, BWG_NOISE_DEADLINE_MISSED);
    ASSERT_TRUE(s_record.worker_quiescent);
}

static void a_socket_left_open_leaves_the_attempt_incomplete(void)
{
    // Arrange
    run_to_proof();

    // Act
    bwg_noise_record_joined(&s_record, FINISHED_US);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_INCOMPLETE);
}

static void a_clock_going_backward_closes_permission_for_good(void)
{
    // Arrange
    admit_running();

    // Act
    bwg_noise_record_tick(&s_record, ADMIT_US - 1);

    // Assert
    ASSERT_FALSE(s_record.clock_valid);
    ASSERT_EQ(s_record.first_failure.category, BWG_NOISE_CATEGORY_CLOCK_INVALID);
    ASSERT_EQ(s_record.resource_failure.detail, BWG_NOISE_DETAIL_CLOCK_DISCONTINUITY);
}

static void a_clock_failure_ends_incomplete_without_times(void)
{
    // Arrange
    admit_running();
    bwg_noise_record_tick(&s_record, ADMIT_US - 1);

    // Act
    bwg_noise_record_joined(&s_record, ADMIT_US + 1000);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_INCOMPLETE);
    ASSERT_EQ(s_record.decided_at_us, BWG_NOISE_NONE);
    ASSERT_EQ(s_record.released_at_us, BWG_NOISE_NONE);
}

static void a_join_without_every_stage_is_rejected_as_missing_evidence(void)
{
    // Arrange
    admit_running();

    // Act
    bwg_noise_record_joined(&s_record, ADMIT_US + 40000);

    // Assert
    ASSERT_EQ(s_record.outcome, BWG_NOISE_OUTCOME_REJECTED);
    ASSERT_EQ(s_record.first_failure.detail, BWG_NOISE_DETAIL_MISSING);
}

static void an_unrepresentable_deadline_is_refused(void)
{
    // Arrange
    const bwg_v2_observation_t observation = observation_at(9007199254740991ull);

    // Act
    const bool admitted = bwg_noise_record_admit(&s_record, ATTEMPT, DIGEST, &observation);

    // Assert
    ASSERT_FALSE(admitted);
}

static void an_idle_status_has_no_job(void)
{
    // Arrange / Act
    yyjson_doc *doc = status_doc(NULL, ADMIT_US);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_STREQ(yyjson_get_str(at(doc, "/schema")), "worker-noise-diagnostic-status-v2");
    ASSERT_STREQ(yyjson_get_str(at(doc, "/state")), "idle");
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job")));
    yyjson_doc_free(doc);
}

static void a_running_status_has_no_failure_outcome_or_deadline(void)
{
    // Arrange
    admit_running();

    // Act
    yyjson_doc *doc = status_doc(&s_record, ADMIT_US + 2000);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/firstFailure")));
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/terminal")));
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/resources/deadlineMet")));
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/resources/failure")));
    yyjson_doc_free(doc);
}

static void an_accepted_status_met_its_deadline_without_failures(void)
{
    // Arrange
    run_accepted();

    // Act
    yyjson_doc *doc = status_doc(&s_record, FINISHED_US);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/firstFailure")));
    ASSERT_TRUE(yyjson_get_bool(at(doc, "/job/resources/deadlineMet")));
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/resources/failure")));
    yyjson_doc_free(doc);
}

static void an_accepted_status_names_each_stage_and_its_bytes(void)
{
    // Arrange
    run_accepted();

    // Act
    yyjson_doc *doc = status_doc(&s_record, FINISHED_US);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_STREQ(yyjson_get_str(at(doc, "/state")), "terminal");
    ASSERT_STREQ(yyjson_get_str(at(doc, "/job/terminal/outcome")), "accepted");
    ASSERT_EQ(yyjson_arr_size(at(doc, "/job/stages")), BWG_NOISE_STAGE_COUNT);
    ASSERT_STREQ(yyjson_get_str(at(doc, "/job/stages/2/stage")), "act_one_written");
    ASSERT_EQ(yyjson_get_uint(at(doc, "/job/stages/2/bytes")), 64);
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/stages/0/bytes")));
    ASSERT_EQ(yyjson_get_uint(at(doc, "/job/stages/7/sequence")), 8);
    ASSERT_EQ(yyjson_get_uint(at(doc, "/job/localSocketPort")), PORT);
    yyjson_doc_free(doc);
}

static void a_quiescent_stage_matches_the_release(void)
{
    // Arrange / Act
    run_accepted();

    // Assert
    const bwg_noise_stage_row_t *last = &s_record.stages[BWG_NOISE_STAGE_COUNT - 1];
    ASSERT_EQ(last->at_us, s_record.released_at_us);
    ASSERT_EQ(last->duration_us, last->at_us - s_record.cleanup_started_at_us);
}

static void a_status_is_never_older_than_its_record(void)
{
    // Arrange
    run_accepted();

    // Act
    yyjson_doc *doc = status_doc(&s_record, ADMIT_US);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_EQ(yyjson_get_uint(at(doc, "/observation/observedAtUs")), FINISHED_US);
    yyjson_doc_free(doc);
}

static void a_failed_status_names_its_cause(void)
{
    // Arrange
    fail_at_read(BWG_NOISE_DETAIL_CERTIFICATE_TIME);

    // Act
    yyjson_doc *doc = status_doc(&s_record, ADMIT_US + 30000);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_STREQ(yyjson_get_str(at(doc, "/state")), "cancelling");
    ASSERT_STREQ(yyjson_get_str(at(doc, "/job/firstFailure/stage")), "act_two_received");
    ASSERT_STREQ(yyjson_get_str(at(doc, "/job/firstFailure/category")), "read");
    ASSERT_STREQ(yyjson_get_str(at(doc, "/job/firstFailure/detail")), "certificate_time");
    ASSERT_TRUE(yyjson_is_null(at(doc, "/job/terminal")));
    yyjson_doc_free(doc);
}

static void a_full_status_fits_its_reservation(void)
{
    // Arrange
    run_accepted();
    s_record.resource_failure = (bwg_noise_failure_t){
        .stage = BWG_NOISE_AT_CANDIDATE_INVENTORY,
        .category = BWG_NOISE_CATEGORY_EVIDENCE_INCOMPLETE,
        .detail = BWG_NOISE_DETAIL_RESOURCE_UNRELEASED,
        .at_us = FINISHED_US,
    };

    // Act
    yyjson_doc *doc = status_doc(&s_record, FINISHED_US);

    // Assert
    ASSERT_TRUE(doc != NULL);
    yyjson_doc_free(doc);
}

void bwg_tests_noise_record(void)
{
    BWG_RUN(a_full_exchange_is_accepted);
    BWG_RUN(a_stage_out_of_order_fails_the_evidence);
    BWG_RUN(a_repeated_stage_fails_the_evidence);
    BWG_RUN(the_first_failure_stays);
    BWG_RUN(a_failure_closes_permission_and_starts_cleanup);
    BWG_RUN(a_failed_attempt_can_still_record_its_cleanup);
    BWG_RUN(a_failed_attempt_adds_no_protocol_stage);
    BWG_RUN(a_rejected_exchange_ends_rejected);
    BWG_RUN(a_cancelled_exchange_ends_cancelled);
    BWG_RUN(a_replaced_session_ends_cancelled);
    BWG_RUN(the_authority_deadline_expires_the_attempt);
    BWG_RUN(a_worker_past_the_tail_leaves_the_attempt_incomplete);
    BWG_RUN(a_late_join_keeps_an_incomplete_outcome);
    BWG_RUN(a_socket_left_open_leaves_the_attempt_incomplete);
    BWG_RUN(a_clock_going_backward_closes_permission_for_good);
    BWG_RUN(a_clock_failure_ends_incomplete_without_times);
    BWG_RUN(a_join_without_every_stage_is_rejected_as_missing_evidence);
    BWG_RUN(an_unrepresentable_deadline_is_refused);
    BWG_RUN(an_idle_status_has_no_job);
    BWG_RUN(a_running_status_has_no_failure_outcome_or_deadline);
    BWG_RUN(an_accepted_status_met_its_deadline_without_failures);
    BWG_RUN(an_accepted_status_names_each_stage_and_its_bytes);
    BWG_RUN(a_quiescent_stage_matches_the_release);
    BWG_RUN(a_status_is_never_older_than_its_record);
    BWG_RUN(a_failed_status_names_its_cause);
    BWG_RUN(a_full_status_fits_its_reservation);
}
