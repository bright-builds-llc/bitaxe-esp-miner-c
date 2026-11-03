#include "bwg_mining_rules.h"

bwg_activity_t bwg_activity_of(const bwg_activity_facts_t *facts)
{
    if (facts->stop_wanted && !facts->stopped) {
        return BWG_ACTIVITY_STOPPING;
    }
    if (facts->hardware_fault || facts->overheat || facts->pools_unavailable || facts->backing_off) {
        return BWG_ACTIVITY_SAFE_BLOCKED;
    }
    // A lease mines from a paused board, and the pause only returns once the lease ends.
    if (facts->leased && facts->lease_has_work) {
        return BWG_ACTIVITY_ACTIVE;
    }
    return facts->operator_paused || facts->stopped ? BWG_ACTIVITY_PAUSED : BWG_ACTIVITY_ACTIVE;
}

const char *bwg_activity_name(bwg_activity_t activity)
{
    switch (activity) {
    case BWG_ACTIVITY_PAUSED:
        return "paused";
    case BWG_ACTIVITY_STOPPING:
        return "stopping";
    case BWG_ACTIVITY_SAFE_BLOCKED:
        return "safe_blocked";
    case BWG_ACTIVITY_ACTIVE:
    default:
        return "active";
    }
}

bwg_lease_watch_t bwg_lease_watch(const bwg_lease_watch_facts_t *facts)
{
    if (!facts->chip_ready) {
        return BWG_LEASE_WATCH_STARTING;
    }
    if (!facts->settings_admissible) {
        return BWG_LEASE_WATCH_UNSAFE_SETTINGS;
    }
    if (!facts->limits_known) {
        return BWG_LEASE_WATCH_NO_LIMITS;
    }
    if (facts->sample_safe) {
        return BWG_LEASE_WATCH_SAFE;
    }
    return facts->since_safe_ms >= BWG_SAFE_SAMPLE_MAX_AGE_MS ?BWG_LEASE_WATCH_UNSAFE_TOO_LONG : BWG_LEASE_WATCH_UNSAFE_FOR_NOW;
}

uint32_t bwg_session_growth(uint32_t seen, uint32_t session)
{
    return session >= seen ? session - seen : session;
}

uint32_t bwg_cool_streak_next(uint32_t streak, bool cool)
{
    if (!cool) {
        return 0;
    }
    return streak == UINT32_MAX ? streak : streak + 1;
}

bool bwg_cool_streak_proves_cooling(uint32_t streak)
{
    return streak >= BWG_COOL_READINGS_TO_PROVE;
}
