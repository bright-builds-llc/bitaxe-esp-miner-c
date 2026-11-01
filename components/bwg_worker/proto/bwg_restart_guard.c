#include "bwg_restart_guard.h"

// Enough to pass the longest backoff before the hold, and small enough that the shift can't overflow.
#define MAX_DOUBLINGS 16u

static void note_failure(bwg_restart_guard_t *guard, uint64_t now_ms)
{
    if (guard->consecutive_failures < UINT32_MAX) {
        guard->consecutive_failures++;
    }
    uint32_t doublings = guard->consecutive_failures - 1;
    if (doublings > MAX_DOUBLINGS) {
        doublings = MAX_DOUBLINGS;
    }
    guard->backoff_until_ms = now_ms + ((uint64_t) BWG_RESTART_BACKOFF_BASE_MS << doublings);
}

static void bank_hashing(bwg_restart_guard_t *guard, uint64_t now_ms)
{
    if (guard->hashing) {
        guard->hashed_ms += now_ms - guard->hashing_since_ms;
    }
    guard->hashing = false;
}

void bwg_restart_guard_observe(bwg_restart_guard_t *guard, bool running, bool hashing, bool pool_failed,
                               uint64_t now_ms)
{
    if (!running) {
        if (!guard->running) {
            return;
        }
        bank_hashing(guard, now_ms);
        guard->running = false;
        if (guard->hashed_ms >= BWG_RESTART_STABLE_RUN_MS) {
            bwg_restart_guard_resume(guard);
        }
        guard->hashed_ms = 0;
        if (pool_failed) {
            note_failure(guard, now_ms);
        }
        return;
    }
    guard->running = true;
    if (!hashing) {
        bank_hashing(guard, now_ms);
    } else if (!guard->hashing) {
        guard->hashing = true;
        guard->hashing_since_ms = now_ms;
    }
}

void bwg_restart_guard_note_failed_start(bwg_restart_guard_t *guard, uint64_t now_ms)
{
    note_failure(guard, now_ms);
}

void bwg_restart_guard_resume(bwg_restart_guard_t *guard)
{
    guard->consecutive_failures = 0;
    guard->backoff_until_ms = 0;
}

bwg_restart_verdict_t bwg_restart_guard_verdict(const bwg_restart_guard_t *guard, uint64_t now_ms)
{
    if (guard->consecutive_failures >= BWG_RESTART_FAILURES_BEFORE_HOLD) {
        return BWG_RESTART_HELD;
    }
    return now_ms < guard->backoff_until_ms ? BWG_RESTART_BACKOFF : BWG_RESTART_ALLOWED;
}
