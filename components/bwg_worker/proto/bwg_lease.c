#include "bwg_lease.h"

#include <string.h>

#include "bwg_codec.h"

#define V2_PROFILE "bwg-worker-stratum-v2-standard/0.1"

static bool is_identifier(bwg_str_t value)
{
    if (value.len == 0 || value.len > 128) {
        return false;
    }
    for (size_t i = 0; i < value.len; i++) {
        char c = value.ptr[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool is_secret(bwg_str_t value)
{
    return value.len > 0 && value.len <= BWG_LEASE_MAX_SECRET_BYTES;
}

static bool is_c_safe(bwg_str_t value)
{
    return memchr(value.ptr, '\0', value.len) == NULL;
}

static bool is_valid_window(uint64_t duration, uint64_t renew_after)
{
    return duration >= 1 && duration <= BWG_LEASE_MAX_DURATION_MS && renew_after >= 1 &&
           renew_after <= BWG_LEASE_MAX_RENEW_AFTER_MS && renew_after < duration;
}

static bool get_str(yyjson_val *object, const char *key, bwg_str_t *out)
{
    return bwg_json_get_str(bwg_json_maybe_field(object, key), out);
}

static bool parse_v1(yyjson_val *stratum, bwg_lease_grant_t *grant)
{
    if (bwg_json_maybe_field(stratum, "authorityPublicKey") != NULL || bwg_json_maybe_field(stratum, "userIdentity") != NULL) {
        return false;
    }
    if (!get_str(stratum, "username", &grant->username) || !get_str(stratum, "password", &grant->password)) {
        return false;
    }
    yyjson_val *maybe_difficulty = bwg_json_maybe_field(stratum, "suggestedDifficulty");
    if (maybe_difficulty == NULL) {
        return true;
    }
    uint64_t difficulty;
    if (!bwg_json_get_u64(maybe_difficulty, &difficulty) || difficulty > UINT16_MAX) {
        return false;
    }
    grant->has_suggested_difficulty = true;
    grant->suggested_difficulty = (uint16_t) difficulty;
    return true;
}

bool bwg_stratum_v2_parse(yyjson_val *stratum, bwg_str_t *endpoint, bwg_str_t *authority_public_key, bwg_str_t *user_identity)
{
    static const char *const required[] = {"endpoint", "profile", "authorityPublicKey", "userIdentity"};
    bwg_str_t profile;
    return bwg_json_has_exact_fields(stratum, required, 4, NULL, 0) && get_str(stratum, "endpoint", endpoint) &&
           get_str(stratum, "profile", &profile) && bwg_str_equals(profile, V2_PROFILE) &&
           get_str(stratum, "authorityPublicKey", authority_public_key) && get_str(stratum, "userIdentity", user_identity);
}

static bool parse_v2(yyjson_val *stratum, bwg_lease_grant_t *grant)
{
    if (!bwg_stratum_v2_parse(stratum, &grant->endpoint, &grant->authority_public_key, &grant->user_identity)) {
        return false;
    }
    grant->stratum_kind = BWG_STRATUM_V2;
    grant->maybe_stratum_v2 = stratum;
    return true;
}

static bool parse_stratum(yyjson_val *stratum, bwg_lease_grant_t *grant)
{
    static const char *const required[] = {"endpoint"};
    static const char *const optional[] = {"username", "password",     "suggestedDifficulty",
                                           "profile",  "userIdentity", "authorityPublicKey"};
    if (!bwg_json_has_exact_fields(stratum, required, 1, optional, 6) || !get_str(stratum, "endpoint", &grant->endpoint)) {
        return false;
    }
    if (bwg_json_maybe_field(stratum, "profile") != NULL) {
        return parse_v2(stratum, grant);
    }
    grant->stratum_kind = BWG_STRATUM_V1;
    return parse_v1(stratum, grant);
}

/** At most one allowance may be present, which bwg_lease_grant_validate enforces, so the last parsed is the one kept. */
static bool parse_extension(yyjson_val *payload, const char *key, yyjson_val **out,
                            bool (*parse)(yyjson_val *object, bwg_allowance_t *allowance), bwg_lease_grant_t *grant)
{
    yyjson_val *value = bwg_json_maybe_field(payload, key);
    *out = value;
    return value == NULL || parse(value, &grant->allowance);
}

bool bwg_lease_grant_parse(yyjson_val *payload, bwg_lease_grant_t *grant)
{
    static const char *const required[] = {
        "protocolVersion", "leaseId", "challengeId", "authorization", "durationMilliseconds", "renewAfterMilliseconds", "stratum",
    };
    static const char *const optional[] = {"acceptanceCampaign", "qualificationAttempt", "soakAllowance"};
    memset(grant, 0, sizeof(*grant));
    return bwg_json_has_exact_fields(payload, required, 7, optional, 3) &&
           get_str(payload, "protocolVersion", &grant->protocol_version) && get_str(payload, "leaseId", &grant->lease_id) &&
           get_str(payload, "challengeId", &grant->challenge_id) &&
           get_str(payload, "authorization", &grant->authorization) &&
           bwg_json_get_u64(bwg_json_maybe_field(payload, "durationMilliseconds"), &grant->duration_ms) &&
           bwg_json_get_u64(bwg_json_maybe_field(payload, "renewAfterMilliseconds"), &grant->renew_after_ms) &&
           parse_stratum(bwg_json_maybe_field(payload, "stratum"), grant) &&
           parse_extension(payload, "acceptanceCampaign", &grant->maybe_acceptance_campaign, bwg_allowance_parse_campaign, grant) &&
           parse_extension(payload, "qualificationAttempt", &grant->maybe_qualification_attempt, bwg_allowance_parse_attempt, grant) &&
           parse_extension(payload, "soakAllowance", &grant->maybe_soak_allowance, bwg_allowance_parse_soak, grant);
}

static bool allowance_is_valid(const bwg_lease_grant_t *grant)
{
    const int present = (grant->maybe_acceptance_campaign != NULL) + (grant->maybe_qualification_attempt != NULL) +
                        (grant->maybe_soak_allowance != NULL);
    if (present == 0) {
        return true;
    }
    if (present != 1 || !bwg_allowance_validate(&grant->allowance)) {
        return false;
    }
    return grant->allowance.kind != BWG_ALLOWANCE_SOAK ||
           (grant->duration_ms == BWG_SOAK_DURATION_MS && grant->renew_after_ms == BWG_SOAK_RENEW_AFTER_MS);
}

static bool v1_stratum_is_valid(const bwg_lease_grant_t *grant)
{
    bwg_str_t host;
    uint16_t port;
    return is_secret(grant->username) && is_secret(grant->password) && is_c_safe(grant->endpoint) &&
           is_c_safe(grant->username) && is_c_safe(grant->password) &&
           bwg_stratum_endpoint_parse(grant->endpoint, &host, &port);
}

bool bwg_stratum_v2_is_valid(bwg_str_t endpoint, bwg_str_t authority_public_key, bwg_str_t user_identity)
{
    bwg_str_t host;
    uint16_t port;
    uint8_t octets[4];
    return bwg_stratum_endpoint_parse(endpoint, &host, &port) && bwg_private_ipv4_parse(host, octets) &&
           bwg_b64url_is_canonical(authority_public_key.ptr, authority_public_key.len, BWG_STRATUM_V2_AUTHORITY_KEY_BYTES) &&
           user_identity.len >= 1 && user_identity.len <= BWG_STRATUM_V2_MAX_USER_BYTES && is_c_safe(user_identity);
}

bool bwg_lease_grant_validate(const bwg_lease_grant_t *grant)
{
    return bwg_str_equals(grant->protocol_version, BWG_CONTROLLER_PROTOCOL) && is_identifier(grant->lease_id) &&
           is_identifier(grant->challenge_id) && is_secret(grant->authorization) &&
           is_valid_window(grant->duration_ms, grant->renew_after_ms) && allowance_is_valid(grant) &&
           (grant->stratum_kind == BWG_STRATUM_V2
                ? bwg_stratum_v2_is_valid(grant->endpoint, grant->authority_public_key, grant->user_identity)
                : v1_stratum_is_valid(grant));
}

bool bwg_lease_renewal_parse(yyjson_val *payload, bwg_lease_renewal_t *renewal)
{
    static const char *const required[] = {
        "protocolVersion", "leaseId", "authorization", "durationMilliseconds", "renewAfterMilliseconds",
    };
    return bwg_json_has_exact_fields(payload, required, 5, NULL, 0) &&
           get_str(payload, "protocolVersion", &renewal->protocol_version) &&
           get_str(payload, "leaseId", &renewal->lease_id) && get_str(payload, "authorization", &renewal->authorization) &&
           bwg_json_get_u64(bwg_json_maybe_field(payload, "durationMilliseconds"), &renewal->duration_ms) &&
           bwg_json_get_u64(bwg_json_maybe_field(payload, "renewAfterMilliseconds"), &renewal->renew_after_ms);
}

bool bwg_lease_renewal_validate(const bwg_lease_renewal_t *renewal)
{
    return bwg_str_equals(renewal->protocol_version, BWG_CONTROLLER_PROTOCOL) && is_identifier(renewal->lease_id) &&
           is_secret(renewal->authorization) && is_valid_window(renewal->duration_ms, renewal->renew_after_ms);
}

static bool parse_port(const char *text, size_t len, uint16_t *port)
{
    if (len == 0 || len > 5 || text[0] == '0') {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (uint32_t) (text[i] - '0');
    }
    if (value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t) value;
    return true;
}

bool bwg_stratum_endpoint_parse(bwg_str_t endpoint, bwg_str_t *host, uint16_t *port)
{
    static const char scheme[] = "stratum+tcp://";
    size_t scheme_len = sizeof(scheme) - 1;
    if (endpoint.len <= scheme_len || memcmp(endpoint.ptr, scheme, scheme_len) != 0 ||
        endpoint.ptr[endpoint.len - 1] != '/') {
        return false;
    }
    const char *authority = endpoint.ptr + scheme_len;
    size_t authority_len = endpoint.len - scheme_len - 1;
    size_t colon = authority_len;
    for (size_t i = 0; i < authority_len; i++) {
        char c = authority[i];
        if (c == '@' || c == '?' || c == '#' || c == '/') {
            return false;
        }
        if (c == ':') {
            colon = i;
        }
    }
    if (colon == authority_len || colon == 0 || !parse_port(authority + colon + 1, authority_len - colon - 1, port)) {
        return false;
    }
    host->ptr = authority;
    host->len = colon;
    return true;
}

static bool parse_octet(const char *text, size_t len, uint8_t *octet)
{
    if (len == 0 || len > 3 || (len > 1 && text[0] == '0')) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (uint32_t) (text[i] - '0');
    }
    *octet = (uint8_t) value;
    return value <= UINT8_MAX;
}

bool bwg_private_ipv4_parse(bwg_str_t text, uint8_t octets[4])
{
    size_t start = 0;
    size_t filled = 0;
    for (size_t i = 0; i <= text.len; i++) {
        if (i < text.len && text.ptr[i] != '.') {
            continue;
        }
        if (filled == 4 || !parse_octet(text.ptr + start, i - start, &octets[filled])) {
            return false;
        }
        filled++;
        start = i + 1;
    }
    if (filled != 4) {
        return false;
    }
    return octets[0] == 10 || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168);
}

static void write_str_field(bwg_jw_t *writer, const char *key, bwg_str_t value)
{
    bwg_jw_key(writer, key);
    bwg_jw_string(writer, value.ptr, value.len);
}

static void write_extension(bwg_jw_t *writer, const char *key, yyjson_val *maybe_value)
{
    if (maybe_value == NULL) {
        return;
    }
    bwg_jw_key(writer, key);
    bwg_json_write_value(writer, maybe_value, BWG_JSON_ORDER_SORTED);
}

static void write_v1_stratum(bwg_jw_t *writer, const bwg_lease_grant_t *grant)
{
    bwg_jw_object_begin(writer);
    write_str_field(writer, "endpoint", grant->endpoint);
    write_str_field(writer, "password", grant->password);
    if (grant->has_suggested_difficulty) {
        bwg_jw_key(writer, "suggestedDifficulty");
        bwg_jw_u64(writer, grant->suggested_difficulty);
    }
    write_str_field(writer, "username", grant->username);
    bwg_jw_object_end(writer);
}

void bwg_lease_write_start_request(bwg_jw_t *writer, const bwg_lease_grant_t *grant)
{
    bwg_jw_object_begin(writer);
    write_extension(writer, "acceptanceCampaign", grant->maybe_acceptance_campaign);
    write_str_field(writer, "challengeId", grant->challenge_id);
    bwg_jw_key(writer, "durationMilliseconds");
    bwg_jw_u64(writer, grant->duration_ms);
    write_str_field(writer, "leaseId", grant->lease_id);
    write_str_field(writer, "protocolVersion", grant->protocol_version);
    write_extension(writer, "qualificationAttempt", grant->maybe_qualification_attempt);
    bwg_jw_key(writer, "renewAfterMilliseconds");
    bwg_jw_u64(writer, grant->renew_after_ms);
    write_extension(writer, "soakAllowance", grant->maybe_soak_allowance);
    bwg_jw_key(writer, "stratum");
    if (grant->stratum_kind == BWG_STRATUM_V2) {
        bwg_json_write_value(writer, grant->maybe_stratum_v2, BWG_JSON_ORDER_SORTED);
    } else {
        write_v1_stratum(writer, grant);
    }
    bwg_jw_object_end(writer);
}

void bwg_lease_write_renew_request(bwg_jw_t *writer, const bwg_lease_renewal_t *renewal)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "durationMilliseconds");
    bwg_jw_u64(writer, renewal->duration_ms);
    write_str_field(writer, "leaseId", renewal->lease_id);
    write_str_field(writer, "protocolVersion", renewal->protocol_version);
    bwg_jw_key(writer, "renewAfterMilliseconds");
    bwg_jw_u64(writer, renewal->renew_after_ms);
    bwg_jw_object_end(writer);
}
