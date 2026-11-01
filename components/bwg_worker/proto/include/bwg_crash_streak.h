#ifndef BWG_CRASH_STREAK_H
#define BWG_CRASH_STREAK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * The crash-loop guard's rules. The device boots paused, with the ASIC unpowered, after this many
 * abnormal resets in a row. NVS holds the streak, which survives the brownouts it counts. A boot that
 * stays up long enough clears it, as does an operator's resume.
 */
#define BWG_CRASH_LOOP_RESETS 3u
#define BWG_CRASH_STABLE_UPTIME_MS (10u * 60u * 1000u)

/** The streak after a boot. A panic, a watchdog or a brownout is abnormal; any other reset clears it. */
uint16_t bwg_crash_streak_next(uint16_t previous, bool abnormal_reset);

bool bwg_crash_streak_starts_paused(uint16_t streak);

bool bwg_crash_streak_boot_is_stable(uint64_t uptime_ms);

#endif // BWG_CRASH_STREAK_H
