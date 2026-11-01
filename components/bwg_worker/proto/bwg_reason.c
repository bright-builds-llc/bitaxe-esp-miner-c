#include "bwg_reason.h"

#include <string.h>

static const char *const NAMES[] = {
    [BWG_REASON_PAUSED] = "paused",
    [BWG_REASON_CANCELLED] = "cancelled",
    [BWG_REASON_LEASE_EXPIRED] = "lease_expired",
    [BWG_REASON_LOST_CONTINUITY] = "lost_continuity",
    [BWG_REASON_MONOTONIC_RESET] = "monotonic_reset",
    [BWG_REASON_REBOOT] = "reboot",
    [BWG_REASON_CHALLENGE_SATISFIED] = "challenge_satisfied",
    [BWG_REASON_CHALLENGE_EXPIRED] = "challenge_expired",
    [BWG_REASON_TAB_CLOSED] = "tab_closed",
    [BWG_REASON_CONNECTIVITY_LOST] = "connectivity_lost",
    [BWG_REASON_CONTROL_FAILED] = "control_failed",
};

const char *bwg_reason_name(bwg_reason_t reason)
{
    return NAMES[reason];
}

bool bwg_reason_parse(const char *text, size_t len, bwg_reason_t *reason)
{
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strlen(NAMES[i]) == len && memcmp(NAMES[i], text, len) == 0) {
            *reason = (bwg_reason_t) i;
            return true;
        }
    }
    return false;
}
