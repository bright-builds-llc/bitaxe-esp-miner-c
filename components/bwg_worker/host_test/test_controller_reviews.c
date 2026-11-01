#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_review.h"
#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

#define REVIEW_PAYLOAD "{}"

static bool frame_as_request_is(rig_t *rig, yyjson_val *request, uint64_t now_ms, bwg_ctrl_error_t expected)
{
    size_t len = 0;
    char *frame = bwg_test_json_line(request, &len);
    bwg_ctrl_error_t error = rig_send_raw(rig, frame, len, now_ms);
    free(frame);
    return error == expected;
}

static void the_conformance_requests_are_sorted_by_shape_alone(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("controller/restoration-qualification-vectors.json");
    ASSERT_TRUE(doc != NULL);
    yyjson_val *requests = yyjson_obj_get(yyjson_doc_get_root(doc), "requests");
    yyjson_val *invalid = yyjson_obj_get(requests, "invalid");
    yyjson_val *valid = yyjson_obj_get(requests, "valid");
    size_t index;
    size_t count;
    yyjson_val *entry;

    // Act and Assert
    yyjson_arr_foreach(invalid, index, count, entry) {
        rig_t rig;
        ASSERT_TRUE(rig_open(&rig));
        ASSERT_TRUE(rig_admit(&rig, 2000));
        ASSERT_TRUE(frame_as_request_is(&rig, yyjson_obj_get(entry, "request"), 2001, BWG_CTRL_INVALID_REQUEST));
        rig_close(&rig);
    }
    yyjson_arr_foreach(valid, index, count, entry) {
        rig_t rig;
        ASSERT_TRUE(rig_open(&rig));
        ASSERT_TRUE(rig_admit(&rig, 2000));
        // With no lease a stimulus is refused for its state, and a review is answered: neither for its shape.
        const bool stimulus = strcmp(yyjson_get_str(yyjson_obj_get(entry, "name")), "stimulus") == 0;
        ASSERT_TRUE(frame_as_request_is(&rig, yyjson_obj_get(entry, "request"), 2001,
                                        stimulus ? BWG_CTRL_INVALID_TRANSITION : BWG_CTRL_OK));
        rig_close(&rig);
    }
    yyjson_doc_free(doc);
}

static void boot_review_names_the_reset_cause(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));
    rig.device.maybe_boot_reset_cause = "brownout";

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "boot_review", REVIEW_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-boot-review-v1"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/resetCause", "brownout"));
    rig_close(&rig);
}

static void a_review_needs_fresh_possession(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "boot_review", REVIEW_PAYLOAD, 2000 + BWG_CONTROLLER_ADMISSION_MAX_AGE_MS);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    rig_close(&rig);
}

static void a_review_is_refused_during_a_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "authorization_rejection_review", REVIEW_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void rejection_review_starts_empty_with_the_fingerprint(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "authorization_rejection_review", REVIEW_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-authorization-rejection-review-v2"));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/bootRejections", 0));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/highWater/fingerprintSha256", RIG_FINGERPRINT));
    ASSERT_TRUE(strstr(rig_reply_text(&rig), "\"last\":null") != NULL);
    ASSERT_TRUE(strstr(rig_reply_text(&rig), "\"advancedThisBoot\":false") != NULL);
    rig_close(&rig);
}

static void rejection_review_reports_an_unreadable_high_water(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));
    rig.device.fingerprint_fails = true;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "authorization_rejection_review", REVIEW_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_PERSISTENCE_FAILED);
    rig_close(&rig);
}

static void rejection_review_is_refused_with_no_store_to_fingerprint(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));
    rig.controller.ops.maybe_high_water_fingerprint = NULL;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "authorization_rejection_review", REVIEW_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_refused_start_is_logged_with_no_stop(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD_WITH("60000", "20000", "fixture-wrong-authentication"), 2001),
              BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_TRUE(rig_admit(&rig, 2002));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "authorization_rejection_review", REVIEW_PAYLOAD, 2003);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/bootRejections", 1));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/last/ordinal", 1));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/operation", "start"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/safeStop", "none"));
    rig_close(&rig);
}

static void a_start_with_no_admission_is_logged_as_absent(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));
    ASSERT_TRUE(rig_reprove(&rig, 2001));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t refused = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 2002);
    ASSERT_TRUE(rig_admit(&rig, 2003));
    bwg_ctrl_error_t review = rig_send(&rig, "authorization_rejection_review", REVIEW_PAYLOAD, 2004);

    // Assert
    ASSERT_EQ(refused, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_EQ(review, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/context", "absent"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/signature", "not_evaluated"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/replayGuard", "not_evaluated"));
    rig_close(&rig);
}

static void an_old_admission_is_logged_as_expired(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 2000));

    // Act
    bwg_ctrl_error_t refused = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 2000 + BWG_CONTROLLER_ADMISSION_MAX_AGE_MS);

    // Assert
    ASSERT_EQ(refused, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_TRUE(rig.controller.rejections.has_last);
    ASSERT_EQ(rig.controller.rejections.last.attribution.context, BWG_AUTH_CONTEXT_EXPIRED);
    rig_close(&rig);
}

static void a_refused_renewal_is_logged_with_its_stop(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease",
                                      RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "60000", "20000", "fixture-wrong-authentication"),
                                      2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_TRUE(rig.controller.rejections.last.has_safe_stop);
    ASSERT_EQ(rig.controller.rejections.last.safe_stop, BWG_REASON_CONTROL_FAILED);
    ASSERT_EQ(rig.controller.rejections.last.operation, BWG_AUTH_OPERATION_RENEW);
    rig_close(&rig);
}

static void a_renewal_for_another_lease_is_not_a_verifier_rejection(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease",
                                      RIG_RENEW_PAYLOAD_WITH("lease_fixture_02", "60000", "20000", RIG_FIXTURE_RENEW_AUTH), 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_EQ(rig.controller.rejections.boot_rejections, 0u);
    rig_close(&rig);
}

static void an_accepted_start_marks_the_high_water_advanced(void)
{
    // Arrange
    rig_t rig;

    // Act
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));

    // Assert
    ASSERT_TRUE(rig.controller.rejections.high_water_advanced);
    ASSERT_EQ(rig.controller.rejections.boot_rejections, 0u);
    rig_close(&rig);
}

static void a_later_stop_never_rewrites_a_recorded_rejection(void)
{
    // Arrange
    bwg_rejection_log_t log = {0};
    const bwg_auth_attribution_t attribution = {.signature = BWG_AUTH_SIGNATURE_VALID};
    bwg_rejection_log_record(&log, BWG_AUTH_OPERATION_RENEW, &attribution);

    // Act
    bwg_rejection_log_attribute_stop(&log, BWG_REASON_CONTROL_FAILED);
    bwg_rejection_log_attribute_stop(&log, BWG_REASON_CONNECTIVITY_LOST);

    // Assert
    ASSERT_EQ(log.last.safe_stop, BWG_REASON_CONTROL_FAILED);
}

static void an_accepted_verification_ends_the_wait_for_a_stop(void)
{
    // Arrange
    bwg_rejection_log_t log = {0};
    const bwg_auth_attribution_t attribution = {0};
    bwg_rejection_log_record(&log, BWG_AUTH_OPERATION_START, &attribution);
    bwg_rejection_log_mark_advanced(&log);

    // Act
    bwg_rejection_log_attribute_stop(&log, BWG_REASON_CONTROL_FAILED);

    // Assert
    ASSERT_FALSE(log.last.has_safe_stop);
}

void bwg_tests_controller_reviews(void)
{
    BWG_RUN(the_conformance_requests_are_sorted_by_shape_alone);
    BWG_RUN(boot_review_names_the_reset_cause);
    BWG_RUN(a_review_needs_fresh_possession);
    BWG_RUN(a_review_is_refused_during_a_lease);
    BWG_RUN(rejection_review_starts_empty_with_the_fingerprint);
    BWG_RUN(rejection_review_reports_an_unreadable_high_water);
    BWG_RUN(rejection_review_is_refused_with_no_store_to_fingerprint);
    BWG_RUN(a_refused_start_is_logged_with_no_stop);
    BWG_RUN(a_start_with_no_admission_is_logged_as_absent);
    BWG_RUN(an_old_admission_is_logged_as_expired);
    BWG_RUN(a_refused_renewal_is_logged_with_its_stop);
    BWG_RUN(a_renewal_for_another_lease_is_not_a_verifier_rejection);
    BWG_RUN(an_accepted_start_marks_the_high_water_advanced);
    BWG_RUN(a_later_stop_never_rewrites_a_recorded_rejection);
    BWG_RUN(an_accepted_verification_ends_the_wait_for_a_stop);
}
