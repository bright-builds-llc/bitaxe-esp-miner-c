#include "bwg_stop.h"

#include "bwg_mining.h"

#include <stdatomic.h>

#include "asic.h"
#include "asic_reset.h"
#include "bwg_limits.h"
#include "bwg_mining_rules.h"
#include "driver/uart.h"
#include "EMC2101.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "stratum_task.h"
#include "thermal.h"
#include "vcore.h"

#define FAN_FULL_PERCENT 100
#define FAN_PAUSED_PERCENT 30
#define NO_FAN_HOLD (-1)
#define COOLING_POLL_MS 1000
// The fan task passes every 100 ms, and a sample older than a second reads as stale.
#define SAMPLE_EVERY_FAN_PASSES 5u
#define COOLING_OVERDUE_MS 120000

static const char *TAG = "bwg_stop";

static _Atomic int s_stage = BWG_STOP_STAGE_NOT_STARTED;
static _Atomic int s_fan_hold = NO_FAN_HOLD;
static _Atomic int s_fan_applied = NO_FAN_HOLD;
static _Atomic uint32_t s_fan_samples;

static void enter(bwg_stop_stage_t stage)
{
    atomic_store(&s_stage, stage);
}

static bool limits_of(GlobalState *GLOBAL_STATE, bwg_board_limits_t *limits)
{
    return bwg_board_limits_for(GLOBAL_STATE->DEVICE_CONFIG.board_version,
                                GLOBAL_STATE->DEVICE_CONFIG.power_consumption_target, limits);
}

float bwg_stop_board_sensor_celsius(GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE->DEVICE_CONFIG.EMC2101 && GLOBAL_STATE->DEVICE_CONFIG.emc_internal_temp) {
        return EMC2101_get_internal_temp();
    }
    return -1.0f;
}

static void prove_cooling(GlobalState *GLOBAL_STATE)
{
    PowerManagementModule *power = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    bwg_board_limits_t limits;
    // A board outside the closed set can only get here from a local handover, which it never starts,
    // so failing to name its limits leaves the stop failed closed.
    const bool has_limits = limits_of(GLOBAL_STATE, &limits);
    const int64_t started_us = esp_timer_get_time();
    bool reported_overdue = false;
    // The Gate judges a reading taken after the proof.
    uint32_t cool_readings = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(COOLING_POLL_MS));
        const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        const bwg_cooling_sample_t sample = {
            .chip_celsius = bwg_stop_board_sensor_celsius(GLOBAL_STATE),
            .regulator_celsius = Power_get_vreg_temp(GLOBAL_STATE),
            .fan_rpm = power->fan_rpm,
            .cooling_elapsed_ms = elapsed_ms,
        };
        power->chip_temp_avg = sample.chip_celsius;
        power->vr_temp = sample.regulator_celsius;
        cool_readings = bwg_cool_streak_next(cool_readings, has_limits && bwg_limits_cooling_proven(&limits, &sample));
        if (bwg_cool_streak_proves_cooling(cool_readings)) {
            ESP_LOGI(TAG, "Cooling proven after %lu ms", (unsigned long)elapsed_ms);
            return;
        }
        if (elapsed_ms >= COOLING_OVERDUE_MS && !reported_overdue) {
            reported_overdue = true;
            ESP_LOGE(TAG, "Cooling not proven after %d ms, staying stopped", COOLING_OVERDUE_MS);
        }
    }
}

void bwg_stop_ordered(GlobalState *GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Stopping mining");
    enter(BWG_STOP_STAGE_STOP_DISPATCH);

    enter(BWG_STOP_STAGE_REDUCE_FREQUENCY);
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = 50;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = 0;
    ASIC_set_frequency(GLOBAL_STATE);
    ASIC_set_nonce_space(GLOBAL_STATE);

    enter(BWG_STOP_STAGE_HOLD_RESET);
    asic_hold_reset_low();
    GLOBAL_STATE->ASIC_initalized = false;
    // Give tasks time to finish a UART operation already under way.
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_flush(UART_NUM_1);

    enter(BWG_STOP_STAGE_DISABLE_CORE_VOLTAGE);
    // One call on this hardware: it drops the ASIC enable pin, then the regulator's output.
    VCORE_set_voltage(GLOBAL_STATE, 0.0f);
    enter(BWG_STOP_STAGE_DISABLE_ASIC);

    enter(BWG_STOP_STAGE_FAN_FULL);
    atomic_store(&s_fan_hold, FAN_FULL_PERCENT);

    // A local handover's stop has no lease to report on.
    if (bwg_mining_lease_pool_active()) {
        bwg_mining_latch_shares(GLOBAL_STATE);
    }
    // After the power and the fan, because a stalled send can hold the mutex this takes. Before the
    // cooling proof, which has no upper limit, because a stopped device has no business on a pool.
    stratum_request_reconnect();

    enter(BWG_STOP_STAGE_COOLING_PROOF);
    prove_cooling(GLOBAL_STATE);

    enter(BWG_STOP_STAGE_FAN_PAUSED);
    atomic_store(&s_fan_hold, FAN_PAUSED_PERCENT);
    ESP_LOGI(TAG, "Mining stopped");
}

void bwg_stop_reset(void)
{
    atomic_store(&s_fan_hold, NO_FAN_HOLD);
    enter(BWG_STOP_STAGE_NOT_STARTED);
}

int bwg_stop_fan_override(void)
{
    return atomic_load(&s_fan_hold);
}

void bwg_stop_hold_fan(int percent)
{
    atomic_store(&s_fan_hold, percent);
}

void bwg_stop_note_fan_sample(int applied_hold)
{
    atomic_store(&s_fan_applied, applied_hold);
    atomic_fetch_add(&s_fan_samples, 1);
}

void bwg_stop_sample_while_stopping(GlobalState *GLOBAL_STATE)
{
    static uint32_t s_passes;
    if (bwg_stop_stage() == BWG_STOP_STAGE_NOT_STARTED || s_passes++ % SAMPLE_EVERY_FAN_PASSES != 0) {
        return;
    }
    float power_watts = 0.0f;
    float current_amps = 0.0f;
    Power_get_output(GLOBAL_STATE, &power_watts, &current_amps);
    const bwg_safety_sample_t sample = {
        .input_volts = Power_get_input_voltage(GLOBAL_STATE) / 1000.0f,
        .power_watts = power_watts,
        .chip_celsius = GLOBAL_STATE->ASIC_initalized ? Thermal_get_chip_temp(GLOBAL_STATE) : bwg_stop_board_sensor_celsius(GLOBAL_STATE),
        .regulator_celsius = Power_get_vreg_temp(GLOBAL_STATE),
        .fan_rpm = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_rpm,
    };
    bwg_mining_note_sample(&sample);
}

int bwg_stop_fan_applied(void)
{
    return atomic_load(&s_fan_applied);
}

uint32_t bwg_stop_fan_samples(void)
{
    return atomic_load(&s_fan_samples);
}

bwg_stop_stage_t bwg_stop_stage(void)
{
    return (bwg_stop_stage_t)atomic_load(&s_stage);
}

const char *bwg_stop_stage_label(bwg_stop_stage_t stage)
{
    switch (stage) {
    case BWG_STOP_STAGE_NOT_STARTED: return "not_started";
    case BWG_STOP_STAGE_STOP_DISPATCH: return "stop_dispatch";
    case BWG_STOP_STAGE_REDUCE_FREQUENCY: return "reduce_frequency_and_reset_nonce";
    case BWG_STOP_STAGE_HOLD_RESET: return "hold_reset_low";
    case BWG_STOP_STAGE_DISABLE_CORE_VOLTAGE: return "disable_core_voltage";
    case BWG_STOP_STAGE_DISABLE_ASIC: return "disable_asic";
    case BWG_STOP_STAGE_FAN_FULL: return "fan_full";
    case BWG_STOP_STAGE_COOLING_PROOF: return "cooling_proof";
    case BWG_STOP_STAGE_FAN_PAUSED: return "fan_paused";
    }
    return "not_started";
}
