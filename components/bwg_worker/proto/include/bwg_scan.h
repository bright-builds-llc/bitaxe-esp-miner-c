#ifndef BWG_SCAN_H
#define BWG_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * An RFC 8259 scanner that finds spans without decoding or allocating, so a payload's raw bytes
 * can be hashed exactly as they were sent. It uses constant stack: nesting is a 128-level bit
 * stack. JSON nested deeper is refused; the JSON parser refuses the same depth.
 */
#define BWG_SCAN_MAX_DEPTH 128

/** Advances `*pos` past spaces, tabs, CR and LF. */
void bwg_scan_whitespace(const uint8_t *text, size_t len, size_t *pos);

/**
 * Scans one string starting at the opening quote, leaving `*pos` after the closing quote. Refuses
 * raw control characters, bad escapes and invalid UTF-8. `maybe_has_escape` reports any backslash.
 * A lone surrogate escape passes here, as the RFC grammar allows; the JSON parser refuses it.
 */
bool bwg_scan_string(const uint8_t *text, size_t len, size_t *pos, bool *maybe_has_escape);

/** Scans one value of any type, leaving `*pos` directly after it. Surrounding whitespace is not consumed. */
bool bwg_scan_value(const uint8_t *text, size_t len, size_t *pos);

/** Whether `text` is exactly one JSON value, with optional surrounding whitespace. */
bool bwg_scan_document(const uint8_t *text, size_t len);

#endif // BWG_SCAN_H
