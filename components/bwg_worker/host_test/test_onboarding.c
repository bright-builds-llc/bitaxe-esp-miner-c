#include <stdlib.h>
#include <string.h>

#include "bwg_codec.h"
#include "bwg_onboarding.h"
#include "bwg_test.h"
#include "test_onboarding_support.h"
#include "test_suites.h"

#define REPLY_CAPACITY 1024u
#define VECTOR_NONCE "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"

static bwg_partition_facts_t c_table_on(bwg_app_slot_t slot, bwg_image_state_t state)
{
    return (bwg_partition_facts_t) {
        .has_factory = true,
        .has_ota_0 = true,
        .has_ota_1 = true,
        .has_otadata = true,
        .ota_0_bytes = 0x400000,
        .ota_1_bytes = 0x400000,
        .running_slot = slot,
        .running_state = state,
        .rollback_possible = true,
    };
}

static yyjson_val *find_named(yyjson_val *array, const char *name)
{
    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, max, item)
    {
        const char *maybe_name = yyjson_get_str(yyjson_obj_get(item, "name"));
        if (maybe_name != NULL && strcmp(maybe_name, name) == 0) {
            return item;
        }
    }
    return NULL;
}

static bool fill_hmacs(bwg_onboarding_verification_t *verification, yyjson_val *entries)
{
    fake_nvs_t nvs;
    uint8_t scratch[BWG_SETTINGS_VALUE_MAX_BYTES];
    if (!fake_nvs_load(&nvs, entries)) {
        return false;
    }
    const bwg_settings_reader_t reader = fake_nvs_reader(&nvs);
    for (int i = 0; i < BWG_SETTINGS_CATEGORY_COUNT; i++) {
        if (!bwg_settings_hmac((bwg_settings_category_t) i, &reader, verification->nonce, scratch, verification->hmacs[i])) {
            return false;
        }
    }
    return true;
}

static void verification_is_the_gates_accepted_reply_byte_for_byte(void)
{
    // Arrange
    yyjson_doc *doc = test_settings_vectors();
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *replies = yyjson_obj_get(root, "verificationReplies");
    yyjson_val *accepted = yyjson_obj_get(find_named(yyjson_obj_get(replies, "accepted"), "stock-settings-on-ota_0"), "reply");
    yyjson_val *stock = yyjson_obj_get(find_named(yyjson_obj_get(root, "vectors"), "stock-settings"), "entries");
    bwg_onboarding_verification_t verification = {
        .firmware_version = "2.13.0",
        .facts = c_table_on(BWG_SLOT_OTA_0, BWG_IMAGE_VALID),
    };
    size_t nonce_len = 0;
    const bool nonce_ok = bwg_b64url_decode(VECTOR_NONCE, strlen(VECTOR_NONCE), verification.nonce,
                                            sizeof(verification.nonce), &nonce_len);
    const bool hmacs_ok = fill_hmacs(&verification, stock);
    size_t expected_len = 0;
    char *expected = bwg_test_json_line(accepted, &expected_len);
    uint8_t actual[REPLY_CAPACITY];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, actual, sizeof(actual));

    // Act
    bwg_onboarding_write_verification(&writer, &verification);

    // Assert
    const bool same = bwg_jw_ok(&writer) && writer.len == expected_len - 1 && memcmp(actual, expected, writer.len) == 0;
    free(expected);
    yyjson_doc_free(doc);
    ASSERT_TRUE(nonce_ok);
    ASSERT_TRUE(hmacs_ok);
    ASSERT_TRUE(same);
}

static yyjson_doc *write_inspection(const bwg_partition_facts_t *facts, bool readable)
{
    static uint8_t buffer[REPLY_CAPACITY];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, buffer, sizeof(buffer));
    bwg_onboarding_write_inspection(&writer, facts, readable);
    return bwg_jw_ok(&writer) ? yyjson_read((const char *) buffer, writer.len, 0) : NULL;
}

static void inspection_reports_the_snapshot_facts(void)
{
    // Arrange
    bwg_partition_facts_t facts = c_table_on(BWG_SLOT_FACTORY, BWG_IMAGE_UNDEFINED);
    facts.ota_1_bytes = 0x300000;

    // Act
    yyjson_doc *doc = write_inspection(&facts, true);

    // Assert
    ASSERT_TRUE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *partition = yyjson_obj_get(root, "partition");
    const bool ok = yyjson_obj_size(root) == 3 && yyjson_get_uint(yyjson_obj_get(root, "settingsSchemaVersion")) == 1 &&
                    yyjson_get_bool(yyjson_obj_get(root, "settingsReadable")) && yyjson_obj_size(partition) == 5 &&
                    strcmp(yyjson_get_str(yyjson_obj_get(partition, "scheme")), "esp32-factory-ota-ab") == 0 &&
                    yyjson_get_uint(yyjson_obj_get(partition, "appSlotBytes")) == 0x300000 &&
                    yyjson_get_bool(yyjson_obj_get(partition, "rollbackAvailable")) &&
                    strcmp(yyjson_get_str(yyjson_obj_get(partition, "activeSlot")), "factory") == 0 &&
                    yyjson_get_bool(yyjson_obj_get(partition, "bootable"));
    yyjson_doc_free(doc);
    ASSERT_TRUE(ok);
}

static void a_table_without_otadata_is_an_unknown_scheme(void)
{
    // Arrange
    bwg_partition_facts_t facts = c_table_on(BWG_SLOT_FACTORY, BWG_IMAGE_UNDEFINED);
    facts.has_otadata = false;

    // Act
    const char *scheme = bwg_onboarding_scheme(&facts);

    // Assert
    ASSERT_STREQ(scheme, "unknown");
}

static void a_pending_image_is_not_yet_bootable(void)
{
    // Arrange / Act / Assert
    ASSERT_FALSE(bwg_onboarding_bootable(BWG_IMAGE_PENDING_VERIFY));
    ASSERT_FALSE(bwg_onboarding_bootable(BWG_IMAGE_NEW));
    ASSERT_TRUE(bwg_onboarding_bootable(BWG_IMAGE_VALID));
    ASSERT_TRUE(bwg_onboarding_bootable(BWG_IMAGE_UNDEFINED));
}

static void a_pending_image_cannot_confirm_preservation(void)
{
    // Arrange
    bwg_onboarding_verification_t verification = {
        .firmware_version = "0.1.0",
        .facts = c_table_on(BWG_SLOT_OTA_1, BWG_IMAGE_PENDING_VERIFY),
    };
    uint8_t buffer[REPLY_CAPACITY];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, buffer, sizeof(buffer));

    // Act
    bwg_onboarding_write_verification(&writer, &verification);

    // Assert
    ASSERT_TRUE(bwg_jw_ok(&writer));
    yyjson_doc *doc = yyjson_read((const char *) buffer, writer.len, 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    const bool confirmed = yyjson_get_bool(yyjson_obj_get(root, "preservationConfirmed"));
    const bool bootable = yyjson_get_bool(yyjson_obj_get(root, "bootable"));
    const size_t fields = yyjson_obj_size(root);
    yyjson_doc_free(doc);
    ASSERT_FALSE(confirmed);
    ASSERT_FALSE(bootable);
    ASSERT_EQ(fields, 7u);
}

static bool parse_verify_text(const char *json)
{
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    uint8_t nonce[BWG_SETTINGS_NONCE_BYTES];
    const bool ok = bwg_onboarding_parse_verify(yyjson_doc_get_root(doc), nonce);
    yyjson_doc_free(doc);
    return ok;
}

static void verify_takes_exactly_a_canonical_32_byte_nonce(void)
{
    // Arrange / Act / Assert
    ASSERT_TRUE(parse_verify_text("{\"nonce\":\"" VECTOR_NONCE "\"}"));
    ASSERT_FALSE(parse_verify_text("{\"nonce\":\"" VECTOR_NONCE "=\"}"));
    ASSERT_FALSE(parse_verify_text("{\"nonce\":\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg\"}"));
    ASSERT_FALSE(parse_verify_text("{\"nonce\":\"" VECTOR_NONCE "\",\"settingsSchema\":1}"));
    ASSERT_FALSE(parse_verify_text("{\"nonce\":7}"));
    ASSERT_FALSE(parse_verify_text("{}"));
}

static void the_capability_names_the_running_version(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("deployment-trust/signed-capability.json");
    char version[BWG_ONBOARDING_VERSION_MAX];

    // Act
    const bool ok = bwg_onboarding_capability_version(yyjson_doc_get_root(doc), version);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_STREQ(version, "0.1.0");
}

void bwg_tests_onboarding(void)
{
    BWG_RUN(verification_is_the_gates_accepted_reply_byte_for_byte);
    BWG_RUN(inspection_reports_the_snapshot_facts);
    BWG_RUN(a_table_without_otadata_is_an_unknown_scheme);
    BWG_RUN(a_pending_image_is_not_yet_bootable);
    BWG_RUN(a_pending_image_cannot_confirm_preservation);
    BWG_RUN(verify_takes_exactly_a_canonical_32_byte_nonce);
    BWG_RUN(the_capability_names_the_running_version);
}
