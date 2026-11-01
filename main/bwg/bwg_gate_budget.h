#ifndef BWG_GATE_BUDGET_H
#define BWG_GATE_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_budget.h"
#include "bwg_controller.h"
#include "global_state.h"

/**
 * The device's budget ledgers, in NVS. Owner task only, except the two cached values below.
 * Starting finishes whatever a previous boot left pending. False: the device must not join the Gate.
 */
bool bwg_gate_budget_start(const bwg_allocator_t *allocator, const GlobalState *GLOBAL_STATE);

/** Charges this lease's budget before any work and gives the active limit, 0 for none. False refuses the lease. */
bool bwg_gate_budget_admit(uint32_t generation, const bwg_allowance_t *allowance, uint32_t *limit_ms);

/** Completes the generation's charge. False leaves it pending, and the next call retries. */
bool bwg_gate_budget_finish(uint32_t generation);

bwg_evidence_t bwg_gate_budget_review(bwg_allowance_kind_t kind, bwg_str_t campaign_id, bwg_jw_t *writer);

void bwg_gate_budget_write_observation(uint32_t generation, uint32_t active_ms, bwg_jw_t *writer);

uint32_t bwg_gate_budget_campaign_reserved_ms(void);
bool bwg_gate_budget_campaign_complete(void);

#endif // BWG_GATE_BUDGET_H
