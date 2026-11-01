#include "bwg_json.h"

#include <sodium.h>

#include <string.h>

#include "bwg_scan.h"

// Keeps the size of each block so yyjson's free, which has no size argument, can zeroize it.
typedef struct {
    size_t size;
    size_t alignment_padding;
} block_header_t;

static void *yy_malloc(void *ctx, size_t size)
{
    const bwg_allocator_t *allocator = ctx;
    block_header_t *header = bwg_alloc(allocator, sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->size = size;
    return header + 1;
}

static void yy_free(void *ctx, void *ptr)
{
    const bwg_allocator_t *allocator = ctx;
    block_header_t *header = (block_header_t *) ptr - 1;
    bwg_free_secret(allocator, header, sizeof(*header) + header->size);
}

static void *yy_realloc(void *ctx, void *ptr, size_t old_size, size_t size)
{
    void *moved = yy_malloc(ctx, size);
    if (moved == NULL) {
        return NULL;
    }
    memcpy(moved, ptr, old_size < size ? old_size : size);
    yy_free(ctx, ptr);
    return moved;
}

static int compare_keys(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t shared = a_len < b_len ? a_len : b_len;
    int order = shared == 0 ? 0 : memcmp(a, b, shared);
    if (order != 0) {
        return order;
    }
    return a_len < b_len ? -1 : (a_len > b_len ? 1 : 0);
}

bool bwg_json_object_is_unique(yyjson_val *object)
{
    size_t count = yyjson_obj_size(object);
    if (count > BWG_JSON_MAX_OBJECT_KEYS) {
        return false;
    }
    yyjson_obj_iter outer = yyjson_obj_iter_with(object);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&outer)) != NULL) {
        yyjson_obj_iter inner = outer;
        yyjson_val *other;
        while ((other = yyjson_obj_iter_next(&inner)) != NULL) {
            if (yyjson_equals(key, other)) {
                return false;
            }
        }
    }
    return true;
}

bool bwg_json_doc_is_unique(yyjson_doc *doc)
{
    // yyjson lays values out contiguously in document order, so a flat walk visits every object
    // without recursion, whatever the nesting.
    yyjson_val *value = yyjson_doc_get_root(doc);
    size_t count = yyjson_doc_get_val_count(doc);
    for (size_t i = 0; i < count; i++) {
        if (yyjson_is_obj(value + i) && !bwg_json_object_is_unique(value + i)) {
            return false;
        }
    }
    return true;
}

bwg_json_status_t bwg_json_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, yyjson_doc **doc)
{
    *doc = NULL;
    if (!bwg_scan_document(text, len)) {
        return BWG_JSON_SYNTAX;
    }
    const yyjson_alc alc = {
        .malloc = yy_malloc,
        .realloc = yy_realloc,
        .free = yy_free,
        .ctx = (void *) allocator,
    };
    yyjson_read_err error;
    // The flags are empty, so yyjson only reads the text; the cast is for its in-situ mode's signature.
    yyjson_doc *parsed = yyjson_read_opts((char *) text, len, YYJSON_READ_NOFLAG, &alc, &error);
    if (parsed == NULL) {
        return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION ? BWG_JSON_NO_MEMORY : BWG_JSON_SYNTAX;
    }
    *doc = parsed;
    return BWG_JSON_OK;
}

bwg_json_status_t bwg_json_parse_strict(const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                                        yyjson_doc **doc)
{
    bwg_json_status_t status = bwg_json_parse(allocator, text, len, doc);
    if (status != BWG_JSON_OK) {
        return status;
    }
    if (!bwg_json_doc_is_unique(*doc)) {
        yyjson_doc_free(*doc);
        *doc = NULL;
        return BWG_JSON_REJECTED;
    }
    return BWG_JSON_OK;
}

static bool key_in_list(yyjson_val *key, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (yyjson_equals_str(key, list[i])) {
            return true;
        }
    }
    return false;
}

bool bwg_json_has_exact_fields(yyjson_val *object, const char *const *required, size_t required_count,
                               const char *const *optional, size_t optional_count)
{
    if (!yyjson_is_obj(object)) {
        return false;
    }
    for (size_t i = 0; i < required_count; i++) {
        if (yyjson_obj_get(object, required[i]) == NULL) {
            return false;
        }
    }
    yyjson_obj_iter iter = yyjson_obj_iter_with(object);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        if (!key_in_list(key, required, required_count) && !key_in_list(key, optional, optional_count)) {
            return false;
        }
    }
    return true;
}

bool bwg_json_get_u64(yyjson_val *value, uint64_t *out)
{
    if (!yyjson_is_uint(value)) {
        return false;
    }
    *out = yyjson_get_uint(value);
    return true;
}

bool bwg_json_get_str(yyjson_val *value, bwg_str_t *out)
{
    if (!yyjson_is_str(value)) {
        return false;
    }
    out->ptr = yyjson_get_str(value);
    out->len = yyjson_get_len(value);
    return true;
}

yyjson_val *bwg_json_field(yyjson_val *object, const char *key)
{
    return yyjson_obj_get(object, key);
}

bool bwg_str_equals(bwg_str_t text, const char *literal)
{
    size_t literal_len = strlen(literal);
    return text.len == literal_len && (literal_len == 0 || memcmp(text.ptr, literal, literal_len) == 0);
}

/** The smallest key strictly above the previous one, so no sort buffer is needed. */
static yyjson_val *next_sorted_key(yyjson_val *object, yyjson_val *maybe_previous)
{
    yyjson_val *maybe_best = NULL;
    yyjson_obj_iter iter = yyjson_obj_iter_with(object);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *text = yyjson_get_str(key);
        size_t len = yyjson_get_len(key);
        if (maybe_previous != NULL &&
            compare_keys(text, len, yyjson_get_str(maybe_previous), yyjson_get_len(maybe_previous)) <= 0) {
            continue;
        }
        if (maybe_best == NULL || compare_keys(text, len, yyjson_get_str(maybe_best), yyjson_get_len(maybe_best)) < 0) {
            maybe_best = key;
        }
    }
    return maybe_best;
}

static bool write_object(bwg_jw_t *writer, yyjson_val *object, bwg_json_order_t order)
{
    bwg_jw_object_begin(writer);
    if (order == BWG_JSON_ORDER_SORTED) {
        yyjson_val *maybe_key = NULL;
        while ((maybe_key = next_sorted_key(object, maybe_key)) != NULL) {
            bwg_jw_key_n(writer, yyjson_get_str(maybe_key), yyjson_get_len(maybe_key));
            if (!bwg_jw_ok(writer) || !bwg_json_write_value(writer, yyjson_obj_iter_get_val(maybe_key), order)) {
                return false;
            }
        }
    } else {
        yyjson_obj_iter iter = yyjson_obj_iter_with(object);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
            bwg_jw_key_n(writer, yyjson_get_str(key), yyjson_get_len(key));
            if (!bwg_jw_ok(writer) || !bwg_json_write_value(writer, yyjson_obj_iter_get_val(key), order)) {
                return false;
            }
        }
    }
    bwg_jw_object_end(writer);
    return bwg_jw_ok(writer);
}

static bool write_array(bwg_jw_t *writer, yyjson_val *array, bwg_json_order_t order)
{
    bwg_jw_array_begin(writer);
    yyjson_arr_iter iter = yyjson_arr_iter_with(array);
    yyjson_val *item;
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
        if (!bwg_jw_ok(writer) || !bwg_json_write_value(writer, item, order)) {
            return false;
        }
    }
    bwg_jw_array_end(writer);
    return bwg_jw_ok(writer);
}

bool bwg_json_write_value(bwg_jw_t *writer, yyjson_val *value, bwg_json_order_t order)
{
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL:
        bwg_jw_null(writer);
        break;
    case YYJSON_TYPE_BOOL:
        bwg_jw_bool(writer, yyjson_get_bool(value));
        break;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_uint(value)) {
            bwg_jw_u64(writer, yyjson_get_uint(value));
        } else if (yyjson_is_sint(value)) {
            bwg_jw_i64(writer, yyjson_get_sint(value));
        } else {
            return false;
        }
        break;
    case YYJSON_TYPE_STR:
        bwg_jw_string(writer, yyjson_get_str(value), yyjson_get_len(value));
        break;
    case YYJSON_TYPE_ARR:
        return write_array(writer, value, order);
    case YYJSON_TYPE_OBJ:
        return write_object(writer, value, order);
    default:
        return false;
    }
    return bwg_jw_ok(writer);
}
