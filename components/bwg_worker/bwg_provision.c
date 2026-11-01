#include "bwg_provision.h"

#include <string.h>

const char *bwg_provision_firmware_commit(void)
{
    return BWG_FIRMWARE_COMMIT;
}

#ifdef BWG_PROVISIONED

extern const uint8_t trust_start[] asm("_binary_trust_json_start");
extern const uint8_t trust_end[] asm("_binary_trust_json_end");
extern const uint8_t capability_205_start[] asm("_binary_capability_205_json_start");
extern const uint8_t capability_205_end[] asm("_binary_capability_205_json_end");
extern const uint8_t capability_601_start[] asm("_binary_capability_601_json_start");
extern const uint8_t capability_601_end[] asm("_binary_capability_601_json_end");

// Each embedded text file ends in a NUL that isn't part of the document.
static size_t text_len(const uint8_t *start, const uint8_t *end)
{
    return (size_t)(end - start) - 1;
}

bool bwg_provision_for_board(const char *board_version, bwg_provision_t *provision)
{
    if (strcmp(board_version, "205") == 0) {
        provision->capability_json = capability_205_start;
        provision->capability_len = text_len(capability_205_start, capability_205_end);
    } else if (strcmp(board_version, "601") == 0) {
        provision->capability_json = capability_601_start;
        provision->capability_len = text_len(capability_601_start, capability_601_end);
    } else {
        return false;
    }
    provision->trust_json = trust_start;
    provision->trust_len = text_len(trust_start, trust_end);
    return true;
}

#else

bool bwg_provision_for_board(const char *board_version, bwg_provision_t *provision)
{
    (void)board_version;
    (void)provision;
    return false;
}

#endif // BWG_PROVISIONED
