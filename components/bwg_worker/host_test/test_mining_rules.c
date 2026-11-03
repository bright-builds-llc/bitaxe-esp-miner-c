#include "bwg_mining_rules.h"
#include "bwg_test.h"
#include "test_suites.h"

static bwg_lease_watch_facts_t safe_running(void)
{
    return (bwg_lease_watch_facts_t){
        .chip_ready = true,
        .settings_admissible = true,
        .limits_known = true,
        .sample_safe = true,
        .since_safe_ms = 0,
    };
}

static void a_wanted_stop_reads_as_stopping_until_it_finishes(void)
{
    // Arrange
    const bwg_activity_facts_t facts = {.stop_wanted = true, .leased = true, .lease_has_work = true, .overheat = true};

    // Act
    const bwg_activity_t activity = bwg_activity_of(&facts);

    // Assert
    ASSERT_EQ(activity, BWG_ACTIVITY_STOPPING);
}

static void a_finished_stop_reads_as_paused(void)
{
    // Arrange
    const bwg_activity_facts_t facts = {.stop_wanted = true, .stopped = true};

    // Act
    const bwg_activity_t activity = bwg_activity_of(&facts);

    // Assert
    ASSERT_EQ(activity, BWG_ACTIVITY_PAUSED);
}

static void each_safety_hold_reads_as_safe_blocked(void)
{
    // Arrange
    const bwg_activity_facts_t holds[] = {
        {.hardware_fault = true},
        {.overheat = true},
        {.pools_unavailable = true},
        {.backing_off = true},
    };

    for (size_t i = 0; i < sizeof(holds) / sizeof(holds[0]); i++) {
        // Act
        const bwg_activity_t activity = bwg_activity_of(&holds[i]);

        // Assert
        ASSERT_EQ(activity, BWG_ACTIVITY_SAFE_BLOCKED);
    }
}

static void a_lease_with_work_is_active_on_a_paused_board(void)
{
    // Arrange
    const bwg_activity_facts_t facts = {.leased = true, .lease_has_work = true, .operator_paused = true};

    // Act
    const bwg_activity_t activity = bwg_activity_of(&facts);

    // Assert
    ASSERT_EQ(activity, BWG_ACTIVITY_ACTIVE);
}

static void a_lease_before_work_on_a_paused_board_reads_as_paused(void)
{
    // Arrange
    const bwg_activity_facts_t facts = {.leased = true, .operator_paused = true};

    // Act
    const bwg_activity_t activity = bwg_activity_of(&facts);

    // Assert
    ASSERT_EQ(activity, BWG_ACTIVITY_PAUSED);
}

static void local_mining_with_nothing_holding_it_is_active(void)
{
    // Arrange
    const bwg_activity_facts_t facts = {0};

    // Act
    const bwg_activity_t activity = bwg_activity_of(&facts);

    // Assert
    ASSERT_EQ(activity, BWG_ACTIVITY_ACTIVE);
}

static void activities_use_the_api_names(void)
{
    // Act / Assert
    ASSERT_STREQ(bwg_activity_name(BWG_ACTIVITY_ACTIVE), "active");
    ASSERT_STREQ(bwg_activity_name(BWG_ACTIVITY_PAUSED), "paused");
    ASSERT_STREQ(bwg_activity_name(BWG_ACTIVITY_STOPPING), "stopping");
    ASSERT_STREQ(bwg_activity_name(BWG_ACTIVITY_SAFE_BLOCKED), "safe_blocked");
}

static void a_starting_chip_restarts_the_safe_clock_whatever_it_reads(void)
{
    // Arrange
    bwg_lease_watch_facts_t facts = safe_running();
    facts.chip_ready = false;
    facts.settings_admissible = false;
    facts.sample_safe = false;
    facts.since_safe_ms = 60000;

    // Act
    const bwg_lease_watch_t watch = bwg_lease_watch(&facts);

    // Assert
    ASSERT_EQ(watch, BWG_LEASE_WATCH_STARTING);
}

static void inadmissible_settings_end_the_lease_even_when_the_sample_is_safe(void)
{
    // Arrange
    bwg_lease_watch_facts_t facts = safe_running();
    facts.settings_admissible = false;

    // Act
    const bwg_lease_watch_t watch = bwg_lease_watch(&facts);

    // Assert
    ASSERT_EQ(watch, BWG_LEASE_WATCH_UNSAFE_SETTINGS);
}

static void a_board_without_limits_is_left_alone(void)
{
    // Arrange
    bwg_lease_watch_facts_t facts = safe_running();
    facts.limits_known = false;
    facts.sample_safe = false;
    facts.since_safe_ms = 60000;

    // Act
    const bwg_lease_watch_t watch = bwg_lease_watch(&facts);

    // Assert
    ASSERT_EQ(watch, BWG_LEASE_WATCH_NO_LIMITS);
}

static void a_safe_sample_is_safe(void)
{
    // Arrange
    const bwg_lease_watch_facts_t facts = safe_running();

    // Act
    const bwg_lease_watch_t watch = bwg_lease_watch(&facts);

    // Assert
    ASSERT_EQ(watch, BWG_LEASE_WATCH_SAFE);
}

static void an_unsafe_sample_ends_the_lease_once_a_second_has_passed(void)
{
    // Arrange
    bwg_lease_watch_facts_t facts = safe_running();
    facts.sample_safe = false;

    // Act
    facts.since_safe_ms = BWG_SAFE_SAMPLE_MAX_AGE_MS - 1;
    const bwg_lease_watch_t just_before = bwg_lease_watch(&facts);
    facts.since_safe_ms = BWG_SAFE_SAMPLE_MAX_AGE_MS;
    const bwg_lease_watch_t at_limit = bwg_lease_watch(&facts);

    // Assert
    ASSERT_EQ(just_before, BWG_LEASE_WATCH_UNSAFE_FOR_NOW);
    ASSERT_EQ(at_limit, BWG_LEASE_WATCH_UNSAFE_TOO_LONG);
}

static void session_growth_is_the_rise_since_last_seen(void)
{
    // Act / Assert
    ASSERT_EQ(bwg_session_growth(5, 9), 4u);
}

static void a_reconnect_counts_its_whole_session_as_new(void)
{
    // Act / Assert
    ASSERT_EQ(bwg_session_growth(9, 2), 2u);
}

static void cooling_is_proven_by_three_cool_readings_in_a_row(void)
{
    // Arrange
    uint32_t streak = 0;

    // Act
    streak = bwg_cool_streak_next(streak, true);
    streak = bwg_cool_streak_next(streak, true);
    const bool after_two = bwg_cool_streak_proves_cooling(streak);
    streak = bwg_cool_streak_next(streak, true);

    // Assert
    ASSERT_FALSE(after_two);
    ASSERT_TRUE(bwg_cool_streak_proves_cooling(streak));
}

static void a_warm_reading_starts_the_cool_streak_over(void)
{
    // Arrange
    const uint32_t streak = 2;

    // Act
    const uint32_t next = bwg_cool_streak_next(streak, false);

    // Assert
    ASSERT_EQ(next, 0u);
}

static void the_cool_streak_stops_at_its_limit(void)
{
    // Act / Assert
    ASSERT_EQ(bwg_cool_streak_next(UINT32_MAX, true), UINT32_MAX);
}

void bwg_tests_mining_rules(void)
{
    BWG_RUN(a_wanted_stop_reads_as_stopping_until_it_finishes);
    BWG_RUN(a_finished_stop_reads_as_paused);
    BWG_RUN(each_safety_hold_reads_as_safe_blocked);
    BWG_RUN(a_lease_with_work_is_active_on_a_paused_board);
    BWG_RUN(a_lease_before_work_on_a_paused_board_reads_as_paused);
    BWG_RUN(local_mining_with_nothing_holding_it_is_active);
    BWG_RUN(activities_use_the_api_names);
    BWG_RUN(a_starting_chip_restarts_the_safe_clock_whatever_it_reads);
    BWG_RUN(inadmissible_settings_end_the_lease_even_when_the_sample_is_safe);
    BWG_RUN(a_board_without_limits_is_left_alone);
    BWG_RUN(a_safe_sample_is_safe);
    BWG_RUN(an_unsafe_sample_ends_the_lease_once_a_second_has_passed);
    BWG_RUN(session_growth_is_the_rise_since_last_seen);
    BWG_RUN(a_reconnect_counts_its_whole_session_as_new);
    BWG_RUN(cooling_is_proven_by_three_cool_readings_in_a_row);
    BWG_RUN(a_warm_reading_starts_the_cool_streak_over);
    BWG_RUN(the_cool_streak_stops_at_its_limit);
}
