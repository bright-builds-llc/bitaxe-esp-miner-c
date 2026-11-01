#include "bwg_local_guard.h"

#include <stdatomic.h>

#include "bwg_mining.h"
#include "bwg_restart_guard.h"
#include "bwg_revocation.h"
#include "connect.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "stratum_task.h"

static const char *TAG = "bwg_local_guard";

// Only the power task touches the guard. Other tasks read what it published.
static bwg_restart_guard_t s_guard;
static bool s_was_paused;
static uint32_t s_reported_failures;
static _Atomic bool s_backing_off;
static _Atomic bool s_holding;

static uint64_t now_ms(void)
{
    return (uint64_t) (esp_timer_get_time() / 1000);
}

static bool authority_is_local(void)
{
    return bwg_revocation_snapshot().state == BWG_AUTHORITY_LOCAL;
}

static void publish(GlobalState *GLOBAL_STATE, uint64_t now)
{
    const bwg_restart_verdict_t verdict = bwg_restart_guard_verdict(&s_guard, now);
    const bool local = authority_is_local();
    if (s_guard.consecutive_failures != s_reported_failures) {
        s_reported_failures = s_guard.consecutive_failures;
        ESP_LOGW(TAG, "Failed runs in a row: %lu, %s", (unsigned long) s_reported_failures,
                 verdict == BWG_RESTART_HELD ? "holding until resumed" : "backing off");
    }
    atomic_store(&s_backing_off, local && verdict == BWG_RESTART_BACKOFF);
    atomic_store(&s_holding, verdict == BWG_RESTART_HELD);
    // A lease that runs from a hold brings the pause back when it ends, as it does for any pause.
    if (local && verdict == BWG_RESTART_HELD && !GLOBAL_STATE->SYSTEM_MODULE.mining_paused) {
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = true;
        s_was_paused = true;
    }
}

void bwg_local_guard_tick(GlobalState *GLOBAL_STATE, bool asic_stopped)
{
    if (GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
        return;
    }
    SystemModule *system = &GLOBAL_STATE->SYSTEM_MODULE;
    const uint64_t now = now_ms();
    const bool paused = bwg_mining_operator_paused(GLOBAL_STATE);
    if (s_was_paused && !paused && s_guard.consecutive_failures > 0) {
        ESP_LOGI(TAG, "Operator resumed, starting over");
        bwg_restart_guard_resume(&s_guard);
        // The stratum task can't clear these itself while the pause keeps it off the pool.
        system->pools_unavailable = false;
        stratum_request_reconnect();
    }
    s_was_paused = paused;

    const bool running = !asic_stopped && authority_is_local();
    const bool hashing = system->work_received > 0 && GLOBAL_STATE->ASIC_initalized;
    bwg_restart_guard_observe(&s_guard, running, hashing, system->pools_unavailable && wifi_is_connected(), now);
    publish(GLOBAL_STATE, now);
}

bool bwg_local_guard_note_failed_start(GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE->SELF_TEST_MODULE.is_active || !authority_is_local()) {
        return false;
    }
    const uint64_t now = now_ms();
    bwg_restart_guard_note_failed_start(&s_guard, now);
    publish(GLOBAL_STATE, now);
    return true;
}

bool bwg_local_guard_backing_off(void)
{
    return atomic_load(&s_backing_off);
}

bool bwg_local_guard_holds_pause(void)
{
    return atomic_load(&s_holding);
}
