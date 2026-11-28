#ifndef BWG_MINING_RULES_H
#define BWG_MINING_RULES_H

#include <stdbool.h>
#include <stdint.h>

/** The decisions `main/bwg` makes about mining, apart from the reads and effects around them. */

/** A lease with no safe sample for this long ends. */
#define BWG_SAFE_SAMPLE_MAX_AGE_MS 1000
/** The sensor's whole degrees flicker at the limit, so cooling counts as proven after this many cool readings in a row. */
#define BWG_COOL_READINGS_TO_PROVE 3u

typedef enum {
    BWG_ACTIVITY_ACTIVE = 0,
    BWG_ACTIVITY_PAUSED,
    BWG_ACTIVITY_STOPPING,
    BWG_ACTIVITY_SAFE_BLOCKED,
} bwg_activity_t;

typedef struct {
    bool stop_wanted;
    bool stopped;
    bool leased;
    bool lease_has_work;
    bool hardware_fault;
    bool overheat;
    bool pools_unavailable;
    bool backing_off;
    bool operator_paused;
} bwg_activity_facts_t;

bwg_activity_t bwg_activity_of(const bwg_activity_facts_t *facts);

/** The API's `miningActivity` name. */
const char *bwg_activity_name(bwg_activity_t activity);

typedef enum {
    /** The chip isn't up yet, so its readings mean nothing and the safe clock restarts. */
    BWG_LEASE_WATCH_STARTING = 0,
    BWG_LEASE_WATCH_UNSAFE_SETTINGS,
    /** No limits to judge by. Admissible settings imply limits, so this can't happen once the settings pass. */
    BWG_LEASE_WATCH_NO_LIMITS,
    BWG_LEASE_WATCH_SAFE,
    BWG_LEASE_WATCH_UNSAFE_FOR_NOW,
    BWG_LEASE_WATCH_UNSAFE_TOO_LONG,
} bwg_lease_watch_t;

typedef struct {
    bool chip_ready;
    bool settings_admissible;
    bool limits_known;
    bool sample_safe;
    uint64_t since_safe_ms;
} bwg_lease_watch_facts_t;

/** What one poll of a lease's readings means. The lease ends on unsafe settings or a sample unsafe for too long. */
bwg_lease_watch_t bwg_lease_watch(const bwg_lease_watch_facts_t *facts);

/**
 * How far a pool session's share count moved since `seen`. A reconnect starts the count over, so a smaller
 * count is all new.
 */
uint32_t bwg_session_growth(uint32_t seen, uint32_t session);

/** The run of cool readings after this one. Any warm reading starts it over. */
uint32_t bwg_cool_streak_next(uint32_t streak, bool cool);

bool bwg_cool_streak_proves_cooling(uint32_t streak);

/**
 * A lease's pool connection that brings no work this long after it opens is left, the same limit a Start has. A pool
 * that accepts a connection and never answers would otherwise hold the board for the rest of the lease.
 */
#define BWG_POOL_FIRST_WORK_MS 25000u

/**
 * A lease's pool connection that has answered none of its shares for this long is left. A pool that stops reading
 * is otherwise noticed only once the send buffer fills, which at a low share rate can take minutes.
 */
#define BWG_POOL_ANSWER_MS 30000u

typedef struct {
    bool lease_pool;
    /** Work arrived since the connection opened. */
    bool had_work;
    uint64_t open_ms;
    /** Stratum V1 answers each share, where a V2 pool may acknowledge them in batches, so only V1's wait counts. */
    bool answers_each_share;
    /** How long a submitted share has waited with no answer to any share, or 0 when none waits. */
    uint64_t unanswered_ms;
} bwg_pool_session_facts_t;

/** Whether to leave a pool connection that has brought no work in time or stopped answering. Only a lease's is left. */
bool bwg_pool_session_overdue(const bwg_pool_session_facts_t *facts);

/** When a pool session's shares began waiting for an answer, from its share counts as they're polled. */
typedef struct {
    uint64_t answered;
    /** 0 while no share waits. */
    uint64_t waiting_since_ms;
} bwg_answer_watch_t;

/**
 * Takes one poll of a session's counts. Any change in the answered count, including a new session's count starting
 * over, restarts the wait, so a pool that drops one answer but goes on answering isn't left.
 */
void bwg_answer_watch_note(bwg_answer_watch_t *watch, uint64_t submitted, uint64_t answered, uint64_t now_ms);

/** How long shares have waited with no answer, or 0. */
uint64_t bwg_answer_watch_waiting_ms(const bwg_answer_watch_t *watch, uint64_t now_ms);

/**
 * Whether a finished pool session counts toward the failures that mark the pools unavailable. Upstream counts one
 * that ended in error without work. A lease's session that brought no work failed however it ended, since leaving
 * a stalled or silent pool ends it cleanly.
 */
bool bwg_pool_session_failed(bool lease_pool, bool ended_ok, bool had_work);

#endif // BWG_MINING_RULES_H
