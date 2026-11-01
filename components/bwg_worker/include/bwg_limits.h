#ifndef BWG_LIMITS_H
#define BWG_LIMITS_H

#include <stdbool.h>
#include <stdint.h>

#define BWG_SAFE_SAMPLE_MAX_AGE_MS 1000

/** A board's lease limits (ADR-0037, ADR-0041 in the Rust firmware). They apply only during a lease. */
typedef struct {
    uint16_t frequency_min_mhz;
    uint16_t frequency_max_mhz;
    uint16_t core_mv_min;
    uint16_t core_mv_max;
    float power_max_watts;
    // Zero on a board with no regulator temperature to check.
    float regulator_max_celsius;
    // Whether the chip reading stays valid with the ASIC down (the 205's EMC2101 internal sensor).
    bool board_sensor;
} bwg_board_limits_t;

typedef struct {
    float input_volts;
    float power_watts;
    float chip_celsius;
    float regulator_celsius;
    uint16_t fan_rpm;
} bwg_safety_sample_t;

/** The fact that made a sample unsafe, in the order `bwg_limits_first_unsafe_fact` checks them. */
typedef enum {
    BWG_SAFETY_FACT_NONE,
    BWG_SAFETY_FACT_BUS_VOLTAGE,
    BWG_SAFETY_FACT_POWER,
    BWG_SAFETY_FACT_CHIP_TEMPERATURE,
    BWG_SAFETY_FACT_VR_TEMPERATURE,
    BWG_SAFETY_FACT_FAN_RPM,
} bwg_safety_fact_t;

typedef struct {
    float chip_celsius;
    float regulator_celsius;
    uint16_t fan_rpm;
    uint32_t cooling_elapsed_ms;
} bwg_cooling_sample_t;

/** Fills `limits` for the Gate's closed board set. False for any other board, which gets no lease. */
bool bwg_board_limits_for(const char *board_version, uint16_t power_target_watts, bwg_board_limits_t *limits);

/** Whether saved settings lie in the board's catalog range. Out of range refuses and never clamps. */
bool bwg_limits_profile_admissible(const bwg_board_limits_t *limits, float frequency_mhz, uint16_t core_mv);

/** Whether one sample may keep a lease running: 4.5 to 5.5 V, power, chip below 75 C, regulator, a turning fan. */
bool bwg_limits_sample_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample);

/** The first fact that keeps a sample from being safe, or `BWG_SAFETY_FACT_NONE` for a safe one. */
bwg_safety_fact_t bwg_limits_first_unsafe_fact(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample);

/** The Gate's name for a fact, as Rust's safety verdict names it. */
const char *bwg_limits_fact_label(bwg_safety_fact_t fact);

/** That fact's reading in thousandths. False for none, or a reading with no number to report. */
bool bwg_limits_fact_value_milli(const bwg_safety_sample_t *sample, bwg_safety_fact_t fact, int32_t *value_milli);

/** Whether a stop has proved cooling: 45 C on a board sensor, otherwise 95 C regulator and 30 s unpowered. */
bool bwg_limits_cooling_proven(const bwg_board_limits_t *limits, const bwg_cooling_sample_t *sample);

/**
 * Whether an idle board with its ASIC down may have its fan lowered. A board sensor must read cool;
 * a board without one has a chip reading only when powered, so it must read unpowered, with a cool
 * regulator. Unlike the stop's proof, this has no term for time.
 */
bool bwg_limits_baseline_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample);

/** Whether an idle board may have its fan proven: mining-safe with a board sensor, otherwise at baseline. */
bool bwg_limits_fan_proof_safe(const bwg_board_limits_t *limits, const bwg_safety_sample_t *sample);

#endif // BWG_LIMITS_H
