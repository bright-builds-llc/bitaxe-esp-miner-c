#include "bwg_crash_streak.h"
#include "bwg_test.h"
#include "test_suites.h"

static void an_abnormal_reset_extends_the_streak(void)
{
    ASSERT_EQ(bwg_crash_streak_next(2, true), 3);
}

static void any_other_reset_clears_the_streak(void)
{
    ASSERT_EQ(bwg_crash_streak_next(2, false), 0);
}

static void the_streak_stops_at_its_limit(void)
{
    ASSERT_EQ(bwg_crash_streak_next(UINT16_MAX, true), UINT16_MAX);
}

static void the_third_abnormal_reset_starts_paused(void)
{
    ASSERT_FALSE(bwg_crash_streak_starts_paused(2));
    ASSERT_TRUE(bwg_crash_streak_starts_paused(3));
}

static void ten_minutes_up_makes_a_boot_stable(void)
{
    ASSERT_FALSE(bwg_crash_streak_boot_is_stable(599999));
    ASSERT_TRUE(bwg_crash_streak_boot_is_stable(600000));
}

void bwg_tests_crash_streak(void)
{
    BWG_RUN(an_abnormal_reset_extends_the_streak);
    BWG_RUN(any_other_reset_clears_the_streak);
    BWG_RUN(the_streak_stops_at_its_limit);
    BWG_RUN(the_third_abnormal_reset_starts_paused);
    BWG_RUN(ten_minutes_up_makes_a_boot_stable);
}
