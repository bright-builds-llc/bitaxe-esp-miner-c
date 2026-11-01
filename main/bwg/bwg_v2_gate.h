#ifndef BWG_V2_GATE_H
#define BWG_V2_GATE_H

#include "bwg_controller.h"
#include "global_state.h"
#include "sv2_noise.h"

/** Fills the controller's V2 commands. Without a reserved record they stay refused. */
void bwg_v2_gate_fill_ops(bwg_controller_ops_t *ops, GlobalState *GLOBAL_STATE);

/** Starts a share attempt for a V2 lease. False when the boot already had an attempt. */
bool bwg_v2_gate_seed_share(bwg_str_t attempt_id);

/** The observer for the attempt in force, which upstream's client passes to its Noise context. */
const sv2_noise_observer_t *bwg_v2_noise_observer(void);

#endif // BWG_V2_GATE_H
