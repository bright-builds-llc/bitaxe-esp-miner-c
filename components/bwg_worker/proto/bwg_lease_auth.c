#include "bwg_lease_auth.h"

#include <sodium.h>

#include <string.h>

#define AUTHORIZATION_PROFILE "bwg-worker-lease-authorization/0.2"
#define AUTHORIZATION_TYPE "bwg-worker-lease-authorization+jws"
#define CONTROLLER_AUDIENCE "bwg-worker-controller/0.4"
#define MAX_COMPACT_BYTES 512u
#define MAX_SEGMENT_CHARS 512u
#define MAX_SIGNATURE_CHARS 86u
// The longest segment decodes to 384 bytes; the two decoded segments and their canonical copies share one block.
#define SEGMENT_BYTES 384u
#define SCRATCH_BYTES (SEGMENT_BYTES * 3u)

typedef struct {
    const char *ptr;
    size_t len;
} segment_t;

typedef struct {
    char kid[BWG_TRUST_KID_MAX_LEN + 1];
    uint64_t sequence;
    bool context_matches;
} verified_t;

static void digest_prefix(bwg_jw_t *writer, const bwg_lease_trust_t *trust, const char *active_challenge_id,
                          size_t challenge_len, const char *operation)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "activeChallengeId");
    bwg_jw_string(writer, active_challenge_id, challenge_len);
    bwg_jw_key(writer, "audience");
    bwg_jw_cstr(writer, CONTROLLER_AUDIENCE);
    bwg_jw_key(writer, "issuer");
    bwg_jw_cstr(writer, trust->issuer);
    bwg_jw_key(writer, "operation");
    bwg_jw_cstr(writer, operation);
    bwg_jw_key(writer, "profile");
    bwg_jw_cstr(writer, AUTHORIZATION_PROFILE);
    bwg_jw_key(writer, "request");
}

static bool digest_finish(bwg_jw_t *writer, crypto_hash_sha256_state *state, char digest[BWG_DIGEST_TEXT_SIZE])
{
    bwg_jw_object_end(writer);
    if (!bwg_jw_ok(writer)) {
        return false;
    }
    uint8_t bytes[BWG_SHA256_BYTES];
    crypto_hash_sha256_final(state, bytes);
    return bwg_b64url_encode(bytes, sizeof(bytes), digest, BWG_DIGEST_TEXT_SIZE, NULL);
}

bool bwg_lease_auth_start_digest(const bwg_lease_trust_t *trust, const bwg_lease_grant_t *grant,
                                 char digest[BWG_DIGEST_TEXT_SIZE])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    bwg_jw_t writer;
    bwg_jw_init_hash(&writer, &state);
    digest_prefix(&writer, trust, grant->challenge_id.ptr, grant->challenge_id.len, "start");
    bwg_lease_write_start_request(&writer, grant);
    return digest_finish(&writer, &state, digest);
}

bool bwg_lease_auth_renew_digest(const bwg_lease_trust_t *trust, const bwg_lease_renewal_t *renewal,
                                 const char *active_challenge_id, char digest[BWG_DIGEST_TEXT_SIZE])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    bwg_jw_t writer;
    bwg_jw_init_hash(&writer, &state);
    digest_prefix(&writer, trust, active_challenge_id, strlen(active_challenge_id), "renew");
    bwg_lease_write_renew_request(&writer, renewal);
    return digest_finish(&writer, &state, digest);
}

/** Splits into exactly three segments. Empty and oversized segments fail in the decoder step. */
static bool split_compact(bwg_str_t compact, segment_t segments[3])
{
    size_t index = 0;
    size_t start = 0;
    for (size_t i = 0; i < compact.len; i++) {
        if (compact.ptr[i] != '.') {
            continue;
        }
        if (index == 2) {
            return false;
        }
        segments[index] = (segment_t) {.ptr = compact.ptr + start, .len = i - start};
        index++;
        start = i + 1;
    }
    if (index != 2) {
        return false;
    }
    segments[2] = (segment_t) {.ptr = compact.ptr + start, .len = compact.len - start};
    return true;
}

static bool decode_segment(segment_t segment, size_t maximum_chars, uint8_t *out, size_t capacity, size_t *written)
{
    return segment.len > 0 && segment.len <= maximum_chars &&
           bwg_b64url_decode(segment.ptr, segment.len, out, capacity, written);
}

static bool is_kid(bwg_str_t value)
{
    if (value.len == 0 || value.len > BWG_TRUST_KID_MAX_LEN) {
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

static bool parse_sequence(bwg_str_t text, uint64_t *sequence)
{
    if (text.len == 0 || text.len > 20 || text.ptr[0] == '0') {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (text.ptr[i] < '0' || text.ptr[i] > '9') {
            return false;
        }
        uint64_t digit = (uint64_t) (text.ptr[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *sequence = value;
    return true;
}

/** Whether the bytes are exactly the canonical serialization of the parsed value. */
static bool is_canonical(yyjson_val *value, const uint8_t *bytes, size_t len, uint8_t *scratch)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, scratch, SEGMENT_BYTES);
    return bwg_json_write_value(&writer, value, BWG_JSON_ORDER_SORTED) && writer.len == len &&
           memcmp(scratch, bytes, len) == 0;
}

typedef struct {
    const bwg_allocator_t *allocator;
    const bwg_lease_trust_t *trust;
    const char *operation;
    const char *binding;
    const char *expected_digest;
    bwg_auth_attribution_t *attribution;
} verify_input_t;

static bool check_header(const verify_input_t *input, const uint8_t *bytes, size_t len, uint8_t *scratch,
                         verified_t *verified)
{
    static const char *const fields[] = {"alg", "kid", "typ"};
    yyjson_doc *doc;
    if (bwg_json_parse_strict(input->allocator, bytes, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bwg_str_t alg;
    bwg_str_t kid;
    bwg_str_t typ;
    bool ok = bwg_json_has_exact_fields(root, fields, 3, NULL, 0) && bwg_json_get_str(bwg_json_field(root, "alg"), &alg) &&
              bwg_json_get_str(bwg_json_field(root, "kid"), &kid) && bwg_json_get_str(bwg_json_field(root, "typ"), &typ) &&
              bwg_str_equals(alg, "Ed25519") && bwg_str_equals(typ, AUTHORIZATION_TYPE) && is_kid(kid) &&
              is_canonical(root, bytes, len, scratch);
    if (ok) {
        memcpy(verified->kid, kid.ptr, kid.len);
        verified->kid[kid.len] = '\0';
    }
    yyjson_doc_free(doc);
    return ok;
}

static bool check_claims(const verify_input_t *input, const uint8_t *bytes, size_t len, uint8_t *scratch,
                         verified_t *verified)
{
    static const char *const fields[] = {"controlSessionBindingSha256", "operation", "requestSha256", "sequence"};
    yyjson_doc *doc;
    if (bwg_json_parse_strict(input->allocator, bytes, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bwg_str_t binding;
    bwg_str_t operation;
    bwg_str_t digest;
    bwg_str_t sequence;
    bool ok = bwg_json_has_exact_fields(root, fields, 4, NULL, 0) &&
              bwg_json_get_str(bwg_json_field(root, "controlSessionBindingSha256"), &binding) &&
              bwg_json_get_str(bwg_json_field(root, "operation"), &operation) &&
              bwg_json_get_str(bwg_json_field(root, "requestSha256"), &digest) &&
              bwg_json_get_str(bwg_json_field(root, "sequence"), &sequence) && bwg_str_equals(operation, input->operation) &&
              bwg_digest_text_is_valid(digest.ptr, digest.len) && is_canonical(root, bytes, len, scratch) &&
              parse_sequence(sequence, &verified->sequence) && bwg_str_equals(digest, input->expected_digest);
    if (ok) {
        // Attributed here, decided only after the signature and the replay comparison are known.
        verified->context_matches = bwg_str_equals(binding, input->binding);
    }
    yyjson_doc_free(doc);
    return ok;
}

static bool verify_jws(const verify_input_t *input, bwg_str_t compact, verified_t *verified)
{
    if (compact.len == 0 || compact.len > MAX_COMPACT_BYTES) {
        return false;
    }
    segment_t segments[3];
    if (!split_compact(compact, segments)) {
        return false;
    }
    bwg_buffer_t scratch;
    if (!bwg_buffer_alloc(input->allocator, &scratch, SCRATCH_BYTES)) {
        return false;
    }
    uint8_t *header_bytes = scratch.data;
    uint8_t *claims_bytes = scratch.data + SEGMENT_BYTES;
    uint8_t *canonical = scratch.data + SEGMENT_BYTES * 2;
    uint8_t signature[crypto_sign_BYTES];
    size_t header_len = 0;
    size_t claims_len = 0;
    size_t signature_len = 0;
    bool ok = decode_segment(segments[0], MAX_SEGMENT_CHARS, header_bytes, SEGMENT_BYTES, &header_len) &&
              decode_segment(segments[1], MAX_SEGMENT_CHARS, claims_bytes, SEGMENT_BYTES, &claims_len) &&
              decode_segment(segments[2], MAX_SIGNATURE_CHARS, signature, sizeof(signature), &signature_len) &&
              signature_len == sizeof(signature) && check_header(input, header_bytes, header_len, canonical, verified) &&
              check_claims(input, claims_bytes, claims_len, canonical, verified);
    bwg_buffer_release(input->allocator, &scratch);
    if (!ok) {
        return false;
    }
    input->attribution->context = verified->context_matches ? BWG_AUTH_CONTEXT_CURRENT : BWG_AUTH_CONTEXT_MISMATCH;
    const bwg_trust_key_t *maybe_key = bwg_trust_find_key(input->trust, verified->kid, strlen(verified->kid));
    if (maybe_key == NULL) {
        return false;
    }
    size_t signing_len = (size_t) (segments[1].ptr + segments[1].len - compact.ptr);
    if (crypto_sign_verify_detached(signature, (const uint8_t *) compact.ptr, signing_len, maybe_key->public_key) != 0) {
        input->attribution->signature = BWG_AUTH_SIGNATURE_INVALID;
        return false;
    }
    input->attribution->signature = BWG_AUTH_SIGNATURE_VALID;
    return true;
}

static bwg_auth_result_t apply_replay_guard(const bwg_sequence_store_t *store, const verified_t *verified,
                                            bwg_auth_attribution_t *attribution)
{
    bool present = false;
    uint64_t current = 0;
    if (!store->load(store->ctx, verified->kid, &present, &current)) {
        attribution->replay_guard = BWG_AUTH_REPLAY_UNAVAILABLE;
        return verified->context_matches ? BWG_AUTH_PERSISTENCE : BWG_AUTH_INVALID;
    }
    const bool replay = present && verified->sequence <= current;
    attribution->replay_guard = replay ? BWG_AUTH_REPLAY_AT_OR_BELOW : BWG_AUTH_REPLAY_FRESH;
    if (!verified->context_matches) {
        return BWG_AUTH_INVALID;
    }
    if (replay) {
        return BWG_AUTH_REPLAY;
    }
    switch (store->compare_and_store(store->ctx, verified->kid, present, current, verified->sequence)) {
    case BWG_STORE_COMMITTED:
        return BWG_AUTH_ACCEPTED;
    case BWG_STORE_STALE:
    case BWG_STORE_ALREADY_COMMITTED:
        attribution->replay_guard = BWG_AUTH_REPLAY_AT_OR_BELOW;
        return BWG_AUTH_REPLAY;
    case BWG_STORE_ERROR:
        break;
    }
    attribution->replay_guard = BWG_AUTH_REPLAY_UNAVAILABLE;
    return BWG_AUTH_PERSISTENCE;
}

static bwg_auth_result_t verify_common(verify_input_t *input, const bwg_sequence_store_t *store, bwg_str_t compact,
                                       bwg_auth_attribution_t *maybe_attribution)
{
    bwg_auth_attribution_t unused;
    input->attribution = maybe_attribution != NULL ? maybe_attribution : &unused;
    *input->attribution = (bwg_auth_attribution_t) {0};
    verified_t verified = {.context_matches = false};
    if (!verify_jws(input, compact, &verified)) {
        return BWG_AUTH_INVALID;
    }
    return apply_replay_guard(store, &verified, input->attribution);
}

bwg_auth_result_t bwg_lease_auth_verify_start(const bwg_allocator_t *allocator, const bwg_lease_trust_t *trust,
                                              const bwg_sequence_store_t *store, const bwg_lease_grant_t *grant,
                                              const char *binding, bwg_auth_attribution_t *maybe_attribution)
{
    char digest[BWG_DIGEST_TEXT_SIZE];
    if (!bwg_lease_auth_start_digest(trust, grant, digest)) {
        return BWG_AUTH_INVALID;
    }
    verify_input_t input = {
        .allocator = allocator,
        .trust = trust,
        .operation = "start",
        .binding = binding,
        .expected_digest = digest,
    };
    return verify_common(&input, store, grant->authorization, maybe_attribution);
}

bwg_auth_result_t bwg_lease_auth_verify_renew(const bwg_allocator_t *allocator, const bwg_lease_trust_t *trust,
                                              const bwg_sequence_store_t *store, const bwg_lease_renewal_t *renewal,
                                              const char *active_challenge_id, const char *binding,
                                              bwg_auth_attribution_t *maybe_attribution)
{
    char digest[BWG_DIGEST_TEXT_SIZE];
    if (!bwg_lease_auth_renew_digest(trust, renewal, active_challenge_id, digest)) {
        return BWG_AUTH_INVALID;
    }
    verify_input_t input = {
        .allocator = allocator,
        .trust = trust,
        .operation = "renew",
        .binding = binding,
        .expected_digest = digest,
    };
    return verify_common(&input, store, renewal->authorization, maybe_attribution);
}
