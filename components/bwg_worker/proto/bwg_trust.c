#include "bwg_trust.h"

#include <sodium.h>

#include <string.h>

#include "bwg_codec.h"
#include "bwg_json.h"

#define TRUST_PROFILE "bwg-worker-deployment-trust/0.2"
#define UPDATE_ROLE "update_authority"
#define UPDATE_AUDIENCE "bwg-reference-firmware-capability/0.2"
#define LEASE_ROLE "work_lease_authority"
#define LEASE_AUDIENCE "bwg-worker-controller/0.4"

static bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_kid(bwg_str_t value)
{
    if (value.len == 0 || value.len > BWG_TRUST_KID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < value.len; i++) {
        if (!is_alnum(value.ptr[i]) && value.ptr[i] != '_' && value.ptr[i] != '-') {
            return false;
        }
    }
    return true;
}

static bool is_issuer(bwg_str_t value)
{
    if (value.len == 0 || value.len > BWG_TRUST_ISSUER_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < value.len; i++) {
        char c = value.ptr[i];
        if (!is_alnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static bool field_equals(yyjson_val *object, const char *key, const char *expected)
{
    bwg_str_t value;
    return bwg_json_get_str(bwg_json_field(object, key), &value) && bwg_str_equals(value, expected);
}

static bool parse_key(yyjson_val *wire, bwg_trust_key_t *key)
{
    static const char *const fields[] = {"kid", "kty", "crv", "x", "alg", "use", "key_ops"};
    if (!bwg_json_has_exact_fields(wire, fields, 7, NULL, 0) || !field_equals(wire, "kty", "OKP") ||
        !field_equals(wire, "crv", "Ed25519") || !field_equals(wire, "alg", "Ed25519") ||
        !field_equals(wire, "use", "sig")) {
        return false;
    }
    yyjson_val *ops = bwg_json_field(wire, "key_ops");
    bwg_str_t op;
    if (!yyjson_is_arr(ops) || yyjson_arr_size(ops) != 1 || !bwg_json_get_str(yyjson_arr_get_first(ops), &op) ||
        !bwg_str_equals(op, "verify")) {
        return false;
    }
    bwg_str_t kid;
    bwg_str_t x;
    size_t written = 0;
    if (!bwg_json_get_str(bwg_json_field(wire, "kid"), &kid) || !is_kid(kid) ||
        !bwg_json_get_str(bwg_json_field(wire, "x"), &x) ||
        !bwg_b64url_decode(x.ptr, x.len, key->public_key, sizeof(key->public_key), &written) ||
        written != sizeof(key->public_key) || crypto_core_ed25519_is_valid_point(key->public_key) != 1) {
        return false;
    }
    memcpy(key->kid, kid.ptr, kid.len);
    key->kid[kid.len] = '\0';
    return true;
}

static bool keys_conflict(const bwg_trust_key_t *a, const bwg_trust_key_t *b)
{
    return strcmp(a->kid, b->kid) == 0 || memcmp(a->public_key, b->public_key, sizeof(a->public_key)) == 0;
}

static bool parse_keys(yyjson_val *array, bwg_trust_key_t *keys, size_t *count)
{
    size_t size = yyjson_arr_size(array);
    if (!yyjson_is_arr(array) || size == 0 || size > BWG_TRUST_MAX_KEYS) {
        return false;
    }
    size_t index = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(array);
    yyjson_val *wire;
    while ((wire = yyjson_arr_iter_next(&iter)) != NULL) {
        if (!parse_key(wire, &keys[index])) {
            return false;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (keys_conflict(&keys[earlier], &keys[index])) {
                return false;
            }
        }
        index++;
    }
    *count = size;
    return true;
}

static bool parse_lease_half(yyjson_val *lease, bwg_lease_trust_t *trust)
{
    static const char *const fields[] = {"profile", "issuer", "audience", "role", "keys"};
    bwg_str_t issuer;
    if (!bwg_json_has_exact_fields(lease, fields, 5, NULL, 0) || !field_equals(lease, "profile", TRUST_PROFILE) ||
        !field_equals(lease, "audience", LEASE_AUDIENCE) || !field_equals(lease, "role", LEASE_ROLE) ||
        !bwg_json_get_str(bwg_json_field(lease, "issuer"), &issuer) || !is_issuer(issuer) ||
        !parse_keys(bwg_json_field(lease, "keys"), trust->keys, &trust->key_count)) {
        return false;
    }
    memcpy(trust->issuer, issuer.ptr, issuer.len);
    trust->issuer[issuer.len] = '\0';
    return true;
}

static bool parse_update_half(yyjson_val *update, const bwg_lease_trust_t *trust)
{
    static const char *const fields[] = {"issuer", "audience", "role", "keys"};
    bwg_str_t issuer;
    bwg_trust_key_t keys[BWG_TRUST_MAX_KEYS];
    size_t count = 0;
    if (!bwg_json_has_exact_fields(update, fields, 4, NULL, 0) || !field_equals(update, "audience", UPDATE_AUDIENCE) ||
        !field_equals(update, "role", UPDATE_ROLE) || !bwg_json_get_str(bwg_json_field(update, "issuer"), &issuer) ||
        !is_issuer(issuer) || !parse_keys(bwg_json_field(update, "keys"), keys, &count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < trust->key_count; j++) {
            if (keys_conflict(&keys[i], &trust->keys[j])) {
                return false;
            }
        }
    }
    return true;
}

static bool parse_document(yyjson_val *root, bwg_lease_trust_t *trust)
{
    static const char *const fields[] = {"profile", "updateAuthority", "workLeaseAuthority"};
    return bwg_json_has_exact_fields(root, fields, 3, NULL, 0) && field_equals(root, "profile", TRUST_PROFILE) &&
           parse_lease_half(bwg_json_field(root, "workLeaseAuthority"), trust) &&
           parse_update_half(bwg_json_field(root, "updateAuthority"), trust);
}

bool bwg_trust_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, bwg_lease_trust_t *trust)
{
    yyjson_doc *doc;
    if (bwg_json_parse_strict(allocator, text, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    bool ok = parse_document(yyjson_doc_get_root(doc), trust);
    yyjson_doc_free(doc);
    return ok;
}

bool bwg_trust_parse_lease_half(const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                                bwg_lease_trust_t *trust)
{
    yyjson_doc *doc;
    if (bwg_json_parse_strict(allocator, text, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    bool ok = parse_lease_half(yyjson_doc_get_root(doc), trust);
    yyjson_doc_free(doc);
    return ok;
}

const bwg_trust_key_t *bwg_trust_find_key(const bwg_lease_trust_t *trust, const char *kid, size_t kid_len)
{
    for (size_t i = 0; i < trust->key_count; i++) {
        if (strlen(trust->keys[i].kid) == kid_len && memcmp(trust->keys[i].kid, kid, kid_len) == 0) {
            return &trust->keys[i];
        }
    }
    return NULL;
}
