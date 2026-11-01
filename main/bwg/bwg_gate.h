#ifndef BWG_GATE_H
#define BWG_GATE_H

#include "global_state.h"

/**
 * Starts the Gate's USB link and controller. A build with no embedded trust, or a board with no signed
 * capability, stays out of the Gate and behaves as upstream does. Call once Wi-Fi is up, since the
 * device identity's first generation needs the radio's entropy.
 */
void bwg_gate_start(GlobalState *GLOBAL_STATE);

/** The serial session's link epoch. Owner task only. */
uint32_t bwg_gate_owner_epoch(void);

#endif // BWG_GATE_H
