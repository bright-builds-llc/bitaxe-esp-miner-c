#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

// ---- The deployment-trust table, through the controller with the real verifier.

/** A fixture Start or Renew payload with `authorization` added. Caller frees it. */
static char *real_payload(const char *input_path, const char *authorization)
{
    yyjson_doc *input = bwg_test_fixture_doc(input_path);
    size_t len = 0;
    char *line = bwg_test_json_line(yyjson_obj_get(yyjson_doc_get_root(input), "request"), &len);
    size_t capacity = len + strlen(authorization) + 64;
    char *payload = malloc(capacity);
    snprintf(payload, capacity, "%.*s,\"authorization\":\"%s\"}", (int) (len - 2), line, authorization);
    free(line);
    yyjson_doc_free(input);
    return payload;
}

/** The compact JWS of an authorization fixture. Caller frees it. */
static char *fixture_jws(const char *path)
{
    yyjson_doc *doc = bwg_test_fixture_doc(path);
    char *jws = strdup(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "compactJws")));
    yyjson_doc_free(doc);
    return jws;
}

static char *real_start_payload(void)
{
    char *jws = fixture_jws("deployment-trust/start-authorization.json");
    char *payload = real_payload("deployment-trust/start-input.json", jws);
    free(jws);
    return payload;
}

static char *real_renew_payload(void)
{
    char *jws = fixture_jws("deployment-trust/renew-authorization.json");
    char *payload = real_payload("deployment-trust/renew-input.json", jws);
    free(jws);
    return payload;
}

static void replace_text(char *text, const char *from, const char *to)
{
    char *found = strstr(text, from);
    if (found != NULL && strlen(from) == strlen(to)) {
        memcpy(found, to, strlen(to));
    }
}

/** Sends the fixture Start with one change at a time, proving each costs no sequence and starts nothing. */
static void expect_refused_start(rig_t *rig, const char *payload, bwg_ctrl_error_t expected)
{
    ASSERT_EQ(rig_send(rig, "start_lease", payload, 1000), expected);
    ASSERT_TRUE(rig_reply_is_rejection(rig, expected));
    ASSERT_FALSE(rig->device.high_water_present);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig->controller));
    ASSERT_STREQ(rig->device.events, "");
}

static void real_valid_renew_is_accepted_after_every_rejection(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    char *renew = real_renew_payload();
    rig.next_nonce = 1;
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act / Assert: a binding other than the signed one.
    expect_refused_start(&rig, start, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_EQ(rig_send_possession(&rig, 0, 1000), BWG_CTRL_OK);
    ASSERT_EQ(rig_confirm(&rig), BWG_CTRL_OK);
    ASSERT_STREQ(rig.controller.admission_binding, RIG_BINDING);

    // A changed pool password.
    char *changed = strdup(start);
    replace_text(changed, RIG_PASSWORD, "fixture-session-passwore");
    expect_refused_start(&rig, changed, BWG_CTRL_AUTHENTICATION_FAILED);
    free(changed);

    // A flipped signature character.
    char *flipped = strdup(start);
    char *end_quote = strrchr(flipped, '"');
    end_quote[-1] = end_quote[-1] == 'A' ? 'Q' : 'A';
    expect_refused_start(&rig, flipped, BWG_CTRL_AUTHENTICATION_FAILED);
    free(flipped);

    // 513 bytes fails validation: the controller's syntax limit, before any signature work.
    char oversized[514];
    memset(oversized, 'a', 513);
    oversized[513] = '\0';
    char *too_long = real_payload("deployment-trust/start-input.json", oversized);
    expect_refused_start(&rig, too_long, BWG_CTRL_INVALID_REQUEST);
    free(too_long);

    // No rejection moved the high water, so the valid pair still goes through.
    ASSERT_EQ(rig_send(&rig, "start_lease", start, 1000), BWG_CTRL_OK);
    ASSERT_EQ(rig.device.high_water, 1);
    ASSERT_EQ(rig_send(&rig, "renew_lease", renew, 1001), BWG_CTRL_OK);
    ASSERT_EQ(rig.device.high_water, 2);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/state", "mining"));
    free(start);
    free(renew);
    rig_close(&rig);
}

static void real_replayed_renew_stops_mining_and_keeps_the_high_water(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    char *renew = real_renew_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", start, 1000), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "renew_lease", renew, 1001), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease", renew, 1002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_EQ(rig.device.high_water, 2);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "mark_pending,start,renew,control_failed,clear_pending");
    free(start);
    free(renew);
    rig_close(&rig);
}

static void real_replayed_renew_is_attributed_to_the_high_water(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    char *renew = real_renew_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", start, 1000), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "renew_lease", renew, 1001), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "renew_lease", renew, 1002), BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_TRUE(rig_admit(&rig, 1003));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "authorization_rejection_review", "{}", 1004);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/operation", "renew"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/signature", "valid"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/replayGuard", "at_or_below_durable_high_water"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/last/safeStop", "control_failed"));
    free(start);
    free(renew);
    rig_close(&rig);
}

static void real_replayed_start_in_a_new_session_is_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", start, 1000), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "pause", NULL, 1001), BWG_CTRL_OK);
    rig.next_nonce = 0;
    ASSERT_TRUE(rig_admit(&rig, 1002));
    ASSERT_STREQ(rig.controller.admission_binding, RIG_BINDING);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 1002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_EQ(rig.device.high_water, 1);
    free(start);
    rig_close(&rig);
}

static void real_tampered_renew_stops_mining_without_advancing(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    char *renew = real_renew_payload();
    char *end_quote = strrchr(renew, '"');
    end_quote[-1] = end_quote[-1] == 'A' ? 'Q' : 'A';
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "start_lease", start, 1000), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "renew_lease", renew, 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_EQ(rig.device.high_water, 1);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    free(start);
    free(renew);
    rig_close(&rig);
}

static void real_corrupt_high_water_refuses_the_start(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    rig.device.high_water_corrupt = true;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_STREQ(rig.device.events, "");
    free(start);
    rig_close(&rig);
}

static void real_out_of_memory_for_the_reply_burns_no_sequence(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    bwg_test_alloc_fail_size(BWG_CONTROLLER_STATUS_MAX_BYTES);

    // Act
    bwg_ctrl_error_t failed = rig_send(&rig, "start_lease", start, 1000);
    bwg_test_alloc_fail_size(0);
    bool stored = rig.device.high_water_present;
    bwg_ctrl_error_t retried = rig_send(&rig, "start_lease", start, 1000);

    // Assert
    ASSERT_EQ(failed, BWG_CTRL_SESSION_FAILED);
    ASSERT_FALSE(stored);
    ASSERT_EQ(retried, BWG_CTRL_OK);
    free(start);
    rig_close(&rig);
}

static void leases_are_disabled_without_a_trust_document(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_trust_text(&rig, NULL));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_STREQ(rig.device.events, "");
    free(start);
    rig_close(&rig);
}

static void leases_are_disabled_by_an_invalid_trust_document(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_trust_text(&rig, "{\"profile\":\"unknown\"}"));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_AUTHENTICATION_FAILED);
    ASSERT_FALSE(rig.device.high_water_present);
    free(start);
    rig_close(&rig);
}

static void negative_withheld_context_needs_new_admission(void)
{
    // Arrange: the fixture's times, established 1000 and presented 61000.
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 61000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_FALSE(rig.device.high_water_present);
    free(start);
    rig_close(&rig);
}

static void negative_post_restore_context_needs_new_admission(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_EQ(rig_send(&rig, "restore", "{\"reason\":\"paused\"}", 1001), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 1002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_FALSE(rig.device.high_water_present);
    free(start);
    rig_close(&rig);
}

static void negative_post_reboot_context_needs_new_admission(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));
    ASSERT_TRUE(rig_reboot(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 1002);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_FALSE(rig.device.high_water_present);
    free(start);
    rig_close(&rig);
}

static void negative_monotonic_reset_clears_the_context(void)
{
    // Arrange: the fixture's times, established 1000 and presented 999.
    rig_t rig;
    ASSERT_TRUE(rig_open_real_trust(&rig));
    char *start = real_start_payload();
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "start_lease", start, 999);
    bwg_ctrl_error_t after = rig_send(&rig, "start_lease", start, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_MONOTONIC_RESET);
    ASSERT_EQ(after, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_FALSE(rig.device.high_water_present);
    free(start);
    rig_close(&rig);
}

static void authorization_boundaries_reach_the_verifier_up_to_512_bytes(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    static const struct {
        size_t bytes;
        bwg_ctrl_error_t expected;
    } CASES[] = {
        {511, BWG_CTRL_AUTHENTICATION_FAILED},
        {512, BWG_CTRL_AUTHENTICATION_FAILED},
        {513, BWG_CTRL_INVALID_REQUEST},
    };
    for (size_t i = 0; i < 3; i++) {
        char authorization[514];
        memset(authorization, 'a', CASES[i].bytes);
        authorization[CASES[i].bytes] = '\0';
        char *payload = real_payload("deployment-trust/start-input.json", authorization);

        // Act
        bwg_ctrl_error_t error = rig_send(&rig, "start_lease", payload, 1000);

        // Assert
        ASSERT_EQ(error, CASES[i].expected);
        free(payload);
    }
    rig_close(&rig);
}

void bwg_tests_controller_trust(void)
{
    BWG_RUN(real_valid_renew_is_accepted_after_every_rejection);
    BWG_RUN(real_replayed_renew_stops_mining_and_keeps_the_high_water);
    BWG_RUN(real_replayed_renew_is_attributed_to_the_high_water);
    BWG_RUN(real_replayed_start_in_a_new_session_is_refused);
    BWG_RUN(real_tampered_renew_stops_mining_without_advancing);
    BWG_RUN(real_corrupt_high_water_refuses_the_start);
    BWG_RUN(real_out_of_memory_for_the_reply_burns_no_sequence);
    BWG_RUN(leases_are_disabled_without_a_trust_document);
    BWG_RUN(leases_are_disabled_by_an_invalid_trust_document);
    BWG_RUN(negative_withheld_context_needs_new_admission);
    BWG_RUN(negative_post_restore_context_needs_new_admission);
    BWG_RUN(negative_post_reboot_context_needs_new_admission);
    BWG_RUN(negative_monotonic_reset_clears_the_context);
    BWG_RUN(authorization_boundaries_reach_the_verifier_up_to_512_bytes);
}
