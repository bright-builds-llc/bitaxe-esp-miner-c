#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

#define ATTEMPT "AAAAAAAAAAAAAAAAAAAAAA"
#define OTHER_ATTEMPT "AQAAAAAAAAAAAAAAAAAAAA"
#define KEY "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA"
#define OBSERVED_US 2000000ull
#define NOW_MS 2001
#define QUERY_IDLE "{\"schema\":\"worker-noise-diagnostic-query-v1\",\"attemptId\":null}"
#define QUERY_ATTEMPT "{\"schema\":\"worker-noise-diagnostic-query-v1\",\"attemptId\":\"" ATTEMPT "\"}"
#define QUERY_OTHER "{\"schema\":\"worker-noise-diagnostic-query-v1\",\"attemptId\":\"" OTHER_ATTEMPT "\"}"
#define START_WITH(boot, observed, ip, port)                                                                           \
    "{\"schema\":\"worker-noise-diagnostic-start-v2\",\"attemptId\":\"" ATTEMPT "\",\"expectedBootOrdinal\":" boot     \
    ",\"networkObservedAtUs\":" observed ",\"fixtureIpv4\":\"" ip "\",\"fixturePort\":" port                           \
    ",\"authorityPublicKey\":\"" KEY "\"}"
#define START START_WITH("2", "2000000", "192.168.1.3", "3336")
/** sha256 of the start with its keys sorted, as computed by the host's canonical JSON. */
#define START_SHA256 "0c1d48b8cba61e95496f35f3cd419eee0f0f40315508846cd4f7446cfb96cccf"

static void set_view(rig_t *rig, uint64_t observed_at_us)
{
    bwg_v2_observation_t *observation = &rig->device.noise_view.observation;
    observation->boot_ordinal = 2;
    observation->worker_generation = 7;
    observation->serial_transport_epoch = 4;
    observation->observed_at_us = observed_at_us;
    observation->clock_valid = true;
    observation->wifi_connected = true;
    strcpy(observation->station_ipv4, "192.168.1.2");
}

static void open_idle(rig_t *rig)
{
    ASSERT_TRUE(rig_open(rig));
    rig->device.has_noise = true;
    set_view(rig, OBSERVED_US);
    ASSERT_TRUE(rig_admit(rig, 2000));
}

static void observe_idle(rig_t *rig)
{
    ASSERT_EQ(rig_send(rig, "noise_diagnostic_status", QUERY_IDLE, NOW_MS), BWG_CTRL_OK);
    ASSERT_EQ(rig_confirm_at(rig, NOW_MS), BWG_CTRL_OK);
    set_view(rig, OBSERVED_US + 1000);
}

static void open_admitted(rig_t *rig)
{
    open_idle(rig);
    observe_idle(rig);
    ASSERT_EQ(rig_send(rig, "noise_diagnostic_start", START, NOW_MS + 1), BWG_CTRL_OK);
}

static void an_idle_status_replies_after_fresh_possession(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_status", QUERY_IDLE, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-noise-diagnostic-status-v2"));
    rig_close(&rig);
}

static void an_idle_status_needs_possession(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.has_noise = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_status", QUERY_IDLE, NOW_MS);

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
    const bwg_ctrl_error_t error =
        rig_send(&rig, "noise_diagnostic_status", "{\"schema\":\"worker-noise-diagnostic-query-v1\",\"attemptId\":null,\"x\":1}", NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_device_without_the_diagnostic_refuses_the_commands(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.controller.ops.maybe_noise = NULL;
    ASSERT_TRUE(rig_admit(&rig, 2000));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_status", QUERY_IDLE, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_start_needs_a_prior_idle_status(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", START, NOW_MS);

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
    ASSERT_EQ(rig_send(&rig, "noise_diagnostic_status", QUERY_IDLE, NOW_MS), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void a_diagnostic_starts_only_after_its_reply_was_sent(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_STREQ(rig.device.events, "noise_admit");

    // Act
    const bwg_ctrl_error_t error = rig_confirm_at(&rig, NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "noise_admit,noise_dispatch");
    rig_close(&rig);
}

static void the_admitted_input_is_hashed_in_canonical_form(void)
{
    // Arrange
    rig_t rig;

    // Act
    open_admitted(&rig);

    // Assert
    ASSERT_STREQ(rig.device.noise_input_sha256, START_SHA256);
    rig_close(&rig);
}

static void a_start_refuses_a_stale_or_mismatched_binding(void)
{
    // Arrange
    const char *const starts[] = {
        START_WITH("3", "2000000", "192.168.1.3", "3336"),
        START_WITH("2", "2000001", "192.168.1.3", "3336"),
    };
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        rig_t rig;
        open_idle(&rig);
        observe_idle(&rig);

        // Act
        const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", starts[i], NOW_MS + 1);

        // Assert
        ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
        rig_close(&rig);
    }
}

static void a_start_refuses_a_changed_network(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig);
    strcpy(rig.device.noise_view.observation.station_ipv4, "192.168.1.9");

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_start_refuses_an_old_observation(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig);
    set_view(&rig, OBSERVED_US + 5000001);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_start_refuses_unsafe_values(void)
{
    // Arrange
    const char *const starts[] = {
        START_WITH("2", "2000000", "8.8.8.8", "3336"),
        START_WITH("2", "2000000", "192.168.1.3", "0"),
        START_WITH("2", "2000000", "192.168.1.3", "65536"),
        START_WITH("0", "2000000", "192.168.1.3", "3336"),
    };
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        rig_t rig;
        open_idle(&rig);
        observe_idle(&rig);

        // Act
        const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", starts[i], NOW_MS + 1);

        // Assert
        ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
        rig_close(&rig);
    }
}

static void a_start_waits_for_a_v2_attempt_to_end(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    observe_idle(&rig);
    rig.device.v2_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", START, NOW_MS + 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_second_start_is_refused_once_one_was_admitted(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);
    rig.device.noise_busy = false;
    rig.controller.has_noise_observation = true;
    rig.controller.noise_observation = rig.device.noise_view.observation;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_start", START_WITH("2", "2001000", "192.168.1.3", "3336"), NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_failed_dispatch_cancels_the_diagnostic(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    rig.device.fail_noise_dispatch = true;

    // Act
    const bwg_ctrl_error_t error = rig_confirm_at(&rig, NOW_MS + 2);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_STREQ(rig.device.events, "noise_admit,noise_dispatch,noise_cancel");
    rig_close(&rig);
}

static void an_attempt_is_read_by_its_id(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_status", QUERY_ATTEMPT, NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-noise-diagnostic-status-v2"));
    rig_close(&rig);
}

static void another_attempts_id_is_refused(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_status", QUERY_OTHER, NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_cancel_reaches_a_live_attempt(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_cancel", QUERY_ATTEMPT, NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "noise_admit,noise_dispatch,noise_cancel");
    rig_close(&rig);
}

static void a_cancel_leaves_a_terminal_attempt_alone(void)
{
    // Arrange
    rig_t rig;
    open_admitted(&rig);
    ASSERT_EQ(rig_confirm_at(&rig, NOW_MS + 2), BWG_CTRL_OK);
    rig.device.noise_view.terminal = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_cancel", QUERY_ATTEMPT, NOW_MS + 3);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "noise_admit,noise_dispatch");
    rig_close(&rig);
}

static void a_cancel_needs_an_attempt_id(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "noise_diagnostic_cancel", QUERY_IDLE, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_clock_stimulus_waits_for_a_diagnostic_to_end(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.noise_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "clock_discontinuity_stimulus", "{}", NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void status_reports_restoration_while_a_diagnostic_holds_the_network(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.noise_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "status", NULL, NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_RESTORATION_PENDING);
    rig_close(&rig);
}

static void a_v2_start_waits_for_a_diagnostic_to_end(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.has_v2 = true;
    rig.device.noise_busy = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "stratum_v2_channel_start", "{}", NOW_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

void bwg_tests_controller_noise(void)
{
    BWG_RUN(an_idle_status_replies_after_fresh_possession);
    BWG_RUN(an_idle_status_needs_possession);
    BWG_RUN(a_query_with_extra_fields_is_invalid);
    BWG_RUN(a_device_without_the_diagnostic_refuses_the_commands);
    BWG_RUN(a_start_needs_a_prior_idle_status);
    BWG_RUN(an_unconfirmed_status_does_not_admit_a_start);
    BWG_RUN(a_diagnostic_starts_only_after_its_reply_was_sent);
    BWG_RUN(the_admitted_input_is_hashed_in_canonical_form);
    BWG_RUN(a_start_refuses_a_stale_or_mismatched_binding);
    BWG_RUN(a_start_refuses_a_changed_network);
    BWG_RUN(a_start_refuses_an_old_observation);
    BWG_RUN(a_start_refuses_unsafe_values);
    BWG_RUN(a_start_waits_for_a_v2_attempt_to_end);
    BWG_RUN(a_second_start_is_refused_once_one_was_admitted);
    BWG_RUN(a_failed_dispatch_cancels_the_diagnostic);
    BWG_RUN(an_attempt_is_read_by_its_id);
    BWG_RUN(another_attempts_id_is_refused);
    BWG_RUN(a_cancel_reaches_a_live_attempt);
    BWG_RUN(a_cancel_leaves_a_terminal_attempt_alone);
    BWG_RUN(a_cancel_needs_an_attempt_id);
    BWG_RUN(a_clock_stimulus_waits_for_a_diagnostic_to_end);
    BWG_RUN(status_reports_restoration_while_a_diagnostic_holds_the_network);
    BWG_RUN(a_v2_start_waits_for_a_diagnostic_to_end);
}
