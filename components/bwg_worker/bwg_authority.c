#include "bwg_authority.h"

#include <stddef.h>

static bool maps_to(bwg_authority_state_t state, bwg_authority_event_t event, bwg_authority_state_t *result)
{
    switch (event) {
    case BWG_AUTHORITY_EVENT_REVOKE:
        *result = BWG_AUTHORITY_STOPPING;
        return state == BWG_AUTHORITY_LOCAL || state == BWG_AUTHORITY_LEASED;
    case BWG_AUTHORITY_EVENT_STOP_CONFIRMED:
        *result = BWG_AUTHORITY_STOPPED;
        return state == BWG_AUTHORITY_STOPPING;
    // Authority reopens only from STOPPED, so the chip has been reset and no old result can follow.
    case BWG_AUTHORITY_EVENT_LEASE_BEGIN:
        *result = BWG_AUTHORITY_LEASED;
        return state == BWG_AUTHORITY_STOPPED;
    case BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL:
        *result = BWG_AUTHORITY_LOCAL;
        return state == BWG_AUTHORITY_STOPPED;
    }
    return false;
}

bool bwg_authority_transition(bwg_authority_state_t state, bwg_authority_event_t event, bwg_authority_state_t *next)
{
    bwg_authority_state_t result;
    if (next == NULL || !maps_to(state, event, &result)) {
        return false;
    }
    *next = result;
    return true;
}

bool bwg_authority_admits(bwg_authority_state_t state)
{
    return state == BWG_AUTHORITY_LOCAL || state == BWG_AUTHORITY_LEASED;
}

bool bwg_authority_stop_wanted(bwg_authority_state_t state)
{
    return !bwg_authority_admits(state);
}
