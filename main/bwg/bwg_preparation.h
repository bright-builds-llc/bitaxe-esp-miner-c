#ifndef BWG_PREPARATION_H
#define BWG_PREPARATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_preparation_receipt.h"

// The Gate's numbers for the steps this firmware's Start has. Its power and reset steps are one call each here, and
// the fan keeps its own task, so 2, 3, 6 and 7 never appear.
#define BWG_PREP_STEP_ADMISSION 1u
#define BWG_PREP_STEP_CORE_VOLTAGE 4u
#define BWG_PREP_STEP_VOLTAGE_SETTLE 5u
#define BWG_PREP_STEP_ASIC_INIT 8u
#define BWG_PREP_STEP_READY 9u

/** Keeps the previous boot's receipt. Call once, before anything records. */
void bwg_preparation_init(uint64_t source_hash, uint64_t boot_ordinal, bool cold_boot);

/** Starts recording for a lease's Start. A local mining start between these two records nothing. */
void bwg_preparation_begin(uint32_t generation);
void bwg_preparation_end(void);

void bwg_preparation_started(uint32_t step);
void bwg_preparation_completed(uint32_t step);
void bwg_preparation_failed(uint32_t step, bwg_prep_failure_t failure);

/** The receipt line for the previous boot or this one. */
bool bwg_preparation_format_line(char *out, size_t size, bool previous_boot);

#endif // BWG_PREPARATION_H
