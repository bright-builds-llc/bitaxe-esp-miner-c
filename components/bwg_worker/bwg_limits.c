#include "bwg_limits.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define INPUT_VOLTS_MIN 4.5f
#define INPUT_VOLTS_MAX 5.5f
#define CHIP_CELSIUS_MAX 75.0f
#define POWER_MARGIN_WATTS 3.0f
#define BOARD_SENSOR_COOL_CELSIUS 45.0f
#define REGULATOR_COOL_CELSIUS 95.0f
#define REGULATOR_MAX_CELSIUS 105.0f
#define UNPOWERED_COOLING_MS 30000u

bool bwg_board_limits_for(const char *board_version, uint16_t power_target_watts, bwg_board_limits_t *limits)
{
    if (board_version == NULL || limits == NULL) {
        return false;
    }
    if (strcmp(board_version, "205") == 0) {
        *limits = (bwg_board_limits_t) {
            .frequency_min_mhz = 400,
            .frequency_max_mhz = 575,
            .core_mv_min = 1100,
            .core_mv_max = 1300,
            .power_max_watts = (float)power_target_watts + POWER_MARGIN_WATTS,
            .board_sensor = true,
        };
        return true;
    }
    if (strcmp(board_version, "601") == 0) {
        *limits = (bwg_board_limits_t) {
            .frequency_min_mhz = 400,
            .frequency_max_mhz = 625,
            .core_mv_min = 1000,
            .core_mv_max = 1250,
            .power_max_watts = (float)power_target_watts + POWER_MARGIN_WATTS,
            .regulator_max_celsius = REGULATOR_MAX_CELSIUS,
        };
        return true;
    }
    return false;
}

bool bwg_limits_profile_admissible(const bwg_board_limits_t *limits, float frequency_mhz, uint16_t core_mv)
{
    return frequency_mhz >= limits->frequency_min_mhz && frequency_mhz <= limits->frequency_max_mhz &&
           core_mv >= limits->core_mv_min && core_mv <= limits->core_mv_max;
}

bwg_safety_fact_t bwg_limits_first_unsafe_fact(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample)
{
    // A failed sensor read arrives as a negative number or NaN, so "below the limit" alone isn't enough.
    if (!(sample->input_volts >= INPUT_VOLTS_MIN && sample->input_volts <= INPUT_VOLTS_MAX)) {
        return BWG_SAFETY_FACT_BUS_VOLTAGE;
    }
    if (!(sample->power_watts >= 0.0f && sample->power_watts <= limits->power_max_watts)) {
        return BWG_SAFETY_FACT_POWER;
    }
    if (!(sample->chip_celsius > 0.0f && sample->chip_celsius < CHIP_CELSIUS_MAX)) {
        return BWG_SAFETY_FACT_CHIP_TEMPERATURE;
    }
    if (limits->regulator_max_celsius > 0.0f &&
        !(sample->regulator_celsius > 0.0f && sample->regulator_celsius <= limits->regulator_max_celsius)) {
        return BWG_SAFETY_FACT_VR_TEMPERATURE;
    }
    return sample->fan_rpm > 0 ? BWG_SAFETY_FACT_NONE : BWG_SAFETY_FACT_FAN_RPM;
}

bool bwg_limits_sample_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample)
{
    return bwg_limits_first_unsafe_fact(limits, sample) == BWG_SAFETY_FACT_NONE;
}

const char *bwg_limits_fact_label(bwg_safety_fact_t fact)
{
    switch (fact) {
    case BWG_SAFETY_FACT_BUS_VOLTAGE: return "bus_voltage";
    case BWG_SAFETY_FACT_POWER: return "power";
    case BWG_SAFETY_FACT_CHIP_TEMPERATURE: return "chip_temperature";
    case BWG_SAFETY_FACT_VR_TEMPERATURE: return "vr_temperature";
    case BWG_SAFETY_FACT_FAN_RPM: return "fan_rpm";
    case BWG_SAFETY_FACT_NONE: break;
    }
    return "none";
}

bool bwg_limits_fact_value_milli(const bwg_safety_sample_t *sample, bwg_safety_fact_t fact, int32_t *value_milli)
{
    float value;
    switch (fact) {
    case BWG_SAFETY_FACT_BUS_VOLTAGE: value = sample->input_volts; break;
    case BWG_SAFETY_FACT_POWER: value = sample->power_watts; break;
    case BWG_SAFETY_FACT_CHIP_TEMPERATURE: value = sample->chip_celsius; break;
    case BWG_SAFETY_FACT_VR_TEMPERATURE: value = sample->regulator_celsius; break;
    case BWG_SAFETY_FACT_FAN_RPM: value = (float)sample->fan_rpm; break;
    default: return false;
    }
    // Past this a thousandth no longer fits, and no real reading gets there.
    if (!isfinite(value) || fabsf(value) > 2000000.0f) {
        return false;
    }
    *value_milli = (int32_t)lroundf(value * 1000.0f);
    return true;
}

static bool supply_and_fan_are_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample)
{
    return sample->input_volts >= INPUT_VOLTS_MIN && sample->input_volts <= INPUT_VOLTS_MAX && sample->power_watts >= 0.0f &&
           sample->power_watts <= limits->power_max_watts && sample->fan_rpm > 0;
}

bool bwg_limits_baseline_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample)
{
    if (!supply_and_fan_are_safe(limits, sample)) {
        return false;
    }
    if (limits->board_sensor) {
        return sample->chip_celsius > 0.0f && sample->chip_celsius <= BOARD_SENSOR_COOL_CELSIUS;
    }
    return sample->chip_celsius < 0.0f && sample->regulator_celsius > 0.0f &&
           sample->regulator_celsius <= REGULATOR_COOL_CELSIUS;
}

bool bwg_limits_fan_proof_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample)
{
    return limits->board_sensor ? bwg_limits_sample_safe(limits, sample) : bwg_limits_baseline_safe(limits, sample);
}

bool bwg_limits_cooling_proven(const bwg_board_limits_t *limits, const bwg_cooling_sample_t *sample)
{
    if (sample->fan_rpm == 0) {
        return false;
    }
    if (limits->board_sensor) {
        return sample->chip_celsius > 0.0f && sample->chip_celsius <= BOARD_SENSOR_COOL_CELSIUS;
    }
    return sample->cooling_elapsed_ms >= UNPOWERED_COOLING_MS &&
           sample->regulator_celsius > 0.0f && sample->regulator_celsius <= REGULATOR_COOL_CELSIUS;
}
