#include "bwg_possession.h"

#include <string.h>

#define POSSESSION_PROFILE "bwg-worker-possession/0.2"
#define PROOF_PROFILE "bwg-worker-possession-proof/0.2"
#define SESSION_PROFILE "bwg-worker-control-session/0.2"
#define PROTECTED_HEADER "{\"alg\":\"Ed25519\",\"typ\":\"bwg-worker-possession+jws\"}"
#define CLAIMS_MAX_BYTES 1400u
// Header, two dots, the encoded claims and the 86-character signature.
#define JWS_MAX_BYTES 2040u

void bwg_identity_from_seed(bwg_identity_t *identity, const uint8_t seed[BWG_SEED_BYTES])
{
    crypto_sign_seed_keypair(identity->public_key, identity->secret_key, seed);
    bwg_b64url_encode(identity->public_key, sizeof(identity->public_key), identity->x, sizeof(identity->x), NULL);
    uint8_t digest[BWG_SHA256_BYTES];
    crypto_hash_sha256(digest, identity->public_key, sizeof(identity->public_key));
    bwg_hex_encode(digest, sizeof(digest), identity->fingerprint_hex);
}

void bwg_identity_wipe(bwg_identity_t *identity)
{
    sodium_memzero(identity, sizeof(*identity));
}

static bool has_identifier_shape(bwg_str_t value, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    if (value.len < prefix_len || value.len > 128 || memcmp(value.ptr, prefix, prefix_len) != 0) {
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

static bool digest_field(yyjson_val *payload, const char *key, bwg_str_t *out)
{
    return bwg_json_get_str(bwg_json_maybe_field(payload, key), out) && bwg_digest_text_is_valid(out->ptr, out->len);
}

static bool nonce_field(yyjson_val *payload, const char *key, size_t bytes, bwg_str_t *out)
{
    return bwg_json_get_str(bwg_json_maybe_field(payload, key), out) && bwg_b64url_is_canonical(out->ptr, out->len, bytes);
}

bool bwg_possession_parse_request(yyjson_val *root, bwg_possession_request_t *request)
{
    static const char *const top[] = {"profile", "requestId", "command", "payload"};
    static const char *const fields[] = {
        "purpose",        "possessionNonce", "challengeBindingSha256", "controllerCapabilitySha256",
        "serialManifestSha256", "sessionId", "hostNonce",              "deviceNonce",
    };
    if (!bwg_json_has_exact_fields(root, top, 4, NULL, 0)) {
        return false;
    }
    yyjson_val *payload = bwg_json_maybe_field(root, "payload");
    bwg_str_t profile;
    bwg_str_t command;
    return bwg_json_has_exact_fields(payload, fields, 8, NULL, 0) &&
           bwg_json_get_str(bwg_json_maybe_field(root, "profile"), &profile) && bwg_str_equals(profile, POSSESSION_PROFILE) &&
           bwg_json_get_str(bwg_json_maybe_field(root, "command"), &command) && bwg_str_equals(command, "prove_possession") &&
           bwg_json_get_str(bwg_json_maybe_field(root, "requestId"), &request->request_id) &&
           has_identifier_shape(request->request_id, "pos_") &&
           bwg_json_get_str(bwg_json_maybe_field(payload, "purpose"), &request->purpose) &&
           (bwg_str_equals(request->purpose, "initial_admission") ||
            bwg_str_equals(request->purpose, "transport_reacquisition")) &&
           digest_field(payload, "possessionNonce", &request->possession_nonce) &&
           digest_field(payload, "challengeBindingSha256", &request->challenge_binding) &&
           digest_field(payload, "controllerCapabilitySha256", &request->capability_digest) &&
           digest_field(payload, "serialManifestSha256", &request->manifest_digest) &&
           nonce_field(payload, "sessionId", BWG_SESSION_ID_BYTES, &request->session_id) &&
           nonce_field(payload, "hostNonce", BWG_NONCE_BYTES, &request->host_nonce) &&
           nonce_field(payload, "deviceNonce", BWG_NONCE_BYTES, &request->device_nonce);
}

bool bwg_possession_matches(const bwg_possession_request_t *request, const char *capability_digest,
                            const char *manifest_digest, const bwg_session_binding_t *binding)
{
    return bwg_str_equals(request->capability_digest, capability_digest) &&
           bwg_str_equals(request->manifest_digest, manifest_digest) &&
           bwg_str_equals(request->session_id, binding->session_id) &&
           bwg_str_equals(request->host_nonce, binding->host_nonce) &&
           bwg_str_equals(request->device_nonce, binding->device_nonce);
}

void bwg_possession_nonce_digest(const bwg_possession_request_t *request, uint8_t out[BWG_POSSESSION_NONCE_DIGEST_BYTES])
{
    crypto_hash_sha256(out, (const uint8_t *) request->possession_nonce.ptr, request->possession_nonce.len);
}

static void write_str_field(bwg_jw_t *writer, const char *key, bwg_str_t value)
{
    bwg_jw_key(writer, key);
    bwg_jw_string(writer, value.ptr, value.len);
}

static void write_cstr_field(bwg_jw_t *writer, const char *key, const char *value)
{
    bwg_jw_key(writer, key);
    bwg_jw_cstr(writer, value);
}

/** Keys here are in the canonical order, which the JWS and the binding both need. */
static void write_jwk(bwg_jw_t *writer, const bwg_identity_t *identity)
{
    bwg_jw_object_begin(writer);
    write_cstr_field(writer, "alg", "Ed25519");
    write_cstr_field(writer, "crv", "Ed25519");
    bwg_jw_key(writer, "key_ops");
    bwg_jw_array_begin(writer);
    bwg_jw_cstr(writer, "verify");
    bwg_jw_array_end(writer);
    write_cstr_field(writer, "kty", "OKP");
    write_cstr_field(writer, "use", "sig");
    write_cstr_field(writer, "x", identity->x);
    bwg_jw_object_end(writer);
}

typedef struct {
    const bwg_identity_t *identity;
    const bwg_possession_request_t *request;
    const char *firmware_commit;
    const char *app_elf_hex;
} proof_input_t;

static void write_claims(bwg_jw_t *writer, const proof_input_t *input)
{
    const bwg_possession_request_t *request = input->request;
    bwg_jw_object_begin(writer);
    write_cstr_field(writer, "appElfSha256", input->app_elf_hex);
    write_str_field(writer, "challengeBindingSha256", request->challenge_binding);
    write_str_field(writer, "controllerCapabilitySha256", request->capability_digest);
    bwg_jw_key(writer, "deviceIdentityJwk");
    write_jwk(writer, input->identity);
    write_str_field(writer, "deviceNonce", request->device_nonce);
    write_cstr_field(writer, "firmwareSourceCommit", input->firmware_commit);
    write_str_field(writer, "hostNonce", request->host_nonce);
    write_str_field(writer, "possessionNonce", request->possession_nonce);
    write_cstr_field(writer, "profile", PROOF_PROFILE);
    write_str_field(writer, "purpose", request->purpose);
    write_str_field(writer, "serialManifestSha256", request->manifest_digest);
    write_str_field(writer, "sessionId", request->session_id);
    bwg_jw_object_end(writer);
}

static void write_response(bwg_jw_t *writer, const proof_input_t *input, const char *compact_jws, size_t jws_len,
                           bool canonical_order)
{
    bwg_jw_object_begin(writer);
    if (canonical_order) {
        bwg_jw_key(writer, "ok");
        bwg_jw_bool(writer, true);
    }
    write_cstr_field(writer, "profile", POSSESSION_PROFILE);
    write_str_field(writer, "requestId", input->request->request_id);
    if (!canonical_order) {
        bwg_jw_key(writer, "ok");
        bwg_jw_bool(writer, true);
    }
    bwg_jw_key(writer, "result");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "claims");
    write_claims(writer, input);
    bwg_jw_key(writer, "compactJws");
    bwg_jw_string(writer, compact_jws, jws_len);
    bwg_jw_object_end(writer);
    bwg_jw_object_end(writer);
}

static void write_request(bwg_jw_t *writer, const bwg_possession_request_t *request)
{
    bwg_jw_object_begin(writer);
    write_cstr_field(writer, "command", "prove_possession");
    bwg_jw_key(writer, "payload");
    bwg_jw_object_begin(writer);
    write_str_field(writer, "challengeBindingSha256", request->challenge_binding);
    write_str_field(writer, "controllerCapabilitySha256", request->capability_digest);
    write_str_field(writer, "deviceNonce", request->device_nonce);
    write_str_field(writer, "hostNonce", request->host_nonce);
    write_str_field(writer, "possessionNonce", request->possession_nonce);
    write_str_field(writer, "purpose", request->purpose);
    write_str_field(writer, "serialManifestSha256", request->manifest_digest);
    write_str_field(writer, "sessionId", request->session_id);
    bwg_jw_object_end(writer);
    write_cstr_field(writer, "profile", POSSESSION_PROFILE);
    write_str_field(writer, "requestId", request->request_id);
    bwg_jw_object_end(writer);
}

static bool compute_binding(const proof_input_t *input, const char *compact_jws, size_t jws_len,
                            char binding[BWG_DIGEST_TEXT_SIZE])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    bwg_jw_t writer;
    bwg_jw_init_hash(&writer, &state);
    bwg_jw_object_begin(&writer);
    write_cstr_field(&writer, "profile", SESSION_PROFILE);
    bwg_jw_key(&writer, "request");
    write_request(&writer, input->request);
    bwg_jw_key(&writer, "response");
    write_response(&writer, input, compact_jws, jws_len, true);
    bwg_jw_object_end(&writer);
    if (!bwg_jw_ok(&writer)) {
        return false;
    }
    uint8_t digest[BWG_SHA256_BYTES];
    crypto_hash_sha256_final(&state, digest);
    return bwg_b64url_encode(digest, sizeof(digest), binding, BWG_DIGEST_TEXT_SIZE, NULL);
}

/** Builds the compact JWS into `out`. `claims` is working space. */
static bool sign_compact(const proof_input_t *input, bwg_buffer_t *claims, char *out, size_t capacity, size_t *written)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, claims->data, claims->capacity);
    write_claims(&writer, input);
    if (!bwg_jw_ok(&writer)) {
        return false;
    }
    size_t at = 0;
    size_t segment = 0;
    if (!bwg_b64url_encode((const uint8_t *) PROTECTED_HEADER, sizeof(PROTECTED_HEADER) - 1, out, capacity, &segment)) {
        return false;
    }
    at += segment;
    out[at++] = '.';
    if (!bwg_b64url_encode(claims->data, writer.len, out + at, capacity - at, &segment)) {
        return false;
    }
    at += segment;
    uint8_t signature[crypto_sign_BYTES];
    crypto_sign_detached(signature, NULL, (const uint8_t *) out, at, input->identity->secret_key);
    out[at++] = '.';
    if (!bwg_b64url_encode(signature, sizeof(signature), out + at, capacity - at, &segment)) {
        return false;
    }
    *written = at + segment;
    return true;
}

bool bwg_possession_prove(const bwg_allocator_t *allocator, const bwg_identity_t *identity,
                          const bwg_possession_request_t *request, const char *firmware_commit, const char *app_elf_hex,
                          bwg_buffer_t *response, char binding[BWG_DIGEST_TEXT_SIZE])
{
    const proof_input_t input = {
        .identity = identity,
        .request = request,
        .firmware_commit = firmware_commit,
        .app_elf_hex = app_elf_hex,
    };
    bwg_buffer_t claims = {0};
    bwg_buffer_t compact = {0};
    bool ok = false;
    size_t jws_len = 0;
    if (!bwg_buffer_alloc(allocator, &claims, CLAIMS_MAX_BYTES) || !bwg_buffer_alloc(allocator, &compact, JWS_MAX_BYTES) ||
        !sign_compact(&input, &claims, (char *) compact.data, compact.capacity, &jws_len) ||
        !compute_binding(&input, (const char *) compact.data, jws_len, binding) ||
        !bwg_buffer_alloc(allocator, response, BWG_POSSESSION_RESPONSE_MAX_BYTES)) {
        goto done;
    }
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, response->data, response->capacity);
    write_response(&writer, &input, (const char *) compact.data, jws_len, false);
    if (!bwg_jw_ok(&writer)) {
        bwg_buffer_release(allocator, response);
        goto done;
    }
    response->len = writer.len;
    ok = true;
done:
    bwg_buffer_release(allocator, &claims);
    bwg_buffer_release(allocator, &compact);
    return ok;
}
