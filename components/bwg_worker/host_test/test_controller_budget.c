#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

#define PROVE "{\"action\":\"prove_fan\"}"
#define RESTORE "{\"action\":\"restore_baseline\"}"
#define CAMPAIGN "{\"campaignId\":\"AAAAAAAAAAAAAAAAAAAAAA\"}"

static void open_with_budget(rig_t *rig)
{
    ASSERT_TRUE(rig_open(rig));
    rig->device.has_budget = true;
    ASSERT_TRUE(rig_admit(rig, 2000));
}

static void ledger_reviews_reach_the_device_by_kind(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);

    // Act
    const bwg_ctrl_error_t attempt = rig_send(&rig, "qualification_attempt_review", "{}", 2001);
    const bwg_allowance_kind_t attempt_kind = rig.device.last_review_kind;
    const bwg_ctrl_error_t soak = rig_send(&rig, "soak_allowance_review", "{}", 2002);

    // Assert
    ASSERT_EQ(attempt, BWG_CTRL_OK);
    ASSERT_EQ(attempt_kind, BWG_ALLOWANCE_ATTEMPT);
    ASSERT_EQ(soak, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.last_review_kind, BWG_ALLOWANCE_SOAK);
    rig_close(&rig);
}

static void a_device_with_no_ledger_answer_fails_the_session(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "soak_allowance_review", "{}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    rig_close(&rig);
}

static void an_unreadable_ledger_fails_the_session(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);
    rig.device.budget_review_fails = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_attempt_review", "{}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    rig_close(&rig);
}

static void the_campaign_review_conformance_payloads_fail_at_their_stage(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("controller/budget-review-vectors.json");
    ASSERT_TRUE(doc != NULL);
    yyjson_val *invalid = yyjson_obj_get(yyjson_doc_get_root(doc), "invalidPayloads");
    size_t index;
    size_t count;
    yyjson_val *payload;

    // Act and Assert
    yyjson_arr_foreach(invalid, index, count, payload) {
        rig_t rig;
        open_with_budget(&rig);
        size_t len = 0;
        char *text = bwg_test_json_line(payload, &len);
        text[len - 1] = '\0';
        ASSERT_EQ(rig_send(&rig, "acceptance_budget_review", text, 2001), BWG_CTRL_INVALID_REQUEST);
        free(text);
        rig_close(&rig);
    }
    yyjson_doc_free(doc);
}

static void a_noncanonical_campaign_id_waits_for_admission_first(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "acceptance_budget_review", "{\"campaignId\":\"AAAAAAAAAAAAAAAAAAAAAB\"}", 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void a_missing_campaign_id_is_refused_before_admission(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "acceptance_budget_review", "{}", 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void the_campaign_review_passes_its_id_to_the_device(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "acceptance_budget_review", CAMPAIGN, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.last_review_kind, BWG_ALLOWANCE_CAMPAIGN);
    ASSERT_TRUE(rig.device.last_review_matched);
    rig_close(&rig);
}

static void a_budget_review_is_refused_during_a_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    rig.device.has_budget = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "soak_allowance_review", "{}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void proving_the_fan_marks_an_effect_and_reports_no_budget(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_cooling", PROVE, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "mark_pending,cooling_prove");
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-cooling-proof-v1"));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/fan_duty_percent", 100));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/fan_rpm", 1200));
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_PENDING);
    rig_close(&rig);
}

static void between_prove_and_restore_a_review_and_a_status_are_held(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);
    ASSERT_EQ(rig_send(&rig, "qualification_cooling", PROVE, 2001), BWG_CTRL_OK);
    bwg_controller_outcome_release(&rig.controller, &rig.outcome);

    // Act
    const bwg_ctrl_error_t review = rig_send(&rig, "soak_allowance_review", "{}", 2002);

    // Assert
    ASSERT_EQ(review, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void restoring_clears_the_effect_and_needs_a_fresh_proof_after(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);
    ASSERT_EQ(rig_send(&rig, "qualification_cooling", PROVE, 2001), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t restored = rig_send(&rig, "qualification_cooling", RESTORE, 2002);
    const bool schema = rig_reply_str_is(&rig, "/result/schema", "worker-cooling-baseline-v1");
    const bwg_effect_state_t effect = rig.device.effect;
    const bwg_ctrl_error_t review = rig_send(&rig, "soak_allowance_review", "{}", 2003);

    // Assert
    ASSERT_EQ(restored, BWG_CTRL_OK);
    ASSERT_TRUE(schema);
    ASSERT_EQ(effect, BWG_EFFECT_CLEAR);
    ASSERT_EQ(review, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void restoring_without_an_owned_effect_is_refused(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_cooling", RESTORE, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_failed_proof_runs_cleanup_and_starts_nothing(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);
    rig.device.fail_cooling_prove = true;

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_cooling", PROVE, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_STREQ(rig.device.events, "mark_pending,cooling_prove,control_failed,clear_pending");
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_CLEAR);
    rig_close(&rig);
}

static void proving_needs_fresh_possession(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);

    // Act
    const bwg_ctrl_error_t error =
        rig_send(&rig, "qualification_cooling", PROVE, 2000 + BWG_CONTROLLER_ADMISSION_MAX_AGE_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void proving_waits_for_the_boot_report(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.has_budget = true;
    ASSERT_TRUE(rig_admit(&rig, 2000));
    rig.device.effect = BWG_EFFECT_PENDING;
    ASSERT_TRUE(rig_reboot(&rig));
    ASSERT_TRUE(rig_admit(&rig, 3000));

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_cooling", PROVE, 3001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void an_unknown_cooling_action_is_refused(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "qualification_cooling", "{\"action\":\"other\"}", 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

#define SOAK_FIELD                                                                                                      \
    ",\"soakAllowance\":{\"schema\":\"worker-soak-allowance-v2\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"ordinal\":1,"      \
    "\"maximumActiveMilliseconds\":600000}}"

static void a_soak_lease_refuses_a_renewal_of_any_other_window(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);
    char payload[1024];
    strcpy(payload, RIG_START_PAYLOAD);
    payload[strlen(payload) - 1] = '\0';
    strcat(payload, SOAK_FIELD);
    ASSERT_EQ(rig_send(&rig, "start_lease", payload, 2001), BWG_CTRL_OK);

    // Act
    const bwg_ctrl_error_t error = rig_send(
        &rig, "renew_lease", RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "30000", "20000", RIG_FIXTURE_RENEW_AUTH), 2002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_soak_lease_needs_the_exact_window_at_start(void)
{
    // Arrange
    rig_t rig;
    open_with_budget(&rig);
    char payload[1024];
    strcpy(payload, RIG_START_PAYLOAD_WITH("30000", "20000", RIG_FIXTURE_AUTH));
    payload[strlen(payload) - 1] = '\0';
    strcat(payload, SOAK_FIELD);

    // Act
    const bwg_ctrl_error_t error = rig_send(&rig, "start_lease", payload, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_STREQ(rig.device.events, "");
    rig_close(&rig);
}

void bwg_tests_controller_budget(void)
{
    BWG_RUN(ledger_reviews_reach_the_device_by_kind);
    BWG_RUN(a_device_with_no_ledger_answer_fails_the_session);
    BWG_RUN(an_unreadable_ledger_fails_the_session);
    BWG_RUN(the_campaign_review_conformance_payloads_fail_at_their_stage);
    BWG_RUN(a_noncanonical_campaign_id_waits_for_admission_first);
    BWG_RUN(a_missing_campaign_id_is_refused_before_admission);
    BWG_RUN(the_campaign_review_passes_its_id_to_the_device);
    BWG_RUN(a_budget_review_is_refused_during_a_lease);
    BWG_RUN(proving_the_fan_marks_an_effect_and_reports_no_budget);
    BWG_RUN(between_prove_and_restore_a_review_and_a_status_are_held);
    BWG_RUN(restoring_clears_the_effect_and_needs_a_fresh_proof_after);
    BWG_RUN(restoring_without_an_owned_effect_is_refused);
    BWG_RUN(a_failed_proof_runs_cleanup_and_starts_nothing);
    BWG_RUN(proving_needs_fresh_possession);
    BWG_RUN(proving_waits_for_the_boot_report);
    BWG_RUN(an_unknown_cooling_action_is_refused);
    BWG_RUN(a_soak_lease_refuses_a_renewal_of_any_other_window);
    BWG_RUN(a_soak_lease_needs_the_exact_window_at_start);
}
