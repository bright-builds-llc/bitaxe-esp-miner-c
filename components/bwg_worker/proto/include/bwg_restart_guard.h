#ifndef BWG_RESTART_GUARD_H
#define BWG_RESTART_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/**
 * When local mining may begin another run, from ADR-0038's restart guard. Upstream retries forever; here a run that
 * fails backs off, doubling from 30 s, and after five failures in a row mining holds until an operator resumes. Ten
 * minutes of hashing in a run starts the count over. It lives in RAM, so a reboot starts over too.
 */
#define BWG_RESTART_BACKOFF_BASE_MS 30000u
#define BWG_RESTART_FAILURES_BEFORE_HOLD 5u
#define BWG_RESTART_STABLE_RUN_MS (10u * 60u * 1000u)

typedef enum {
    BWG_RESTART_ALLOWED = 0,
    BWG_RESTART_BACKOFF,
    BWG_RESTART_HELD,
} bwg_restart_verdict_t;

typedef struct {
    uint32_t consecutive_failures;
    uint64_t backoff_until_ms;
    bool running;
    bool hashing;
    uint64_t hashing_since_ms;
    uint64_t hashed_ms;
} bwg_restart_guard_t;

/**
 * One tick's facts about local mining. A run ends when `running` falls, and `pool_failed` says it ended because its
 * pools failed. Any other ending was requested or environmental: a pause, a Gate link or a lost network.
 */
void bwg_restart_guard_observe(bwg_restart_guard_t *guard, bool running, bool hashing, bool pool_failed,
                               uint64_t now_ms);

/** A run's start failed, so it never ran. */
void bwg_restart_guard_note_failed_start(bwg_restart_guard_t *guard, uint64_t now_ms);

/** An operator resumed, so the count starts over. */
void bwg_restart_guard_resume(bwg_restart_guard_t *guard);

bwg_restart_verdict_t bwg_restart_guard_verdict(const bwg_restart_guard_t *guard, uint64_t now_ms);

#endif // BWG_RESTART_GUARD_H
