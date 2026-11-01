#ifndef BWG_PROVISION_H
#define BWG_PROVISION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** What a Gate build embeds for one board. Points into the image and never needs freeing. */
typedef struct {
    const uint8_t *trust_json;
    size_t trust_len;
    const uint8_t *capability_json;
    size_t capability_len;
} bwg_provision_t;

/** False on a build with no trust directory or for a board with no signed capability. */
bool bwg_provision_for_board(const char *board_version, bwg_provision_t *provision);

/** The 40 hex characters of the commit this image was configured from. */
const char *bwg_provision_firmware_commit(void);

#endif // BWG_PROVISION_H
