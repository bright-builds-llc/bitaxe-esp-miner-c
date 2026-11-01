#ifndef BWG_UNSAFE_DETAIL_H
#define BWG_UNSAFE_DETAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_limits.h"

/** A revocation for this sample has just been asked for. Keeps its detail if it won as `unsafe_observation`. */
void bwg_unsafe_detail_note_sample(const bwg_safety_sample_t *sample, bwg_safety_fact_t fact, uint32_t since_safe_ms);

/** The same for upstream's overheat check, whose limits aren't the lease's: the reading is the fact's own. */
void bwg_unsafe_detail_note_overheat(bool chip, float celsius, uint32_t since_safe_ms);

/** The current detail line for the last such generation. False with none to report. */
bool bwg_unsafe_detail_format(char *line, size_t capacity);

#endif // BWG_UNSAFE_DETAIL_H
