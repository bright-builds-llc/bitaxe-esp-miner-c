#ifndef BWG_CUTOFF_H
#define BWG_CUTOFF_H

#include <stdbool.h>
#include <stdint.h>

/** Which output latches a fatal path sets and clears to take a board's ASIC off. */
typedef struct {
    uint32_t set_mask;
    uint32_t clear_mask;
} bwg_cutoff_plan_t;

/** A board with no enable pin has only its reset line to cut; its regulator stays up through the reboot. */
bwg_cutoff_plan_t bwg_cutoff_plan(uint32_t reset_pin, bool has_enable_pin, uint32_t enable_pin,
                                  bool enable_active_high);

/** Whether the plan's pins are driven as outputs and their latches already hold it. Other pins don't count. */
bool bwg_cutoff_outputs_off(bwg_cutoff_plan_t plan, uint32_t output_enable_reg, uint32_t output_reg);

#endif // BWG_CUTOFF_H
