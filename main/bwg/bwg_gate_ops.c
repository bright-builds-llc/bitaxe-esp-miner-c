#include "bwg_noise_gate.h"
#include "bwg_v2_gate.h"
#include "bwg_v2_runtime.h"
#include "bwg_gate_ops.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_codec.h"
#include "bwg_cooling.h"
#include "bwg_core_dump.h"
#include "bwg_evidence.h"
#include "bwg_gate_budget.h"
#include "bwg_limits.h"
#include "bwg_mining.h"
#include "bwg_owner_watchdog.h"
#include "bwg_panic_cutoff.h"
#include "bwg_preparation.h"
#include "bwg_progress.h"
#include "bwg_revocation.h"
#include "bwg_seq_doc.h"
#include "bwg_settings_digest.h"
#include "bwg_stop.h"
#include "bwg_store.h"
#include "bwg_telemetry_cadence.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "sodium.h"

#define LEASE_FIELD_BYTES (BWG_LEASE_MAX_SECRET_BYTES + 1)
#define SHUTDOWN_BUDGET_MS 22550u
#define DIGEST_HEX_BYTES (crypto_hash_sha256_BYTES * 2 + 1)

static bwg_trace_t *s_trace;
static const char *TAG = "bwg_gate";

static const bwg_allocator_t *s_allocator;
static GlobalState *s_global_state;
static uint32_t s_lease_generation;
static uint8_t s_boot_pool_digest[crypto_hash_sha256_BYTES];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static char *copy_counted(char *buffer, bwg_str_t text)
{
    // An empty password has no pointer to copy from.
    if (text.len > 0) {
        memcpy(buffer, text.ptr, text.len);
    }
    buffer[text.len] = '\0';
    return buffer;
}

static bool op_start(void *ctx, const bwg_lease_grant_t *grant, bwg_lease_deadlines_t deadlines)
{
    bwg_str_t host;
    uint16_t port;
    if (!bwg_stratum_endpoint_parse(grant->endpoint, &host, &port) || host.len >= LEASE_FIELD_BYTES ||
        !bwg_mining_may_start()) {
        return false;
    }
    const bool is_v2 = grant->stratum_kind == BWG_STRATUM_V2;
    uint8_t authority_key[BWG_STRATUM_V2_AUTHORITY_KEY_BYTES];
    size_t authority_key_len = 0;
    if (is_v2 && (!bwg_b64url_decode(grant->authority_public_key.ptr, grant->authority_public_key.len, authority_key,
                                     sizeof(authority_key), &authority_key_len) ||
                  authority_key_len != sizeof(authority_key))) {
        return false;
    }
    // Seeded before any charge, so that a boot's second attempt costs nothing.
    if (is_v2 && (grant->maybe_qualification_attempt == NULL || !bwg_v2_gate_seed_share(grant->allowance.id))) {
        return false;
    }
    const size_t size = LEASE_FIELD_BYTES * 3;
    char *fields = bwg_alloc(s_allocator, size);
    if (fields == NULL) {
        bwg_v2_runtime_never_started();
        return false;
    }
    const bwg_lease_pool_t pool = {
        .host = copy_counted(fields, host),
        .port = port,
        .username = copy_counted(fields + LEASE_FIELD_BYTES, is_v2 ? grant->user_identity : grant->username),
        .password = copy_counted(fields + LEASE_FIELD_BYTES * 2, grant->password),
        .suggested_difficulty = grant->has_suggested_difficulty ? grant->suggested_difficulty : 0,
        .maybe_authority_key = is_v2 ? authority_key : NULL,
    };
    s_lease_generation++;
    uint32_t limit_ms = 0;
    bwg_preparation_begin(s_lease_generation);
    const bool started = bwg_gate_budget_admit(s_lease_generation, &grant->allowance, &limit_ms) &&
                         bwg_mining_start(&pool, (uint32_t)grant->duration_ms, limit_ms);
    bwg_preparation_end();
    bwg_free_secret(s_allocator, fields, size);
    if (!started) {
        bwg_v2_runtime_never_started();
    }
    return started;
}

static bool op_renew(void *ctx, const bwg_lease_renewal_t *renewal, bwg_lease_deadlines_t deadlines)
{
    return bwg_mining_renew((uint32_t)renewal->duration_ms);
}

static bwg_stop_reason_t stop_reason_for(bwg_reason_t reason)
{
    switch (reason) {
    case BWG_REASON_LEASE_EXPIRED:
        return BWG_STOP_REASON_LEASE_EXPIRED;
    case BWG_REASON_CONNECTIVITY_LOST:
        return BWG_STOP_REASON_LINK_CLOSED;
    case BWG_REASON_CONTROL_FAILED:
    case BWG_REASON_MONOTONIC_RESET:
        return BWG_STOP_REASON_CONTROL_FAILED;
    default:
        return BWG_STOP_REASON_RESTORATION_REQUESTED;
    }
}

static bool op_safe_stop(void *ctx, bwg_reason_t reason)
{
    const bool stopped = bwg_mining_safe_stop(stop_reason_for(reason)) && bwg_cooling_release(s_global_state) &&
                         bwg_gate_budget_finish(s_lease_generation);
    ESP_LOGI(TAG, "Lease stop finished ok=%d", stopped);
    return stopped;
}

static bool op_high_water_load(void *ctx, const char *kid, bool *present, uint64_t *value)
{
    return bwg_store_high_water_load(s_allocator, kid, present, value);
}

static bwg_store_result_t op_high_water_store(void *ctx, const char *kid, bool has_expected, uint64_t expected, uint64_t next)
{
    return bwg_store_high_water_compare_and_store(s_allocator, kid, has_expected, expected, next);
}

static bool op_effect_get(void *ctx, bwg_effect_state_t *state)
{
    return bwg_store_effect_get(state);
}

static bool op_effect_set(void *ctx, bwg_effect_state_t state)
{
    return bwg_store_effect_set(state);
}

static const char *qualification_reason(bwg_stop_reason_t reason)
{
    switch (reason) {
    case BWG_STOP_REASON_NONE:
        return "none";
    case BWG_STOP_REASON_HEARTBEAT_TIMEOUT:
        return "heartbeat_timeout";
    case BWG_STOP_REASON_LEASE_EXPIRED:
        return "lease_or_budget_expired";
    case BWG_STOP_REASON_UNSAFE_OBSERVATION:
    case BWG_STOP_REASON_LOCAL_STOP:
        return "unsafe_observation";
    case BWG_STOP_REASON_LINK_CLOSED:
        return "link_closed";
    case BWG_STOP_REASON_CONTROL_FAILED:
        return "control_failed";
    case BWG_STOP_REASON_LOCAL_HANDOVER:
    case BWG_STOP_REASON_RESTORATION_REQUESTED:
        return "restoration_requested";
    }
    return "control_failed";
}

static bwg_evidence_t op_budget_review(void *ctx, bwg_allowance_kind_t kind, bwg_str_t campaign_id, bwg_jw_t *writer)
{
    return bwg_gate_budget_review(kind, campaign_id, writer);
}

static bool op_cooling_prove(void *ctx, uint32_t *fan_rpm)
{
    return bwg_cooling_prove(s_global_state, fan_rpm);
}

static bool op_cooling_restore(void *ctx)
{
    return bwg_cooling_restore(s_global_state);
}

static bool op_restart_context(void *ctx, uint64_t *boot_ordinal)
{
    // Only with nothing to come back to: a board that would start mining by itself may not restart for a test.
    if (nvs_config_get_bool(NVS_CONFIG_MINE_ON_BOOT) || bwg_revocation_snapshot().state != BWG_AUTHORITY_STOPPED) {
        return false;
    }
    *boot_ordinal = bwg_evidence_boot_ordinal();
    return true;
}

static void op_restart(void *ctx)
{
    ESP_LOGI(TAG, "Restarting for the Gate's qualification");
    esp_restart();
}

static bool op_core_dump_context(void *ctx, uint64_t *boot_ordinal)
{
    // The panic is requested only of a board with its outputs proven off and a store ready to take it.
    return bwg_panic_cutoff_outputs_off() && bwg_core_dump_is_ready() && op_restart_context(ctx, boot_ordinal);
}

static void op_core_dump_self_test(void *ctx)
{
    ESP_LOGW(TAG, "Panicking for the Gate's core dump self-test");
    bwg_panic_cutoff_mark_self_test();
    bwg_core_dump_mark_self_test();
    abort();
}

static bool op_write_endpoint(void *ctx, bwg_jw_t *writer)
{
    return bwg_evidence_write_endpoint(writer, s_lease_generation);
}

static void op_write_trace(void *ctx, bwg_jw_t *writer)
{
    bwg_trace_write(s_trace, writer);
}

static bool op_cadence_arm(void *ctx, bwg_cadence_phase_t phase, bwg_jw_t *writer)
{
    // The mining phase names the lease that starts next, and that lease's first work begins its capture.
    return bwg_telemetry_cadence_arm(phase, s_lease_generation + 1, writer);
}

static void op_write_cadence(void *ctx, bwg_jw_t *writer)
{
    bwg_telemetry_cadence_write_review(writer);
}

static void op_note_max_probe(void *ctx)
{
    bwg_telemetry_cadence_note_max_probe();
}

uint32_t bwg_gate_ops_lease_generation(void)
{
    return s_lease_generation;
}

void bwg_gate_ops_set_trace(bwg_trace_t *trace)
{
    s_trace = trace;
}

static void write_u32_or_null(bwg_jw_t *writer, const char *key, uint32_t value)
{
    bwg_jw_key(writer, key);
    if (value == 0) {
        bwg_jw_null(writer);
    } else {
        bwg_jw_u64(writer, value);
    }
}

// The core writer has no float form. These are reports, not signed values, so a fixed format is enough.
static void write_sample(bwg_jw_t *writer, const char *key, bool fresh, float value)
{
    bwg_jw_key(writer, key);
    if (!fresh || !isfinite(value)) {
        bwg_jw_null(writer);
        return;
    }
    char text[24];
    const int len = snprintf(text, sizeof(text), "%.3f", value);
    bwg_jw_raw_value(writer, (const uint8_t *)text, (size_t)len);
}

static void write_flag(bwg_jw_t *writer, const char *key, bool value)
{
    bwg_jw_key(writer, key);
    bwg_jw_bool(writer, value);
}

static void write_u32(bwg_jw_t *writer, const char *key, uint32_t value)
{
    bwg_jw_key(writer, key);
    bwg_jw_u64(writer, value);
}

static const char *owner_phase(const bwg_revocation_snapshot_t *snapshot)
{
    return snapshot->state == BWG_AUTHORITY_STOPPED ? "shutdown_complete" : "active";
}

// Read while this reply is built, by the owner task itself, so it is never older than the status.
static void write_owner_resources(bwg_jw_t *writer, const bwg_revocation_snapshot_t *snapshot)
{
    char observed[24];
    snprintf(observed, sizeof(observed), "%" PRIu32, now_ms());
    bwg_jw_key(writer, "owner_resources");
    bwg_jw_object_begin(writer);
    write_u32(writer, "generation", s_lease_generation);
    write_u32(writer, "heap_free_bytes", (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    write_u32(writer, "heap_largest_bytes", (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bwg_jw_key(writer, "observed_at_ms");
    bwg_jw_cstr(writer, observed);
    bwg_jw_key(writer, "phase");
    bwg_jw_cstr(writer, owner_phase(snapshot));
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-owner-resources-v1");
    write_u32(writer, "stack_free_bytes", (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    bwg_jw_object_end(writer);
}

static bwg_evidence_t op_qualification(void *ctx, bwg_jw_t *writer)
{
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    const bool has_lease = s_lease_generation != 0;
    const uint32_t end_ms = snapshot.lease_closed_ms != 0 ? snapshot.lease_closed_ms : now_ms();
    const bwg_budget_timing_t timing =
        has_lease ? bwg_budget_timing(snapshot.first_work_ms, snapshot.lease_closed_ms, snapshot.active_limit_ms,
                                      snapshot.budget_deadline_ms, now_ms())
                  : (bwg_budget_timing_t) {0};
    const uint32_t active_ms = timing.active_ms;
    const uint32_t elapsed_ms = has_lease ? end_ms - snapshot.activated_ms : 0;
    // The Gate asks about the lease, and a handover after it closes authority again.
    const bool lease_ended = has_lease && snapshot.lease_closed_ms != 0;

    bwg_safety_sample_t sample = {0};
    uint64_t age_ms = UINT64_MAX;
    const bool fresh = bwg_mining_latest_sample(&sample, &age_ms) && age_ms <= BWG_SAMPLE_FRESH_MS;
    if (fresh && snapshot.state == BWG_AUTHORITY_STOPPED) {
        // With the ASIC down the board's own sensor is the reading the stop proved cooling from, where there is one.
        sample.chip_celsius = bwg_stop_board_sensor_celsius(s_global_state);
    }

    uint32_t accepted = 0;
    uint32_t rejected = 0;
    bwg_mining_lease_shares(&accepted, &rejected);

    bwg_jw_object_begin(writer);
    write_u32(writer, "accepted", accepted);
    write_u32_or_null(writer, "active_limit_ms", has_lease ? snapshot.active_limit_ms : 0);
    write_u32(writer, "active_ms", active_ms);
    bwg_gate_budget_write_observation(s_lease_generation, active_ms, writer);
    write_flag(writer, "budget_complete", bwg_gate_budget_campaign_complete());
    write_u32(writer, "budget_reserved_ms", bwg_gate_budget_campaign_reserved_ms());
    // The chip reading is -1 while the ASIC is down, which isn't a temperature.
    const bool chip_fresh = fresh && isfinite(sample.chip_celsius) && sample.chip_celsius >= 0;
    // The Gate refuses a fresh flag beside a null reading, and a failed sensor read can give a non-finite one.
    const bool power_fresh = fresh && isfinite(sample.power_watts);
    const bool voltage_fresh = fresh && isfinite(sample.input_volts);
    write_sample(writer, "chip_temp_celsius", chip_fresh, sample.chip_celsius);
    bwg_jw_key(writer, "fan_rpm");
    if (fresh) {
        bwg_jw_u64(writer, sample.fan_rpm);
    } else {
        bwg_jw_null(writer);
    }
    write_flag(writer, "fan_fresh", fresh);
    write_u32_or_null(writer, "gate_closed_ms", lease_ended ? snapshot.lease_closed_ms : snapshot.closed_ms);
    write_u32(writer, "generation", s_lease_generation);
    write_u32(writer, "generation_elapsed_ms", elapsed_ms);
    write_u32(writer, "last_valid_heartbeat_ms", snapshot.last_heartbeat_ms);
    write_flag(writer, "mine_on_boot", nvs_config_get_bool(NVS_CONFIG_MINE_ON_BOOT));
    write_u32(writer, "nonce_work_correlations", has_lease ? snapshot.nonces : 0);
    if (has_lease) {
        bwg_jw_key(writer, "mining_progress");
        bwg_progress_write(writer, s_lease_generation, now_ms());
    }
    if (has_lease) {
        write_owner_resources(writer, &snapshot);
    }
    write_flag(writer, "power_fresh", power_fresh);
    write_sample(writer, "power_watts", power_fresh, sample.power_watts);
    write_u32(writer, "rejected", rejected);
    bwg_jw_key(writer, "revocation_reason");
    bwg_jw_cstr(writer, qualification_reason(lease_ended ? snapshot.lease_reason : snapshot.reason));
    write_flag(writer, "safe_stop_complete", snapshot.state == BWG_AUTHORITY_STOPPED);
    bwg_jw_key(writer, "safe_stop_stage");
    bwg_jw_cstr(writer, bwg_stop_stage_label(bwg_stop_stage()));
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-qualification-v1");
    write_u32(writer, "shutdown_budget_ms", has_lease ? SHUTDOWN_BUDGET_MS : 0);
    write_u32_or_null(writer, "shutdown_started_ms", lease_ended ? snapshot.lease_stop_started_ms : snapshot.stop_started_ms);
    write_u32(writer, "submitted", has_lease ? snapshot.admitted_shares : 0);
    write_flag(writer, "temperature_fresh", chip_fresh);
    write_flag(writer, "voltage_fresh", voltage_fresh);
    write_sample(writer, "voltage_volts", voltage_fresh, sample.input_volts);
    write_flag(writer, "watchdog_alive", bwg_owner_watchdog_alive());
    write_u32(writer, "work_dispatched", has_lease ? snapshot.admitted_work : 0);
    // Zero is a closed gate here, which the Gate tells apart from no budget at all.
    bwg_jw_key(writer, "work_gate_remaining_ms");
    if (timing.has_remaining) {
        bwg_jw_u64(writer, timing.remaining_ms);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_object_end(writer);
    return BWG_EVIDENCE_WRITTEN;
}

static void hash_text(crypto_hash_sha256_state *hash, const char *maybe_text)
{
    const char *text = maybe_text == NULL ? "" : maybe_text;
    crypto_hash_sha256_update(hash, (const uint8_t *)text, strlen(text) + 1);
}

static void digest_pools(GlobalState *GLOBAL_STATE, uint8_t digest[crypto_hash_sha256_BYTES])
{
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    for (size_t i = 0; i < MAX_POOLS; i++) {
        const PoolConfig *pool = &GLOBAL_STATE->SYSTEM_MODULE.pools[i];
        hash_text(&hash, pool->url);
        hash_text(&hash, pool->user);
        hash_text(&hash, pool->pass);
        crypto_hash_sha256_update(&hash, (const uint8_t *)&pool->port, sizeof(pool->port));
    }
    crypto_hash_sha256_final(&hash, digest);
    sodium_memzero(&hash, sizeof(hash));
}

static void write_digest_hex(bwg_jw_t *writer, const char *key, const uint8_t digest[crypto_hash_sha256_BYTES])
{
    char hex[DIGEST_HEX_BYTES];
    sodium_bin2hex(hex, sizeof(hex), digest, crypto_hash_sha256_BYTES);
    bwg_jw_key(writer, key);
    bwg_jw_cstr(writer, hex);
}

static bool high_water_digest(uint8_t digest[crypto_hash_sha256_BYTES])
{
    uint8_t text[BWG_SEQ_DOC_MAX_BYTES];
    size_t len = 0;
    if (!bwg_store_high_water_read_raw(text, sizeof(text), &len)) {
        return false;
    }
    crypto_hash_sha256(digest, text, len);
    return true;
}

static bool op_high_water_fingerprint(void *ctx, char fingerprint[BWG_FINGERPRINT_HEX_SIZE])
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    if (!high_water_digest(digest)) {
        return false;
    }
    sodium_bin2hex(fingerprint, BWG_FINGERPRINT_HEX_SIZE, digest, sizeof(digest));
    return true;
}

const char *bwg_gate_ops_reset_cause(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_SW:
        return "software_cpu";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}

static const char *op_boot_reset_cause(void *ctx)
{
    return bwg_gate_ops_reset_cause();
}

static bwg_evidence_t op_preservation(void *ctx, bwg_jw_t *writer, const char *device_identity_sha256)
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    if (!high_water_digest(digest)) {
        return BWG_EVIDENCE_PERSISTENCE_FAILED;
    }
    uint8_t pools[crypto_hash_sha256_BYTES];
    digest_pools(s_global_state, pools);

    bwg_jw_object_begin(writer);
    write_digest_hex(writer, "authorization_high_water_sha256", digest);
    bwg_jw_key(writer, "device_identity_sha256");
    bwg_jw_cstr(writer, device_identity_sha256);
    write_flag(writer, "mine_on_boot", nvs_config_get_bool(NVS_CONFIG_MINE_ON_BOOT));
    write_flag(writer, "pool_configuration_unchanged_since_boot", sodium_memcmp(pools, s_boot_pool_digest, sizeof(pools)) == 0);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-preservation-v2");
    bwg_settings_digest(digest);
    write_digest_hex(writer, "settings_sha256", digest);
    bwg_jw_object_end(writer);
    return BWG_EVIDENCE_WRITTEN;
}

void bwg_gate_ops_fill(bwg_controller_ops_t *ops, const bwg_allocator_t *allocator, GlobalState *GLOBAL_STATE)
{
    s_allocator = allocator;
    s_global_state = GLOBAL_STATE;
    digest_pools(GLOBAL_STATE, s_boot_pool_digest);
    *ops = (bwg_controller_ops_t) {
        .start = op_start,
        .renew = op_renew,
        .safe_stop = op_safe_stop,
        .high_water_load = op_high_water_load,
        .high_water_compare_and_store = op_high_water_store,
        .effect_get = op_effect_get,
        .effect_set = op_effect_set,
        .maybe_append_qualification = op_qualification,
        .maybe_append_preservation = op_preservation,
        .maybe_high_water_fingerprint = op_high_water_fingerprint,
        .maybe_boot_reset_cause = op_boot_reset_cause,
        .maybe_budget_review = op_budget_review,
        .maybe_cooling_prove = op_cooling_prove,
        .maybe_cooling_restore = op_cooling_restore,
        .maybe_restart_context = op_restart_context,
        .maybe_restart = op_restart,
        .maybe_core_dump_context = op_core_dump_context,
        .maybe_core_dump_self_test = op_core_dump_self_test,
        .maybe_write_endpoint = op_write_endpoint,
        .maybe_write_trace = op_write_trace,
        .maybe_cadence_arm = op_cadence_arm,
        .maybe_write_cadence = op_write_cadence,
        .maybe_note_max_probe = op_note_max_probe,
    };
    bwg_v2_gate_fill_ops(ops, GLOBAL_STATE);
    bwg_noise_gate_fill_ops(ops, GLOBAL_STATE);
}

static const char *admission_stage(const bwg_revocation_snapshot_t *snapshot)
{
    switch (snapshot->state) {
    case BWG_AUTHORITY_LEASED:
        return snapshot->admitted_work > 0 ? "active" : "pool_activation";
    case BWG_AUTHORITY_STOPPING:
        return "cleanup";
    default:
        return "idle";
    }
}

// Bits 0 to 5: run intent, network, protocol, fresh safety, lease available, actuation owners.
static unsigned admission_readiness(const bwg_revocation_snapshot_t *snapshot)
{
    const SystemModule *system = &s_global_state->SYSTEM_MODULE;
    bwg_safety_sample_t sample;
    uint64_t age_ms = UINT64_MAX;
    const bool fresh = bwg_mining_latest_sample(&sample, &age_ms) && age_ms <= BWG_SAMPLE_FRESH_MS;
    unsigned readiness = 1u << 2;
    readiness |= bwg_mining_operator_paused(s_global_state) ? 0u : 1u << 0;
    readiness |= system->is_connected ? 1u << 1 : 0u;
    readiness |= fresh ? 1u << 3 : 0u;
    readiness |= snapshot->state == BWG_AUTHORITY_STOPPED ? 1u << 4 : 0u;
    readiness |= 1u << 5;
    return readiness;
}

void bwg_gate_ops_format_admission(char *line, size_t capacity)
{
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    snprintf(line, capacity,
             "worker_admission schema=v1 stage=%s first_failure=none readiness=%u budget_reserved_ms=%" PRIu32 " budget_complete=%s redacted=true",
             admission_stage(&snapshot), admission_readiness(&snapshot), bwg_gate_budget_campaign_reserved_ms(),
             bwg_gate_budget_campaign_complete() ? "true" : "false");
}

void bwg_gate_ops_finish_stopped_lease(void)
{
    if (bwg_revocation_snapshot().state == BWG_AUTHORITY_STOPPED && !bwg_gate_budget_finish(s_lease_generation)) {
        ESP_LOGW(TAG, "A stopped lease's charge isn't finished yet");
    }
}

void bwg_gate_ops_note_owner_stack(uint32_t free_bytes)
{
    static uint32_t lowest = UINT32_MAX;
    if (free_bytes < lowest) {
        lowest = free_bytes;
        ESP_LOGI(TAG, "owner stack low water %" PRIu32 " bytes free", lowest);
    }
}
