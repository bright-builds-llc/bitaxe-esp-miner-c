#include "bwg_settings_digest.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nvs_config.h"

typedef enum {
    KIND_BOOL,
    KIND_U16,
    KIND_I32,
    KIND_FLOAT,
    KIND_TEXT,
} setting_kind_t;

typedef struct {
    const char *name;
    NvsConfigKey key;
    setting_kind_t kind;
} setting_t;

// The Rust image's public allowlist. Its digest differs, which is fine: the Gate only compares one board with itself.
static const setting_t SETTINGS[] = {
    {"asicfrequency_f", NVS_CONFIG_ASIC_FREQUENCY, KIND_FLOAT},
    {"asicvoltage", NVS_CONFIG_ASIC_VOLTAGE, KIND_U16},
    {"autofanspeed", NVS_CONFIG_AUTO_FAN_SPEED, KIND_BOOL},
    {"display", NVS_CONFIG_DISPLAY, KIND_TEXT},
    {"displayOffset", NVS_CONFIG_DISPLAY_OFFSET, KIND_U16},
    {"displayTimeout", NVS_CONFIG_DISPLAY_TIMEOUT, KIND_I32},
    {"invertscreen", NVS_CONFIG_INVERT_SCREEN, KIND_BOOL},
    {"manualfanspeed", NVS_CONFIG_MANUAL_FAN_SPEED, KIND_U16},
    {"mineonboot", NVS_CONFIG_MINE_ON_BOOT, KIND_BOOL},
    {"minfanspeed", NVS_CONFIG_MIN_FAN_SPEED, KIND_U16},
    {"oc_enabled", NVS_CONFIG_OVERCLOCK_ENABLED, KIND_BOOL},
    {"overheat_mode", NVS_CONFIG_OVERHEAT_MODE, KIND_BOOL},
    {"rotation", NVS_CONFIG_ROTATION, KIND_U16},
    {"statsFrequency", NVS_CONFIG_STATISTICS_FREQUENCY, KIND_U16},
    {"temptarget", NVS_CONFIG_TEMP_TARGET, KIND_U16},
    {"themecolor", NVS_CONFIG_THEME_COLOR, KIND_TEXT},
    {"themescheme", NVS_CONFIG_THEME_SCHEME, KIND_TEXT},
};

static void hash_framed(crypto_hash_sha256_state *hash, const void *bytes, uint32_t len)
{
    const uint8_t length[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    crypto_hash_sha256_update(hash, length, sizeof(length));
    crypto_hash_sha256_update(hash, bytes, len);
}

static void hash_setting(crypto_hash_sha256_state *hash, const setting_t *setting)
{
    hash_framed(hash, setting->name, (uint32_t)strlen(setting->name));
    switch (setting->kind) {
    case KIND_BOOL: {
        const uint8_t value = nvs_config_get_bool(setting->key) ? 1 : 0;
        hash_framed(hash, &value, sizeof(value));
        return;
    }
    case KIND_U16: {
        const uint16_t value = nvs_config_get_u16(setting->key);
        hash_framed(hash, &value, sizeof(value));
        return;
    }
    case KIND_I32: {
        const int32_t value = nvs_config_get_i32(setting->key);
        hash_framed(hash, &value, sizeof(value));
        return;
    }
    case KIND_FLOAT: {
        const float value = nvs_config_get_float(setting->key);
        hash_framed(hash, &value, sizeof(value));
        return;
    }
    case KIND_TEXT: {
        char *maybe_value = nvs_config_get_string(setting->key);
        // Length zero can't be confused with a value, since a value always carries its terminator here.
        hash_framed(hash, maybe_value == NULL ? "" : maybe_value, maybe_value == NULL ? 0 : (uint32_t)strlen(maybe_value) + 1);
        free(maybe_value);
        return;
    }
    }
}

void bwg_settings_digest(uint8_t digest[crypto_hash_sha256_BYTES])
{
    static const char DOMAIN[] = "worker-public-settings-c-v1";
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    hash_framed(&hash, DOMAIN, sizeof(DOMAIN));
    for (size_t index = 0; index < sizeof(SETTINGS) / sizeof(SETTINGS[0]); index++) {
        hash_setting(&hash, &SETTINGS[index]);
    }
    crypto_hash_sha256_final(&hash, digest);
}
