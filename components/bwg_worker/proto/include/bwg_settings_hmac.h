#ifndef BWG_SETTINGS_HMAC_H
#define BWG_SETTINGS_HMAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Every ESP-Miner build is settings schema 1 (`bwg-settings-hmac/1`, ADR-0111 in the Gate). */
#define BWG_SETTINGS_SCHEMA 1u
#define BWG_SETTINGS_NAMESPACE "main"
/** NVS's own string limit, and so the largest value a schema-1 key holds. A larger one is a read error. */
#define BWG_SETTINGS_VALUE_MAX_BYTES 4000u
#define BWG_SETTINGS_NONCE_BYTES 32u
#define BWG_SETTINGS_HMAC_BYTES 32u

/** ESP-IDF's `nvs_type_t` codes, which the encoding carries. */
typedef enum {
    BWG_NVS_U8 = 0x01,
    BWG_NVS_I8 = 0x11,
    BWG_NVS_U16 = 0x02,
    BWG_NVS_I16 = 0x12,
    BWG_NVS_U32 = 0x04,
    BWG_NVS_I32 = 0x14,
    BWG_NVS_U64 = 0x08,
    BWG_NVS_I64 = 0x18,
    BWG_NVS_STR = 0x21,
    BWG_NVS_BLOB = 0x42,
} bwg_nvs_type_t;

typedef enum {
    BWG_SETTINGS_ALL = 0,
    BWG_SETTINGS_NETWORK,
    BWG_SETTINGS_POOL,
    BWG_SETTINGS_CATEGORY_COUNT,
} bwg_settings_category_t;

typedef enum {
    BWG_SETTING_PRESENT = 0,
    BWG_SETTING_ABSENT,
    BWG_SETTING_ERROR,
} bwg_setting_read_t;

/**
 * Reads one key of the `main` namespace into `value`: its type code, and its bytes as NVS stores
 * them (integers little-endian at their width, a string without its terminator). ERROR for any
 * failure but a missing key, including a value larger than `capacity`.
 */
typedef bwg_setting_read_t (*bwg_setting_reader_fn)(void *ctx, const char *key, uint8_t *type, uint8_t *value,
                                                    size_t capacity, size_t *len);

typedef struct {
    void *ctx;
    bwg_setting_reader_fn read;
} bwg_settings_reader_t;

/** Receives the message as it is built. */
typedef void (*bwg_settings_sink_fn)(void *ctx, const uint8_t *bytes, size_t len);

/** The category's wire name: `all`, `network` or `pool`. */
const char *bwg_settings_category_name(bwg_settings_category_t category);

/** The category's schema-1 keys in byte-wise ascending order. */
const char *const *bwg_settings_category_keys(bwg_settings_category_t category, size_t *count);

/**
 * Streams the category's `bwg-settings-hmac/1` message into `sink`, reading each key once.
 * `scratch` holds BWG_SETTINGS_VALUE_MAX_BYTES and is left holding a value, so wipe it after.
 * False when a read fails or returns an unknown type, a wrong integer width, or a string with a NUL.
 */
bool bwg_settings_encode(bwg_settings_category_t category, const bwg_settings_reader_t *reader, uint8_t *scratch,
                         bwg_settings_sink_fn sink, void *sink_ctx);

/** HMAC-SHA-256 of the category's message under the nonce. False as for bwg_settings_encode. */
bool bwg_settings_hmac(bwg_settings_category_t category, const bwg_settings_reader_t *reader,
                       const uint8_t nonce[BWG_SETTINGS_NONCE_BYTES], uint8_t *scratch,
                       uint8_t out[BWG_SETTINGS_HMAC_BYTES]);

/** Whether every key of `all` reads cleanly, which covers every category. */
bool bwg_settings_readable(const bwg_settings_reader_t *reader, uint8_t *scratch);

#endif // BWG_SETTINGS_HMAC_H
