#include "test_jws.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "bwg_codec.h"

const uint8_t BWG_TEST_RFC8032_SEED[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                           0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                           0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

void bwg_test_sign_lease(const uint8_t seed[32], const char *kid, const char *binding, const char *operation,
                         const char *request_digest, const char *sequence, char *out, size_t capacity)
{
    char header[256];
    char claims[256];
    snprintf(header, sizeof(header), "{\"alg\":\"Ed25519\",\"kid\":\"%s\",\"typ\":\"bwg-worker-lease-authorization+jws\"}",
             kid);
    snprintf(claims, sizeof(claims),
             "{\"controlSessionBindingSha256\":\"%s\",\"operation\":\"%s\",\"requestSha256\":\"%s\",\"sequence\":\"%s\"}",
             binding, operation, request_digest, sequence);
    bwg_test_sign_raw(seed, header, claims, out, capacity);
}

void bwg_test_sign_raw(const uint8_t seed[32], const char *header, const char *claims, char *out, size_t capacity)
{
    size_t at = 0;
    size_t written = 0;
    bwg_b64url_encode((const uint8_t *) header, strlen(header), out, capacity, &written);
    at += written;
    out[at++] = '.';
    bwg_b64url_encode((const uint8_t *) claims, strlen(claims), out + at, capacity - at, &written);
    at += written;
    uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
    uint8_t secret_key[crypto_sign_SECRETKEYBYTES];
    crypto_sign_seed_keypair(public_key, secret_key, seed);
    uint8_t signature[crypto_sign_BYTES];
    crypto_sign_detached(signature, NULL, (const uint8_t *) out, at, secret_key);
    out[at++] = '.';
    bwg_b64url_encode(signature, sizeof(signature), out + at, capacity - at, &written);
}

void bwg_test_lease_half_text(const uint8_t seed[32], const char *kid, const char *issuer, char *out, size_t capacity)
{
    uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
    uint8_t secret_key[crypto_sign_SECRETKEYBYTES];
    crypto_sign_seed_keypair(public_key, secret_key, seed);
    char x[BWG_DIGEST_TEXT_SIZE];
    bwg_b64url_encode(public_key, sizeof(public_key), x, sizeof(x), NULL);
    snprintf(out, capacity,
             "{\"profile\":\"bwg-worker-deployment-trust/0.2\",\"issuer\":\"%s\",\"audience\":\"bwg-worker-controller/0.4\","
             "\"role\":\"work_lease_authority\",\"keys\":[{\"kid\":\"%s\",\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"%s\","
             "\"alg\":\"Ed25519\",\"use\":\"sig\",\"key_ops\":[\"verify\"]}]}",
             issuer, kid, x);
}
