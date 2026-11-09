#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_codec.h"
#include "bwg_test.h"
#include "test_onboarding_support.h"

typedef struct {
    const char *name;
    uint8_t code;
    int width;
    bool is_signed;
} type_spec_t;

static const type_spec_t TYPES[] = {
    {"u8", BWG_NVS_U8, 1, false},   {"i8", BWG_NVS_I8, 1, true},     {"u16", BWG_NVS_U16, 2, false},
    {"i16", BWG_NVS_I16, 2, true},  {"u32", BWG_NVS_U32, 4, false},  {"i32", BWG_NVS_I32, 4, true},
    {"u64", BWG_NVS_U64, 8, false}, {"i64", BWG_NVS_I64, 8, true},   {"str", BWG_NVS_STR, 0, false},
    {"blob", BWG_NVS_BLOB, 0, false},
};

static const type_spec_t *find_type(const char *name)
{
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (strcmp(TYPES[i].name, name) == 0) {
            return &TYPES[i];
        }
    }
    return NULL;
}

static bool encode_integer(const type_spec_t *spec, const char *text, fake_nvs_entry_t *entry)
{
    errno = 0;
    char *end = NULL;
    uint64_t bits = spec->is_signed ? (uint64_t) strtoll(text, &end, 10) : strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    for (int i = 0; i < spec->width; i++) {
        entry->value[i] = (uint8_t) (bits >> (8 * i));
    }
    entry->len = (size_t) spec->width;
    return true;
}

static bool load_entry(yyjson_val *item, fake_nvs_entry_t *entry)
{
    const char *key = yyjson_get_str(yyjson_obj_get(item, "key"));
    const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
    yyjson_val *value = yyjson_obj_get(item, "value");
    const type_spec_t *spec = type == NULL ? NULL : find_type(type);
    if (key == NULL || strlen(key) >= sizeof(entry->key) || spec == NULL || !yyjson_is_str(value)) {
        return false;
    }
    strcpy(entry->key, key);
    entry->type = spec->code;
    const char *text = yyjson_get_str(value);
    const size_t text_len = yyjson_get_len(value);
    if (spec->width > 0) {
        return encode_integer(spec, text, entry);
    }
    if (spec->code == BWG_NVS_STR) {
        if (text_len > sizeof(entry->value)) {
            return false;
        }
        memcpy(entry->value, text, text_len);
        entry->len = text_len;
        return true;
    }
    return bwg_b64url_decode(text, text_len, entry->value, sizeof(entry->value), &entry->len);
}

bool fake_nvs_load(fake_nvs_t *nvs, yyjson_val *entries)
{
    memset(nvs, 0, sizeof(*nvs));
    size_t index;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(entries, index, max, item)
    {
        if (nvs->count == FAKE_NVS_MAX_ENTRIES || !load_entry(item, &nvs->entries[nvs->count])) {
            return false;
        }
        nvs->count++;
    }
    return true;
}

static bwg_setting_read_t fake_read(void *ctx, const char *key, uint8_t *type, uint8_t *value, size_t capacity,
                                    size_t *len)
{
    fake_nvs_t *nvs = ctx;
    nvs->reads++;
    if (nvs->maybe_failing_key != NULL && strcmp(nvs->maybe_failing_key, key) == 0) {
        return BWG_SETTING_ERROR;
    }
    for (size_t i = 0; i < nvs->count; i++) {
        const fake_nvs_entry_t *entry = &nvs->entries[i];
        if (strcmp(entry->key, key) != 0) {
            continue;
        }
        if (entry->len > capacity) {
            return BWG_SETTING_ERROR;
        }
        *type = entry->type;
        memcpy(value, entry->value, entry->len);
        *len = entry->len;
        return BWG_SETTING_PRESENT;
    }
    return BWG_SETTING_ABSENT;
}

bwg_settings_reader_t fake_nvs_reader(fake_nvs_t *nvs)
{
    return (bwg_settings_reader_t) {.ctx = nvs, .read = fake_read};
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

bool test_hex_decode(const char *hex, uint8_t *out, size_t capacity, size_t *len)
{
    const size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || hex_len / 2 > capacity) {
        return false;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        const int high = hex_digit(hex[2 * i]);
        const int low = hex_digit(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t) (high << 4 | low);
    }
    *len = hex_len / 2;
    return true;
}

yyjson_doc *test_settings_vectors(void)
{
    return bwg_test_fixture_doc("onboarding/settings-hmac-vectors.json");
}
