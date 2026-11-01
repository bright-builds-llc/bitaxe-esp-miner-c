#ifndef BWG_BOOT_GUARD_H
#define BWG_BOOT_GUARD_H

#include "global_state.h"

/** Extends or clears the crash streak from this boot's reset, and pauses mining once it trips. Never stops the boot. */
void bwg_boot_guard_apply(GlobalState *GLOBAL_STATE);

/** Clears the streak once the boot is stable or the operator has resumed. Power task. */
void bwg_boot_guard_tick(const GlobalState *GLOBAL_STATE);

/** Whether the guard paused this boot and no operator has resumed since. */
bool bwg_boot_guard_holds_pause(void);

#endif // BWG_BOOT_GUARD_H
