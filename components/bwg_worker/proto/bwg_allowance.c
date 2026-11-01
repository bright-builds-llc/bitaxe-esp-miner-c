#include "bwg_allowance.h"

#include "bwg_codec.h"

#define ATTEMPT_SCHEMA "worker-qualification-attempt-v1"
#define SOAK_SCHEMA "worker-soak-allowance-v2"
#define ID_BYTES 16u

static bool parse_purpose(bwg_str_t text, bwg_attempt_purpose_t *out)
{
    static const bwg_attempt_purpose_t PURPOSES[] = {
        BWG_ATTEMPT_DIAGNOSTIC,
        BWG_ATTEMPT_NORMAL,
        BWG_ATTEMPT_FOREGROUND_LOSS,
        BWG_ATTEMPT_HEARTBEAT_LOSS,
    };
    for (size_t i = 0; i < sizeof(PURPOSES) / sizeof(PURPOSES[0]); i++) {
        if (bwg_str_equals(text, bwg_attempt_purpose_label(PURPOSES[i]))) {
            *out = PURPOSES[i];
            return true;
        }
    }
    return false;
}

static bool get_u32(yyjson_val *object, const char *key, uint32_t *out)
{
    uint64_t value;
    if (!bwg_json_get_u64(bwg_json_field(object, key), &value) || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t) value;
    return true;
}

bool bwg_allowance_parse_attempt(yyjson_val *object, bwg_allowance_t *out)
{
    static const char *const REQUIRED[] = {"schema", "id", "ordinal", "purpose", "maximumActiveMilliseconds"};
    bwg_str_t purpose;
    out->kind = BWG_ALLOWANCE_ATTEMPT;
    return bwg_json_has_exact_fields(object, REQUIRED, 5, NULL, 0) &&
           bwg_json_get_str(bwg_json_field(object, "schema"), &out->schema) &&
           bwg_json_get_str(bwg_json_field(object, "id"), &out->id) && get_u32(object, "ordinal", &out->ordinal) &&
           bwg_json_get_str(bwg_json_field(object, "purpose"), &purpose) && parse_purpose(purpose, &out->purpose) &&
           bwg_json_get_u64(bwg_json_field(object, "maximumActiveMilliseconds"), &out->maximum_active_ms);
}

bool bwg_allowance_parse_campaign(yyjson_val *object, bwg_allowance_t *out)
{
    static const char *const REQUIRED[] = {"id", "window", "maximumActiveMilliseconds"};
    uint64_t window;
    out->kind = BWG_ALLOWANCE_CAMPAIGN;
    if (!bwg_json_has_exact_fields(object, REQUIRED, 3, NULL, 0) ||
        !bwg_json_get_str(bwg_json_field(object, "id"), &out->id) ||
        !bwg_json_get_u64(bwg_json_field(object, "window"), &window) || window > UINT8_MAX) {
        return false;
    }
    out->window = (uint8_t) window;
    return bwg_json_get_u64(bwg_json_field(object, "maximumActiveMilliseconds"), &out->maximum_active_ms);
}

bool bwg_allowance_parse_soak(yyjson_val *object, bwg_allowance_t *out)
{
    static const char *const REQUIRED[] = {"schema", "id", "ordinal", "maximumActiveMilliseconds"};
    out->kind = BWG_ALLOWANCE_SOAK;
    return bwg_json_has_exact_fields(object, REQUIRED, 4, NULL, 0) &&
           bwg_json_get_str(bwg_json_field(object, "schema"), &out->schema) &&
           bwg_json_get_str(bwg_json_field(object, "id"), &out->id) && get_u32(object, "ordinal", &out->ordinal) &&
           bwg_json_get_u64(bwg_json_field(object, "maximumActiveMilliseconds"), &out->maximum_active_ms);
}

bool bwg_allowance_validate(const bwg_allowance_t *allowance)
{
    if (!bwg_b64url_is_canonical(allowance->id.ptr, allowance->id.len, ID_BYTES)) {
        return false;
    }
    switch (allowance->kind) {
    case BWG_ALLOWANCE_ATTEMPT:
        return bwg_str_equals(allowance->schema, ATTEMPT_SCHEMA) && allowance->ordinal > 0 &&
               allowance->maximum_active_ms == bwg_attempt_limit_ms(allowance->purpose);
    case BWG_ALLOWANCE_CAMPAIGN:
        return allowance->window < BWG_CAMPAIGN_WINDOWS &&
               allowance->maximum_active_ms == bwg_campaign_window_ms(allowance->window);
    case BWG_ALLOWANCE_SOAK:
        return bwg_str_equals(allowance->schema, SOAK_SCHEMA) && allowance->ordinal > 0 &&
               allowance->maximum_active_ms == BWG_SOAK_MS;
    case BWG_ALLOWANCE_NONE:
        break;
    }
    return false;
}

uint64_t bwg_attempt_limit_ms(bwg_attempt_purpose_t purpose)
{
    return purpose == BWG_ATTEMPT_NORMAL ? BWG_ATTEMPT_NORMAL_MS : BWG_ATTEMPT_SHORT_MS;
}

uint64_t bwg_campaign_window_ms(uint8_t window)
{
    return window == 0 ? BWG_CAMPAIGN_FIRST_WINDOW_MS : BWG_CAMPAIGN_LATER_WINDOW_MS;
}

const char *bwg_attempt_purpose_label(bwg_attempt_purpose_t purpose)
{
    switch (purpose) {
    case BWG_ATTEMPT_DIAGNOSTIC:
        return "diagnostic";
    case BWG_ATTEMPT_NORMAL:
        return "normal";
    case BWG_ATTEMPT_FOREGROUND_LOSS:
        return "foreground_loss";
    case BWG_ATTEMPT_HEARTBEAT_LOSS:
        return "heartbeat_loss";
    }
    return "diagnostic";
}
