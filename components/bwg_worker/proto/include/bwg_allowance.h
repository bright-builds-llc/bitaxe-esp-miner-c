#ifndef BWG_ALLOWANCE_H
#define BWG_ALLOWANCE_H

#include <stdbool.h>
#include <stdint.h>
#include <yyjson.h>

#include "bwg_json.h"

/** A canonical 16-byte identifier as base64url text, with its NUL. */
#define BWG_ALLOWANCE_ID_SIZE 23u
#define BWG_ATTEMPT_SHORT_MS 30000u
#define BWG_ATTEMPT_NORMAL_MS 180000u
#define BWG_CAMPAIGN_FIRST_WINDOW_MS 180000u
#define BWG_CAMPAIGN_LATER_WINDOW_MS 30000u
#define BWG_CAMPAIGN_WINDOWS 3u
#define BWG_CAMPAIGN_TOTAL_MS 240000u
#define BWG_SOAK_MS 600000u
#define BWG_SOAK_DURATION_MS 60000u
#define BWG_SOAK_RENEW_AFTER_MS 20000u

typedef enum {
    BWG_ALLOWANCE_NONE = 0,
    BWG_ALLOWANCE_ATTEMPT,
    BWG_ALLOWANCE_CAMPAIGN,
    BWG_ALLOWANCE_SOAK,
} bwg_allowance_kind_t;

typedef enum {
    BWG_ATTEMPT_DIAGNOSTIC = 0,
    BWG_ATTEMPT_NORMAL,
    BWG_ATTEMPT_FOREGROUND_LOSS,
    BWG_ATTEMPT_HEARTBEAT_LOSS,
} bwg_attempt_purpose_t;

/** A signed grant's budget, borrowing its text from the parsed document. It holds no secret. */
typedef struct {
    bwg_allowance_kind_t kind;
    bwg_str_t schema;
    bwg_str_t id;
    uint32_t ordinal;
    uint8_t window;
    bwg_attempt_purpose_t purpose;
    uint64_t maximum_active_ms;
} bwg_allowance_t;

/** Exact fields and JSON types only, so a shape error is refused before admission is considered. */
bool bwg_allowance_parse_attempt(yyjson_val *object, bwg_allowance_t *out);
bool bwg_allowance_parse_campaign(yyjson_val *object, bwg_allowance_t *out);
bool bwg_allowance_parse_soak(yyjson_val *object, bwg_allowance_t *out);

/** The value rules: schema, canonical id, ordinal and the exact limit for its kind. */
bool bwg_allowance_validate(const bwg_allowance_t *allowance);

/** The active limit a whole window of this size charges. */
uint64_t bwg_attempt_limit_ms(bwg_attempt_purpose_t purpose);
uint64_t bwg_campaign_window_ms(uint8_t window);

const char *bwg_attempt_purpose_label(bwg_attempt_purpose_t purpose);

#endif // BWG_ALLOWANCE_H
