#include "bwg_store.h"

#include <string.h>

#include "bwg_seq_doc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "sodium.h"

#define NAMESPACE "bwg_worker"
#define KEY_SEED "device_seed"
#define KEY_SEQUENCES "lease_seq"
#define KEY_EFFECT "effect_pending"
#define KEY_CRASH_STREAK "crash_streak"

static const char *TAG = "bwg_store";

static bool is_all_zero(const uint8_t *bytes, size_t len)
{
    return sodium_is_zero(bytes, len) == 1;
}

static bool seed_read(nvs_handle_t handle, uint8_t seed[BWG_DEVICE_SEED_BYTES], bool *present)
{
    size_t len = BWG_DEVICE_SEED_BYTES;
    const esp_err_t err = nvs_get_blob(handle, KEY_SEED, seed, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *present = false;
        return true;
    }
    if (err != ESP_OK || len != BWG_DEVICE_SEED_BYTES || is_all_zero(seed, BWG_DEVICE_SEED_BYTES)) {
        return false;
    }
    *present = true;
    return true;
}

static bool seed_create(nvs_handle_t handle, uint8_t seed[BWG_DEVICE_SEED_BYTES])
{
    esp_fill_random(seed, BWG_DEVICE_SEED_BYTES);
    if (is_all_zero(seed, BWG_DEVICE_SEED_BYTES)) {
        return false;
    }
    if (nvs_set_blob(handle, KEY_SEED, seed, BWG_DEVICE_SEED_BYTES) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        return false;
    }
    uint8_t check[BWG_DEVICE_SEED_BYTES];
    bool present = false;
    const bool matches = seed_read(handle, check, &present) && present && sodium_memcmp(check, seed, BWG_DEVICE_SEED_BYTES) == 0;
    sodium_memzero(check, sizeof(check));
    return matches;
}

bool bwg_store_load_or_create_seed(uint8_t seed[BWG_DEVICE_SEED_BYTES])
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    bool present = false;
    bool ok = seed_read(handle, seed, &present);
    if (ok && !present) {
        ESP_LOGI(TAG, "Creating the device identity");
        ok = seed_create(handle, seed);
    }
    nvs_close(handle);
    if (!ok) {
        sodium_memzero(seed, BWG_DEVICE_SEED_BYTES);
    }
    return ok;
}

// An absent document is an empty one. Anything else that won't parse is corruption, which refuses
// leases rather than forgetting replay state.
static bool sequences_read(const bwg_allocator_t *allocator, nvs_handle_t handle, bwg_seq_doc_t *doc)
{
    uint8_t text[BWG_SEQ_DOC_MAX_BYTES];
    size_t len = sizeof(text);
    const esp_err_t err = nvs_get_blob(handle, KEY_SEQUENCES, text, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(doc, 0, sizeof(*doc));
        return true;
    }
    return err == ESP_OK && bwg_seq_doc_parse(allocator, text, len, doc);
}

bool bwg_store_high_water_load(const bwg_allocator_t *allocator, const char *kid, bool *present, uint64_t *value)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        // The namespace exists once the seed does, so this is a failure and not an empty store.
        return false;
    }
    bwg_seq_doc_t doc;
    const bool ok = sequences_read(allocator, handle, &doc);
    nvs_close(handle);
    if (ok) {
        *present = bwg_seq_doc_get(&doc, kid, value);
    }
    return ok;
}

static bool sequences_write(const bwg_allocator_t *allocator, nvs_handle_t handle, const bwg_seq_doc_t *doc)
{
    uint8_t text[BWG_SEQ_DOC_MAX_BYTES];
    const size_t len = bwg_seq_doc_encode(doc, text, sizeof(text));
    if (len == 0 || nvs_set_blob(handle, KEY_SEQUENCES, text, len) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        return false;
    }
    bwg_seq_doc_t check;
    return sequences_read(allocator, handle, &check) && bwg_seq_doc_equal(doc, &check);
}

bwg_store_result_t bwg_store_high_water_compare_and_store(const bwg_allocator_t *allocator, const char *kid, bool has_expected,
                                                          uint64_t expected, uint64_t next)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return BWG_STORE_ERROR;
    }
    bwg_store_result_t result = BWG_STORE_ERROR;
    bwg_seq_doc_t doc;
    if (sequences_read(allocator, handle, &doc)) {
        switch (bwg_seq_doc_judge(&doc, kid, has_expected, expected, next)) {
        case BWG_SEQ_ALREADY_COMMITTED:
            result = BWG_STORE_ALREADY_COMMITTED;
            break;
        case BWG_SEQ_STALE:
            result = BWG_STORE_STALE;
            break;
        case BWG_SEQ_WRITE:
            if (bwg_seq_doc_put(&doc, kid, next) && sequences_write(allocator, handle, &doc)) {
                result = BWG_STORE_COMMITTED;
            }
            break;
        }
    }
    nvs_close(handle);
    return result;
}

bool bwg_store_high_water_read_raw(uint8_t *text, size_t capacity, size_t *len)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    *len = capacity;
    esp_err_t err = nvs_get_blob(handle, KEY_SEQUENCES, text, len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *len = 0;
        err = ESP_OK;
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static bool effect_read(nvs_handle_t handle, bwg_effect_state_t *state)
{
    uint8_t stored = 0;
    const esp_err_t err = nvs_get_u8(handle, KEY_EFFECT, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *state = BWG_EFFECT_CLEAR;
        return true;
    }
    if (err != ESP_OK || (stored != BWG_EFFECT_PENDING && stored != BWG_EFFECT_REBOOT_CONFIRMED)) {
        return false;
    }
    *state = (bwg_effect_state_t)stored;
    return true;
}

bool bwg_store_effect_get(bwg_effect_state_t *state)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    const bool ok = effect_read(handle, state);
    nvs_close(handle);
    return ok;
}

bool bwg_store_effect_set(bwg_effect_state_t state)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = ESP_OK;
    if (state == BWG_EFFECT_CLEAR) {
        err = nvs_erase_key(handle, KEY_EFFECT);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_u8(handle, KEY_EFFECT, (uint8_t)state);
    }
    bwg_effect_state_t check = BWG_EFFECT_CLEAR;
    const bool ok = err == ESP_OK && nvs_commit(handle) == ESP_OK && effect_read(handle, &check) && check == state;
    nvs_close(handle);
    return ok;
}

bool bwg_store_ledger_load(const char *key, uint8_t *out, size_t capacity, size_t *len, bool *present)
{
    *present = false;
    *len = 0;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (opened != ESP_OK) {
        return false;
    }
    size_t stored = 0;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return true;
    }
    if (err == ESP_OK && (stored == 0 || stored > capacity)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, key, out, &stored);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    *len = stored;
    *present = true;
    return true;
}

bool bwg_store_ledger_store(const char *key, const uint8_t *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool bwg_store_crash_streak_get(uint16_t *streak)
{
    *streak = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_get_u16(handle, KEY_CRASH_STREAK, streak);
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

bool bwg_store_crash_streak_set(uint16_t streak)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u16(handle, KEY_CRASH_STREAK, streak);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}
