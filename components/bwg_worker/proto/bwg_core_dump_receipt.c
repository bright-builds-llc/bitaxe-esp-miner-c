#include "bwg_core_dump_receipt.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define OPTIONAL_TEXT_BYTES 12

static const char *const STAGES[] = {
    "ready",          "store_entered", "init_entered",  "init_returned", "prepare_entered", "prepare_returned",
    "start_entered",  "start_returned", "end_entered",  "end_returned",  "store_returned",
};

uint64_t bwg_source_hash(const char *commit)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char *at = commit; *at != '\0'; at++) {
        hash ^= (uint8_t)*at;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void bwg_core_dump_ready(uint32_t words[BWG_CORE_DUMP_WORDS], uint64_t source_hash, uint64_t boot_ordinal,
                         uint32_t capacity_bytes)
{
    memset(words, 0, BWG_CORE_DUMP_WORDS * sizeof(uint32_t));
    words[0] = BWG_CORE_DUMP_COMMIT;
    words[1] = 1;
    words[2] = (uint32_t)source_hash;
    words[3] = (uint32_t)(source_hash >> 32);
    words[4] = (uint32_t)boot_ordinal;
    words[5] = (uint32_t)(boot_ordinal >> 32);
    words[7] = capacity_bytes;
    words[17] = 1;
    words[18] = bwg_core_dump_checksum(words);
    words[19] = ~words[18];
}

static uint64_t u64_at(const uint32_t *words, size_t index)
{
    return (uint64_t)words[index] | ((uint64_t)words[index + 1] << 32);
}

bwg_core_dump_status_t bwg_core_dump_classify(const uint32_t words[BWG_CORE_DUMP_WORDS], uint64_t source_hash,
                                              uint64_t expected_boot)
{
    uint32_t any = 0;
    for (size_t index = 0; index < BWG_CORE_DUMP_WORDS; index++) {
        any |= words[index];
    }
    if (any == 0) {
        return BWG_CORE_DUMP_UNAVAILABLE;
    }
    if (words[0] != BWG_CORE_DUMP_COMMIT || words[1] != 1 || words[6] > BWG_CORE_DUMP_LAST_STAGE ||
        (words[8] & ~127u) != 0 || words[16] > 1 || words[17] == 0 || words[18] != bwg_core_dump_checksum(words) ||
        words[19] != ~words[18]) {
        return BWG_CORE_DUMP_CORRUPT;
    }
    if (u64_at(words, 2) != source_hash) {
        return BWG_CORE_DUMP_WRONG_FIRMWARE;
    }
    if (expected_boot == 0 || u64_at(words, 4) != expected_boot) {
        return BWG_CORE_DUMP_WRONG_BOOT;
    }
    return BWG_CORE_DUMP_VALID;
}

static const char *status_label(bwg_core_dump_status_t status)
{
    switch (status) {
    case BWG_CORE_DUMP_VALID:
        return "valid";
    case BWG_CORE_DUMP_CORRUPT:
        return "corrupt";
    case BWG_CORE_DUMP_WRONG_FIRMWARE:
        return "wrong_firmware";
    case BWG_CORE_DUMP_WRONG_BOOT:
        return "wrong_boot";
    case BWG_CORE_DUMP_UNAVAILABLE:
        return "unavailable";
    }
    return "unavailable";
}

static const char *optional_u32(char text[OPTIONAL_TEXT_BYTES], const uint32_t *words, size_t index)
{
    if ((words[8] & (1u << (index - 9))) == 0) {
        return "unavailable";
    }
    snprintf(text, OPTIONAL_TEXT_BYTES, "%" PRIu32, words[index]);
    return text;
}

static const char *optional_i32(char text[OPTIONAL_TEXT_BYTES], const uint32_t *words, size_t index)
{
    if ((words[8] & (1u << (index - 9))) == 0) {
        return "unavailable";
    }
    snprintf(text, OPTIONAL_TEXT_BYTES, "%" PRId32, (int32_t)words[index]);
    return text;
}

bool bwg_core_dump_format(char *out, size_t size, const uint32_t words[BWG_CORE_DUMP_WORDS], uint64_t source_hash,
                          uint64_t expected_boot, bool previous_boot)
{
    const char *origin = previous_boot ? "previous_boot" : "current_boot";
    const bwg_core_dump_status_t status = bwg_core_dump_classify(words, source_hash, expected_boot);
    int written;
    if (status != BWG_CORE_DUMP_VALID) {
        written = snprintf(out, size, "core_dump_store_receipt schema=v1 origin=%s status=%s redacted=true", origin,
                           status_label(status));
    } else {
        char requested[OPTIONAL_TEXT_BYTES];
        char prepared[OPTIONAL_TEXT_BYTES];
        char init[OPTIONAL_TEXT_BYTES];
        char prepare[OPTIONAL_TEXT_BYTES];
        char start[OPTIONAL_TEXT_BYTES];
        char end[OPTIONAL_TEXT_BYTES];
        char store[OPTIONAL_TEXT_BYTES];
        written = snprintf(out, size,
                           "core_dump_store_receipt schema=v1 origin=%s status=valid source_hash=%016" PRIx64
                           " boot_ordinal=%" PRIu64 " stage=%s capacity_bytes=%" PRIu32
                           " requested_bytes=%s prepared_bytes=%s init_result=%s prepare_result=%s start_result=%s"
                           " end_result=%s store_result=%s self_test_marked=%s redacted=true",
                           origin, source_hash, expected_boot, STAGES[words[6]], words[7],
                           optional_u32(requested, words, 9), optional_u32(prepared, words, 10),
                           optional_i32(init, words, 11), optional_i32(prepare, words, 12),
                           optional_i32(start, words, 13), optional_i32(end, words, 14),
                           optional_i32(store, words, 15), words[16] == 1 ? "true" : "false");
    }
    return written > 0 && (size_t)written < size;
}
