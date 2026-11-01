#include "bwg_frame.h"

#include <sodium.h>

#include <string.h>

#include "bwg_codec.h"
#include "bwg_json_writer.h"
#include "bwg_scan.h"

enum {
    FIELD_PROFILE = 1 << 0,
    FIELD_KIND = 1 << 1,
    FIELD_SESSION_ID = 1 << 2,
    FIELD_SEQUENCE = 1 << 3,
    FIELD_PAYLOAD_BYTES = 1 << 4,
    FIELD_PAYLOAD_SHA256 = 1 << 5,
    FIELD_PAYLOAD = 1 << 6,
    FIELD_ALL = (1 << 7) - 1,
};

typedef struct {
    const char *ptr;
    size_t len;
} span_t;

typedef struct {
    span_t profile;
    span_t kind;
    span_t maybe_session_id;
    uint32_t sequence;
    uint32_t payload_bytes;
    span_t payload_sha256;
    const uint8_t *payload;
    size_t payload_len;
} envelope_t;

static bool span_equals(span_t span, const char *literal)
{
    size_t len = strlen(literal);
    return span.len == len && memcmp(span.ptr, literal, len) == 0;
}

static const char *kind_name(bwg_kind_t kind)
{
    switch (kind) {
    case BWG_KIND_SESSION: return "session";
    case BWG_KIND_CONTROL: return "control";
    case BWG_KIND_HEARTBEAT: return "heartbeat";
    case BWG_KIND_DIAGNOSTIC: return "diagnostic";
    }
    return "";
}

static bool kind_from_name(span_t name, bwg_kind_t *kind)
{
    static const bwg_kind_t kinds[] = {BWG_KIND_SESSION, BWG_KIND_CONTROL, BWG_KIND_HEARTBEAT, BWG_KIND_DIAGNOSTIC};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (span_equals(name, kind_name(kinds[i]))) {
            *kind = kinds[i];
            return true;
        }
    }
    return false;
}

/** An envelope string: raw span between its quotes, refused if it holds any escape. */
static bool parse_plain_string(const uint8_t *line, size_t len, size_t *pos, span_t *out)
{
    size_t start = *pos;
    bool has_escape = false;
    if (!bwg_scan_string(line, len, pos, &has_escape) || has_escape) {
        return false;
    }
    out->ptr = (const char *) line + start + 1;
    out->len = *pos - start - 2;
    return true;
}

/** The integer grammar only: no sign, fraction, exponent or leading zero. */
static bool parse_u32(const uint8_t *line, size_t len, size_t *pos, uint32_t *out)
{
    size_t at = *pos;
    uint64_t value = 0;
    while (at < len && line[at] >= '0' && line[at] <= '9') {
        value = value * 10 + (uint64_t) (line[at] - '0');
        if (value > UINT32_MAX) {
            return false;
        }
        at++;
    }
    size_t digits = at - *pos;
    if (digits == 0 || (digits > 1 && line[*pos] == '0')) {
        return false;
    }
    if (at < len && (line[at] == '.' || line[at] == 'e' || line[at] == 'E')) {
        return false;
    }
    *pos = at;
    *out = (uint32_t) value;
    return true;
}

static bool parse_session_id(const uint8_t *line, size_t len, size_t *pos, span_t *out)
{
    static const char null_literal[] = "null";
    if (len - *pos >= 4 && memcmp(line + *pos, null_literal, 4) == 0) {
        *pos += 4;
        out->ptr = NULL;
        out->len = 0;
        return true;
    }
    return parse_plain_string(line, len, pos, out);
}

static bool parse_field(const uint8_t *line, size_t len, size_t *pos, span_t key, envelope_t *envelope, unsigned *seen)
{
    unsigned field;
    bool ok;
    if (span_equals(key, "profile")) {
        field = FIELD_PROFILE;
        ok = parse_plain_string(line, len, pos, &envelope->profile);
    } else if (span_equals(key, "kind")) {
        field = FIELD_KIND;
        ok = parse_plain_string(line, len, pos, &envelope->kind);
    } else if (span_equals(key, "sessionId")) {
        field = FIELD_SESSION_ID;
        ok = parse_session_id(line, len, pos, &envelope->maybe_session_id);
    } else if (span_equals(key, "sequence")) {
        field = FIELD_SEQUENCE;
        ok = parse_u32(line, len, pos, &envelope->sequence);
    } else if (span_equals(key, "payloadBytes")) {
        field = FIELD_PAYLOAD_BYTES;
        ok = parse_u32(line, len, pos, &envelope->payload_bytes);
    } else if (span_equals(key, "payloadSha256")) {
        field = FIELD_PAYLOAD_SHA256;
        ok = parse_plain_string(line, len, pos, &envelope->payload_sha256);
    } else if (span_equals(key, "payload")) {
        field = FIELD_PAYLOAD;
        envelope->payload = line + *pos;
        size_t start = *pos;
        ok = bwg_scan_value(line, len, pos);
        envelope->payload_len = *pos - start;
    } else {
        return false;
    }
    if (!ok || (*seen & field) != 0) {
        return false;
    }
    *seen |= field;
    return true;
}

static bool parse_envelope(const uint8_t *line, size_t len, envelope_t *envelope)
{
    size_t pos = 0;
    unsigned seen = 0;
    bwg_scan_whitespace(line, len, &pos);
    if (pos >= len || line[pos] != '{') {
        return false;
    }
    pos++;
    for (;;) {
        span_t key;
        bwg_scan_whitespace(line, len, &pos);
        // An escaped key cannot name a field: the raw comparison treats it as unknown.
        if (!parse_plain_string(line, len, &pos, &key)) {
            return false;
        }
        bwg_scan_whitespace(line, len, &pos);
        if (pos >= len || line[pos] != ':') {
            return false;
        }
        pos++;
        bwg_scan_whitespace(line, len, &pos);
        if (!parse_field(line, len, &pos, key, envelope, &seen)) {
            return false;
        }
        bwg_scan_whitespace(line, len, &pos);
        if (pos >= len) {
            return false;
        }
        uint8_t separator = line[pos];
        pos++;
        if (separator == '}') {
            break;
        }
        if (separator != ',') {
            return false;
        }
    }
    bwg_scan_whitespace(line, len, &pos);
    return pos == len && seen == FIELD_ALL;
}

bwg_frame_status_t bwg_record_parse(const uint8_t *line, size_t len, bwg_record_t *record)
{
    if (len > BWG_MAX_WIRE_FRAME_BYTES) {
        return BWG_FRAME_OVERSIZED;
    }
    if (len == 0 || line[len - 1] != '\n') {
        return BWG_FRAME_INVALID;
    }
    size_t body_len = len - 1;
    if (memchr(line, '\n', body_len) != NULL || memchr(line, '\r', body_len) != NULL) {
        return BWG_FRAME_INVALID;
    }
    envelope_t envelope = {0};
    bwg_kind_t kind;
    if (!parse_envelope(line, body_len, &envelope) || !kind_from_name(envelope.kind, &kind) ||
        !span_equals(envelope.profile, BWG_SERIAL_PROFILE) || envelope.payload[0] != '{') {
        return BWG_FRAME_INVALID;
    }
    if (envelope.maybe_session_id.ptr != NULL &&
        !bwg_b64url_is_canonical(envelope.maybe_session_id.ptr, envelope.maybe_session_id.len, BWG_SESSION_ID_BYTES)) {
        return BWG_FRAME_INVALID;
    }
    if (kind == BWG_KIND_CONTROL && envelope.payload_len > BWG_MAX_CONTROL_PAYLOAD_BYTES) {
        return BWG_FRAME_OVERSIZED;
    }
    if (envelope.payload_len != envelope.payload_bytes ||
        !bwg_b64url_is_canonical(envelope.payload_sha256.ptr, envelope.payload_sha256.len, BWG_SHA256_BYTES)) {
        return BWG_FRAME_INTEGRITY;
    }
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_sha256_text(envelope.payload, envelope.payload_len, digest);
    if (envelope.payload_sha256.len != BWG_DIGEST_TEXT_LEN ||
        memcmp(digest, envelope.payload_sha256.ptr, BWG_DIGEST_TEXT_LEN) != 0) {
        return BWG_FRAME_INTEGRITY;
    }
    record->kind = kind;
    record->maybe_session_id = envelope.maybe_session_id.ptr;
    record->sequence = envelope.sequence;
    record->payload = envelope.payload;
    record->payload_len = envelope.payload_len;
    return BWG_FRAME_OK;
}

bwg_frame_status_t bwg_record_encode(bwg_kind_t kind, const char *maybe_session_id, uint32_t sequence,
                                     const uint8_t *payload, size_t len, uint8_t *out, size_t capacity,
                                     size_t *out_len)
{
    if (len == 0 || payload[0] != '{' || !bwg_scan_document(payload, len)) {
        return BWG_FRAME_INVALID;
    }
    if (len + BWG_ENVELOPE_OVERHEAD_BYTES > capacity) {
        return BWG_FRAME_OVERSIZED;
    }
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_sha256_text(payload, len, digest);
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, capacity - 1);
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "profile");
    bwg_jw_cstr(&writer, BWG_SERIAL_PROFILE);
    bwg_jw_key(&writer, "kind");
    bwg_jw_cstr(&writer, kind_name(kind));
    bwg_jw_key(&writer, "sessionId");
    if (maybe_session_id == NULL) {
        bwg_jw_null(&writer);
    } else {
        bwg_jw_string(&writer, maybe_session_id, strlen(maybe_session_id));
    }
    bwg_jw_key(&writer, "sequence");
    bwg_jw_u64(&writer, sequence);
    bwg_jw_key(&writer, "payloadBytes");
    bwg_jw_u64(&writer, len);
    bwg_jw_key(&writer, "payloadSha256");
    bwg_jw_cstr(&writer, digest);
    bwg_jw_key(&writer, "payload");
    bwg_jw_raw_value(&writer, payload, len);
    bwg_jw_object_end(&writer);
    if (!bwg_jw_ok(&writer)) {
        return BWG_FRAME_OVERSIZED;
    }
    out[writer.len] = '\n';
    size_t total = writer.len + 1;
    bwg_record_t check;
    bwg_frame_status_t status = bwg_record_parse(out, total, &check);
    if (status != BWG_FRAME_OK) {
        return status;
    }
    *out_len = total;
    return BWG_FRAME_OK;
}

void bwg_accumulator_init(bwg_accumulator_t *accumulator, const bwg_allocator_t *allocator)
{
    accumulator->allocator = allocator;
    accumulator->maybe_bytes = NULL;
    accumulator->len = 0;
    accumulator->discarding = false;
}

bwg_acc_result_t bwg_accumulator_push(bwg_accumulator_t *accumulator, uint8_t byte, const uint8_t **line,
                                      size_t *line_len)
{
    if (accumulator->discarding) {
        accumulator->discarding = byte != '\n';
        return BWG_ACC_NONE;
    }
    if (accumulator->len >= BWG_MAX_WIRE_FRAME_BYTES - 1 && byte != '\n') {
        bwg_accumulator_clear(accumulator);
        accumulator->discarding = true;
        return BWG_ACC_OVERSIZED;
    }
    if (accumulator->maybe_bytes == NULL) {
        accumulator->maybe_bytes = bwg_alloc(accumulator->allocator, BWG_MAX_WIRE_FRAME_BYTES);
        if (accumulator->maybe_bytes == NULL) {
            accumulator->discarding = byte != '\n';
            return BWG_ACC_UNAVAILABLE;
        }
    }
    accumulator->maybe_bytes[accumulator->len] = byte;
    accumulator->len++;
    if (byte != '\n') {
        return BWG_ACC_NONE;
    }
    *line = accumulator->maybe_bytes;
    *line_len = accumulator->len;
    return BWG_ACC_LINE;
}

void bwg_accumulator_clear(bwg_accumulator_t *accumulator)
{
    if (accumulator->maybe_bytes != NULL && accumulator->len > 0) {
        sodium_memzero(accumulator->maybe_bytes, accumulator->len);
    }
    accumulator->len = 0;
    accumulator->discarding = false;
}

void bwg_accumulator_deinit(bwg_accumulator_t *accumulator)
{
    if (accumulator->maybe_bytes != NULL) {
        bwg_free_secret(accumulator->allocator, accumulator->maybe_bytes, accumulator->len);
        accumulator->maybe_bytes = NULL;
    }
    accumulator->len = 0;
    accumulator->discarding = false;
}
