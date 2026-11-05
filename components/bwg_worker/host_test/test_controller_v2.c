#include <stdio.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

#define ATTEMPT "AAAAAAAAAAAAAAAAAAAAAA"
#define OTHER_ATTEMPT "AQAAAAAAAAAAAAAAAAAAAA"
#define V2_KEY "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA"
#define OBSERVED_US 2000000ull
#define NOW_MS 2001
#define QUERY_CHANNEL_IDLE "{\"schema\":\"worker-stratum-v2-query-v1\",\"scope\":\"channel\",\"attemptId\":null}"
#define QUERY_SHARE_IDLE "{\"schema\":\"worker-stratum-v2-query-v1\",\"scope\":\"share\",\"attemptId\":null}"
#define QUERY_ATTEMPT "{\"schema\":\"worker-stratum-v2-query-v1\",\"scope\":\"channel\",\"attemptId\":\"" ATTEMPT "\"}"
#define CANCEL_ATTEMPT "{\"schema\":\"worker-stratum-v2-channel-cancel-v1\",\"attemptId\":\"" ATTEMPT "\"}"
#define V2_STRATUM                                                                                                     \
    "{\"endpoint\":\"stratum+tcp://192.168.1.2:3336/\",\"profile\":\"bwg-worker-stratum-v2-standard/0.1\","            \
    "\"authorityPublicKey\":\"" V2_KEY "\",\"userIdentity\":\"worker\"}"
#define CHANNEL_START_WITH(attempt, boot, observed, stratum)                                                           \
    "{\"schema\":\"worker-stratum-v2-channel-start-v1\",\"attemptId\":\"" attempt "\",\"expectedBootOrdinal\":" boot   \
    ",\"networkObservedAtUs\":" observed ",\"stratum\":" stratum "}"
#define CHANNEL_START CHANNEL_START_WITH(ATTEMPT, "2", "2000000", V2_STRATUM)
#define SHARE_START                                                                                                    \
    "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"lease_fixture_01\",\"challengeId\":"             \
    "\"challenge_00000000000000000000000000000001\",\"authorization\":\"" RIG_FIXTURE_AUTH                             \
    "\",\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":20000,\"stratum\":" V2_STRATUM "}"

static void set_view(rig_t *rig, uint64_t observed_at_us)
{
    bwg_v2_view_t *view = &rig->device.v2_view;
    view->observation.boot_ordinal = 2;
    view->observation.worker_generation = 7;
    view->observation.serial_transport_epoch = 4;
    view->observation.observed_at_us = observed_at_us;
    view->observation.clock_valid = true;
    view->observation.wifi_connected = true;
    strcpy(view->observation.station_ipv4, "192.168.1.2");
}

static void open_idle(rig_t *rig)
{
    ASSERT_TRUE(rig_open(rig));
    rig->device.has_v2 = true;
    set_view(rig, OBSERVED_US);
    ASSERT_TRUE(rig_admit(rig, 2000));
}

static void observe_idle(rig_t *rig, const char *query)
{
    ASSERT_EQ(rig_send(rig, "stratum_v2_status", query, NOW_MS), BWG_CTRL_OK);
    ASSERT_EQ(rig_confirm_at(rig, NOW_MS), BWG_CTRL_OK);
}

static void open_admitted(rig_t *rig)
{
    open_idle(rig);
    observe_idle(rig, QUERY_CHANNEL_IDLE);
    set_view(rig, OBSERVED_US + 1000);
    ASSERT_EQ(rig_send(rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS + 1), BWG_CTRL_OK);
}

static void an_idle_status_replies_after_fresh_possession(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_status", QUERY_CHANNEL_IDLE, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-stratum-v2-status-v1"));
    rig_close(&rig);
}

static void an_idle_status_reports_the_observation_a_start_must_name(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    observe_idle(&rig, QUERY_CHANNEL_IDLE);

    // Assert
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/observedAtUs", rig.controller.v2_observation.observed_at_us));
    rig_close(&rig);
}

static void an_idle_status_needs_possession(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.has_v2 = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_status", QUERY_CHANNEL_IDLE, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void a_query_with_extra_fields_is_invalid(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(
        &rig, "stratum_v2_status", "{\"schema\":\"worker-stratum-v2-query-v1\",\"scope\":\"channel\",\"attemptId\":null,\"x\":1}", NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_device_without_v2_refuses_the_commands(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.controller.ops.maybe_v2 = NULL;
    ASSERT_TRUE(rig_admit(&rig, 2000));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_status", QUERY_CHANNEL_IDLE, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_dispatch_without_v2_is_stale(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.controller.ops.maybe_v2 = NULL;
    const bwg_ctrl_effect_t effect = {.kind = BWG_CTRL_EFFECT_V2_DISPATCH, .generation = rig.controller.generation};

    // Act
    const bwg_ctrl_error_t error = bwg_controller_confirm_sent(&rig.controller, &effect, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_STALE_RESPONSE);
    rig_close(&rig);
}

static void a_channel_start_needs_a_prior_idle_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

static void an_unconfirmed_status_does_not_admit_a_start(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    ASSERT_EQ(rig_send(&rig, "stratum_v2_status", QUERY_CHANNEL_IDLE, NOW_MS), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void a_channel_starts_only_after_its_reply_was_sent(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_CHANNEL_IDLE);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS + 1);
    const bool dispatched_early = strstr(rig.device.events, "v2_dispatch") != NULL;
    const bwg_ctrl_error_t confirmed = rig_confirm_at(&rig, NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_FALSE(dispatched_early);
    ASSERT_EQ(confirmed, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "v2_admit,v2_dispatch");
    rig_close(&rig);
}

static void a_channel_start_refuses_a_stale_or_mismatched_binding(void)
{
    // Arrange
    const struct {
        const char *payload;
        uint64_t device_observed_us;
    } refused[] = {
        {CHANNEL_START_WITH(ATTEMPT, "3", "2000000", V2_STRATUM), OBSERVED_US + 1000},
        {CHANNEL_START_WITH(ATTEMPT, "2", "2000001", V2_STRATUM), OBSERVED_US + 1000},
        {CHANNEL_START, OBSERVED_US + 5000001},
    };

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        rig_t rig;
        open_idle(&rig);
        observe_idle(&rig, QUERY_CHANNEL_IDLE);
        set_view(&rig, refused[i].device_observed_us);

        // Act
        const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", refused[i].payload, NOW_MS + 1);

        // Assert
        ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
        ASSERT_STREQ(rig.device.events, "");
        rig_close(&rig);
    }
}

static void a_channel_start_refuses_a_changed_network(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_CHANNEL_IDLE);
    strcpy(rig.device.v2_view.observation.station_ipv4, "192.168.1.3");

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_channel_start_refuses_a_share_scope_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_SHARE_IDLE);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_channel_start_refuses_an_unsafe_stratum(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_CHANNEL_IDLE);
    const char *public_endpoint = CHANNEL_START_WITH(ATTEMPT, "2", "2000000",
                                                     "{\"endpoint\":\"stratum+tcp://8.8.8.8:3336/\",\"profile\":"
                                                     "\"bwg-worker-stratum-v2-standard/0.1\",\"authorityPublicKey\":\"" V2_KEY
                                                     "\",\"userIdentity\":\"worker\"}");

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", public_endpoint, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

static void a_failed_dispatch_cancels_the_attempt(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    rig.device.fail_v2_dispatch = true;

    // Act
    const bwg_ctrl_error_t error = rig_confirm_at(&rig, NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_STREQ(rig.device.events, "v2_admit,v2_dispatch,v2_cancel");
    rig_close(&rig);
}

static void a_dispatch_from_another_session_is_stale_and_cancels(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    bwg_ctrl_effect_t effect = rig.outcome.effect;
    effect.generation++;

    // Act
    const bwg_ctrl_error_t error = bwg_controller_confirm_sent(&rig.controller, &effect, NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_STALE_RESPONSE);
    ASSERT_STREQ(rig.device.events, "v2_admit,v2_cancel");
    rig_close(&rig);
}

static void an_attempt_is_read_by_its_id(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_status", QUERY_ATTEMPT, NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-stratum-v2-status-v1"));
    rig_close(&rig);
}

static void another_attempts_id_is_refused(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(
        &rig, "stratum_v2_status", "{\"schema\":\"worker-stratum-v2-query-v1\",\"scope\":\"channel\",\"attemptId\":\"" OTHER_ATTEMPT "\"}",
        NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_cancel_reaches_a_live_attempt_only(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t live = rig_send(&rig, "stratum_v2_channel_cancel", CANCEL_ATTEMPT, NOW_MS + 3);
    rig.device.v2_view.state = BWG_V2_STATE_TERMINAL;
    const bwg_ctrl_error_t finished = rig_send(&rig, "stratum_v2_channel_cancel", CANCEL_ATTEMPT, NOW_MS + 4);

    // Assert
    ASSERT_EQ(live, BWG_CTRL_OK);
    ASSERT_EQ(finished, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "v2_admit,v2_dispatch,v2_cancel");
    rig_close(&rig);
}

static void a_new_session_loses_the_retained_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_CHANNEL_IDLE);
    ASSERT_TRUE(rig_admit(&rig, NOW_MS + 1));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", CHANNEL_START, NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void a_v2_lease_needs_a_share_status_first(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "start_lease", SHARE_START, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

static void a_v2_lease_starts_after_a_share_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_SHARE_IDLE);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "start_lease", SHARE_START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    rig_close(&rig);
}

static void a_v2_lease_refuses_a_channel_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_CHANNEL_IDLE);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "start_lease", SHARE_START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_v2_lease_refuses_a_stale_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_SHARE_IDLE);
    set_view(&rig, OBSERVED_US + 5000001);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "start_lease", SHARE_START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_v2_lease_refuses_a_renewal_of_any_other_window(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig, QUERY_SHARE_IDLE);
    ASSERT_EQ(rig_send(&rig, "start_lease", SHARE_START, NOW_MS + 1), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(
        &rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "30000", "20000", RIG_FIXTURE_RENEW_AUTH), NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_lease_waits_for_a_v2_attempt_to_end(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.v2_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void status_reports_restoration_while_an_attempt_holds_the_network(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.v2_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "status", NULL, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_RESTORATION_PENDING);
    rig_close(&rig);
}

static void a_restart_waits_for_a_v2_attempt_to_end(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.restart_allowed = true;
    rig.device.boot_ordinal = 2;
    rig.device.v2_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(
        &rig, "qualification_restart", "{\"requestNonce\":\"" ATTEMPT "\",\"expectedBootOrdinal\":2}", NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_clock_stimulus_waits_for_a_v2_attempt_to_end(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    rig.device.v2_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "clock_discontinuity_stimulus", "{\"requestNonce\":\"" ATTEMPT "\"}", NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

void bwg_tests_controller_v2(void)
{
    BWG_RUN(an_idle_status_replies_after_fresh_possession);
    BWG_RUN(an_idle_status_reports_the_observation_a_start_must_name);
    BWG_RUN(an_idle_status_needs_possession);
    BWG_RUN(a_query_with_extra_fields_is_invalid);
    BWG_RUN(a_device_without_v2_refuses_the_commands);
    BWG_RUN(a_dispatch_without_v2_is_stale);
    BWG_RUN(a_channel_start_needs_a_prior_idle_status);
    BWG_RUN(an_unconfirmed_status_does_not_admit_a_start);
    BWG_RUN(a_channel_starts_only_after_its_reply_was_sent);
    BWG_RUN(a_channel_start_refuses_a_stale_or_mismatched_binding);
    BWG_RUN(a_channel_start_refuses_a_changed_network);
    BWG_RUN(a_channel_start_refuses_a_share_scope_status);
    BWG_RUN(a_channel_start_refuses_an_unsafe_stratum);
    BWG_RUN(a_failed_dispatch_cancels_the_attempt);
    BWG_RUN(a_dispatch_from_another_session_is_stale_and_cancels);
    BWG_RUN(an_attempt_is_read_by_its_id);
    BWG_RUN(another_attempts_id_is_refused);
    BWG_RUN(a_cancel_reaches_a_live_attempt_only);
    BWG_RUN(a_new_session_loses_the_retained_status);
    BWG_RUN(a_v2_lease_needs_a_share_status_first);
    BWG_RUN(a_v2_lease_starts_after_a_share_status);
    BWG_RUN(a_v2_lease_refuses_a_channel_status);
    BWG_RUN(a_v2_lease_refuses_a_stale_status);
    BWG_RUN(a_v2_lease_refuses_a_renewal_of_any_other_window);
    BWG_RUN(a_lease_waits_for_a_v2_attempt_to_end);
    BWG_RUN(status_reports_restoration_while_an_attempt_holds_the_network);
    BWG_RUN(a_restart_waits_for_a_v2_attempt_to_end);
    BWG_RUN(a_clock_stimulus_waits_for_a_v2_attempt_to_end);
}
