#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_jws.h"
#include "test_suites.h"

static void probe_round_trips_the_largest_payloads(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));
    size_t response_overhead = strlen("{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"serial_test\",\"ok\":true,"
                                      "\"result\":{\"padding\":\"\",\"requestPaddingBytes\":65437}}");
    size_t response_padding = BWG_MAX_CONTROL_PAYLOAD_BYTES - response_overhead;
    size_t request_overhead = (size_t) snprintf(NULL, 0,
                                                RIG_ENVELOPE_HEAD "\"command\":\"transport_probe\",\"payload\":{\"padding\":\"\","
                                                              "\"responsePaddingBytes\":%zu}}",
                                                response_padding);
    size_t request_padding = BWG_MAX_CONTROL_PAYLOAD_BYTES - request_overhead;
    char *padding = malloc(request_padding + 1);
    memset(padding, 'x', request_padding);
    padding[request_padding] = '\0';
    char *frame = malloc(BWG_MAX_CONTROL_PAYLOAD_BYTES + 2);
    int written = snprintf(frame, BWG_MAX_CONTROL_PAYLOAD_BYTES + 2,
                           RIG_ENVELOPE_HEAD "\"command\":\"transport_probe\",\"payload\":{\"padding\":\"%s\","
                                         "\"responsePaddingBytes\":%zu}}\n",
                           padding, response_padding);

    // Act
    bwg_ctrl_error_t error = rig_send_raw(&rig, frame, (size_t) written, 1001);

    // Assert
    ASSERT_EQ(written, (int) BWG_MAX_CONTROL_PAYLOAD_BYTES + 1);
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_EQ(rig.outcome.response.len, BWG_MAX_CONTROL_PAYLOAD_BYTES);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/requestPaddingBytes", request_padding));
    ASSERT_EQ(rig.device.max_probes, 1);
    ASSERT_STREQ(rig.device.events, "");
    free(padding);
    free(frame);
    rig_close(&rig);
}

static void probe_refuses_arbitrary_data(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "transport_probe", "{\"padding\":\"arbitrary\",\"responsePaddingBytes\":9}", 1001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void probe_refuses_truncation_and_oversize(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    bwg_ctrl_error_t truncating = rig_send(&rig, "transport_probe", "{\"padding\":\"x\",\"responsePaddingBytes\":0}", 1001);
    bwg_ctrl_error_t oversized = rig_send(&rig, "transport_probe", "{\"padding\":\"x\",\"responsePaddingBytes\":65536}", 1001);

    // Assert
    ASSERT_EQ(truncating, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(oversized, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void probe_echoes_the_requested_size(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 1000));

    // Act
    ASSERT_EQ(rig_send(&rig, "transport_probe", "{\"padding\":\"xx\",\"responsePaddingBytes\":5}", 1001), BWG_CTRL_OK);
    ASSERT_EQ(rig.device.max_probes, 0);

    // Assert
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/padding", "xxxxx"));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/requestPaddingBytes", 2));
    ASSERT_FALSE(rig_reply_has(&rig, "/result/qualification"));
    rig_close(&rig);
}

static void probe_needs_a_fresh_admission_and_no_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    const char *payload = "{\"padding\":\"x\",\"responsePaddingBytes\":1}";

    // Act
    bwg_ctrl_error_t unadmitted = rig_send(&rig, "transport_probe", payload, 1000);
    ASSERT_TRUE(rig_admit(&rig, 1000));
    bwg_ctrl_error_t expired = rig_send(&rig, "transport_probe", payload, 61000);
    ASSERT_TRUE(rig_reprove(&rig, 61000));
    ASSERT_EQ(rig_send(&rig, "start_lease", RIG_START_PAYLOAD, 61000), BWG_CTRL_OK);
    bwg_ctrl_error_t leased = rig_send(&rig, "transport_probe", payload, 61001);
    bwg_ctrl_error_t bad_shape = rig_send(&rig, "transport_probe", "{\"padding\":\"x\"}", 61001);

    // Assert
    ASSERT_EQ(unadmitted, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_EQ(expired, BWG_CTRL_ADMISSION_REQUIRED);
    ASSERT_EQ(leased, BWG_CTRL_INVALID_TRANSITION);
    ASSERT_EQ(bad_shape, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void out_of_memory_while_parsing_is_session_failed_with_no_reply(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_test_alloc_fail_after(0);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "status", NULL, 1);

    // Assert
    bwg_test_alloc_fail_after(-1);
    ASSERT_EQ(error, BWG_CTRL_SESSION_FAILED);
    ASSERT_FALSE(rig.outcome.has_reply);
    ASSERT_TRUE(rig.outcome.revoke_after_send);
    rig_close(&rig);
}

static const bwg_controller_ops_t *rig_ops_of(const rig_t *rig)
{
    return &rig->controller.ops;
}

static bool open_with_config(const bwg_controller_config_t *config)
{
    rig_t rig;
    if (!rig_open(&rig)) {
        return false;
    }
    bwg_controller_t second;
    bool ok = bwg_controller_init(&second, &bwg_test_allocator, config, rig_ops_of(&rig));
    if (ok) {
        bwg_controller_deinit(&second);
    }
    rig_close(&rig);
    return ok;
}

static bwg_controller_config_t base_config(void)
{
    static const char CAPABILITY[] = "{\"protocolVersion\":\"bwg-worker-controller/0.4\"}";
    return (bwg_controller_config_t) {
        .capability_json = (const uint8_t *) CAPABILITY,
        .capability_len = strlen(CAPABILITY),
        .manifest_json = (const uint8_t *) BWG_SERIAL_MANIFEST_JSON,
        .manifest_len = strlen(BWG_SERIAL_MANIFEST_JSON),
        .firmware_commit = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .app_elf_hex = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        .seed = BWG_TEST_RFC8032_SEED,
    };
}

static void init_refuses_bad_configuration(void)
{
    // Arrange
    bwg_controller_config_t bad_commit = base_config();
    bad_commit.firmware_commit = "AAAA";
    bwg_controller_config_t bad_elf = base_config();
    bad_elf.app_elf_hex = "bbbb";
    bwg_controller_config_t no_seed = base_config();
    no_seed.seed = NULL;
    bwg_controller_config_t not_object = base_config();
    not_object.capability_json = (const uint8_t *) "[]";
    not_object.capability_len = 2;
    bwg_controller_config_t bad_manifest = base_config();
    bad_manifest.manifest_json = (const uint8_t *) "{";
    bad_manifest.manifest_len = 1;

    // Act / Assert
    ASSERT_FALSE(open_with_config(&bad_commit));
    ASSERT_FALSE(open_with_config(&bad_elf));
    ASSERT_FALSE(open_with_config(&no_seed));
    ASSERT_FALSE(open_with_config(&not_object));
    ASSERT_FALSE(open_with_config(&bad_manifest));
    bwg_controller_config_t good = base_config();
    ASSERT_TRUE(open_with_config(&good));
}

static void init_survives_running_out_of_memory_at_every_step(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    bwg_controller_config_t config = base_config();

    // Act / Assert: the harness reports any leak from the failed attempts.
    bool succeeded = false;
    for (long failure = 0; failure < 64 && !succeeded; failure++) {
        bwg_test_alloc_fail_after(failure);
        bwg_controller_t second;
        succeeded = bwg_controller_init(&second, &bwg_test_allocator, &config, rig_ops_of(&rig));
        bwg_test_alloc_fail_after(-1);
        if (succeeded) {
            bwg_controller_deinit(&second);
        }
    }
    ASSERT_TRUE(succeeded);
    rig_close(&rig);
}

static void init_refuses_an_unreadable_marker(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect_unreadable = true;
    bwg_controller_config_t config = base_config();
    bwg_controller_t second;

    // Act
    bool ok = bwg_controller_init(&second, &bwg_test_allocator, &config, rig_ops_of(&rig));

    // Assert
    ASSERT_FALSE(ok);
    rig_close(&rig);
}

static void init_refuses_a_marker_that_will_not_move(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    rig.device.effect = BWG_EFFECT_PENDING;
    rig.device.mark_failures = 1;
    bwg_controller_config_t config = base_config();
    bwg_controller_t second;

    // Act
    bool ok = bwg_controller_init(&second, &bwg_test_allocator, &config, rig_ops_of(&rig));

    // Assert
    ASSERT_FALSE(ok);
    rig_close(&rig);
}

static void a_capability_past_the_payload_limit_cannot_be_discovered(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    size_t padding = BWG_MAX_CONTROL_PAYLOAD_BYTES;
    char *capability = malloc(padding + 16);
    int written = snprintf(capability, padding + 16, "{\"a\":\"");
    memset(capability + written, 'x', padding);
    strcpy(capability + (size_t) written + padding, "\"}");
    bwg_controller_config_t config = base_config();
    config.capability_json = (const uint8_t *) capability;
    config.capability_len = strlen(capability);
    bwg_controller_t second;
    ASSERT_TRUE(bwg_controller_init(&second, &bwg_test_allocator, &config, rig_ops_of(&rig)));
    bwg_controller_deinit(&rig.controller);
    rig.controller = second;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "discover", NULL, 1);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_FRAME);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_INVALID_FRAME));
    free(capability);
    rig_close(&rig);
}

void bwg_tests_controller_probe(void)
{
    BWG_RUN(probe_round_trips_the_largest_payloads);
    BWG_RUN(probe_refuses_arbitrary_data);
    BWG_RUN(probe_refuses_truncation_and_oversize);
    BWG_RUN(probe_echoes_the_requested_size);
    BWG_RUN(probe_needs_a_fresh_admission_and_no_lease);
    BWG_RUN(out_of_memory_while_parsing_is_session_failed_with_no_reply);
    BWG_RUN(init_refuses_bad_configuration);
    BWG_RUN(init_survives_running_out_of_memory_at_every_step);
    BWG_RUN(init_refuses_an_unreadable_marker);
    BWG_RUN(init_refuses_a_marker_that_will_not_move);
    BWG_RUN(a_capability_past_the_payload_limit_cannot_be_discovered);
}
