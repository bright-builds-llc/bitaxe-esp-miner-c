#ifndef BWG_AUTHORITY_H
#define BWG_AUTHORITY_H

#include <stdbool.h>

typedef enum {
    // Upstream's own mining, with no Gate involved. Zero, so an untouched device starts here.
    BWG_AUTHORITY_LOCAL = 0,
    BWG_AUTHORITY_LEASED,
    BWG_AUTHORITY_STOPPING,
    BWG_AUTHORITY_STOPPED,
} bwg_authority_state_t;

typedef enum {
    BWG_AUTHORITY_EVENT_REVOKE,
    BWG_AUTHORITY_EVENT_STOP_CONFIRMED,
    BWG_AUTHORITY_EVENT_LEASE_BEGIN,
    BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL,
} bwg_authority_event_t;

/** Returns false, leaving `next` untouched, when `event` is not legal in `state`. */
bool bwg_authority_transition(bwg_authority_state_t state, bwg_authority_event_t event, bwg_authority_state_t *next);

/** Whether work may go to the ASIC and shares to the pool. */
bool bwg_authority_admits(bwg_authority_state_t state);

/** Whether the ASIC must be stopped, or kept stopped. */
bool bwg_authority_stop_wanted(bwg_authority_state_t state);

#endif // BWG_AUTHORITY_H
