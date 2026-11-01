#ifndef BWG_CODEC_H
#define BWG_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Text length of a SHA-256 digest in unpadded base64url. */
#define BWG_DIGEST_TEXT_LEN 43
/** A digest text plus its NUL. */
#define BWG_DIGEST_TEXT_SIZE (BWG_DIGEST_TEXT_LEN + 1)
#define BWG_SHA256_BYTES 32

/** Characters needed to encode `bytes` in unpadded base64url. */
size_t bwg_b64url_encoded_len(size_t bytes);

/**
 * Writes unpadded base64url and a NUL. `capacity` must hold both, else returns false and writes
 * nothing. `maybe_written` receives the length without the NUL.
 */
bool bwg_b64url_encode(const uint8_t *bytes, size_t len, char *out, size_t capacity, size_t *maybe_written);

/**
 * Decodes canonical unpadded base64url only: padding, other alphabets, a length of 1 mod 4 and
 * nonzero trailing bits all return false. Decoding is all or nothing against `capacity`.
 */
bool bwg_b64url_decode(const char *text, size_t len, uint8_t *out, size_t capacity, size_t *written);

/** Whether `text` is the canonical encoding of exactly `expected_bytes` (at most 64) bytes. */
bool bwg_b64url_is_canonical(const char *text, size_t len, size_t expected_bytes);

/** Writes `2 * len` lowercase hex characters and a NUL. `out` must hold `2 * len + 1`. */
void bwg_hex_encode(const uint8_t *bytes, size_t len, char *out);

/** Whether `text` is exactly `expected_len` lowercase hex characters. */
bool bwg_hex_is_lower(const char *text, size_t len, size_t expected_len);

/** Whether `text` is 43 characters of the base64url alphabet. The protocol's weak digest check. */
bool bwg_digest_text_is_valid(const char *text, size_t len);

/** SHA-256 of `bytes` as a NUL-terminated digest text. */
void bwg_sha256_text(const uint8_t *bytes, size_t len, char out[BWG_DIGEST_TEXT_SIZE]);

#endif // BWG_CODEC_H
