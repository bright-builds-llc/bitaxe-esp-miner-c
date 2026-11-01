#include "bwg_cutoff.h"

bwg_cutoff_plan_t bwg_cutoff_plan(uint32_t reset_pin, bool has_enable_pin, uint32_t enable_pin,
                                  bool enable_active_high)
{
    bwg_cutoff_plan_t plan = {.set_mask = 0, .clear_mask = 1u << reset_pin};
    if (!has_enable_pin) {
        return plan;
    }
    if (enable_active_high) {
        plan.clear_mask |= 1u << enable_pin;
    } else {
        plan.set_mask |= 1u << enable_pin;
    }
    return plan;
}

bool bwg_cutoff_outputs_off(bwg_cutoff_plan_t plan, uint32_t output_enable_reg, uint32_t output_reg)
{
    const uint32_t pins = plan.set_mask | plan.clear_mask;
    return (output_enable_reg & pins) == pins && (output_reg & pins) == plan.set_mask;
}
