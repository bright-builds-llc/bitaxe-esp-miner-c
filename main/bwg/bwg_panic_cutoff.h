#ifndef BWG_PANIC_CUTOFF_H
#define BWG_PANIC_CUTOFF_H

#include <stdbool.h>

#include "device_config.h"

/** Names this board's enable pin for the fatal path. Until then a panic can only cut the reset line. */
void bwg_panic_cutoff_configure(const DeviceConfig *config);

/** Reads the hardware: the board's ASIC pins are driven as outputs and their latches hold it off. */
bool bwg_panic_cutoff_outputs_off(void);

/** Marks the next panic as a requested self-test in the receipt a core dump carries. */
void bwg_panic_cutoff_mark_self_test(void);

#endif // BWG_PANIC_CUTOFF_H
