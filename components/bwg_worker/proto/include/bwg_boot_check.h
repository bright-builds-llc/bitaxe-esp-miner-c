#ifndef BWG_BOOT_CHECK_H
#define BWG_BOOT_CHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_onboarding.h"

/**
 * How long a pending image has, from boot, to pass its startup check. The Gate link starts only after
 * Wi-Fi, the NTP wait (up to 32 s) and ASIC initialization, so this leaves room for a slow network.
 * Provisional until measured on the 601.
 */
#define BWG_BOOT_CHECK_DEADLINE_MS 120000u

typedef enum {
    /** The image isn't on trial: it is valid, factory, or already judged. */
    BWG_BOOT_NOTHING = 0,
    BWG_BOOT_WAIT,
    BWG_BOOT_MARK_VALID,
    /** Mark the image invalid and restart into the previous slot. */
    BWG_BOOT_ROLL_BACK,
} bwg_boot_verdict_t;

typedef struct {
    bwg_image_state_t state;
    /** The Gate is provisioned for this board and the boot isn't a self-test. */
    bool link_required;
    bool link_ready;
    /** Every schema-1 key read back cleanly. */
    bool settings_readable;
    uint64_t elapsed_ms;
} bwg_boot_facts_t;

/**
 * Judges an image on trial: one the bootloader marked pending, or a new one under a bootloader without
 * rollback, which can still roll itself back. Unreadable settings roll back at once; a missing link
 * rolls back at the deadline.
 */
bwg_boot_verdict_t bwg_boot_check_verdict(const bwg_boot_facts_t *facts);

#endif // BWG_BOOT_CHECK_H
