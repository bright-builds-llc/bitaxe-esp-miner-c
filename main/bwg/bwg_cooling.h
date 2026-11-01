#ifndef BWG_COOLING_H
#define BWG_COOLING_H

#include <stdbool.h>
#include <stdint.h>

#include "global_state.h"

/**
 * The Gate's fan qualification, with the ASIC down and no budget touched. Owner task only.
 * Holds the fan at 100% and proves it turning from a sample taken after the command applied.
 */
bool bwg_cooling_prove(GlobalState *GLOBAL_STATE, uint32_t *fan_rpm);

/** Returns the fan to its 30% baseline, once the board is at its cool baseline. */
bool bwg_cooling_restore(GlobalState *GLOBAL_STATE);

/** Restores a fan this module still holds. False means it couldn't, so a stop must not be acknowledged. */
bool bwg_cooling_release(GlobalState *GLOBAL_STATE);

#endif // BWG_COOLING_H
