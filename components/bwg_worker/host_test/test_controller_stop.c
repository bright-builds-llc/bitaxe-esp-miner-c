#include <stdio.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

static void every_restoration_reason_stops_and_clears_admission(void)
{
    static const char *const REASONS[] = {"paused", "cancelled", "lease_expired", "lost_continuity",
                                          "monotonic_reset", "reboot", "challenge_satisfied", "challenge_expired",
                                          "tab_closed", "connectivity_lost", "control_failed"};
    for (size_t i = 0; i < sizeof(REASONS) / sizeof(REASONS[0]); i++) {
        // Arrange
        rig_t rig;
        ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"reason\":\"%s\"}", REASONS[i]);
        char expected[128];
        snprintf(expected, sizeof(expected), "verify_start,mark_pending,start,%s,clear_pending", REASONS[i]);

        // Act
        bwg_ctrl_error_t restore = rig_send(&rig, "restore", payload, 1001);

        // Assert
        ASSERT_EQ(restore, BWG_CTRL_OK);
        ASSERT_TRUE(rig_reply_str_is(&rig, "/result/state", "baseline"));
        ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/status", "confirmed"));
        ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/reason", REASONS[i]));
        ASSERT_STREQ(rig.device.events, expected);
        ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1002), BWG_CTRL_ADMISSION_REQUIRED);
        rig_close(&rig);
    }
}

static void restore_refuses_a_reason_outside_the_list(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t unknown = rig_send(&rig, "restore", "{\"reason\":\"because\"}", 1001);
    bwg_ctrl_error_t extra = rig_send(&rig, "restore", "{\"reason\":\"paused\",\"x\":1}", 1001);
    bwg_ctrl_error_t absent = rig_send(&rig, "restore", NULL, 1001);

    // Assert
    ASSERT_EQ(unknown, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(extra, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(absent, BWG_CTRL_INVALID_REQUEST);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void pause_and_cancel_report_their_reason(void)
{
    // Arrange
    rig_t paused;
    rig_t cancelled;
    ASSERT_TRUE(rig_open_with_lease(&paused, 1000));
    ASSERT_TRUE(rig_open_with_lease(&cancelled, 1000));

    // Act
    ASSERT_EQ(rig_send(&paused, "pause", NULL, 1001), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&cancelled, "cancel", NULL, 1001), BWG_CTRL_OK);

    // Assert
    ASSERT_TRUE(rig_reply_str_is(&paused, "/result/restoration/reason", "paused"));
    ASSERT_TRUE(rig_reply_str_is(&cancelled, "/result/restoration/reason", "cancelled"));
    rig_close(&paused);
    rig_close(&cancelled);
}

static void pause_with_no_lease_confirms_without_a_stop(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "pause", NULL, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "");
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/reason", "paused"));
    rig_close(&rig);
}

static void disconnect_stops_before_it_drops_the_session(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = bwg_controller_disconnect(&rig.controller, 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,connectivity_lost,clear_pending");
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1002), BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void disconnect_drops_the_session_even_if_the_stop_failed(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
    rig.device.safe_stop_failures = 1;

    // Act
    bwg_ctrl_error_t error = bwg_controller_disconnect(&rig.controller, 1001);
    bool admitted = bwg_controller_is_admitted(&rig.controller);
    bwg_ctrl_error_t retry = bwg_controller_tick(&rig.controller, 1002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_FALSE(admitted);
    ASSERT_EQ(retry, BWG_CTRL_OK);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void reboot_and_control_failure_stop_with_their_reasons(void)
{
    // Arrange
    rig_t rebooted;
    rig_t failed;
    ASSERT_TRUE(rig_open_with_lease(&rebooted, 1000));
    ASSERT_TRUE(rig_open_with_lease(&failed, 1000));

    // Act
    bwg_ctrl_error_t reboot = bwg_controller_reboot(&rebooted.controller, 1001);
    bwg_ctrl_error_t control = bwg_controller_control_failed(&failed.controller, 1001);

    // Assert
    ASSERT_EQ(reboot, BWG_CTRL_OK);
    ASSERT_EQ(control, BWG_CTRL_OK);
    ASSERT_STREQ(rebooted.device.events, "verify_start,mark_pending,start,reboot,clear_pending");
    ASSERT_STREQ(failed.device.events, "verify_start,mark_pending,start,control_failed,clear_pending");
    rig_close(&rebooted);
    rig_close(&failed);
}

static void tick_stops_the_lease_exactly_at_its_deadline(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t before = bwg_controller_tick(&rig.controller, 60999);
    bool active_before = bwg_controller_has_active_lease(&rig.controller);
    bwg_ctrl_error_t at = bwg_controller_tick(&rig.controller, 61000);

    // Assert
    ASSERT_EQ(before, BWG_CTRL_OK);
    ASSERT_TRUE(active_before);
    ASSERT_EQ(at, BWG_CTRL_OK);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,lease_expired,clear_pending");
    rig_close(&rig);
}

static void tick_stops_when_the_clock_goes_backwards(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = bwg_controller_tick(&rig.controller, 999);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_MONOTONIC_RESET);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start,monotonic_reset,clear_pending");
    rig_close(&rig);
}

static void a_frame_with_a_backwards_clock_gets_the_clock_rejection(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "status", NULL, 999);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_MONOTONIC_RESET);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_MONOTONIC_RESET));
    rig_close(&rig);
}

static void the_clock_failure_wins_over_a_malformed_frame(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send_raw(&rig, "{invalid}\n", 10, 999);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_MONOTONIC_RESET);
    ASSERT_FALSE(rig.outcome.has_reply);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void a_frame_after_the_deadline_finds_the_lease_expired(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "status", NULL, 61000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/state", "baseline"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/reason", "lease_expired"));
    rig_close(&rig);
}

static void no_secret_stays_in_the_controller_or_the_reply(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    bwg_test_alloc_watch_secret(RIG_PASSWORD);

    // Act
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000), BWG_CTRL_OK);
    const char *reply = rig_reply_text(&rig);

    // Assert
    ASSERT_TRUE(strstr(reply, RIG_PASSWORD) == NULL);
    ASSERT_TRUE(strstr(reply, "fixture-session-user") == NULL);
    ASSERT_TRUE(strstr(reply, RIG_FIXTURE_AUTH) == NULL);
    ASSERT_TRUE(memmem(&rig.controller, sizeof(rig.controller), RIG_PASSWORD, strlen(RIG_PASSWORD)) == NULL);
    bwg_test_alloc_watch_secret(NULL);
    rig_close(&rig);
}

static void out_of_memory_for_the_reply_refuses_start_before_verifying(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    bwg_test_alloc_fail_size(BWG_CONTROLLER_STATUS_MAX_BYTES);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);

    // Assert
    bwg_test_alloc_fail_size(0);
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

static void out_of_memory_for_the_reply_stops_a_renewing_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 1000));
    bwg_test_alloc_fail_size(BWG_CONTROLLER_STATUS_MAX_BYTES);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD, 1001);

    // Assert
    bwg_test_alloc_fail_size(0);
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

void bwg_tests_controller_stop(void)
{
    BWG_RUN(every_restoration_reason_stops_and_clears_admission);
    BWG_RUN(restore_refuses_a_reason_outside_the_list);
    BWG_RUN(pause_and_cancel_report_their_reason);
    BWG_RUN(pause_with_no_lease_confirms_without_a_stop);
    BWG_RUN(disconnect_stops_before_it_drops_the_session);
    BWG_RUN(disconnect_drops_the_session_even_if_the_stop_failed);
    BWG_RUN(reboot_and_control_failure_stop_with_their_reasons);
    BWG_RUN(tick_stops_the_lease_exactly_at_its_deadline);
    BWG_RUN(tick_stops_when_the_clock_goes_backwards);
    BWG_RUN(a_frame_with_a_backwards_clock_gets_the_clock_rejection);
    BWG_RUN(the_clock_failure_wins_over_a_malformed_frame);
    BWG_RUN(a_frame_after_the_deadline_finds_the_lease_expired);
    BWG_RUN(no_secret_stays_in_the_controller_or_the_reply);
    BWG_RUN(out_of_memory_for_the_reply_refuses_start_before_verifying);
    BWG_RUN(out_of_memory_for_the_reply_stops_a_renewing_lease);
}
