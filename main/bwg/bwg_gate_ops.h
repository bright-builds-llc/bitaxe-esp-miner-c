#ifndef BWG_GATE_OPS_H
#define BWG_GATE_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "bwg_controller.h"
#include "bwg_trace.h"
#include "global_state.h"

/** Fills the controller's device operations: leases through bwg_mining, durable state through bwg_store. */
void bwg_gate_ops_fill(bwg_controller_ops_t *ops, const bwg_allocator_t *allocator, GlobalState *GLOBAL_STATE);

/** Writes the `worker_admission` line. Any task. */
void bwg_gate_ops_format_admission(char *line, size_t capacity);

/** Completes the charge of a lease the device ended itself, so its status reads complete before any restore. */
void bwg_gate_ops_finish_stopped_lease(void);

const char *bwg_gate_ops_reset_cause(void);

void bwg_gate_ops_set_trace(bwg_trace_t *trace);

/** The lease most recently admitted, or 0 before the first. */
uint32_t bwg_gate_ops_lease_generation(void);

/** Remembers the owner task's lowest free stack, in bytes. */
void bwg_gate_ops_note_owner_stack(uint32_t free_bytes);

#endif // BWG_GATE_OPS_H
