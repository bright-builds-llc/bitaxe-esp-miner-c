#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

static void possession_reply_is_the_exact_fixture_proof(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    yyjson_doc *fixture = bwg_test_fixture_doc("possession/fixtures.json");
    yyjson_val *vector = yyjson_obj_get(yyjson_doc_get_root(fixture), "initialAdmission");
    size_t len = 0;
    char *frame = bwg_test_json_line(yyjson_obj_get(vector, "request"), &len);

    // Act
    bwg_ctrl_error_t error = rig_send_raw(&rig, frame, len, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    yyjson_doc *reply = yyjson_read(rig_reply_text(&rig), rig.outcome.response.len, 0);
    ASSERT_TRUE(yyjson_equals(yyjson_doc_get_root(reply), yyjson_obj_get(vector, "response")));
    ASSERT_EQ(rig.outcome.effect.kind, BWG_CTRL_EFFECT_ADMIT);
    ASSERT_STREQ(rig.outcome.effect.binding, RIG_BINDING);
    yyjson_doc_free(reply);
    free(frame);
    yyjson_doc_free(fixture);
    rig_close(&rig);
}

static void proof_admits_only_after_it_was_confirmed_sent(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000), BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_EQ(rig_send_possession(&rig, 0, 1000), BWG_CTRL_OK);
    bwg_ctrl_effect_t effect = rig.outcome.effect;

    // Act
    bool admitted_before_send = bwg_controller_is_admitted(&rig.controller);
    bwg_ctrl_error_t unsent = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);
    bwg_ctrl_error_t confirm = bwg_controller_confirm_sent(&rig.controller, &effect, 0);
    bwg_ctrl_error_t sent = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);

    // Assert
    ASSERT_FALSE(admitted_before_send);
    ASSERT_EQ(unsent, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_EQ(confirm, BWG_CTRL_OK);
    ASSERT_EQ(sent, BWG_CTRL_OK);
    ASSERT_STREQ(rig.device.events, "verify_start,mark_pending,start");
    rig_close(&rig);
}

static void possession_without_a_session_needs_admission(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send_possession(&rig, 0, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_FALSE(rig.outcome.has_reply);
    rig_close(&rig);
}

static void possession_from_another_session_is_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    session.session_id[0] = 'Z';
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send_possession(&rig, 0, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_PROOF);
    ASSERT_FALSE(rig.outcome.has_reply);
    ASSERT_TRUE(rig.outcome.revoke_after_send);
    rig_close(&rig);
}

static void malformed_possession_gets_no_reply(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send_json(&rig, "{\"profile\":\"bwg-worker-possession/0.2\"}", 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_FALSE(rig.outcome.has_reply);
    ASSERT_TRUE(rig.outcome.revoke_after_send);
    rig_close(&rig);
}

static void second_proof_before_the_first_is_confirmed_is_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    ASSERT_EQ(rig_send_possession(&rig, 0, 1000), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send_possession(&rig, 1, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_PROOF);
    rig_close(&rig);
}

static void possession_during_a_lease_is_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send_possession(&rig, 5, 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_PROOF);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void possession_nonce_is_not_evicted_after_seventeen_proofs(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 0));
    for (int i = 1; i <= 17; i++) {
        ASSERT_TRUE(rig_reprove(&rig, (uint64_t) i));
    }

    // Act
    bwg_ctrl_error_t replay = rig_send_possession(&rig, 0, 18);

    // Assert
    ASSERT_EQ(replay, BWG_CTRL_INVALID_PROOF);
    rig_close(&rig);
}

static void possession_nonce_capacity_fails_closed(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 0));
    for (int i = 1; i < (int) BWG_CONTROLLER_MAX_SEEN_NONCES; i++) {
        ASSERT_TRUE(rig_reprove(&rig, (uint64_t) i));
    }

    // Act
    bwg_ctrl_error_t overflow = rig_send_possession(&rig, 4000, 300);

    // Assert
    ASSERT_EQ(overflow, BWG_CTRL_INVALID_PROOF);
    rig_close(&rig);
}

static void a_new_session_forgets_seen_nonces(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 0));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send_possession(&rig, 0, 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    rig_close(&rig);
}

static void a_new_session_cannot_replace_active_work(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1001), BWG_CTRL_OK);
    bwg_session_binding_t session = rig_fixture_session();

    // Act
    bwg_ctrl_error_t error = bwg_controller_begin_session(&rig.controller, &session);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

static void a_new_session_makes_an_unconfirmed_proof_stale(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    ASSERT_EQ(rig_send_possession(&rig, 0, 1000), BWG_CTRL_OK);
    bwg_ctrl_effect_t effect = rig.outcome.effect;
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = bwg_controller_confirm_sent(&rig.controller, &effect, 0);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_STALE_RESPONSE);
    ASSERT_FALSE(bwg_controller_is_admitted(&rig.controller));
    rig_close(&rig);
}

static void a_proof_confirms_only_once(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_confirm(&rig);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_STALE_RESPONSE);
    rig_close(&rig);
}

static void a_stop_makes_an_unconfirmed_proof_stale(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    ASSERT_EQ(rig_send_possession(&rig, 0, 1000), BWG_CTRL_OK);
    bwg_ctrl_effect_t effect = rig.outcome.effect;
    ASSERT_EQ(rig_send(&rig, "pause", NULL, 1001), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = bwg_controller_confirm_sent(&rig.controller, &effect, 0);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_STALE_RESPONSE);
    ASSERT_FALSE(bwg_controller_is_admitted(&rig.controller));
    rig_close(&rig);
}

static void unused_context_expires_at_sixty_seconds(void)
{
    // Arrange
    rig_t accepted;
    rig_t expired;
    ASSERT_TRUE(rig_open(&accepted));
    ASSERT_TRUE(rig_open(&expired));
    ASSERT_TRUE(rig_admit(&accepted, 1000));
    ASSERT_TRUE(rig_admit(&expired, 1000));

    // Act
    bwg_ctrl_error_t last_millisecond = rig_send(&accepted, "start_lease", RIG_START_PAYLOAD, 60999);
    bwg_ctrl_error_t boundary = rig_send(&expired, "start_lease", RIG_START_PAYLOAD, 61000);

    // Assert
    ASSERT_EQ(last_millisecond, BWG_CTRL_OK);
    ASSERT_EQ(boundary, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_STREQ(expired.device.events, "");
    rig_close(&accepted);
    rig_close(&expired);
}

static void boot_marker_pending_moves_to_reboot_confirmed(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_PENDING;

    // Act
    ASSERT_TRUE(rig_reboot(&rig));

    // Assert
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_REBOOT_CONFIRMED);
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/reason", "reboot"));
    rig_close(&rig);
}

static void boot_marker_already_confirmed_is_not_rewritten(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_REBOOT_CONFIRMED;
    rig.device.effect_writes = 0;

    // Act
    ASSERT_TRUE(rig_reboot(&rig));

    // Assert
    ASSERT_EQ(rig.device.effect_writes, 0);
    rig_close(&rig);
}

static void reboot_report_stays_durable_until_a_followup_request(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_PENDING;
    ASSERT_TRUE(rig_reboot(&rig));
    rig.device.events[0] = '\0';
    ASSERT_EQ(bwg_controller_disconnect(&rig.controller, 1), BWG_CTRL_OK);

    // Act
    ASSERT_EQ(rig_send(&rig, "status", NULL, 2), BWG_CTRL_OK);
    bool reported_reboot = rig_reply_str_is(&rig, "/result/restoration/reason", "reboot");
    ASSERT_EQ(rig.outcome.effect.kind, BWG_CTRL_EFFECT_BOOT_REPORTED);
    ASSERT_EQ(rig_confirm(&rig), BWG_CTRL_OK);
    bwg_effect_state_t after_status = rig.device.effect;
    bwg_ctrl_error_t invalid = rig_send_raw(&rig, "{invalid}\n", 10, 3);
    bwg_effect_state_t after_invalid = rig.device.effect;
    bwg_ctrl_error_t followup = rig_send(&rig, "status", NULL, 4);

    // Assert
    ASSERT_TRUE(reported_reboot);
    ASSERT_EQ(after_status, BWG_EFFECT_REBOOT_CONFIRMED);
    ASSERT_EQ(invalid, BWG_CTRL_INVALID_FRAME);
    ASSERT_EQ(after_invalid, BWG_EFFECT_REBOOT_CONFIRMED);
    ASSERT_EQ(followup, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_CLEAR);
    ASSERT_STREQ(rig.device.events, "clear_pending");
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/reason", "connectivity_lost"));
    rig_close(&rig);
}

static void a_new_session_cannot_acknowledge_an_old_status_reply(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_PENDING;
    ASSERT_TRUE(rig_reboot(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1), BWG_CTRL_OK);
    ASSERT_EQ(rig_confirm(&rig), BWG_CTRL_OK);
    ASSERT_EQ(bwg_controller_disconnect(&rig.controller, 2), BWG_CTRL_OK);

    // Act
    ASSERT_EQ(rig_send(&rig, "discover", NULL, 3), BWG_CTRL_OK);
    bwg_effect_state_t after_discover = rig.device.effect;
    ASSERT_EQ(rig_send(&rig, "status", NULL, 4), BWG_CTRL_OK);

    // Assert
    ASSERT_EQ(after_discover, BWG_EFFECT_REBOOT_CONFIRMED);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/restoration/reason", "reboot"));
    rig_close(&rig);
}

static void a_status_reply_from_an_old_session_is_stale(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_PENDING;
    ASSERT_TRUE(rig_reboot(&rig));
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1), BWG_CTRL_OK);
    bwg_ctrl_effect_t effect = rig.outcome.effect;
    ASSERT_EQ(bwg_controller_disconnect(&rig.controller, 2), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = bwg_controller_confirm_sent(&rig.controller, &effect, 0);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_STALE_RESPONSE);
    rig_close(&rig);
}

static void start_before_the_reboot_was_reported_is_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_REBOOT_CONFIRMED;
    ASSERT_TRUE(rig_reboot(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_INVALID_TRANSITION));
    rig_close(&rig);
}

static void possession_after_the_report_acknowledges_the_reboot(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_REBOOT_CONFIRMED;
    ASSERT_TRUE(rig_reboot(&rig));
    bwg_session_binding_t session = rig_fixture_session();
    ASSERT_EQ(bwg_controller_begin_session(&rig.controller, &session), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1), BWG_CTRL_OK);
    ASSERT_EQ(rig_confirm(&rig), BWG_CTRL_OK);

    // Act
    ASSERT_TRUE(rig_reprove(&rig, 2));
    bwg_effect_state_t after_proof = rig.device.effect;
    bwg_ctrl_error_t start = rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 3);

    // Assert
    ASSERT_EQ(after_proof, BWG_EFFECT_CLEAR);
    ASSERT_EQ(start, BWG_CTRL_OK);
    ASSERT_EQ(rig.device.effect, BWG_EFFECT_PENDING);
    rig_close(&rig);
}

void bwg_tests_controller_session(void)
{
    BWG_RUN(possession_reply_is_the_exact_fixture_proof);
    BWG_RUN(proof_admits_only_after_it_was_confirmed_sent);
    BWG_RUN(possession_without_a_session_needs_admission);
    BWG_RUN(possession_from_another_session_is_refused);
    BWG_RUN(malformed_possession_gets_no_reply);
    BWG_RUN(second_proof_before_the_first_is_confirmed_is_refused);
    BWG_RUN(possession_during_a_lease_is_refused);
    BWG_RUN(possession_nonce_is_not_evicted_after_seventeen_proofs);
    BWG_RUN(possession_nonce_capacity_fails_closed);
    BWG_RUN(a_new_session_forgets_seen_nonces);
    BWG_RUN(a_new_session_cannot_replace_active_work);
    BWG_RUN(a_new_session_makes_an_unconfirmed_proof_stale);
    BWG_RUN(a_proof_confirms_only_once);
    BWG_RUN(a_stop_makes_an_unconfirmed_proof_stale);
    BWG_RUN(unused_context_expires_at_sixty_seconds);
    BWG_RUN(boot_marker_pending_moves_to_reboot_confirmed);
    BWG_RUN(boot_marker_already_confirmed_is_not_rewritten);
    BWG_RUN(reboot_report_stays_durable_until_a_followup_request);
    BWG_RUN(a_new_session_cannot_acknowledge_an_old_status_reply);
    BWG_RUN(a_status_reply_from_an_old_session_is_stale);
    BWG_RUN(start_before_the_reboot_was_reported_is_refused);
    BWG_RUN(possession_after_the_report_acknowledges_the_reboot);
}
