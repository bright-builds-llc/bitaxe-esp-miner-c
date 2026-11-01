#ifndef BWG_NOISE_GATE_H
#define BWG_NOISE_GATE_H

#include "bwg_controller.h"
#include "global_state.h"

/** Fills the controller's Noise diagnostic commands. Without a reserved record they stay refused. */
void bwg_noise_gate_fill_ops(bwg_controller_ops_t *ops, GlobalState *GLOBAL_STATE);

/** The owner's tick: deadlines, and the session a diagnostic depends on. */
void bwg_noise_gate_poll(uint64_t generation, bool session_alive);

#endif // BWG_NOISE_GATE_H
