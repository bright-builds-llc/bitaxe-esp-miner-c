#include "bwg_crash_streak.h"

uint16_t bwg_crash_streak_next(uint16_t previous, bool abnormal_reset)
{
    if (!abnormal_reset) {
        return 0;
    }
    return previous == UINT16_MAX ? previous : (uint16_t) (previous + 1);
}

bool bwg_crash_streak_starts_paused(uint16_t streak)
{
    return streak >= BWG_CRASH_LOOP_RESETS;
}

bool bwg_crash_streak_boot_is_stable(uint64_t uptime_ms)
{
    return uptime_ms >= BWG_CRASH_STABLE_UPTIME_MS;
}
