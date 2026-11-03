#include "bwg_seq_doc.h"

#include <string.h>

#include "bwg_json.h"

static bool kid_is_valid(const char *kid, size_t len)
{
    if (len == 0 || len >= BWG_SEQ_DOC_KID_SIZE) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = kid[i];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool bwg_seq_doc_put(bwg_seq_doc_t *doc, const char *kid, uint64_t sequence)
{
    const size_t kid_len = strlen(kid);
    if (sequence == 0 || !kid_is_valid(kid, kid_len)) {
        return false;
    }
    size_t index = 0;
    while (index < doc->count && strcmp(doc->entries[index].kid, kid) < 0) {
        index++;
    }
    if (index < doc->count && strcmp(doc->entries[index].kid, kid) == 0) {
        doc->entries[index].sequence = sequence;
        return true;
    }
    if (doc->count == BWG_SEQ_DOC_MAX_KEYS) {
        return false;
    }
    memmove(&doc->entries[index + 1], &doc->entries[index], (doc->count - index) * sizeof(doc->entries[0]));
    memcpy(doc->entries[index].kid, kid, kid_len + 1);
    doc->entries[index].sequence = sequence;
    doc->count++;
    return true;
}

bool bwg_seq_doc_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, bwg_seq_doc_t *doc)
{
    memset(doc, 0, sizeof(*doc));
    if (len == 0 || len > BWG_SEQ_DOC_MAX_BYTES) {
        return false;
    }
    yyjson_doc *parsed = NULL;
    if (bwg_json_parse_strict(allocator, text, len, &parsed) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(parsed);
    bool valid = yyjson_is_obj(root);
    yyjson_obj_iter iter = yyjson_obj_iter_with(root);
    yyjson_val *key;
    while (valid && (key = yyjson_obj_iter_next(&iter)) != NULL) {
        uint64_t sequence = 0;
        valid = bwg_json_get_u64(yyjson_obj_iter_get_val(key), &sequence) && strlen(yyjson_get_str(key)) == yyjson_get_len(key) &&
                bwg_seq_doc_put(doc, yyjson_get_str(key), sequence);
    }
    yyjson_doc_free(parsed);
    return valid;
}

size_t bwg_seq_doc_encode(const bwg_seq_doc_t *doc, uint8_t *buffer, size_t capacity)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, buffer, capacity);
    bwg_jw_object_begin(&writer);
    for (size_t i = 0; i < doc->count; i++) {
        bwg_jw_key(&writer, doc->entries[i].kid);
        bwg_jw_u64(&writer, doc->entries[i].sequence);
    }
    bwg_jw_object_end(&writer);
    if (!bwg_jw_ok(&writer) || writer.len > BWG_SEQ_DOC_MAX_BYTES) {
        return 0;
    }
    return writer.len;
}

bool bwg_seq_doc_get(const bwg_seq_doc_t *doc, const char *kid, uint64_t *sequence)
{
    for (size_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->entries[i].kid, kid) == 0) {
            *sequence = doc->entries[i].sequence;
            return true;
        }
    }
    return false;
}

bwg_seq_verdict_t bwg_seq_doc_judge(const bwg_seq_doc_t *doc, const char *kid, bool has_expected, uint64_t expected,
                                    uint64_t next)
{
    uint64_t current = 0;
    const bool present = bwg_seq_doc_get(doc, kid, &current);
    if (present && current == next) {
        return BWG_SEQ_ALREADY_COMMITTED;
    }
    if (present != has_expected || (present && current != expected)) {
        return BWG_SEQ_STALE;
    }
    return BWG_SEQ_WRITE;
}

bool bwg_seq_doc_equal(const bwg_seq_doc_t *left, const bwg_seq_doc_t *right)
{
    if (left->count != right->count) {
        return false;
    }
    for (size_t i = 0; i < left->count; i++) {
        if (left->entries[i].sequence != right->entries[i].sequence || strcmp(left->entries[i].kid, right->entries[i].kid) != 0) {
            return false;
        }
    }
    return true;
}
