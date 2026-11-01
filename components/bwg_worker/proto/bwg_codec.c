#include "bwg_codec.h"

#include <sodium.h>

#include <string.h>

static const char BASE64URL_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char HEX_ALPHABET[] = "0123456789abcdef";

#define MAX_CANONICAL_CHECK_BYTES 64

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

size_t bwg_b64url_encoded_len(size_t bytes)
{
    return bytes / 3 * 4 + (bytes % 3 == 0 ? 0 : bytes % 3 + 1);
}

bool bwg_b64url_encode(const uint8_t *bytes, size_t len, char *out, size_t capacity, size_t *maybe_written)
{
    size_t encoded = bwg_b64url_encoded_len(len);
    if (capacity < encoded + 1) {
        return false;
    }
    size_t at = 0;
    uint32_t accumulator = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        accumulator = (accumulator << 8) | bytes[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out[at++] = BASE64URL_ALPHABET[(accumulator >> bits) & 63];
        }
    }
    if (bits > 0) {
        out[at++] = BASE64URL_ALPHABET[(accumulator << (6 - bits)) & 63];
    }
    out[at] = '\0';
    if (maybe_written != NULL) {
        *maybe_written = at;
    }
    return true;
}

bool bwg_b64url_decode(const char *text, size_t len, uint8_t *out, size_t capacity, size_t *written)
{
    if (len % 4 == 1) {
        return false;
    }
    size_t decoded = len / 4 * 3 + (len % 4 == 0 ? 0 : len % 4 - 1);
    if (decoded > capacity) {
        return false;
    }
    size_t at = 0;
    uint32_t accumulator = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int value = base64url_value(text[i]);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | (uint32_t) value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[at++] = (uint8_t) (accumulator >> bits);
        }
    }
    if (bits > 0 && (accumulator & ((1U << bits) - 1)) != 0) {
        return false;
    }
    *written = at;
    return true;
}

bool bwg_b64url_is_canonical(const char *text, size_t len, size_t expected_bytes)
{
    if (expected_bytes > MAX_CANONICAL_CHECK_BYTES || len != bwg_b64url_encoded_len(expected_bytes)) {
        return false;
    }
    uint8_t decoded[MAX_CANONICAL_CHECK_BYTES];
    size_t written = 0;
    bool canonical = bwg_b64url_decode(text, len, decoded, sizeof(decoded), &written) && written == expected_bytes;
    sodium_memzero(decoded, sizeof(decoded));
    return canonical;
}

void bwg_hex_encode(const uint8_t *bytes, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = HEX_ALPHABET[bytes[i] >> 4];
        out[2 * i + 1] = HEX_ALPHABET[bytes[i] & 15];
    }
    out[2 * len] = '\0';
}

bool bwg_hex_is_lower(const char *text, size_t len, size_t expected_len)
{
    if (len != expected_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        bool digit = text[i] >= '0' && text[i] <= '9';
        bool lower = text[i] >= 'a' && text[i] <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

bool bwg_digest_text_is_valid(const char *text, size_t len)
{
    if (len != BWG_DIGEST_TEXT_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (base64url_value(text[i]) < 0) {
            return false;
        }
    }
    return true;
}

void bwg_sha256_text(const uint8_t *bytes, size_t len, char out[BWG_DIGEST_TEXT_SIZE])
{
    uint8_t digest[BWG_SHA256_BYTES];
    crypto_hash_sha256(digest, bytes, len);
    bwg_b64url_encode(digest, sizeof(digest), out, BWG_DIGEST_TEXT_SIZE, NULL);
}
