#ifndef BWG_SEQ_DOC_H
#define BWG_SEQ_DOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"

#define BWG_SEQ_DOC_MAX_KEYS 8u
#define BWG_SEQ_DOC_MAX_BYTES 512u
#define BWG_SEQ_DOC_KID_SIZE 33u

typedef struct {
    char kid[BWG_SEQ_DOC_KID_SIZE];
    uint64_t sequence;
} bwg_seq_entry_t;

/**
 * The `lease_seq` NVS blob: compact JSON from key ID to high water, keys sorted, as the Rust firmware
 * stored it, so a device keeps its replay state. Entries stay sorted by byte order.
 */
typedef struct {
    size_t count;
    bwg_seq_entry_t entries[BWG_SEQ_DOC_MAX_KEYS];
} bwg_seq_doc_t;

/** False for anything but at most 8 valid key IDs with nonzero values, within 1 to 512 bytes. */
bool bwg_seq_doc_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, bwg_seq_doc_t *doc);

/** Writes the canonical document. Returns its length, or 0 if it doesn't fit in `capacity` or the 512-byte limit. */
size_t bwg_seq_doc_encode(const bwg_seq_doc_t *doc, uint8_t *buffer, size_t capacity);

/** Reads the high water for `kid`. */
bool bwg_seq_doc_get(const bwg_seq_doc_t *doc, const char *kid, uint64_t *sequence);

/** Sets the high water for `kid`, keeping order. False for an invalid key, a zero value, or a ninth key. */
bool bwg_seq_doc_put(bwg_seq_doc_t *doc, const char *kid, uint64_t sequence);

typedef enum {
    BWG_SEQ_WRITE = 0,
    /** `next` is already the high water, so a write that landed before a failed reply needs nothing more. */
    BWG_SEQ_ALREADY_COMMITTED,
    /** The high water isn't the one the caller expected, so it moved, or it never existed. */
    BWG_SEQ_STALE,
} bwg_seq_verdict_t;

/** Whether `kid`'s high water may move to `next`, given what the caller read before (`has_expected`, `expected`). */
bwg_seq_verdict_t bwg_seq_doc_judge(const bwg_seq_doc_t *doc, const char *kid, bool has_expected, uint64_t expected,
                                    uint64_t next);

/** Whether two documents hold the same entries. */
bool bwg_seq_doc_equal(const bwg_seq_doc_t *left, const bwg_seq_doc_t *right);

#endif // BWG_SEQ_DOC_H
