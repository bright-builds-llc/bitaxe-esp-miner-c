#include "bwg_scan.h"

#include <string.h>

typedef struct {
    uint64_t is_object[BWG_SCAN_MAX_DEPTH / 64];
    size_t depth;
} nesting_t;

static bool push(nesting_t *nesting, bool is_object)
{
    if (nesting->depth >= BWG_SCAN_MAX_DEPTH) {
        return false;
    }
    uint64_t bit = 1ULL << (nesting->depth % 64);
    if (is_object) {
        nesting->is_object[nesting->depth / 64] |= bit;
    } else {
        nesting->is_object[nesting->depth / 64] &= ~bit;
    }
    nesting->depth++;
    return true;
}

static bool top_is_object(const nesting_t *nesting)
{
    size_t top = nesting->depth - 1;
    return (nesting->is_object[top / 64] >> (top % 64)) & 1;
}

static bool is_hex(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_continuation(uint8_t c)
{
    return (c & 0xC0) == 0x80;
}

/** Validates one UTF-8 sequence at `at`, returning its length or 0. Overlongs and surrogates are refused. */
static size_t utf8_sequence(const uint8_t *text, size_t len, size_t at)
{
    uint8_t lead = text[at];
    size_t remaining = len - at;
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return remaining >= 2 && is_continuation(text[at + 1]) ? 2 : 0;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (remaining < 3 || !is_continuation(text[at + 1]) || !is_continuation(text[at + 2])) {
            return 0;
        }
        if ((lead == 0xE0 && text[at + 1] < 0xA0) || (lead == 0xED && text[at + 1] > 0x9F)) {
            return 0;
        }
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (remaining < 4 || !is_continuation(text[at + 1]) || !is_continuation(text[at + 2]) ||
            !is_continuation(text[at + 3])) {
            return 0;
        }
        if ((lead == 0xF0 && text[at + 1] < 0x90) || (lead == 0xF4 && text[at + 1] > 0x8F)) {
            return 0;
        }
        return 4;
    }
    return 0;
}

static bool scan_escape(const uint8_t *text, size_t len, size_t *pos)
{
    size_t at = *pos + 1;
    if (at >= len) {
        return false;
    }
    switch (text[at]) {
    case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
        *pos = at + 1;
        return true;
    case 'u':
        if (len - at < 5) {
            return false;
        }
        for (size_t i = 1; i <= 4; i++) {
            if (!is_hex(text[at + i])) {
                return false;
            }
        }
        *pos = at + 5;
        return true;
    default:
        return false;
    }
}

static bool scan_digits(const uint8_t *text, size_t len, size_t *pos)
{
    size_t start = *pos;
    while (*pos < len && is_digit(text[*pos])) {
        (*pos)++;
    }
    return *pos > start;
}

static bool scan_number(const uint8_t *text, size_t len, size_t *pos)
{
    size_t at = *pos;
    if (at < len && text[at] == '-') {
        at++;
    }
    if (at >= len) {
        return false;
    }
    if (text[at] == '0') {
        at++;
    } else if (!scan_digits(text, len, &at)) {
        return false;
    }
    if (at < len && text[at] == '.') {
        at++;
        if (!scan_digits(text, len, &at)) {
            return false;
        }
    }
    if (at < len && (text[at] == 'e' || text[at] == 'E')) {
        at++;
        if (at < len && (text[at] == '+' || text[at] == '-')) {
            at++;
        }
        if (!scan_digits(text, len, &at)) {
            return false;
        }
    }
    *pos = at;
    return true;
}

static bool scan_literal(const uint8_t *text, size_t len, size_t *pos, const char *literal)
{
    size_t literal_len = strlen(literal);
    if (len - *pos < literal_len || memcmp(text + *pos, literal, literal_len) != 0) {
        return false;
    }
    *pos += literal_len;
    return true;
}

static bool scan_scalar(const uint8_t *text, size_t len, size_t *pos)
{
    switch (text[*pos]) {
    case '"': return bwg_scan_string(text, len, pos, NULL);
    case 't': return scan_literal(text, len, pos, "true");
    case 'f': return scan_literal(text, len, pos, "false");
    case 'n': return scan_literal(text, len, pos, "null");
    default: return scan_number(text, len, pos);
    }
}

static bool scan_key_and_colon(const uint8_t *text, size_t len, size_t *pos)
{
    bwg_scan_whitespace(text, len, pos);
    if (*pos >= len || text[*pos] != '"' || !bwg_scan_string(text, len, pos, NULL)) {
        return false;
    }
    bwg_scan_whitespace(text, len, pos);
    if (*pos >= len || text[*pos] != ':') {
        return false;
    }
    (*pos)++;
    return true;
}

/**
 * After a value inside a container: consumes closers and one comma. Sets `*done` when the
 * outermost container has closed; otherwise the caller scans the next value.
 */
static bool after_value(const uint8_t *text, size_t len, size_t *pos, nesting_t *nesting, bool *done)
{
    while (nesting->depth > 0) {
        bwg_scan_whitespace(text, len, pos);
        if (*pos >= len) {
            return false;
        }
        uint8_t c = text[*pos];
        (*pos)++;
        if (c == ',') {
            return !top_is_object(nesting) || scan_key_and_colon(text, len, pos);
        }
        if (c != (top_is_object(nesting) ? '}' : ']')) {
            return false;
        }
        nesting->depth--;
    }
    *done = true;
    return true;
}

void bwg_scan_whitespace(const uint8_t *text, size_t len, size_t *pos)
{
    while (*pos < len && (text[*pos] == ' ' || text[*pos] == '\t' || text[*pos] == '\n' || text[*pos] == '\r')) {
        (*pos)++;
    }
}

bool bwg_scan_string(const uint8_t *text, size_t len, size_t *pos, bool *maybe_has_escape)
{
    size_t at = *pos;
    if (at >= len || text[at] != '"') {
        return false;
    }
    at++;
    bool has_escape = false;
    while (at < len) {
        uint8_t c = text[at];
        if (c == '"') {
            *pos = at + 1;
            if (maybe_has_escape != NULL) {
                *maybe_has_escape = has_escape;
            }
            return true;
        }
        if (c < 0x20) {
            return false;
        }
        if (c == '\\') {
            has_escape = true;
            if (!scan_escape(text, len, &at)) {
                return false;
            }
            continue;
        }
        size_t sequence = utf8_sequence(text, len, at);
        if (sequence == 0) {
            return false;
        }
        at += sequence;
    }
    return false;
}

bool bwg_scan_value(const uint8_t *text, size_t len, size_t *pos)
{
    nesting_t nesting = {.depth = 0};
    bool done = false;
    while (!done) {
        bwg_scan_whitespace(text, len, pos);
        if (*pos >= len) {
            return false;
        }
        uint8_t c = text[*pos];
        if (c != '{' && c != '[') {
            if (!scan_scalar(text, len, pos)) {
                return false;
            }
        } else {
            (*pos)++;
            if (!push(&nesting, c == '{')) {
                return false;
            }
            bwg_scan_whitespace(text, len, pos);
            if (*pos >= len) {
                return false;
            }
            if (text[*pos] != (c == '{' ? '}' : ']')) {
                if (c == '{' && !scan_key_and_colon(text, len, pos)) {
                    return false;
                }
                continue;
            }
            (*pos)++;
            nesting.depth--;
        }
        if (!after_value(text, len, pos, &nesting, &done)) {
            return false;
        }
    }
    return true;
}

bool bwg_scan_document(const uint8_t *text, size_t len)
{
    size_t pos = 0;
    if (!bwg_scan_value(text, len, &pos)) {
        return false;
    }
    bwg_scan_whitespace(text, len, &pos);
    return pos == len;
}
