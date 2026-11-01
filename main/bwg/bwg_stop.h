#ifndef BWG_STOP_H
#define BWG_STOP_H

#include <stdbool.h>
#include <stdint.h>

#include "global_state.h"

typedef enum {
    BWG_STOP_STAGE_NOT_STARTED = 0,
    BWG_STOP_STAGE_STOP_DISPATCH,
    BWG_STOP_STAGE_REDUCE_FREQUENCY,
    BWG_STOP_STAGE_HOLD_RESET,
    BWG_STOP_STAGE_DISABLE_CORE_VOLTAGE,
    BWG_STOP_STAGE_DISABLE_ASIC,
    BWG_STOP_STAGE_FAN_FULL,
    BWG_STOP_STAGE_COOLING_PROOF,
    BWG_STOP_STAGE_FAN_PAUSED,
} bwg_stop_stage_t;

/**
 * The contract's ordered stop, for the power task to run once authority has closed: frequency and
 * nonce, reset, core voltage, ASIC power, fan 100%, proven cooling, fan 30%. Returns after cooling
 * is proven, however long that takes, because the device stays failed closed until then.
 */
void bwg_stop_ordered(GlobalState *GLOBAL_STATE);

/** Clears the fan hold and stage when mining restarts. */
void bwg_stop_reset(void);

/** The fan duty the stop holds, in percent, or a negative number when it holds none. */
int bwg_stop_fan_override(void);

/** Changes the hold, for the fan's qualification with the ASIC already down. */
void bwg_stop_hold_fan(int percent);

/** The fan task reports the hold it applied, once it has read the fan speed that followed. */
void bwg_stop_note_fan_sample(int applied_hold);
int bwg_stop_fan_applied(void);
uint32_t bwg_stop_fan_samples(void);

/** The board's own temperature sensor, which still reads with the ASIC down. Negative when it has none. */
float bwg_stop_board_sensor_celsius(GlobalState *GLOBAL_STATE);

/** The power task is busy carrying the stop out, so the fan task keeps the Gate's readings fresh until it ends. */
void bwg_stop_sample_while_stopping(GlobalState *GLOBAL_STATE);

bwg_stop_stage_t bwg_stop_stage(void);
const char *bwg_stop_stage_label(bwg_stop_stage_t stage);

#endif // BWG_STOP_H
