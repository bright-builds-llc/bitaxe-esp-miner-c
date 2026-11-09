#include <string.h>

#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_onboarding.h"

#define HMAC_TEXT_SIZE 44u

const char *bwg_onboarding_scheme(const bwg_partition_facts_t *facts)
{
    const bool known = facts->has_factory && facts->has_ota_0 && facts->has_ota_1 && facts->has_otadata;
    return known ? BWG_ONBOARDING_SCHEME : "unknown";
}

uint32_t bwg_onboarding_app_slot_bytes(const bwg_partition_facts_t *facts)
{
    if (!facts->has_ota_0 || !facts->has_ota_1) {
        return 0;
    }
    return facts->ota_0_bytes < facts->ota_1_bytes ? facts->ota_0_bytes : facts->ota_1_bytes;
}

const char *bwg_onboarding_slot_name(bwg_app_slot_t slot)
{
    switch (slot) {
    case BWG_SLOT_FACTORY:
        return "factory";
    case BWG_SLOT_OTA_0:
        return "ota_0";
    case BWG_SLOT_OTA_1:
        return "ota_1";
    default:
        return "unknown";
    }
}

bool bwg_onboarding_bootable(bwg_image_state_t state)
{
    return state == BWG_IMAGE_VALID || state == BWG_IMAGE_UNDEFINED;
}

void bwg_onboarding_write_inspection(bwg_jw_t *writer, const bwg_partition_facts_t *facts, bool settings_readable)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "settingsSchemaVersion");
    bwg_jw_u64(writer, BWG_SETTINGS_SCHEMA);
    bwg_jw_key(writer, "settingsReadable");
    bwg_jw_bool(writer, settings_readable);
    bwg_jw_key(writer, "partition");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "scheme");
    bwg_jw_cstr(writer, bwg_onboarding_scheme(facts));
    bwg_jw_key(writer, "appSlotBytes");
    bwg_jw_u64(writer, bwg_onboarding_app_slot_bytes(facts));
    bwg_jw_key(writer, "rollbackAvailable");
    bwg_jw_bool(writer, facts->rollback_possible);
    bwg_jw_key(writer, "activeSlot");
    bwg_jw_cstr(writer, bwg_onboarding_slot_name(facts->running_slot));
    bwg_jw_key(writer, "bootable");
    bwg_jw_bool(writer, bwg_onboarding_bootable(facts->running_state));
    bwg_jw_object_end(writer);
    bwg_jw_object_end(writer);
}

static void write_b64url(bwg_jw_t *writer, const uint8_t *bytes, size_t len)
{
    char text[HMAC_TEXT_SIZE];
    size_t written = 0;
    // 32 bytes always fit; a failure fails the writer rather than writing a short value.
    if (!bwg_b64url_encode(bytes, len, text, sizeof(text), &written)) {
        writer->failed = true;
        return;
    }
    bwg_jw_string(writer, text, written);
}

void bwg_onboarding_write_verification(bwg_jw_t *writer, const bwg_onboarding_verification_t *verification)
{
    const bool bootable = bwg_onboarding_bootable(verification->facts.running_state);
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "preservationConfirmed");
    bwg_jw_bool(writer, bootable);
    bwg_jw_key(writer, "runningFirmwareVersion");
    bwg_jw_cstr(writer, verification->firmware_version);
    bwg_jw_key(writer, "settingsSchemaVersion");
    bwg_jw_u64(writer, BWG_SETTINGS_SCHEMA);
    bwg_jw_key(writer, "activeSlot");
    bwg_jw_cstr(writer, bwg_onboarding_slot_name(verification->facts.running_slot));
    bwg_jw_key(writer, "bootable");
    bwg_jw_bool(writer, bootable);
    bwg_jw_key(writer, "nonce");
    write_b64url(writer, verification->nonce, sizeof(verification->nonce));
    bwg_jw_key(writer, "categories");
    bwg_jw_array_begin(writer);
    for (int i = 0; i < BWG_SETTINGS_CATEGORY_COUNT; i++) {
        bwg_jw_object_begin(writer);
        bwg_jw_key(writer, "category");
        bwg_jw_cstr(writer, bwg_settings_category_name((bwg_settings_category_t) i));
        bwg_jw_key(writer, "hmacSha256");
        write_b64url(writer, verification->hmacs[i], BWG_SETTINGS_HMAC_BYTES);
        bwg_jw_object_end(writer);
    }
    bwg_jw_array_end(writer);
    bwg_jw_object_end(writer);
}

bool bwg_onboarding_parse_verify(yyjson_val *payload, uint8_t nonce[BWG_SETTINGS_NONCE_BYTES])
{
    static const char *const REQUIRED[] = {"nonce"};
    bwg_str_t text;
    size_t written = 0;
    return payload != NULL && bwg_json_has_exact_fields(payload, REQUIRED, 1, NULL, 0) &&
           bwg_json_get_str(bwg_json_maybe_field(payload, "nonce"), &text) &&
           bwg_b64url_is_canonical(text.ptr, text.len, BWG_SETTINGS_NONCE_BYTES) &&
           bwg_b64url_decode(text.ptr, text.len, nonce, BWG_SETTINGS_NONCE_BYTES, &written) &&
           written == BWG_SETTINGS_NONCE_BYTES;
}

bool bwg_onboarding_capability_version(yyjson_val *capability, char out[BWG_ONBOARDING_VERSION_MAX])
{
    bwg_str_t version;
    if (!bwg_json_get_str(bwg_json_maybe_field(bwg_json_maybe_field(capability, "firmware"), "version"), &version) ||
        version.len == 0 || version.len >= BWG_ONBOARDING_VERSION_MAX) {
        return false;
    }
    memcpy(out, version.ptr, version.len);
    out[version.len] = '\0';
    return true;
}
