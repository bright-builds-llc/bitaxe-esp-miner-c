#ifndef BWG_POSSESSION_H
#define BWG_POSSESSION_H

#include <sodium.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

#include "bwg_alloc.h"
#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_link.h"

#define BWG_SEED_BYTES 32u
#define BWG_POSSESSION_NONCE_DIGEST_BYTES 32u
#define BWG_POSSESSION_RESPONSE_MAX_BYTES 4096u

/** The device's signing identity. Secret until bwg_identity_wipe. */
typedef struct {
    uint8_t secret_key[crypto_sign_SECRETKEYBYTES];
    uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
    /** The public key as base64url, for the JWK. */
    char x[BWG_DIGEST_TEXT_SIZE];
    /** SHA-256 of the raw public key in lowercase hex, for `device_identity_sha256`. */
    char fingerprint_hex[65];
} bwg_identity_t;

/** Derives the identity from a 32-byte seed. The caller wipes its own copy of the seed. */
void bwg_identity_from_seed(bwg_identity_t *identity, const uint8_t seed[BWG_SEED_BYTES]);

/** Zeroizes every field. */
void bwg_identity_wipe(bwg_identity_t *identity);

/** A shape-checked request, borrowing from its parsed document. */
typedef struct {
    bwg_str_t request_id;
    bwg_str_t purpose;
    bwg_str_t possession_nonce;
    bwg_str_t challenge_binding;
    bwg_str_t capability_digest;
    bwg_str_t manifest_digest;
    bwg_str_t session_id;
    bwg_str_t host_nonce;
    bwg_str_t device_nonce;
} bwg_possession_request_t;

/**
 * Checks the exact fields, profile, command, identifier and digest shapes. Keys must already be
 * known unique. False is the protocol's `invalid_request`.
 */
bool bwg_possession_parse_request(yyjson_val *root, bwg_possession_request_t *request);

/** Whether the request names this session, this capability and this manifest. False is `invalid_proof`. */
bool bwg_possession_matches(const bwg_possession_request_t *request, const char *capability_digest,
                            const char *manifest_digest, const bwg_session_binding_t *binding);

/** The fixed-size form of the nonce that the replay set keeps. */
void bwg_possession_nonce_digest(const bwg_possession_request_t *request, uint8_t out[BWG_POSSESSION_NONCE_DIGEST_BYTES]);

/**
 * Signs a deterministic proof. On true, `*response` owns the response JSON without LF, and
 * `binding` holds `controlSessionBindingSha256`. False means no memory or a request that does not
 * fit the bound; nothing is allocated then. Needs about 2 KiB of stack inside libsodium.
 */
bool bwg_possession_prove(const bwg_allocator_t *allocator, const bwg_identity_t *identity,
                          const bwg_possession_request_t *request, const char *firmware_commit, const char *app_elf_hex,
                          bwg_buffer_t *response, char binding[BWG_DIGEST_TEXT_SIZE]);

#endif // BWG_POSSESSION_H
