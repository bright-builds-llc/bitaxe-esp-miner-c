#ifndef TEST_ONBOARDING_SUPPORT_H
#define TEST_ONBOARDING_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

#include "bwg_settings_hmac.h"

#define FAKE_NVS_MAX_ENTRIES 32u
#define FAKE_NVS_MAX_VALUE 256u

typedef struct {
    char key[16];
    uint8_t type;
    uint8_t value[FAKE_NVS_MAX_VALUE];
    size_t len;
} fake_nvs_entry_t;

/** A `main` namespace held in memory, as a vector's entry list describes it. */
typedef struct {
    fake_nvs_entry_t entries[FAKE_NVS_MAX_ENTRIES];
    size_t count;
    /** A read of this key fails, as a corrupt page would. */
    const char *maybe_failing_key;
    int reads;
} fake_nvs_t;

/** Loads a vector's `entries` (`{key, type, value}` with ESP-IDF type names). False on anything unexpected. */
bool fake_nvs_load(fake_nvs_t *nvs, yyjson_val *entries);

/** A reader over the fake namespace. */
bwg_settings_reader_t fake_nvs_reader(fake_nvs_t *nvs);

/** Decodes lowercase hex into `out`. False on odd length, bad digits or too little room. */
bool test_hex_decode(const char *hex, uint8_t *out, size_t capacity, size_t *len);

/** The settings vectors file, parsed. The caller frees it with yyjson_doc_free. */
yyjson_doc *test_settings_vectors(void);

#endif // TEST_ONBOARDING_SUPPORT_H
