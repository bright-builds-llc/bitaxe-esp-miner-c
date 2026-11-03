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

#endif // BWG_MINING_RULES_H
