#ifndef BWG_LOCAL_GUARD_H
#define BWG_LOCAL_GUARD_H

#include <stdbool.h>

#include "global_state.h"

/** Follows local mining's runs, and pauses mining once the restart guard holds. Power task. */
void bwg_local_guard_tick(GlobalState *GLOBAL_STATE, bool asic_stopped);

/** A start of the ASIC failed. Whether the guard paces the next one, which it does only for local mining. Power task. */
bool bwg_local_guard_note_failed_start(GlobalState *GLOBAL_STATE);

/** Whether local mining is waiting out a backoff, which keeps the ASIC off and the device off its pools. */
bool bwg_local_guard_backing_off(void);

/** Whether the guard paused mining and no operator has resumed since. */
bool bwg_local_guard_holds_pause(void);

#endif // BWG_LOCAL_GUARD_H
