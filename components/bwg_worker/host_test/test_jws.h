#ifndef BWG_TEST_JWS_H
#define BWG_TEST_JWS_H

#include <stddef.h>
#include <stdint.h>

/** The RFC 8032 test 1 seed, the fixtures' signing key for hint, attempt and soak vectors. */
extern const uint8_t BWG_TEST_RFC8032_SEED[32];

/** Signs a lease authorization the way an authority does. `out` needs at least 700 bytes. */
void bwg_test_sign_lease(const uint8_t seed[32], const char *kid, const char *binding, const char *operation,
                         const char *request_digest, const char *sequence, char *out, size_t capacity);

/** Signs exact header and claim texts, so tests can sign malformed shapes. */
void bwg_test_sign_raw(const uint8_t seed[32], const char *header, const char *claims, char *out, size_t capacity);

/** The trust text for one lease key derived from the seed, as a standalone authority object. */
void bwg_test_lease_half_text(const uint8_t seed[32], const char *kid, const char *issuer, char *out, size_t capacity);

#endif // BWG_TEST_JWS_H
