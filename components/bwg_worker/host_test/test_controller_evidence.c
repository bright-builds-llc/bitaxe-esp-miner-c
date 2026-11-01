#include <string.h>

#include "bwg_evidence_lines.h"
#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

#define RESTART_OK "{\"requestNonce\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"expectedBootOrdinal\":7}"
#define RESTART_WRONG_BOOT "{\"requestNonce\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"expectedBootOrdinal\":6}"
#define RESTART_BAD_NONCE "{\"requestNonce\":\"AAAAAAAAAAAAAAAAAAAAAB\",\"expectedBootOrdinal\":7}"

static void open_idle(rig_t *rig)
{
    ASSERT_TRUE(rig_open(rig));
    rig->device.boot_ordinal = 7;
    rig->device.restart_allowed = true;
    ASSERT_TRUE(rig_admit(rig, 2000));
}

static void a_restart_waits_for_its_reply_to_be_sent(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_restart", RESTART_OK, 2001);
    const int restarts_before_send = rig.device.restarts;
    const bwg_ctrl_error_t confirmed = rig_confirm_at(&rig, 2002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_EQ(restarts_before_send, 0);
    ASSERT_EQ(confirmed, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.restarts, 1);
    rig_close(&rig);
}

static void a_core_dump_self_test_waits_for_its_reply_to_be_sent(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.outputs_off = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_core_dump_self_test", RESTART_OK, 2001);
    const int panics_before_send = rig.device.core_dump_self_tests;
    const bwg_ctrl_error_t confirmed = rig_confirm_at(&rig, 2002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_EQ(panics_before_send, 0);
    ASSERT_EQ(confirmed, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.core_dump_self_tests, 1);
    ASSERT_EQ(rig.device.restarts, 0);
    rig_close(&rig);
}

static void a_core_dump_self_test_needs_outputs_proven_off(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.outputs_off = false;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_core_dump_self_test", RESTART_OK, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_EQ(rig.device.core_dump_self_tests, 0);
    rig_close(&rig);
}

static void a_restart_and_a_self_test_share_one_use(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.outputs_off = true;
    ASSERT_EQ(rig_send(&rig, "qualification_restart", RESTART_OK, 2001), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t second = rig_send(&rig, "qualification_core_dump_self_test", RESTART_OK, 2002);

    // Assert
    ASSERT_EQ(second, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_second_restart_in_a_session_is_refused(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    ASSERT_EQ(rig_send(&rig, "qualification_restart", RESTART_OK, 2001), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t second = rig_send(&rig, "qualification_restart", RESTART_OK, 2002);

    // Assert
    ASSERT_EQ(second, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_restart_for_another_boot_is_refused(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_restart", RESTART_WRONG_BOOT, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_EQ(rig.device.restarts, 0);
    rig_close(&rig);
}

static void a_restart_with_a_noncanonical_nonce_is_invalid(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_restart", RESTART_BAD_NONCE, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_device_that_would_mine_on_boot_refuses_a_restart(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.restart_allowed = false;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_restart", RESTART_OK, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_restart_needs_fresh_possession(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.boot_ordinal = 7;
    rig.device.restart_allowed = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_restart", RESTART_OK, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void the_endpoint_is_read_from_the_device(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.has_endpoint = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "telemetry_cadence_endpoint", "{}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(strstr(rig_reply_text(&rig), "worker-telemetry-endpoint-v1") != NULL);
    rig_close(&rig);
}

static void a_device_with_no_address_refuses_the_endpoint(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    rig.device.has_endpoint = false;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "telemetry_cadence_endpoint", "{}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_warm_reset_counts_up(void)
{
    // Arrange
    bwg_boot_record_t record = {0};
    bwg_boot_record_advance(&record, true);

    // Act
    const uint32_t ordinal = bwg_boot_record_advance(&record, false);

    // Assert
    ASSERT_EQ(ordinal, 2u);
}

static void a_cold_boot_starts_again(void)
{
    // Arrange
    bwg_boot_record_t record = {0};
    bwg_boot_record_advance(&record, true);
    bwg_boot_record_advance(&record, false);

    // Act
    const uint32_t ordinal = bwg_boot_record_advance(&record, true);

    // Assert
    ASSERT_EQ(ordinal, 1u);
}

static void a_corrupt_record_starts_again(void)
{
    // Arrange
    bwg_boot_record_t record = {0};
    bwg_boot_record_advance(&record, true);
    record.ordinal = 99;

    // Act
    const uint32_t ordinal = bwg_boot_record_advance(&record, false);

    // Assert
    ASSERT_EQ(ordinal, 1u);
}

static void a_line_that_does_not_fit_is_refused(void)
{
    // Arrange
    char small[8];

    // Act
    const bool written = bwg_line_runtime_ready(small, sizeof(small), 1);

    // Assert
    ASSERT_FALSE(written);
}

static void the_allocation_receipt_pads_its_capabilities(void)
{
    // Arrange
    char line[128];

    // Act
    ASSERT_TRUE(bwg_line_allocation_failure(line, sizeof(line), 512, 0x400));

    // Assert
    ASSERT_STREQ(line, "allocation_failure_receipt schema=v1 requested_bytes=512 capabilities=00000400 redacted=true");
}

static void an_unsafe_detail_names_its_fact_and_reading(void)
{
    // Arrange
    char line[256];
    const bwg_unsafe_detail_t detail = {.generation = 3, .trigger = "unsafe_sample", .fact = "chip_temperature", .has_value = true, .value_milli = 75500, .since_safe_ms = 1200, .closed_ms = 90000};

    // Act
    ASSERT_TRUE(bwg_line_unsafe_detail(line, sizeof(line), &detail));

    // Assert
    ASSERT_STREQ(line, "worker_revocation_detail schema=v1 generation=3 reason=unsafe_observation trigger=unsafe_sample fact=chip_temperature state=out_of_range value_milli=75500 age_ms=unavailable since_safe_ms=1200 closed_ms=90000 redacted=true");
}

static void an_unsafe_detail_with_no_number_says_so(void)
{
    // Arrange
    char line[256];
    const bwg_unsafe_detail_t detail = {.generation = 3, .trigger = "unsafe_sample", .fact = "power", .since_safe_ms = 1, .closed_ms = 2};

    // Act
    ASSERT_TRUE(bwg_line_unsafe_detail(line, sizeof(line), &detail));

    // Assert
    ASSERT_STREQ(line, "worker_revocation_detail schema=v1 generation=3 reason=unsafe_observation trigger=unsafe_sample fact=power state=unavailable value_milli=unavailable age_ms=unavailable since_safe_ms=1 closed_ms=2 redacted=true");
}

static void a_zero_fan_detail_names_no_fact(void)
{
    // Arrange
    char line[256];
    const bwg_unsafe_detail_t detail = {.generation = 1, .trigger = "zero_fan", .fact = "none", .since_safe_ms = 1000, .closed_ms = 5};

    // Act
    ASSERT_TRUE(bwg_line_unsafe_detail(line, sizeof(line), &detail));

    // Assert
    ASSERT_STREQ(line, "worker_revocation_detail schema=v1 generation=1 reason=unsafe_observation trigger=zero_fan fact=none state=none value_milli=unavailable age_ms=unavailable since_safe_ms=1000 closed_ms=5 redacted=true");
}

static void an_unsafe_detail_refuses_a_buffer_too_small(void)
{
    // Arrange
    char line[32];
    const bwg_unsafe_detail_t detail = {.generation = 1, .trigger = "zero_fan", .fact = "none"};

    // Act / Assert
    ASSERT_TRUE(!bwg_line_unsafe_detail(line, sizeof(line), &detail));
}

static void note_loop_iteration(rig_t *rig)
{
    const bwg_cadence_iteration_t iteration = {.started_at_us = 1000, .finished_at_us = 1100, .subscribers = 1, .priority = 5};
    bwg_cadence_note_iteration(&rig->device.cadence, &iteration);
}

static void a_cadence_arm_replies_with_the_phase(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    note_loop_iteration(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "telemetry_cadence_arm", "{\"phase\":\"idle\"}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-telemetry-cadence-arm-v1"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/phase", "idle"));
    rig_close(&rig);
}

static void a_second_arm_of_a_phase_is_an_invalid_transition(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    note_loop_iteration(&rig);
    ASSERT_EQ(rig_send(&rig, "telemetry_cadence_arm", "{\"phase\":\"idle\"}", 2001), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "telemetry_cadence_arm", "{\"phase\":\"idle\"}", 2002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void an_unknown_cadence_phase_is_an_invalid_request(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);
    note_loop_iteration(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "telemetry_cadence_arm", "{\"phase\":\"soak\"}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_cadence_review_lists_the_three_phases(void)
{
    // Arrange
    rig_t rig;
    open_idle(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "telemetry_cadence_review", "{}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-telemetry-cadence-v1"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/phases/2/phase", "mining"));
    rig_close(&rig);
}

void bwg_tests_controller_evidence(void)
{
    BWG_RUN(a_cadence_arm_replies_with_the_phase);
    BWG_RUN(a_second_arm_of_a_phase_is_an_invalid_transition);
    BWG_RUN(an_unknown_cadence_phase_is_an_invalid_request);
    BWG_RUN(a_cadence_review_lists_the_three_phases);
    BWG_RUN(a_restart_waits_for_its_reply_to_be_sent);
    BWG_RUN(a_core_dump_self_test_waits_for_its_reply_to_be_sent);
    BWG_RUN(a_core_dump_self_test_needs_outputs_proven_off);
    BWG_RUN(a_restart_and_a_self_test_share_one_use);
    BWG_RUN(a_second_restart_in_a_session_is_refused);
    BWG_RUN(a_restart_for_another_boot_is_refused);
    BWG_RUN(a_restart_with_a_noncanonical_nonce_is_invalid);
    BWG_RUN(a_device_that_would_mine_on_boot_refuses_a_restart);
    BWG_RUN(a_restart_needs_fresh_possession);
    BWG_RUN(the_endpoint_is_read_from_the_device);
    BWG_RUN(a_device_with_no_address_refuses_the_endpoint);
    BWG_RUN(a_warm_reset_counts_up);
    BWG_RUN(a_cold_boot_starts_again);
    BWG_RUN(a_corrupt_record_starts_again);
    BWG_RUN(a_line_that_does_not_fit_is_refused);
    BWG_RUN(the_allocation_receipt_pads_its_capabilities);
    BWG_RUN(an_unsafe_detail_names_its_fact_and_reading);
    BWG_RUN(an_unsafe_detail_with_no_number_says_so);
    BWG_RUN(a_zero_fan_detail_names_no_fact);
    BWG_RUN(an_unsafe_detail_refuses_a_buffer_too_small);
}
