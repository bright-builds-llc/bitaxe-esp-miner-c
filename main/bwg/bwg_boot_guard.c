#include "bwg_boot_guard.h"

#include "nvs_config.h"

#include "bwg_crash_streak.h"
#include "bwg_mining.h"
#include "bwg_store.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "bwg_boot_guard";

// A failed save waits this long before the next try.
#define SAVE_RETRY_MS 30000u

static uint16_t s_streak;
static bool s_tripped;
static bool s_was_paused;
static bool s_clear_pending;
static uint64_t s_retry_at_ms;

static bool reset_was_abnormal(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

void bwg_boot_guard_apply(GlobalState *GLOBAL_STATE)
{
    // An operator's pause ends with the boot; this one is the owner's standing choice.
    if (!GLOBAL_STATE->SELF_TEST_MODULE.is_active && !nvs_config_get_bool(NVS_CONFIG_MINE_ON_BOOT)) {
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = true;
    }
    uint16_t previous = 0;
    if (!bwg_store_crash_streak_get(&previous)) {
        ESP_LOGW(TAG, "Streak unreadable, counting from zero");
    }
    s_streak = bwg_crash_streak_next(previous, reset_was_abnormal());
    if (s_streak != previous && !bwg_store_crash_streak_set(s_streak)) {
        ESP_LOGE(TAG, "Streak not saved");
    }
    if (GLOBAL_STATE->SELF_TEST_MODULE.is_active || !bwg_crash_streak_starts_paused(s_streak)) {
        return;
    }
    ESP_LOGW(TAG, "Boot paused after %u abnormal resets in a row", (unsigned) s_streak);
    GLOBAL_STATE->SYSTEM_MODULE.mining_paused = true;
    s_tripped = true;
    s_was_paused = true;
}

void bwg_boot_guard_tick(const GlobalState *GLOBAL_STATE)
{
    const uint64_t now_ms = (uint64_t) (esp_timer_get_time() / 1000);
    const bool paused = bwg_mining_operator_paused(GLOBAL_STATE);
    const bool resumed = s_was_paused && !paused;
    s_was_paused = paused;
    if (resumed) {
        s_tripped = false;
    }
    if (s_streak == 0) {
        return;
    }
    if (resumed || bwg_crash_streak_boot_is_stable(now_ms)) {
        s_clear_pending = true;
    }
    if (!s_clear_pending || now_ms < s_retry_at_ms) {
        return;
    }
    if (!bwg_store_crash_streak_set(0)) {
        ESP_LOGE(TAG, "Streak not cleared");
        s_retry_at_ms = now_ms + SAVE_RETRY_MS;
        return;
    }
    ESP_LOGI(TAG, "Streak cleared");
    s_streak = 0;
    s_clear_pending = false;
}

bool bwg_boot_guard_holds_pause(void)
{
    return s_tripped;
}
