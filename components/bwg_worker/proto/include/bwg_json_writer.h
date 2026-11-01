#ifndef BWG_JSON_WRITER_H
#define BWG_JSON_WRITER_H

#include <sodium.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Containers the writer can track. Deeper output sets `failed`. */
#define BWG_JW_MAX_DEPTH 31

/**
 * A bounded JSON writer with no whitespace and no reordering: canonical output needs the caller
 * to emit keys in sorted order. Bytes go to a buffer, or into a running SHA-256 so nothing has to
 * be held in memory. Any overflow latches `failed`; later calls do nothing. Check it once at the
 * end. The writer never reads or keeps strings beyond each call.
 */
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t len;
    crypto_hash_sha256_state *maybe_hash;
    uint32_t first_in_container;
    uint8_t depth;
    bool after_key;
    bool failed;
} bwg_jw_t;

/** Writes into `buffer`, which is not NUL terminated. */
void bwg_jw_init_buffer(bwg_jw_t *writer, uint8_t *buffer, size_t capacity);

/** Feeds an already initialized SHA-256 state. The caller finalizes it. */
void bwg_jw_init_hash(bwg_jw_t *writer, crypto_hash_sha256_state *hash);

/** Opens or closes a container. Past BWG_JW_MAX_DEPTH, or closing with none open, sets `failed`. */
void bwg_jw_object_begin(bwg_jw_t *writer);
void bwg_jw_object_end(bwg_jw_t *writer);
void bwg_jw_array_begin(bwg_jw_t *writer);
void bwg_jw_array_end(bwg_jw_t *writer);

/** Writes a NUL-terminated key that needs no escaping beyond serde_json's rules, then the colon. */
void bwg_jw_key(bwg_jw_t *writer, const char *key);
/** Same, for a key of known length, such as one decoded from a parsed document. */
void bwg_jw_key_n(bwg_jw_t *writer, const char *key, size_t len);

/** Writes a quoted value with serde_json escaping: `"`, `\`, `\b\f\n\r\t`, `\u00xx`; the rest raw. */
void bwg_jw_string(bwg_jw_t *writer, const char *text, size_t len);
/** Same, for a NUL-terminated value. */
void bwg_jw_cstr(bwg_jw_t *writer, const char *text);

/** Writes a quoted value of `count` copies of `byte`, which must need no escaping. Costs no extra memory. */
void bwg_jw_string_repeat(bwg_jw_t *writer, char byte, size_t count);

/** Each writes one scalar value, placing the comma as needed. */
void bwg_jw_u64(bwg_jw_t *writer, uint64_t value);
void bwg_jw_i64(bwg_jw_t *writer, int64_t value);
void bwg_jw_bool(bwg_jw_t *writer, bool value);
void bwg_jw_null(bwg_jw_t *writer);

/** Writes one already encoded JSON value, keeping comma placement correct. */
void bwg_jw_raw_value(bwg_jw_t *writer, const uint8_t *json, size_t len);

/** Whether everything so far fitted. */
bool bwg_jw_ok(const bwg_jw_t *writer);

#endif // BWG_JSON_WRITER_H
