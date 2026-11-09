#include <string.h>

#include "bwg_onboarding.h"
#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_onboarding_support.h"
#include "test_suites.h"

#define NONCE "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"
#define VERIFY_PAYLOAD "{\"nonce\":\"" NONCE "\"}"
// The pool entry is the longest value, so its tail outlives the shorter reads after it.
#define POOL_SECRET "worker.example"

// The rig's context is its device, so the onboarding half keeps its own state here.
static fake_nvs_t s_nvs;
static bwg_partition_facts_t s_facts;
static bool s_facts_fail;

static bool op_partition_facts(void *ctx, bwg_partition_facts_t *facts)
{
    (void) ctx;
    *facts = s_facts;
    return !s_facts_fail;
}

static bwg_setting_read_t op_read_setting(void *ctx, const char *key, uint8_t *type, uint8_t *value, size_t capacity,
                                          size_t *len)
{
    (void) ctx;
    const bwg_settings_reader_t reader = fake_nvs_reader(&s_nvs);
    return reader.read(reader.ctx, key, type, value, capacity, len);
}

static const bwg_onboarding_ops_t ONBOARDING_OPS = {
    .partition_facts = op_partition_facts,
    .read_setting = op_read_setting,
};

static bool open_with_onboarding(rig_t *rig)
{
    yyjson_doc *doc = test_settings_vectors();
    yyjson_val *vectors = yyjson_obj_get(yyjson_doc_get_root(doc), "vectors");
    const bool loaded = fake_nvs_load(&s_nvs, yyjson_obj_get(yyjson_arr_get(vectors, 1), "entries"));
    yyjson_doc_free(doc);
    s_facts = (bwg_partition_facts_t) {
        .has_factory = true,
        .has_ota_0 = true,
        .has_ota_1 = true,
        .has_otadata = true,
        .ota_0_bytes = 0x400000,
        .ota_1_bytes = 0x400000,
        .running_slot = BWG_SLOT_OTA_0,
        .running_state = BWG_IMAGE_VALID,
        .rollback_possible = true,
    };
    s_facts_fail = false;
    if (!loaded || !rig_open(rig)) {
        return false;
    }
    rig->controller.ops.maybe_onboarding = &ONBOARDING_OPS;
    return true;
}

static void inspect_answers_without_admission(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_inspect", NULL, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/settingsSchemaVersion", 1));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/partition/scheme", "esp32-factory-ota-ab"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/partition/activeSlot", "ota_0"));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/partition/appSlotBytes", 0x400000));
    ASSERT_FALSE(rig_reply_has(&rig, "/result/capabilities"));
    ASSERT_TRUE(strstr(rig_reply_text(&rig), "\"settingsReadable\":true") != NULL);
    rig_close(&rig);
}

static void inspect_reports_unreadable_settings(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));
    s_nvs.maybe_failing_key = "wifipass";

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_inspect", NULL, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(strstr(rig_reply_text(&rig), "\"settingsReadable\":false") != NULL);
    rig_close(&rig);
}

static void verify_answers_under_the_hosts_nonce(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_verify", VERIFY_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/nonce", NONCE));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/runningFirmwareVersion", "0.1.0"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/categories/0/hmacSha256", "_WYg-IJe-edFGTGJWVP3cZKBN-RjoRHHhDEl11r1Hw0"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/categories/1/hmacSha256", "aunQUKzM3bX9-78n2JOx3BWUN1smAIsMtHEIySbywNw"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/categories/2/hmacSha256", "jaj5169aT5klDQfPCIuhD5uqGo1LHkDCG2usrGo1JAo"));
    ASSERT_FALSE(rig_reply_has(&rig, "/result/rollbackState"));
    ASSERT_FALSE(rig_reply_has(&rig, "/result/categories/0/digestSha256"));
    rig_close(&rig);
}

static void verify_leaves_no_setting_in_freed_memory(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));
    bwg_test_alloc_watch_secret(POOL_SECRET);

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_verify", VERIFY_PAYLOAD, 1000);
    rig_close(&rig);
    bwg_test_alloc_watch_secret(NULL);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
}

static void verify_refuses_unreadable_settings(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));
    s_nvs.maybe_failing_key = "pool_1";

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_verify", VERIFY_PAYLOAD, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_PERSISTENCE_FAILED);
    ASSERT_TRUE(rig_reply_is_rejection(&rig, BWG_CTRL_PERSISTENCE_FAILED));
    rig_close(&rig);
}

static void unreadable_partition_facts_are_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));
    s_facts_fail = true;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_inspect", NULL, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_PERSISTENCE_FAILED);
    rig_close(&rig);
}

static void malformed_onboarding_requests_are_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));

    // Act
    bwg_ctrl_error_t inspect_with_payload = rig_send(&rig, "onboarding_inspect", "{}", 1000);
    bwg_ctrl_error_t verify_without_payload = rig_send(&rig, "onboarding_verify", NULL, 1001);
    bwg_ctrl_error_t verify_with_short_nonce = rig_send(&rig, "onboarding_verify", "{\"nonce\":\"AAEC\"}", 1002);

    // Assert
    ASSERT_EQ(inspect_with_payload, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(verify_without_payload, BWG_CTRL_INVALID_REQUEST);
    ASSERT_EQ(verify_with_short_nonce, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void without_onboarding_ops_the_commands_are_refused(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_inspect", NULL, 1000);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    rig_close(&rig);
}

static void verify_answers_during_a_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(open_with_onboarding(&rig));
    rig_close(&rig);
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    rig.controller.ops.maybe_onboarding = &ONBOARDING_OPS;

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "onboarding_verify", VERIFY_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    rig_close(&rig);
}

void bwg_tests_controller_onboarding(void)
{
    BWG_RUN(inspect_answers_without_admission);
    BWG_RUN(inspect_reports_unreadable_settings);
    BWG_RUN(verify_answers_under_the_hosts_nonce);
    BWG_RUN(verify_leaves_no_setting_in_freed_memory);
    BWG_RUN(verify_refuses_unreadable_settings);
    BWG_RUN(unreadable_partition_facts_are_refused);
    BWG_RUN(malformed_onboarding_requests_are_refused);
    BWG_RUN(without_onboarding_ops_the_commands_are_refused);
    BWG_RUN(verify_answers_during_a_lease);
}
