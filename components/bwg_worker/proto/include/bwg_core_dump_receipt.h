#ifndef BWG_CORE_DUMP_RECEIPT_H
#define BWG_CORE_DUMP_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Numeric checkpoints around the core-dump store, bound to one firmware and one boot. Evidence, never authority. */
#define BWG_CORE_DUMP_WORDS 20u
#define BWG_CORE_DUMP_COMMIT 0x43445331u

#define BWG_CORE_DUMP_FIELD_NONE 0u
#define BWG_CORE_DUMP_FIELD_REQUESTED 9u
#define BWG_CORE_DUMP_FIELD_PREPARED 10u
#define BWG_CORE_DUMP_FIELD_SELF_TEST 16u
#define BWG_CORE_DUMP_LAST_STAGE 10u

typedef enum {
    BWG_CORE_DUMP_VALID,
    BWG_CORE_DUMP_UNAVAILABLE,
    BWG_CORE_DUMP_CORRUPT,
    BWG_CORE_DUMP_WRONG_FIRMWARE,
    BWG_CORE_DUMP_WRONG_BOOT,
} bwg_core_dump_status_t;

// These two run on the fatal path, so they stay inline in its IRAM and compile to no flash-resident tables.
static inline __attribute__((always_inline)) uint32_t bwg_core_dump_checksum(const uint32_t *words)
{
    uint32_t checksum = 0x9e3779b9u;
    for (uint32_t index = 1; index < 18; index++) {
        checksum = ((checksum << 5) | (checksum >> 27)) ^ words[index];
    }
    return checksum;
}

static inline __attribute__((always_inline)) void bwg_core_dump_checkpoint(uint32_t *words, uint32_t stage,
                                                                           uint32_t field, uint32_t value,
                                                                           int32_t result)
{
    words[6] = stage;
    if (field == BWG_CORE_DUMP_FIELD_REQUESTED) {
        words[9] = value;
        words[8] |= 1u;
    }
    if (field == BWG_CORE_DUMP_FIELD_PREPARED) {
        words[10] = value;
        words[8] |= 2u;
    }
    if (field == BWG_CORE_DUMP_FIELD_SELF_TEST) {
        words[16] = value;
    }
    uint32_t result_index = 0;
    if (stage >= 3 && stage <= 9 && (stage & 1u) == 1u) {
        result_index = stage / 2 + 10;
    } else if (stage == 10) {
        result_index = 15;
    }
    if (result_index != 0) {
        words[result_index] = (uint32_t)result;
        words[8] |= 1u << (result_index - 9);
    }
    words[17]++;
    words[18] = bwg_core_dump_checksum(words);
    words[19] = ~words[18];
}

/** The fingerprint a receipt names its firmware by: FNV-1a 64 of the source commit. */
uint64_t bwg_source_hash(const char *commit);

void bwg_core_dump_ready(uint32_t words[BWG_CORE_DUMP_WORDS], uint64_t source_hash, uint64_t boot_ordinal,
                         uint32_t capacity_bytes);

bwg_core_dump_status_t bwg_core_dump_classify(const uint32_t words[BWG_CORE_DUMP_WORDS], uint64_t source_hash,
                                              uint64_t expected_boot);

/** Renders the receipt line in ordinary task context. An invalid receipt shows only its status. */
bool bwg_core_dump_format(char *out, size_t size, const uint32_t words[BWG_CORE_DUMP_WORDS], uint64_t source_hash,
                          uint64_t expected_boot, bool previous_boot);

#endif // BWG_CORE_DUMP_RECEIPT_H
