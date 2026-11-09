#include "bwg_onboarding_gate.h"

#include <string.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"

static bool has_partition(esp_partition_type_t type, esp_partition_subtype_t subtype, uint32_t *maybe_size)
{
    const esp_partition_t *maybe_partition = esp_partition_find_first(type, subtype, NULL);
    if (maybe_partition != NULL && maybe_size != NULL) {
        *maybe_size = maybe_partition->size;
    }
    return maybe_partition != NULL;
}

static bwg_app_slot_t slot_of(const esp_partition_t *partition)
{
    switch (partition->subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY:
        return BWG_SLOT_FACTORY;
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:
        return BWG_SLOT_OTA_0;
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:
        return BWG_SLOT_OTA_1;
    default:
        return BWG_SLOT_OTHER;
    }
}

static bwg_image_state_t state_of(const esp_partition_t *partition)
{
    esp_ota_img_states_t state;
    // `factory` has no otadata entry (ESP_ERR_NOT_SUPPORTED), and neither does a slot otadata never named.
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        return BWG_IMAGE_UNDEFINED;
    }
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return BWG_IMAGE_NEW;
    case ESP_OTA_IMG_PENDING_VERIFY:
        return BWG_IMAGE_PENDING_VERIFY;
    case ESP_OTA_IMG_VALID:
        return BWG_IMAGE_VALID;
    case ESP_OTA_IMG_INVALID:
        return BWG_IMAGE_INVALID;
    case ESP_OTA_IMG_ABORTED:
        return BWG_IMAGE_ABORTED;
    default:
        return BWG_IMAGE_UNDEFINED;
    }
}

bool bwg_onboarding_partition_facts(void *ctx, bwg_partition_facts_t *facts)
{
    (void) ctx;
    const esp_partition_t *maybe_running = esp_ota_get_running_partition();
    if (maybe_running == NULL) {
        return false;
    }
    memset(facts, 0, sizeof(*facts));
    facts->has_factory = has_partition(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    facts->has_ota_0 = has_partition(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, &facts->ota_0_bytes);
    facts->has_ota_1 = has_partition(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, &facts->ota_1_bytes);
    facts->has_otadata = has_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    facts->running_slot = slot_of(maybe_running);
    facts->running_state = state_of(maybe_running);
    facts->rollback_possible = esp_ota_check_rollback_is_possible();
    return true;
}

static void put_le(uint8_t *value, uint64_t bits, size_t width, size_t *len)
{
    for (size_t i = 0; i < width; i++) {
        value[i] = (uint8_t) (bits >> (8 * i));
    }
    *len = width;
}

static esp_err_t read_integer(nvs_handle_t handle, const char *key, nvs_type_t type, uint8_t *value, size_t *len)
{
    esp_err_t err = ESP_ERR_NVS_TYPE_MISMATCH;
    union {
        uint8_t u8;
        int8_t i8;
        uint16_t u16;
        int16_t i16;
        uint32_t u32;
        int32_t i32;
        uint64_t u64;
        int64_t i64;
    } v = {0};
    switch (type) {
    case NVS_TYPE_U8:
        err = nvs_get_u8(handle, key, &v.u8);
        put_le(value, v.u8, 1, len);
        break;
    case NVS_TYPE_I8:
        err = nvs_get_i8(handle, key, &v.i8);
        put_le(value, (uint8_t) v.i8, 1, len);
        break;
    case NVS_TYPE_U16:
        err = nvs_get_u16(handle, key, &v.u16);
        put_le(value, v.u16, 2, len);
        break;
    case NVS_TYPE_I16:
        err = nvs_get_i16(handle, key, &v.i16);
        put_le(value, (uint16_t) v.i16, 2, len);
        break;
    case NVS_TYPE_U32:
        err = nvs_get_u32(handle, key, &v.u32);
        put_le(value, v.u32, 4, len);
        break;
    case NVS_TYPE_I32:
        err = nvs_get_i32(handle, key, &v.i32);
        put_le(value, (uint32_t) v.i32, 4, len);
        break;
    case NVS_TYPE_U64:
        err = nvs_get_u64(handle, key, &v.u64);
        put_le(value, v.u64, 8, len);
        break;
    case NVS_TYPE_I64:
        err = nvs_get_i64(handle, key, &v.i64);
        put_le(value, (uint64_t) v.i64, 8, len);
        break;
    default:
        break;
    }
    return err;
}

static esp_err_t read_value(nvs_handle_t handle, const char *key, nvs_type_t type, uint8_t *value, size_t capacity,
                            size_t *len)
{
    size_t required = 0;
    if (type == NVS_TYPE_STR) {
        // The stored length counts the terminator, which the encoding leaves out.
        esp_err_t err = nvs_get_str(handle, key, NULL, &required);
        if (err != ESP_OK || required == 0 || required > capacity) {
            return err != ESP_OK ? err : ESP_ERR_NVS_INVALID_LENGTH;
        }
        err = nvs_get_str(handle, key, (char *) value, &required);
        *len = required - 1;
        return err;
    }
    if (type == NVS_TYPE_BLOB) {
        esp_err_t err = nvs_get_blob(handle, key, NULL, &required);
        if (err != ESP_OK || required > capacity) {
            return err != ESP_OK ? err : ESP_ERR_NVS_INVALID_LENGTH;
        }
        err = nvs_get_blob(handle, key, value, &required);
        *len = required;
        return err;
    }
    return read_integer(handle, key, type, value, len);
}

bwg_setting_read_t bwg_onboarding_read_setting(void *ctx, const char *key, uint8_t *type, uint8_t *value,
                                               size_t capacity, size_t *len)
{
    (void) ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BWG_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return BWG_SETTING_ABSENT;
    }
    if (err != ESP_OK) {
        return BWG_SETTING_ERROR;
    }
    nvs_type_t found = NVS_TYPE_ANY;
    err = nvs_find_key(handle, key, &found);
    if (err == ESP_OK) {
        err = read_value(handle, key, found, value, capacity, len);
    }
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return BWG_SETTING_ABSENT;
    }
    // ESP-IDF's type codes are the encoding's.
    *type = (uint8_t) found;
    return err == ESP_OK ? BWG_SETTING_PRESENT : BWG_SETTING_ERROR;
}

static const bwg_onboarding_ops_t ONBOARDING_OPS = {
    .partition_facts = bwg_onboarding_partition_facts,
    .read_setting = bwg_onboarding_read_setting,
};

void bwg_onboarding_gate_fill_ops(bwg_controller_ops_t *ops)
{
    ops->maybe_onboarding = &ONBOARDING_OPS;
}
