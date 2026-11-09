#ifndef BWG_ONBOARDING_H
#define BWG_ONBOARDING_H

#include <stdbool.h>
#include <stdint.h>
#include <yyjson.h>

#include "bwg_json_writer.h"
#include "bwg_settings_hmac.h"

/** The only partition scheme the Gate admits: upstream's and the C image's table. */
#define BWG_ONBOARDING_SCHEME "esp32-factory-ota-ab"
#define BWG_ONBOARDING_VERSION_MAX 32u

typedef enum {
    BWG_SLOT_FACTORY = 0,
    BWG_SLOT_OTA_0,
    BWG_SLOT_OTA_1,
    BWG_SLOT_OTHER,
} bwg_app_slot_t;

/** The running image's OTA state. UNDEFINED covers `factory`, which has no otadata entry. */
typedef enum {
    BWG_IMAGE_UNDEFINED = 0,
    BWG_IMAGE_NEW,
    BWG_IMAGE_PENDING_VERIFY,
    BWG_IMAGE_VALID,
    BWG_IMAGE_INVALID,
    BWG_IMAGE_ABORTED,
} bwg_image_state_t;

/** What the partition table and otadata say, gathered by the shell. */
typedef struct {
    bool has_factory;
    bool has_ota_0;
    bool has_ota_1;
    bool has_otadata;
    uint32_t ota_0_bytes;
    uint32_t ota_1_bytes;
    bwg_app_slot_t running_slot;
    bwg_image_state_t running_state;
    /** Another slot holds an image the bootloader would accept. */
    bool rollback_possible;
} bwg_partition_facts_t;

/** The shell's half of the onboarding commands. Whole when present. */
typedef struct {
    /** Gathers the facts. False when it can't read the table or otadata. */
    bool (*partition_facts)(void *ctx, bwg_partition_facts_t *facts);
    /** Reads one key of the `main` namespace, as bwg_setting_reader_fn. */
    bwg_setting_reader_fn read_setting;
} bwg_onboarding_ops_t;

/** BWG_ONBOARDING_SCHEME when the table has `factory`, `ota_0`, `ota_1` and `otadata`, else `unknown`. */
const char *bwg_onboarding_scheme(const bwg_partition_facts_t *facts);

/** The smaller install slot, since an install writes only `ota_0` or `ota_1`. 0 without both. */
uint32_t bwg_onboarding_app_slot_bytes(const bwg_partition_facts_t *facts);

/** `factory`, `ota_0`, `ota_1`, or `unknown`, which the Gate refuses. */
const char *bwg_onboarding_slot_name(bwg_app_slot_t slot);

/** The running image boots on every reset: VALID, or UNDEFINED as `factory` is. A pending image is not yet. */
bool bwg_onboarding_bootable(bwg_image_state_t state);

/** Writes the inspection: the Gate's snapshot without `capabilities`, which `discover` gives. */
void bwg_onboarding_write_inspection(bwg_jw_t *writer, const bwg_partition_facts_t *facts, bool settings_readable);

typedef struct {
    /** The signed capability's `firmware.version`. */
    const char *firmware_version;
    bwg_partition_facts_t facts;
    uint8_t nonce[BWG_SETTINGS_NONCE_BYTES];
    /** Indexed by bwg_settings_category_t. */
    uint8_t hmacs[BWG_SETTINGS_CATEGORY_COUNT][BWG_SETTINGS_HMAC_BYTES];
} bwg_onboarding_verification_t;

/**
 * Writes the post-reboot proof, exactly the fields the Gate accepts. The settings read cleanly, or no
 * HMACs could exist, so `preservationConfirmed` is whether the image boots on its own.
 */
void bwg_onboarding_write_verification(bwg_jw_t *writer, const bwg_onboarding_verification_t *verification);

/** Takes the nonce from `{nonce}`: exactly that field, canonical base64url of 32 bytes. */
bool bwg_onboarding_parse_verify(yyjson_val *payload, uint8_t nonce[BWG_SETTINGS_NONCE_BYTES]);

/** Copies `firmware.version` out of a capability document. False when it is missing or too long. */
bool bwg_onboarding_capability_version(yyjson_val *capability, char out[BWG_ONBOARDING_VERSION_MAX]);

#endif // BWG_ONBOARDING_H
