#include "bwg_mining.h"
#include "bwg_progress.h"

#include "bwg_boot_guard.h"
#include "bwg_local_guard.h"

#include <stdatomic.h>
#include <string.h>

#include "bwg_limits.h"
#include "bwg_owner_watchdog.h"
#include "bwg_preparation.h"
#include "bwg_stop.h"
#include "bwg_unsafe_detail.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "sodium.h"
#include "stratum_task.h"
#include "sv2_protocol.h"

// The pool client keeps 255 bytes of each. A longer value would reach the pool changed, so it's refused.
#define LEASE_POOL_FIELD_BYTES 256
#define START_WAIT_MS 25000
#define STOP_WAIT_MS 140000
#define WAIT_POLL_MS 50

static const char *TAG = "bwg_mining";

// The tags that log pool endpoints, users and passwords. They stay silent during a lease.
static const char *const POOL_LOG_TAGS[] = {"stratum_api", "stratum_socket", "stratum_task", "stratum_v1", "stratum_v2", "sv2_noise", "sv2_protocol"};

static GlobalState *s_global_state;
static bool s_return_to_pause;
static PoolConfig s_lease_pool;
static char s_lease_host[LEASE_POOL_FIELD_BYTES];
static char s_lease_user[LEASE_POOL_FIELD_BYTES];
static char s_lease_pass[LEASE_POOL_FIELD_BYTES];
static uint8_t s_lease_authority_key[BWG_STRATUM_V2_AUTHORITY_KEY_BYTES];
static _Atomic bool s_lease_pool_active;
static uint64_t s_last_safe_ms;
// Written by the power task, read by the owner task for evidence. A torn read is one odd status line.
static bwg_safety_sample_t s_latest_sample;
static _Atomic uint64_t s_latest_sample_ms;
static _Atomic uint32_t s_lease_accepted;
static _Atomic uint32_t s_lease_rejected;
// The session counts last latched. A reconnect zeroes them, and the lease's own counts must not go back.
static uint32_t s_session_accepted;
static uint32_t s_session_rejected;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void set_pool_logging(esp_log_level_t level)
{
    for (size_t i = 0; i < sizeof(POOL_LOG_TAGS) / sizeof(POOL_LOG_TAGS[0]); i++) {
        esp_log_level_set(POOL_LOG_TAGS[i], level);
    }
}

void bwg_mining_quiet_pool_logs(bool quiet)
{
    set_pool_logging(quiet ? ESP_LOG_NONE : ESP_LOG_INFO);
}

static bool copy_field(char *destination, const char *source)
{
    const size_t length = strlen(source);
    if (length == 0 || length >= LEASE_POOL_FIELD_BYTES) {
        return false;
    }
    memcpy(destination, source, length + 1);
    return true;
}

static void wipe_lease_pool(void)
{
    atomic_store(&s_lease_pool_active, false);
    sodium_memzero(s_lease_host, sizeof(s_lease_host));
    sodium_memzero(s_lease_user, sizeof(s_lease_user));
    sodium_memzero(s_lease_pass, sizeof(s_lease_pass));
    sodium_memzero(s_lease_authority_key, sizeof(s_lease_authority_key));
    set_pool_logging(ESP_LOG_INFO);
}

static bool install_lease_pool(const bwg_lease_pool_t *pool)
{
    const bool is_v2 = pool->maybe_authority_key != NULL;
    if (!copy_field(s_lease_host, pool->host) || !copy_field(s_lease_user, pool->username) ||
        (!is_v2 && !copy_field(s_lease_pass, pool->password))) {
        wipe_lease_pool();
        return false;
    }
    if (is_v2) {
        memcpy(s_lease_authority_key, pool->maybe_authority_key, sizeof(s_lease_authority_key));
    }
    s_lease_pool = (PoolConfig) {
        .url = s_lease_host,
        .port = pool->port,
        .user = s_lease_user,
        .pass = s_lease_pass,
        .protocol = is_v2 ? STRATUM_PROTOCOL_V2 : STRATUM_PROTOCOL_V1,
        .difficulty = pool->suggested_difficulty,
        .sv2_channel_type = SV2_CHANNEL_STANDARD,
        .sv2_require_auth = is_v2,
    };
    set_pool_logging(ESP_LOG_NONE);
    atomic_store(&s_lease_pool_active, true);
    return true;
}

static bool settings_admissible(GlobalState *GLOBAL_STATE)
{
    bwg_board_limits_t limits;
    if (!bwg_board_limits_for(GLOBAL_STATE->DEVICE_CONFIG.board_version,
                              GLOBAL_STATE->DEVICE_CONFIG.power_consumption_target, &limits)) {
        return false;
    }
    return bwg_limits_profile_admissible(&limits, nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY),
                                         nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE));
}

static bool device_may_start(GlobalState *GLOBAL_STATE)
{
    const SystemModule *system = &GLOBAL_STATE->SYSTEM_MODULE;
    return !system->hardware_fault && !system->overheat_mode &&
           settings_admissible(GLOBAL_STATE);
}

// Pool failures belong to the pools that had them. A lease or a return to local mining starts over,
// and the stratum task can't clear them itself while authority keeps it off the pool.
static void forget_pool_failures(GlobalState *GLOBAL_STATE)
{
    GLOBAL_STATE->SYSTEM_MODULE.pools_unavailable = false;
    stratum_request_reconnect();
}

void bwg_mining_init(GlobalState *GLOBAL_STATE)
{
    s_global_state = GLOBAL_STATE;
}

bool bwg_mining_may_start(void)
{
    return s_global_state != NULL && device_may_start(s_global_state);
}

bool bwg_mining_start(const bwg_lease_pool_t *pool, uint32_t duration_ms, uint32_t active_limit_ms)
{
    GlobalState *GLOBAL_STATE = s_global_state;
    bwg_preparation_started(BWG_PREP_STEP_ADMISSION);
    if (GLOBAL_STATE == NULL || !device_may_start(GLOBAL_STATE)) {
        ESP_LOGW(TAG, "Device refuses the lease");
        bwg_preparation_failed(BWG_PREP_STEP_ADMISSION, BWG_PREP_FAILURE_UNSUPPORTED_PROFILE);
        return false;
    }
    if (!install_lease_pool(pool)) {
        bwg_preparation_failed(BWG_PREP_STEP_ADMISSION, BWG_PREP_FAILURE_UNSUPPORTED_PROFILE);
        return false;
    }
    bwg_preparation_completed(BWG_PREP_STEP_ADMISSION);

    SystemModule *system = &GLOBAL_STATE->SYSTEM_MODULE;
    system->is_using_fallback = false;
    system->work_received = 0;
    forget_pool_failures(GLOBAL_STATE);
    SYSTEM_reset_pool_session(GLOBAL_STATE);
    atomic_store(&s_lease_accepted, 0);
    atomic_store(&s_lease_rejected, 0);
    s_session_accepted = 0;
    s_session_rejected = 0;
    s_last_safe_ms = now_ms();
    bwg_stop_reset();
    bwg_progress_begin();

    // A lease outranks an operator's pause, which returns with local mining. A pause during the lease ends it.
    const bool was_paused = system->mining_paused;
    s_return_to_pause = s_return_to_pause || was_paused;
    system->mining_paused = false;
    if (!bwg_revocation_begin_limited_lease(duration_ms, active_limit_ms)) {
        system->mining_paused = was_paused;
        wipe_lease_pool();
        return false;
    }

    const uint64_t deadline_ms = now_ms() + START_WAIT_MS;
    while (now_ms() < deadline_ms) {
        if (bwg_revocation_stop_wanted()) {
            bwg_preparation_failed(BWG_PREP_STEP_READY, BWG_PREP_FAILURE_CANCELLED);
            return false;
        }
        // The Gate reads the board's safety from the reply on, and the power task resumes sampling only after init.
        if (GLOBAL_STATE->ASIC_initalized && system->work_received > 0 && now_ms() - atomic_load(&s_latest_sample_ms) <= BWG_SAMPLE_FRESH_MS) {
            ESP_LOGI(TAG, "Lease active");
            bwg_preparation_completed(BWG_PREP_STEP_READY);
            return true;
        }
        bwg_owner_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
    }
    ESP_LOGW(TAG, "Lease did not reach work within %d ms: chip ready %d, work received %d, sample age %llu ms", START_WAIT_MS,
             GLOBAL_STATE->ASIC_initalized ? 1 : 0, system->work_received > 0 ? 1 : 0,
             (unsigned long long)(now_ms() - atomic_load(&s_latest_sample_ms)));
    bwg_preparation_failed(BWG_PREP_STEP_READY, BWG_PREP_FAILURE_REPLY_TIMEOUT);
    return false;
}

bool bwg_mining_renew(uint32_t duration_ms)
{
    return bwg_revocation_renew_lease(duration_ms);
}

bool bwg_mining_safe_stop(bwg_stop_reason_t reason)
{
    bwg_revocation_revoke_lease(reason);
    const uint64_t deadline_ms = now_ms() + STOP_WAIT_MS;
    bool stopped = true;
    while (stopped && bwg_revocation_snapshot().state == BWG_AUTHORITY_STOPPING) {
        if (now_ms() >= deadline_ms) {
            ESP_LOGE(TAG, "Stop did not finish within %d ms", STOP_WAIT_MS);
            stopped = false;
        } else {
            bwg_owner_watchdog_feed();
            vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
        }
    }
    // A board that never proves cooling still has no use for the grant's pool.
    wipe_lease_pool();
    return stopped;
}

bool bwg_mining_operator_paused(const GlobalState *GLOBAL_STATE)
{
    return GLOBAL_STATE->SYSTEM_MODULE.mining_paused || s_return_to_pause;
}

bool bwg_mining_local_is_stopped(void)
{
    // The local stop cuts the chip's power before it clears this flag.
    return s_global_state->SYSTEM_MODULE.mining_paused && !s_global_state->ASIC_initalized;
}

bool bwg_mining_release_to_local(void)
{
    bwg_progress_freeze();
    forget_pool_failures(s_global_state);
    s_global_state->SYSTEM_MODULE.mining_paused = bwg_mining_operator_paused(s_global_state);
    s_return_to_pause = false;
    return bwg_revocation_release_to_local();
}

bool bwg_mining_lease_authority_key(uint8_t key[BWG_STRATUM_V2_AUTHORITY_KEY_BYTES])
{
    if (!atomic_load(&s_lease_pool_active) || s_lease_pool.protocol != STRATUM_PROTOCOL_V2) {
        return false;
    }
    memcpy(key, s_lease_authority_key, sizeof(s_lease_authority_key));
    return true;
}

PoolConfig *bwg_mining_pool_or(PoolConfig *saved)
{
    return atomic_load(&s_lease_pool_active) ? &s_lease_pool : saved;
}

bool bwg_mining_holds_pool(const GlobalState *GLOBAL_STATE)
{
    return (atomic_load(&s_lease_pool_active) && !GLOBAL_STATE->ASIC_initalized) || bwg_local_guard_backing_off();
}

bool bwg_mining_lease_pool_active(void)
{
    return atomic_load(&s_lease_pool_active);
}

void bwg_mining_note_sample(const bwg_safety_sample_t *sample)
{
    s_latest_sample = *sample;
    atomic_store(&s_latest_sample_ms, now_ms());
}

void bwg_mining_check_limits(GlobalState *GLOBAL_STATE)
{
    const uint64_t now = now_ms();
    const PowerManagementModule *power = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    const bwg_safety_sample_t sample = {
        .input_volts = power->voltage / 1000.0f,
        .power_watts = power->power,
        .chip_celsius = power->chip_temp_avg,
        .regulator_celsius = power->vr_temp,
        .fan_rpm = power->fan_rpm,
    };
    bwg_mining_note_sample(&sample);

    if (bwg_revocation_snapshot().state != BWG_AUTHORITY_LEASED) {
        return;
    }
    bwg_mining_latch_shares(GLOBAL_STATE);
    if (!GLOBAL_STATE->ASIC_initalized) {
        // Still starting: readings mean nothing yet, and the Start reply has its own limit.
        s_last_safe_ms = now;
        return;
    }
    if (!settings_admissible(GLOBAL_STATE)) {
        bwg_revocation_revoke_lease(BWG_STOP_REASON_UNSAFE_OBSERVATION);
        return;
    }
    bwg_board_limits_t limits;
    if (!bwg_board_limits_for(GLOBAL_STATE->DEVICE_CONFIG.board_version,
                              GLOBAL_STATE->DEVICE_CONFIG.power_consumption_target, &limits)) {
        return;
    }
    if (bwg_limits_sample_safe(&limits, &sample)) {
        s_last_safe_ms = now;
        return;
    }
    if (now - s_last_safe_ms >= BWG_SAFE_SAMPLE_MAX_AGE_MS) {
        ESP_LOGW(TAG, "No safe sample for %d ms", BWG_SAFE_SAMPLE_MAX_AGE_MS);
        bwg_revocation_revoke_lease(BWG_STOP_REASON_UNSAFE_OBSERVATION);
        bwg_unsafe_detail_note_sample(&sample, bwg_limits_first_unsafe_fact(&limits, &sample), (uint32_t)(now - s_last_safe_ms));
    }
}

void bwg_mining_end_lease_on_overheat(bool chip, float celsius)
{
    bwg_revocation_revoke_lease(BWG_STOP_REASON_UNSAFE_OBSERVATION);
    bwg_unsafe_detail_note_overheat(chip, celsius, (uint32_t)(now_ms() - s_last_safe_ms));
}

static void add_session_growth(_Atomic uint32_t *total, uint32_t *seen, uint32_t session)
{
    atomic_fetch_add(total, session >= *seen ? session - *seen : session);
    *seen = session;
}

void bwg_mining_latch_shares(const GlobalState *GLOBAL_STATE)
{
    add_session_growth(&s_lease_accepted, &s_session_accepted, (uint32_t)GLOBAL_STATE->SYSTEM_MODULE.shares_accepted);
    add_session_growth(&s_lease_rejected, &s_session_rejected, (uint32_t)GLOBAL_STATE->SYSTEM_MODULE.shares_rejected);
}

void bwg_mining_lease_shares(uint32_t *accepted, uint32_t *rejected)
{
    *accepted = atomic_load(&s_lease_accepted);
    *rejected = atomic_load(&s_lease_rejected);
}

bool bwg_mining_latest_sample(bwg_safety_sample_t *sample, uint64_t *age_ms)
{
    const uint64_t stamp = atomic_load(&s_latest_sample_ms);
    if (stamp == 0) {
        return false;
    }
    *sample = s_latest_sample;
    *age_ms = now_ms() - stamp;
    return true;
}

const char *bwg_mining_pause_reason(const GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE->SYSTEM_MODULE.overheat_mode) {
        return "overheat";
    }
    if (bwg_boot_guard_holds_pause()) {
        return "crash_loop";
    }
    return bwg_local_guard_holds_pause() ? "restart_guard" : NULL;
}

const char *bwg_mining_activity(const GlobalState *GLOBAL_STATE)
{
    const SystemModule *system = &GLOBAL_STATE->SYSTEM_MODULE;
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    if (bwg_revocation_stop_wanted() && snapshot.state != BWG_AUTHORITY_STOPPED) {
        return "stopping";
    }
    if (system->hardware_fault || system->overheat_mode || system->pools_unavailable || bwg_local_guard_backing_off()) {
        return "safe_blocked";
    }
    return bwg_mining_operator_paused(GLOBAL_STATE) || snapshot.state == BWG_AUTHORITY_STOPPED ? "paused" : "active";
}
