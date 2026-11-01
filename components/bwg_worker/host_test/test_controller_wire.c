#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_jws.h"
#include "test_suites.h"

static void malformed_frames_are_invalid_frame_with_no_reply(void)
{
    static const struct {
        const char *bytes;
        size_t len;
    } CASES[] = {
        {"\n", 1},
        {"{}", 2},
        {"{\n}\n", 4},
        {"{\r}\n", 4},
        {"{\"a\":\xff}\n", 8},
        {"{\"a\":\"\xc0\xaf\"}\n", 10},
        {"[1,]\n", 5},
        {"{} {}\n", 6},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        // Arrange
        rig_t rig;
        ASSERT_TRUE(rig_open(&rig));

        // Act
        bwg_ctrl_error_t error = rig_send_raw(&rig, CASES[i].bytes, CASES[i].len, 1000);

        // Assert
        ASSERT_EQ(error, BWG_CTRL_INVALID_FRAME);
        ASSERT_FALSE(rig.outcome.has_reply);
        ASSERT_TRUE(rig.outcome.revoke_after_send);
        rig_close(&rig);
    }
}

static void an_oversized_frame_is_invalid_frame(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    size_t len = BWG_MAX_CONTROL_PAYLOAD_BYTES + 2;
    char *frame = malloc(len);
    memset(frame, ' ', len);
    frame[len - 1] = '\n';

    // Act
    bwg_ctrl_error_t error = rig_send_raw(&rig, frame, len, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_FRAME);
    free(frame);
    rig_close(&rig);
}

static void structurally_invalid_requests_are_invalid_request_with_no_reply(void)
{
    static const char *const CASES[] = {
        "[]",
        "null",
        "{\"profile\":1}",
        "{\"profile\":null}",
        "{\"requestId\":\"serial_test\",\"command\":\"status\"}",
        RIG_ENVELOPE_HEAD "\"command\":\"status\",\"extra\":1}",
        RIG_ENVELOPE_HEAD "\"command\":\"status\",\"command\":\"status\"}",
        RIG_ENVELOPE_HEAD "\"command\":1}",
        "{\"protocolVersion\":\"bwg-worker-controller/0.3\",\"requestId\":\"serial_test\",\"command\":\"status\"}",
        "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"test\",\"command\":\"status\"}",
        "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"serial_a b\",\"command\":\"status\"}",
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        // Arrange
        rig_t rig;
        ASSERT_TRUE(rig_open(&rig));

        // Act
        bwg_ctrl_error_t error = rig_send_json(&rig, CASES[i], 1000);

        // Assert
        ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
        ASSERT_FALSE(rig.outcome.has_reply);
        ASSERT_TRUE(rig.outcome.revoke_after_send);
        rig_close(&rig);
    }
}

static void request_id_length_is_bounded_at_128(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    char id[130];
    memset(id, 'a', sizeof(id));
    memcpy(id, "serial_", 7);
    char json[300];
    id[128] = '\0';
    snprintf(json, sizeof(json), "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"%s\",\"command\":\"status\"}", id);
    id[128] = 'a';
    id[129] = '\0';
    char too_long[300];
    snprintf(too_long, sizeof(too_long), "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"%s\",\"command\":\"status\"}", id);

    // Act
    bwg_ctrl_error_t longest = rig_send_json(&rig, json, 1000);
    bwg_ctrl_error_t over = rig_send_json(&rig, too_long, 1000);

    // Assert
    ASSERT_EQ(longest, BWG_CTRL_OK);
    ASSERT_EQ(over, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void a_duplicate_key_in_the_payload_gets_a_correlated_rejection(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "restore", "{\"reason\":\"paused\",\"reason\":\"paused\"}", 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_INVALID_REQUEST));
    rig_close(&rig);
}

static void an_unknown_command_gets_a_correlated_rejection(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "no_such_command", "{}", 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_INVALID_REQUEST));
    rig_close(&rig);
}

static void commands_that_take_no_payload_refuse_one(void)
{
    static const char *const COMMANDS[] = {"discover", "status", "pause", "cancel"};
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        // Arrange
        rig_t rig;
        ASSERT_TRUE(rig_open(&rig));

        // Act
        bwg_ctrl_error_t with_object = rig_send(&rig, COMMANDS[i], "{}", 1000);
        bwg_ctrl_error_t with_null = rig_send(&rig, COMMANDS[i], "null", 1000);

        // Assert
        ASSERT_EQ(with_object, BWG_CTRL_INVALID_REQUEST);
        ASSERT_EQ(with_null, BWG_CTRL_OK);
        rig_close(&rig);
    }
}

static void every_category_has_its_closed_text(void)
{
    // Arrange
    static const char *const EXPECTED[] = {"invalid_frame",        "invalid_request",     "admission_required",
                                           "invalid_proof",        "authentication_failed", "invalid_transition",
                                           "persistence_failed",   "monotonic_reset",     "session_failed",
                                           "restoration_pending",  "stale_response",      "encoding_failed"};

    // Act / Assert
    ASSERT_TRUE(bwg_ctrl_error_category(BWG_CTRL_OK) == NULL);
    for (int i = 0; i < 12; i++) {
        ASSERT_STREQ(bwg_ctrl_error_category((bwg_ctrl_error_t) (i + 1)), EXPECTED[i]);
    }
}

static void discover_sends_the_capability_verbatim_and_minified(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    yyjson_doc *fixture = bwg_test_fixture_doc("deployment-trust/signed-capability.json");
    size_t len = 0;
    char *compact = bwg_test_json_line(yyjson_doc_get_root(fixture), &len);
    compact[len - 1] = '\0';

    // Act
    ASSERT_EQ(rig_send(&rig, "discover", NULL, 1000), BWG_CTRL_OK);
    const char *reply = rig_reply_text(&rig);

    // Assert
    ASSERT_TRUE(strstr(reply, compact) != NULL);
    ASSERT_FALSE(rig.outcome.revoke_after_send);
    ASSERT_STREQ(bwg_controller_capability_digest(&rig.controller), "OUAcmoLu-Sjv6nq6fqavz8_3Ev18z7ziNXuEN3FVUrg");
    free(compact);
    yyjson_doc_free(fixture);
    rig_close(&rig);
}

static void fresh_status_is_baseline_not_required(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    ASSERT_EQ(rig_send(&rig, "status", NULL, 123), BWG_CTRL_OK);

    // Assert
    ASSERT_STREQ(rig_reply_text(&rig),
                 "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"serial_test\",\"ok\":true,\"result\":"
                 "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"state\":\"baseline\",\"monotonicMilliseconds\":123,"
                 "\"restoration\":{\"status\":\"not_required\"}}}");
    ASSERT_EQ(rig.outcome.effect.kind, BWG_CTRL_EFFECT_NONE);
    rig_close(&rig);
}

static void absent_evidence_leaves_no_key_behind(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.maybe_qualification_json = "{\"partial\":true}";

    // Act
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1), BWG_CTRL_OK);

    // Assert
    ASSERT_FALSE(rig_reply_has(&rig, "/result/qualification"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/state", "baseline"));
    rig_close(&rig);
}

static void qualification_joins_every_status_bearing_result(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    rig.device.maybe_qualification_json = "{\"schema\":\"fixture\",\"generation\":7}";
    rig.device.qualification_result = BWG_EVIDENCE_WRITTEN;

    // Act / Assert
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1000), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/qualification/generation", 7));
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 1000), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/qualification/generation", 7));
    ASSERT_EQ(rig_send(&rig, "renew_lease", RIG_RENEW_PAYLOAD, 1001), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/qualification/generation", 7));
    ASSERT_EQ(rig_send(&rig, "pause", NULL, 1002), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/qualification/generation", 7));
    ASSERT_EQ(rig_send(&rig, "cancel", NULL, 1003), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/qualification/generation", 7));
    ASSERT_EQ(rig_send(&rig, "restore", "{\"reason\":\"tab_closed\"}", 1004), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/qualification/generation", 7));
    rig_close(&rig);
}

static void discover_carries_no_evidence(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.maybe_qualification_json = "{\"schema\":\"fixture\"}";
    rig.device.qualification_result = BWG_EVIDENCE_WRITTEN;

    // Act
    ASSERT_EQ(rig_send(&rig, "discover", NULL, 1), BWG_CTRL_OK);

    // Assert
    ASSERT_FALSE(rig_reply_has(&rig, "/result/qualification"));
    rig_close(&rig);
}

static void preservation_needs_possession_in_this_session(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.preservation_result = BWG_EVIDENCE_WRITTEN;

    // Act
    ASSERT_EQ(rig_send(&rig, "status", NULL, 1), BWG_CTRL_OK);
    bool before = rig_reply_has(&rig, "/result/preservation");
    ASSERT_TRUE(rig_admit(&rig, 2));
    ASSERT_EQ(rig_send(&rig, "status", NULL, 3), BWG_CTRL_OK);
    bool after = rig_reply_str_is(&rig, "/result/preservation/device_identity_sha256", rig.controller.identity.fingerprint_hex);
    ASSERT_EQ(bwg_controller_disconnect(&rig.controller, 4), BWG_CTRL_OK);
    ASSERT_EQ(rig_send(&rig, "status", NULL, 5), BWG_CTRL_OK);
    bool after_disconnect = rig_reply_has(&rig, "/result/preservation");

    // Assert
    ASSERT_FALSE(before);
    ASSERT_TRUE(after);
    ASSERT_FALSE(after_disconnect);
    rig_close(&rig);
}

static void preservation_failures_keep_their_categories(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1));

    // Act
    rig.device.preservation_result = BWG_EVIDENCE_SESSION_FAILED;
    bwg_ctrl_error_t settings = rig_send(&rig, "status", NULL, 2);
    rig.device.preservation_result = BWG_EVIDENCE_PERSISTENCE_FAILED;
    bwg_ctrl_error_t high_water = rig_send(&rig, "status", NULL, 3);

    // Assert
    ASSERT_EQ(settings, BWG_CTRL_SESSION_FAILED);
    ASSERT_EQ(high_water, BWG_CTRL_PERSISTENCE_FAILED);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_PERSISTENCE_FAILED));
    rig_close(&rig);
}

static void evidence_past_the_status_buffer_is_an_encoding_failure(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.qualification_fill_bytes = BWG_CONTROLLER_STATUS_MAX_BYTES;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "status", NULL, 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_ENCODING_FAILED);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_ENCODING_FAILED));
    rig_close(&rig);
}

void bwg_tests_controller_wire(void)
{
    BWG_RUN(malformed_frames_are_invalid_frame_with_no_reply);
    BWG_RUN(an_oversized_frame_is_invalid_frame);
    BWG_RUN(structurally_invalid_requests_are_invalid_request_with_no_reply);
    BWG_RUN(request_id_length_is_bounded_at_128);
    BWG_RUN(a_duplicate_key_in_the_payload_gets_a_correlated_rejection);
    BWG_RUN(an_unknown_command_gets_a_correlated_rejection);
    BWG_RUN(commands_that_take_no_payload_refuse_one);
    BWG_RUN(every_category_has_its_closed_text);
    BWG_RUN(discover_sends_the_capability_verbatim_and_minified);
    BWG_RUN(fresh_status_is_baseline_not_required);
    BWG_RUN(absent_evidence_leaves_no_key_behind);
    BWG_RUN(qualification_joins_every_status_bearing_result);
    BWG_RUN(discover_carries_no_evidence);
    BWG_RUN(preservation_needs_possession_in_this_session);
    BWG_RUN(preservation_failures_keep_their_categories);
    BWG_RUN(evidence_past_the_status_buffer_is_an_encoding_failure);
}
