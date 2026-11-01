#ifndef BWG_JSON_H
#define BWG_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

#include "bwg_alloc.h"
#include "bwg_json_writer.h"

/** Longest object the strict check will examine. No protocol object comes near it. */
#define BWG_JSON_MAX_OBJECT_KEYS 256

/** A counted string, usually pointing into a parsed document. Not NUL terminated by contract. */
typedef struct {
    const char *ptr;
    size_t len;
} bwg_str_t;

typedef enum {
    BWG_JSON_OK = 0,
    /** Not one valid RFC 8259 value, or nested past BWG_SCAN_MAX_DEPTH. */
    BWG_JSON_SYNTAX,
    /** Valid JSON, refused for a duplicate key or an oversized object. */
    BWG_JSON_REJECTED,
    BWG_JSON_NO_MEMORY,
} bwg_json_status_t;

typedef enum {
    BWG_JSON_ORDER_DOCUMENT = 0,
    BWG_JSON_ORDER_SORTED,
} bwg_json_order_t;

/**
 * Parses without judging key uniqueness. On BWG_JSON_OK, `*doc` owns memory from `allocator` that
 * is zeroized when freed. Free it with yyjson_doc_free. `allocator` must outlive the document.
 */
bwg_json_status_t bwg_json_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, yyjson_doc **doc);

/** bwg_json_parse plus bwg_json_doc_is_unique. A refused document is freed and `*doc` is NULL. */
bwg_json_status_t bwg_json_parse_strict(const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                                        yyjson_doc **doc);

/** Whether no object at any depth repeats a key, comparing decoded keys. Uses no recursion. */
bool bwg_json_doc_is_unique(yyjson_doc *doc);

/** Whether this one object has no repeated decoded key and at most BWG_JSON_MAX_OBJECT_KEYS keys. */
bool bwg_json_object_is_unique(yyjson_val *object);

/**
 * Whether `object` is an object holding every required key and no key outside the two lists.
 * Assumes keys are already known to be unique.
 */
bool bwg_json_has_exact_fields(yyjson_val *object, const char *const *required, size_t required_count,
                               const char *const *optional, size_t optional_count);

/** Reads a non-negative integer. Floats, exponents, negative numbers and -0 all return false. */
bool bwg_json_get_u64(yyjson_val *value, uint64_t *out);

/** Reads a string. Other types return false. */
bool bwg_json_get_str(yyjson_val *value, bwg_str_t *out);

/** Field lookup by decoded key. Returns NULL when absent. */
yyjson_val *bwg_json_field(yyjson_val *object, const char *key);

/** Whether the counted string equals the NUL-terminated `literal` exactly. */
bool bwg_str_equals(bwg_str_t text, const char *literal);

/**
 * Writes `value` with serde_json string escaping, without whitespace. BWG_JSON_ORDER_SORTED sorts
 * keys in byte order at every depth, the protocol's canonical form. Non-integer numbers return
 * false, as does anything deeper than the writer's limit; that limit also bounds this recursion.
 */
bool bwg_json_write_value(bwg_jw_t *writer, yyjson_val *value, bwg_json_order_t order);

#endif // BWG_JSON_H
