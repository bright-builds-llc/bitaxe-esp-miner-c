#include "bwg_cooling.h"

#include <stdatomic.h>

#include "bwg_limits.h"
#include "bwg_mining.h"
#include "bwg_owner_watchdog.h"
#include "bwg_revocation.h"
#include "bwg_stop.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power/power.h"

#define FAN_STEP_TIMEOUT_MS 3000
#define FAN_STEP_POLL_MS 50
#define FULL_PERCENT 100
#define BASELINE_PERCENT 30

static const char *TAG = "bwg_cooling";

// Owner task only: a lease-free fan effect this module must return before the stop can be acknowledged.
static bool s_holds_fan;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// The chip's own reading is -1 while it is down, so the board sensor stands in, as it does for the stop.
static bwg_safety_sample_t idle_sample(GlobalState *GLOBAL_STATE)
{
    uint64_t age_ms;
    bwg_safety_sample_t sample = {0};
    bwg_mining_latest_sample(&sample, &age_ms);
    sample.chip_celsius = bwg_stop_board_sensor_celsius(GLOBAL_STATE);
    sample.regulator_celsius = Power_get_vreg_temp(GLOBAL_STATE);
    sample.fan_rpm = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_rpm;
    return sample;
}

static bool hardware_is_idle(void)
{
    return bwg_revocation_snapshot().state == BWG_AUTHORITY_STOPPED;
}

static bool limits_of(GlobalState *GLOBAL_STATE, bwg_board_limits_t *limits)
{
    return bwg_board_limits_for(GLOBAL_STATE->DEVICE_CONFIG.board_version,
                                GLOBAL_STATE->DEVICE_CONFIG.power_consumption_target, limits);
}

bool bwg_cooling_prove(GlobalState *GLOBAL_STATE, uint32_t *fan_rpm)
{
    bwg_board_limits_t limits;
    if (!hardware_is_idle() || !limits_of(GLOBAL_STATE, &limits)) {
        return false;
    }
    bwg_safety_sample_t sample = idle_sample(GLOBAL_STATE);
    if (!bwg_limits_fan_proof_safe(&limits, &sample)) {
        ESP_LOGW(TAG, "Fan proof refused: the board isn't idle and safe");
        return false;
    }

    const uint32_t samples_before = bwg_stop_fan_samples();
    bwg_stop_hold_fan(FULL_PERCENT);
    s_holds_fan = true;

    const int64_t deadline_ms = now_ms() + FAN_STEP_TIMEOUT_MS;
    while (now_ms() < deadline_ms) {
        bwg_owner_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(FAN_STEP_POLL_MS));
        sample = idle_sample(GLOBAL_STATE);
        if (!hardware_is_idle() || !bwg_limits_fan_proof_safe(&limits, &sample)) {
            break;
        }
        // Two samples past the command, since the one that applied it may count the old duty's pulses.
        if (bwg_stop_fan_applied() == FULL_PERCENT && bwg_stop_fan_samples() - samples_before >= 2 && sample.fan_rpm > 0) {
            *fan_rpm = sample.fan_rpm;
            return true;
        }
    }
    ESP_LOGW(TAG, "Fan proof failed");
    return false;
}

bool bwg_cooling_restore(GlobalState *GLOBAL_STATE)
{
    bwg_board_limits_t limits;
    if (!hardware_is_idle() || !limits_of(GLOBAL_STATE, &limits)) {
        return false;
    }
    const int64_t deadline_ms = now_ms() + FAN_STEP_TIMEOUT_MS;
    while (now_ms() < deadline_ms) {
        const bwg_safety_sample_t sample = idle_sample(GLOBAL_STATE);
        if (bwg_limits_baseline_safe(&limits, &sample)) {
            bwg_stop_hold_fan(BASELINE_PERCENT);
        }
        if (bwg_stop_fan_applied() == BASELINE_PERCENT && bwg_stop_fan_override() == BASELINE_PERCENT) {
            s_holds_fan = false;
            return true;
        }
        bwg_owner_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(FAN_STEP_POLL_MS));
    }
    ESP_LOGW(TAG, "Fan baseline not restored");
    return false;
}

bool bwg_cooling_release(GlobalState *GLOBAL_STATE)
{
    return !s_holds_fan || bwg_cooling_restore(GLOBAL_STATE);
}
