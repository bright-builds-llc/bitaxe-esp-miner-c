#ifndef BWG_REASON_H
#define BWG_REASON_H

#include <stdbool.h>
#include <stddef.h>

/** The closed list of reasons a lease can be restored. Names are protocol text. */
typedef enum {
    BWG_REASON_PAUSED = 0,
    BWG_REASON_CANCELLED,
    BWG_REASON_LEASE_EXPIRED,
    BWG_REASON_LOST_CONTINUITY,
    BWG_REASON_MONOTONIC_RESET,
    BWG_REASON_REBOOT,
    BWG_REASON_CHALLENGE_SATISFIED,
    BWG_REASON_CHALLENGE_EXPIRED,
    BWG_REASON_TAB_CLOSED,
    BWG_REASON_CONNECTIVITY_LOST,
    BWG_REASON_CONTROL_FAILED,
} bwg_reason_t;

/** The protocol name of a reason. */
const char *bwg_reason_name(bwg_reason_t reason);

/** Parses a protocol name of known length. Anything outside the closed list returns false. */
bool bwg_reason_parse(const char *text, size_t len, bwg_reason_t *reason);

#endif // BWG_REASON_H
