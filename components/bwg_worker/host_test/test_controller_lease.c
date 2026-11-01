#include <stdio.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

static void start_runs_verify_then_marker_then_effect(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start");
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_PENDING);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/requestId", "serial_test"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/state", "mining"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/status", "pending"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/lease/leaseId", "lease_fixture_01"));
    rig_close(&rig);
}

static void start_deadlines_come_from_one_captured_clock(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD_WITH("45000", "12345", RIG_FIXTURE_AUTH), 1234), BWG_CTRL_OK);

    // Assert
    ASSERT_EQ(rig.device.start_deadlines.renew_at_ms, 13579);
    ASSERT_EQ(rig.device.start_deadlines.expires_at_ms, 46234);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/monotonicMilliseconds", 1234));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/lease/renewAtMonotonicMilliseconds", 13579));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/lease/expiresAtMonotonicMilliseconds", 46234));
    rig_close(&rig);
}

static void start_checks_admission_before_transition(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
    ASSERT_EQ(bwg_controller_disconnect(&rig.controller, 1001), BWG_CTRL_OK);

    // Act: nothing is admitted, and the payload's window is bad too.
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD_WITH("60001", "20000", RIG_FIXTURE_AUTH), 1002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void start_checks_transition_before_validation(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
    rig.device.events[0] = '\0';

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD_WITH("60001", "20000", RIG_FIXTURE_AUTH), 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_STREQ(rig.device.events, "");
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void start_validates_before_verifying(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD_WITH("60001", "20000", RIG_FIXTURE_AUTH), 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

static void start_with_a_refused_authorization_does_nothing(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD_WITH("60000", "20000", "invalid"), 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_STREQ(rig.device.events, "verify_start");
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_CLEAR);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_AUTHENTICATION_FAILED));
    rig_close(&rig);
}

static void start_refuses_a_payload_it_cannot_parse(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act: the shape is checked even before admission.
    bwg_ctrl_error_t absent = rig_send(&rig, "start_lease", NULL, 1000);
    bwg_ctrl_error_t empty = rig_send(&rig, "start_lease", "{}", 1000);

    // Assert
    ASSERT_EQ(absent, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(empty, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void start_with_a_malformed_allowance_is_a_clean_rejection(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    char payload[1024];
    strcpy(payload, RIG_START_PAYLOAD);
    payload[strlen(payload) - 1] = '\0';
    strcat(payload, ",\"soakAllowance\":{\"schema\":\"worker-soak-allowance-v2\"}}");

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", payload, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

static void marker_failure_stops_before_the_effect(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    rig.device.mark_failures = 1;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_PERSISTENCE_FAILED);
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending");
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void failed_marker_clear_stays_retryable(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
    rig.device.clear_failures = 1;

    // Act
    bwg_ctrl_error_t first_stop = rig_send(&rig, "pause", NULL, 1001);
    bool active_after_failure = bwg_controller_has_active_lease(&rig.controller);
    bwg_effect_state_t marker_after_failure = rig.device.effect;
    bwg_ctrl_error_t retry = bwg_controller_tick(&rig.controller, 1002);

    // Assert
    ASSERT_EQ(first_stop, BWG_CTRL_PERSISTENCE_FAILED);
    ASSERT_TRUE(active_after_failure);
    ASSERT_EQ(marker_after_failure, BWG_EFFECT_PENDING);
    ASSERT_EQ(retry, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_CLEAR);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events,
                 "verify_start,mark_pending,start,paused,clear_pending,paused,clear_pending");
    rig_close(&rig);
}

static void failed_start_keeps_cleanup_until_the_stop_finishes(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    rig.device.fail_start = true;
    rig.device.safe_stop_failures = 1;

    // Act
    bwg_ctrl_error_t start = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);
    bool active_after_failure = bwg_controller_has_active_lease(&rig.controller);
    bwg_ctrl_error_t retry = bwg_controller_tick(&rig.controller, 1001);

    // Assert
    ASSERT_EQ(start, BWG_CTRL_SESSION_FAILED);
    ASSERT_TRUE(active_after_failure);
    ASSERT_EQ(retry, BWG_CTRL_OK);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,control_failed,control_failed,clear_pending");
    rig_close(&rig);
}

static void failed_start_reports_session_failed_after_a_clean_stop(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    rig.device.fail_start = true;

    // Act
    bwg_ctrl_error_t start = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(start, BWG_CTRL_SESSION_FAILED);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_SESSION_FAILED));
    ASSERT_FALSE(bwg_controller_is_admitted(&rig.controller));
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_CLEAR);
    rig_close(&rig);
}

static void renew_extends_from_its_own_clock(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error =
        rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "30000", "10000", RIG_FIXTURE_RENEW_AUTH), 5000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.renew_deadlines.renew_at_ms, 15000);
    ASSERT_EQ(rig.device.renew_deadlines.expires_at_ms, 35000);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/lease/renewAtMonotonicMilliseconds", 15000));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/lease/expiresAtMonotonicMilliseconds", 35000));
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,verify_renew,renew");
    rig_close(&rig);
}

static void renew_has_no_sixty_second_limit(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 50000), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD, 70000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    rig_close(&rig);
}

static void renew_with_no_admission_needs_admission_first(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error =
        rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "60001", "20000", RIG_FIXTURE_RENEW_AUTH), 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void renew_validates_before_checking_for_a_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t bad_window =
        rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "60001", "20000", RIG_FIXTURE_RENEW_AUTH), 1000);
    bwg_ctrl_error_t no_lease = rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(bad_window, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(no_lease, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void renew_with_a_bad_authorization_stops_mining(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error =
        rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "60000", "20000", "invalid"), 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_AUTHENTICATION_FAILED));
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,verify_renew,control_failed,clear_pending");
    rig_close(&rig);
}

static void renew_of_another_lease_stops_mining_without_verifying(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error =
        rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_other_01", "60000", "20000", RIG_FIXTURE_RENEW_AUTH), 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,control_failed,clear_pending");
    rig_close(&rig);
}

static void renew_session_failure_stops_mining(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
    rig.device.fail_renew = true;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD, 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,verify_renew,renew,control_failed,clear_pending");
    rig_close(&rig);
}

void bwg_tests_controller_lease(void)
{
    BWG_RUN(start_runs_verify_then_marker_then_effect);
    BWG_RUN(start_deadlines_come_from_one_captured_clock);
    BWG_RUN(start_checks_admission_before_transition);
    BWG_RUN(start_checks_transition_before_validation);
    BWG_RUN(start_validates_before_verifying);
    BWG_RUN(start_with_a_refused_authorization_does_nothing);
    BWG_RUN(start_refuses_a_payload_it_cannot_parse);
    BWG_RUN(start_with_a_malformed_allowance_is_a_clean_rejection);
    BWG_RUN(marker_failure_stops_before_the_effect);
    BWG_RUN(failed_marker_clear_stays_retryable);
    BWG_RUN(failed_start_keeps_cleanup_until_the_stop_finishes);
    BWG_RUN(failed_start_reports_session_failed_after_a_clean_stop);
    BWG_RUN(renew_extends_from_its_own_clock);
    BWG_RUN(renew_has_no_sixty_second_limit);
    BWG_RUN(renew_with_no_admission_needs_admission_first);
    BWG_RUN(renew_validates_before_checking_for_a_lease);
    BWG_RUN(renew_with_a_bad_authorization_stops_mining);
    BWG_RUN(renew_of_another_lease_stops_mining_without_verifying);
    BWG_RUN(renew_session_failure_stops_mining);
}
