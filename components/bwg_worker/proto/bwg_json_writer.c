#include "bwg_json_writer.h"

#include <string.h>

static void emit(bwg_jw_t *writer, const void *bytes, size_t len)
{
    if (writer->failed) {
        return;
    }
    if (writer->maybe_hash != NULL) {
        crypto_hash_sha256_update(writer->maybe_hash, bytes, len);
        return;
    }
    if (len > writer->capacity - writer->len) {
        writer->failed = true;
        return;
    }
    memcpy(writer->buffer + writer->len, bytes, len);
    writer->len += len;
}

static void emit_byte(bwg_jw_t *writer, char byte)
{
    emit(writer, &byte, 1);
}

static void before_value(bwg_jw_t *writer)
{
    if (writer->after_key) {
        writer->after_key = false;
        return;
    }
    if (writer->depth == 0) {
        return;
    }
    uint32_t bit = 1U << (writer->depth - 1);
    if ((writer->first_in_container & bit) != 0) {
        writer->first_in_container &= ~bit;
        return;
    }
    emit_byte(writer, ',');
}

static void container_begin(bwg_jw_t *writer, char open)
{
    before_value(writer);
    if (writer->depth >= BWG_JW_MAX_DEPTH) {
        writer->failed = true;
        return;
    }
    emit_byte(writer, open);
    writer->depth++;
    writer->first_in_container |= 1U << (writer->depth - 1);
}

static void container_end(bwg_jw_t *writer, char close)
{
    if (writer->depth == 0) {
        writer->failed = true;
        return;
    }
    writer->first_in_container &= ~(1U << (writer->depth - 1));
    writer->depth--;
    emit_byte(writer, close);
}

static void emit_escaped(bwg_jw_t *writer, const char *text, size_t len)
{
    static const char HEX[] = "0123456789abcdef";
    emit_byte(writer, '"');
    size_t run_start = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) text[i];
        if (c >= 0x20 && c != '"' && c != '\\') {
            continue;
        }
        emit(writer, text + run_start, i - run_start);
        run_start = i + 1;
        switch (c) {
        case '"': emit(writer, "\\\"", 2); break;
        case '\\': emit(writer, "\\\\", 2); break;
        case '\b': emit(writer, "\\b", 2); break;
        case '\f': emit(writer, "\\f", 2); break;
        case '\n': emit(writer, "\\n", 2); break;
        case '\r': emit(writer, "\\r", 2); break;
        case '\t': emit(writer, "\\t", 2); break;
        default: {
            char unicode[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15]};
            emit(writer, unicode, sizeof(unicode));
            break;
        }
        }
    }
    emit(writer, text + run_start, len - run_start);
    emit_byte(writer, '"');
}

void bwg_jw_init_buffer(bwg_jw_t *writer, uint8_t *buffer, size_t capacity)
{
    memset(writer, 0, sizeof(*writer));
    writer->buffer = buffer;
    writer->capacity = capacity;
}

void bwg_jw_init_hash(bwg_jw_t *writer, crypto_hash_sha256_state *hash)
{
    memset(writer, 0, sizeof(*writer));
    writer->maybe_hash = hash;
}

void bwg_jw_object_begin(bwg_jw_t *writer)
{
    container_begin(writer, '{');
}

void bwg_jw_object_end(bwg_jw_t *writer)
{
    container_end(writer, '}');
}

void bwg_jw_array_begin(bwg_jw_t *writer)
{
    container_begin(writer, '[');
}

void bwg_jw_array_end(bwg_jw_t *writer)
{
    container_end(writer, ']');
}

void bwg_jw_key_n(bwg_jw_t *writer, const char *key, size_t len)
{
    before_value(writer);
    emit_escaped(writer, key, len);
    emit_byte(writer, ':');
    writer->after_key = true;
}

void bwg_jw_key(bwg_jw_t *writer, const char *key)
{
    bwg_jw_key_n(writer, key, strlen(key));
}

void bwg_jw_string(bwg_jw_t *writer, const char *text, size_t len)
{
    before_value(writer);
    emit_escaped(writer, text, len);
}

void bwg_jw_cstr(bwg_jw_t *writer, const char *text)
{
    bwg_jw_string(writer, text, strlen(text));
}

void bwg_jw_u64(bwg_jw_t *writer, uint64_t value)
{
    char digits[20];
    size_t at = sizeof(digits);
    do {
        digits[--at] = (char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);
    before_value(writer);
    emit(writer, digits + at, sizeof(digits) - at);
}

void bwg_jw_i64(bwg_jw_t *writer, int64_t value)
{
    if (value >= 0) {
        bwg_jw_u64(writer, (uint64_t) value);
        return;
    }
    before_value(writer);
    emit_byte(writer, '-');
    // The first value is already placed, so the magnitude must not add another separator.
    writer->after_key = true;
    bwg_jw_u64(writer, ~(uint64_t) value + 1);
}

void bwg_jw_bool(bwg_jw_t *writer, bool value)
{
    before_value(writer);
    if (value) {
        emit(writer, "true", 4);
    } else {
        emit(writer, "false", 5);
    }
}

void bwg_jw_null(bwg_jw_t *writer)
{
    before_value(writer);
    emit(writer, "null", 4);
}

void bwg_jw_string_repeat(bwg_jw_t *writer, char byte, size_t count)
{
    before_value(writer);
    emit_byte(writer, '"');
    if (writer->failed) {
        return;
    }
    if (writer->maybe_hash != NULL) {
        for (size_t i = 0; i < count; i++) {
            emit_byte(writer, byte);
        }
    } else if (count > writer->capacity - writer->len) {
        writer->failed = true;
    } else {
        memset(writer->buffer + writer->len, byte, count);
        writer->len += count;
    }
    emit_byte(writer, '"');
}

void bwg_jw_raw_value(bwg_jw_t *writer, const uint8_t *json, size_t len)
{
    before_value(writer);
    emit(writer, json, len);
}

bool bwg_jw_ok(const bwg_jw_t *writer)
{
    return !writer->failed;
}
