#include "bwg_revocation.h"

#include <stdatomic.h>
#include <stddef.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bwg_revocation";

#define REASON_SHIFT 8
#define STATE_MASK 0xffu

// State and first reason share one word, so a reader never sees one without the other.
// These stay in internal RAM: atomics do not work on PSRAM on this chip.
static _Atomic uint32_t s_authority;

static _Atomic uint32_t s_last_heartbeat_ms;
static _Atomic uint32_t s_closed_ms;
static _Atomic uint32_t s_lease_closed_ms;
static _Atomic uint32_t s_lease_stop_started_ms;
static _Atomic uint32_t s_lease_reason;
static _Atomic uint32_t s_stop_started_ms;
static _Atomic uint32_t s_stopped_ms;
static _Atomic uint32_t s_last_work_ms;
static _Atomic uint32_t s_last_share_ms;
static _Atomic uint32_t s_refused_work;
static _Atomic uint32_t s_refused_shares;
static _Atomic uint32_t s_activated_ms;
static _Atomic uint32_t s_first_work_ms;
static _Atomic uint32_t s_admitted_work;
static _Atomic uint32_t s_admitted_shares;
static _Atomic uint32_t s_nonces;
static _Atomic uint32_t s_active_limit_ms;
static _Atomic uint32_t s_budget_deadline_ms;

static esp_timer_handle_t s_heartbeat_timer;
static esp_timer_handle_t s_lease_timer;
static esp_timer_handle_t s_budget_timer;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t IRAM_ATTR bwg_revocation_panic_word(void)
{
    return atomic_load_explicit(&s_authority, memory_order_relaxed);
}

static bwg_authority_state_t state_of(uint32_t word)
{
    return (bwg_authority_state_t)(word & STATE_MASK);
}

static uint32_t word_of(bwg_authority_state_t state, bwg_stop_reason_t reason)
{
    return (uint32_t)state | ((uint32_t)reason << REASON_SHIFT);
}

static bwg_authority_state_t load_state(void)
{
    return state_of(atomic_load_explicit(&s_authority, memory_order_acquire));
}

static bool apply_from(bwg_authority_event_t event, bwg_stop_reason_t reason, bwg_authority_state_t *from)
{
    uint32_t word = atomic_load_explicit(&s_authority, memory_order_acquire);
    bwg_authority_state_t next;
    while (bwg_authority_transition(state_of(word), event, &next)) {
        if (atomic_compare_exchange_weak_explicit(&s_authority, &word, word_of(next, reason),
                                                  memory_order_acq_rel, memory_order_acquire)) {
            *from = state_of(word);
            return true;
        }
    }
    return false;
}

static bool apply(bwg_authority_event_t event, bwg_stop_reason_t reason)
{
    bwg_authority_state_t from;
    return apply_from(event, reason, &from);
}

static void stop_deadlines(void)
{
    // A stopped or expired timer reports an error here, which is the state we want.
    if (s_heartbeat_timer != NULL) {
        esp_timer_stop(s_heartbeat_timer);
    }
    if (s_lease_timer != NULL) {
        esp_timer_stop(s_lease_timer);
    }
    if (s_budget_timer != NULL) {
        esp_timer_stop(s_budget_timer);
    }
}

static void closed_now(bwg_authority_state_t from, bwg_stop_reason_t reason)
{
    const uint32_t now = now_ms();
    atomic_store_explicit(&s_closed_ms, now, memory_order_release);
    // A later handover closes authority again, and must not move the end of the last lease.
    if (from == BWG_AUTHORITY_LEASED) {
        atomic_store_explicit(&s_lease_closed_ms, now, memory_order_relaxed);
        atomic_store_explicit(&s_lease_reason, reason, memory_order_relaxed);
    }
    atomic_store_explicit(&s_stop_started_ms, 0, memory_order_release);
    atomic_store_explicit(&s_stopped_ms, 0, memory_order_release);
    stop_deadlines();
}

static void heartbeat_expired(void *arg)
{
    bwg_revocation_revoke_lease(BWG_STOP_REASON_HEARTBEAT_TIMEOUT);
}

static void lease_expired(void *arg)
{
    bwg_revocation_revoke_lease(BWG_STOP_REASON_LEASE_EXPIRED);
}

static void budget_expired(void *arg)
{
    bwg_revocation_revoke_lease(BWG_STOP_REASON_LEASE_EXPIRED);
}

static bool ensure_timer(esp_timer_handle_t *timer, esp_timer_cb_t callback, const char *name)
{
    if (*timer != NULL) {
        return true;
    }
    const esp_timer_create_args_t args = {
        .callback = callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = name,
    };
    return esp_timer_create(&args, timer) == ESP_OK;
}

static bool duration_is_valid(uint32_t duration_ms)
{
    return duration_ms > 0 && duration_ms <= BWG_LEASE_MAX_MS;
}

/** Whether a budget, which starts with the first work so preparation spends none of it, has run out. */
static bool budget_spent(uint32_t now)
{
    return atomic_load_explicit(&s_active_limit_ms, memory_order_relaxed) != 0 &&
           atomic_load_explicit(&s_first_work_ms, memory_order_acquire) != 0 &&
           (int32_t)(now - atomic_load_explicit(&s_budget_deadline_ms, memory_order_acquire)) >= 0;
}

static bool note_first_work(uint32_t now)
{
    const uint32_t limit = atomic_load_explicit(&s_active_limit_ms, memory_order_relaxed);
    // Published before the first-work time, so no reader sees a started budget without its deadline.
    if (limit != 0 && atomic_load_explicit(&s_first_work_ms, memory_order_acquire) == 0) {
        atomic_store_explicit(&s_budget_deadline_ms, now + limit, memory_order_release);
    }
    uint32_t unset = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_first_work_ms, &unset, now, memory_order_acq_rel, memory_order_relaxed)) {
        return true;
    }
    return limit == 0 || esp_timer_start_once(s_budget_timer, limit * 1000ULL) == ESP_OK;
}

bool bwg_revocation_admit_work(void)
{
    if (!bwg_authority_admits(load_state())) {
        atomic_fetch_add_explicit(&s_refused_work, 1, memory_order_relaxed);
        return false;
    }
    const uint32_t now = now_ms();
    if (load_state() == BWG_AUTHORITY_LEASED) {
        if (budget_spent(now)) {
            bwg_revocation_revoke_lease(BWG_STOP_REASON_LEASE_EXPIRED);
            atomic_fetch_add_explicit(&s_refused_work, 1, memory_order_relaxed);
            return false;
        }
        if (!note_first_work(now)) {
            // Work with no deadline on its budget must not continue.
            bwg_revocation_revoke_lease(BWG_STOP_REASON_CONTROL_FAILED);
            atomic_fetch_add_explicit(&s_refused_work, 1, memory_order_relaxed);
            return false;
        }
        atomic_fetch_add_explicit(&s_admitted_work, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&s_last_work_ms, now, memory_order_relaxed);
    return true;
}

bool bwg_revocation_admit_share(void)
{
    if (!bwg_authority_admits(load_state())) {
        atomic_fetch_add_explicit(&s_refused_shares, 1, memory_order_relaxed);
        return false;
    }
    atomic_store_explicit(&s_last_share_ms, now_ms(), memory_order_relaxed);
    if (load_state() == BWG_AUTHORITY_LEASED) {
        atomic_fetch_add_explicit(&s_admitted_shares, 1, memory_order_relaxed);
    }
    return true;
}

void bwg_revocation_note_nonce(void)
{
    if (load_state() == BWG_AUTHORITY_LEASED) {
        atomic_fetch_add_explicit(&s_nonces, 1, memory_order_relaxed);
    }
}

void bwg_revocation_revoke(bwg_stop_reason_t reason)
{
    bwg_authority_state_t from;
    if (apply_from(BWG_AUTHORITY_EVENT_REVOKE, reason, &from)) {
        closed_now(from, reason);
    }
}

void bwg_revocation_revoke_lease(bwg_stop_reason_t reason)
{
    uint32_t leased = word_of(BWG_AUTHORITY_LEASED, BWG_STOP_REASON_NONE);
    if (atomic_compare_exchange_strong_explicit(&s_authority, &leased, word_of(BWG_AUTHORITY_STOPPING, reason),
                                                memory_order_acq_rel, memory_order_acquire)) {
        closed_now(BWG_AUTHORITY_LEASED, reason);
    }
}

bool bwg_revocation_stop_wanted(void)
{
    return bwg_authority_stop_wanted(load_state());
}

void bwg_revocation_note_stop_started(void)
{
    if (load_state() != BWG_AUTHORITY_STOPPING) {
        return;
    }
    const uint32_t now = now_ms();
    atomic_store_explicit(&s_stop_started_ms, now, memory_order_release);
    if (atomic_load_explicit(&s_closed_ms, memory_order_acquire) ==
        atomic_load_explicit(&s_lease_closed_ms, memory_order_relaxed)) {
        atomic_store_explicit(&s_lease_stop_started_ms, now, memory_order_relaxed);
    }
}

void bwg_revocation_note_stopped(void)
{
    uint32_t word = atomic_load_explicit(&s_authority, memory_order_acquire);
    bwg_stop_reason_t reason = (bwg_stop_reason_t)(word >> REASON_SHIFT);
    if (!apply(BWG_AUTHORITY_EVENT_STOP_CONFIRMED, reason)) {
        return;
    }
    atomic_store_explicit(&s_stopped_ms, now_ms(), memory_order_release);

    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    ESP_LOGW(TAG, "stopped reason=%s heartbeat=%lu closed=%lu stop_started=%lu stopped=%lu last_work=%lu last_share=%lu refused_work=%lu refused_shares=%lu",
             bwg_stop_reason_label(snapshot.reason), (unsigned long)snapshot.last_heartbeat_ms, (unsigned long)snapshot.closed_ms,
             (unsigned long)snapshot.stop_started_ms, (unsigned long)snapshot.stopped_ms, (unsigned long)snapshot.last_work_ms,
             (unsigned long)snapshot.last_share_ms, (unsigned long)snapshot.refused_work, (unsigned long)snapshot.refused_shares);
}

static bool active_limit_is_valid(uint32_t limit_ms)
{
    return limit_ms <= BWG_ACTIVE_LIMIT_MAX_MS || limit_ms == BWG_ACTIVE_LIMIT_SOAK_MS;
}

bool bwg_revocation_begin_lease(uint32_t duration_ms)
{
    return bwg_revocation_begin_limited_lease(duration_ms, 0);
}

bool bwg_revocation_begin_limited_lease(uint32_t duration_ms, uint32_t active_limit_ms)
{
    if (!duration_is_valid(duration_ms) || !active_limit_is_valid(active_limit_ms) ||
        !ensure_timer(&s_heartbeat_timer, heartbeat_expired, "bwg_heartbeat") ||
        !ensure_timer(&s_lease_timer, lease_expired, "bwg_lease") ||
        !ensure_timer(&s_budget_timer, budget_expired, "bwg_budget")) {
        return false;
    }
    // Before authority opens, so the first work this lease admits sees its own limit.
    atomic_store_explicit(&s_first_work_ms, 0, memory_order_release);
    atomic_store_explicit(&s_budget_deadline_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_active_limit_ms, active_limit_ms, memory_order_relaxed);
    if (!apply(BWG_AUTHORITY_EVENT_LEASE_BEGIN, BWG_STOP_REASON_NONE)) {
        return false;
    }
    atomic_store_explicit(&s_closed_ms, 0, memory_order_release);
    atomic_store_explicit(&s_lease_closed_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_lease_stop_started_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_lease_reason, BWG_STOP_REASON_NONE, memory_order_relaxed);
    atomic_store_explicit(&s_stop_started_ms, 0, memory_order_release);
    atomic_store_explicit(&s_stopped_ms, 0, memory_order_release);
    atomic_store_explicit(&s_activated_ms, now_ms(), memory_order_relaxed);
    atomic_store_explicit(&s_admitted_work, 0, memory_order_relaxed);
    atomic_store_explicit(&s_admitted_shares, 0, memory_order_relaxed);
    atomic_store_explicit(&s_nonces, 0, memory_order_relaxed);
    atomic_store_explicit(&s_last_heartbeat_ms, now_ms(), memory_order_release);
    if (esp_timer_start_once(s_heartbeat_timer, BWG_HEARTBEAT_CUTOFF_MS * 1000ULL) != ESP_OK ||
        esp_timer_start_once(s_lease_timer, duration_ms * 1000ULL) != ESP_OK) {
        // An open lease without its deadlines must not continue.
        bwg_revocation_revoke_lease(BWG_STOP_REASON_CONTROL_FAILED);
        return false;
    }
    return true;
}

bool bwg_revocation_feed_heartbeat_at(uint32_t observed_ms)
{
    // The stamp predates the timer and the link's poll counts from it too, so the reported gap from here to the
    // close is never short of the cutoff, whichever of them closes.
    if (load_state() != BWG_AUTHORITY_LEASED || esp_timer_restart(s_heartbeat_timer, BWG_HEARTBEAT_CUTOFF_MS * 1000ULL) != ESP_OK) {
        return false;
    }
    atomic_store_explicit(&s_last_heartbeat_ms, observed_ms, memory_order_release);
    return true;
}

bool bwg_revocation_feed_heartbeat(void)
{
    return bwg_revocation_feed_heartbeat_at(now_ms());
}

bool bwg_revocation_renew_lease(uint32_t duration_ms)
{
    return duration_is_valid(duration_ms) && load_state() == BWG_AUTHORITY_LEASED &&
           esp_timer_restart(s_lease_timer, duration_ms * 1000ULL) == ESP_OK;
}

bool bwg_revocation_release_to_local(void)
{
    if (!apply(BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL, BWG_STOP_REASON_NONE)) {
        return false;
    }
    return true;
}

bwg_revocation_snapshot_t bwg_revocation_snapshot(void)
{
    const uint32_t word = atomic_load_explicit(&s_authority, memory_order_acquire);
    return (bwg_revocation_snapshot_t) {
        .state = state_of(word),
        .reason = (bwg_stop_reason_t)(word >> REASON_SHIFT),
        .last_heartbeat_ms = atomic_load_explicit(&s_last_heartbeat_ms, memory_order_acquire),
        .closed_ms = atomic_load_explicit(&s_closed_ms, memory_order_acquire),
        .lease_closed_ms = atomic_load_explicit(&s_lease_closed_ms, memory_order_relaxed),
        .lease_stop_started_ms = atomic_load_explicit(&s_lease_stop_started_ms, memory_order_relaxed),
        .lease_reason = (bwg_stop_reason_t)atomic_load_explicit(&s_lease_reason, memory_order_relaxed),
        .stop_started_ms = atomic_load_explicit(&s_stop_started_ms, memory_order_acquire),
        .stopped_ms = atomic_load_explicit(&s_stopped_ms, memory_order_acquire),
        .last_work_ms = atomic_load_explicit(&s_last_work_ms, memory_order_relaxed),
        .last_share_ms = atomic_load_explicit(&s_last_share_ms, memory_order_relaxed),
        .refused_work = atomic_load_explicit(&s_refused_work, memory_order_relaxed),
        .refused_shares = atomic_load_explicit(&s_refused_shares, memory_order_relaxed),
        .activated_ms = atomic_load_explicit(&s_activated_ms, memory_order_relaxed),
        .first_work_ms = atomic_load_explicit(&s_first_work_ms, memory_order_relaxed),
        .admitted_work = atomic_load_explicit(&s_admitted_work, memory_order_relaxed),
        .admitted_shares = atomic_load_explicit(&s_admitted_shares, memory_order_relaxed),
        .nonces = atomic_load_explicit(&s_nonces, memory_order_relaxed),
        .active_limit_ms = atomic_load_explicit(&s_active_limit_ms, memory_order_relaxed),
        .budget_deadline_ms = atomic_load_explicit(&s_budget_deadline_ms, memory_order_acquire),
    };
}

const char *bwg_stop_reason_label(bwg_stop_reason_t reason)
{
    switch (reason) {
    case BWG_STOP_REASON_NONE: return "none";
    case BWG_STOP_REASON_HEARTBEAT_TIMEOUT: return "heartbeat_timeout";
    case BWG_STOP_REASON_LEASE_EXPIRED: return "lease_expired";
    case BWG_STOP_REASON_LINK_CLOSED: return "link_closed";
    case BWG_STOP_REASON_CONTROL_FAILED: return "control_failed";
    case BWG_STOP_REASON_UNSAFE_OBSERVATION: return "unsafe_observation";
    case BWG_STOP_REASON_LOCAL_STOP: return "local_stop";
    case BWG_STOP_REASON_LOCAL_HANDOVER: return "local_handover";
    case BWG_STOP_REASON_RESTORATION_REQUESTED: return "restoration_requested";
    }
    return "unknown";
}
