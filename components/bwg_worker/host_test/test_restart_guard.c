#include "bwg_restart_guard.h"
#include "bwg_test.h"
#include "test_suites.h"

#define SECOND_MS 1000u

static void end_failed_run(bwg_restart_guard_t *guard, uint64_t now_ms)
{
    bwg_restart_guard_observe(guard, true, false, false, now_ms);
    bwg_restart_guard_observe(guard, false, false, true, now_ms);
}

static void a_new_guard_allows_a_run(void)
{
    // Arrange
    const bwg_restart_guard_t guard = {0};

    // Act / Assert
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, 0), BWG_RESTART_ALLOWED);
}

static void a_failed_run_backs_off_for_30_seconds(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};

    // Act
    end_failed_run(&guard, 1000);

    // Assert
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, 1000 + 30 * SECOND_MS - 1), BWG_RESTART_BACKOFF);
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, 1000 + 30 * SECOND_MS), BWG_RESTART_ALLOWED);
}

static void each_further_failure_doubles_the_backoff_up_to_240_seconds(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    const uint32_t expected_seconds[] = {30, 60, 120, 240};

    for (uint32_t i = 0; i < 4; i++) {
        // Act
        end_failed_run(&guard, 0);

        // Assert
        ASSERT_EQ(bwg_restart_guard_verdict(&guard, expected_seconds[i] * SECOND_MS - 1), BWG_RESTART_BACKOFF);
        ASSERT_EQ(bwg_restart_guard_verdict(&guard, expected_seconds[i] * SECOND_MS), BWG_RESTART_ALLOWED);
    }
}

static void the_fifth_failure_holds_however_long_it_waits(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};

    // Act
    for (uint32_t i = 0; i < BWG_RESTART_FAILURES_BEFORE_HOLD; i++) {
        end_failed_run(&guard, 0);
    }

    // Assert
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, UINT32_MAX), BWG_RESTART_HELD);
}

static void an_operators_resume_ends_the_hold(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    for (uint32_t i = 0; i < BWG_RESTART_FAILURES_BEFORE_HOLD; i++) {
        end_failed_run(&guard, 0);
    }

    // Act
    bwg_restart_guard_resume(&guard);

    // Assert
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, 0), BWG_RESTART_ALLOWED);
    ASSERT_EQ(guard.consecutive_failures, 0);
}

static void a_requested_ending_spends_nothing(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    bwg_restart_guard_observe(&guard, true, true, false, 0);

    // Act
    bwg_restart_guard_observe(&guard, false, false, false, 5 * SECOND_MS);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 0);
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, 5 * SECOND_MS), BWG_RESTART_ALLOWED);
}

static void a_requested_ending_keeps_earlier_failures(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    bwg_restart_guard_observe(&guard, true, true, false, 40 * SECOND_MS);

    // Act
    bwg_restart_guard_observe(&guard, false, false, false, 50 * SECOND_MS);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 1);
}

static void a_stable_run_that_then_fails_counts_as_the_first(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    end_failed_run(&guard, 0);
    bwg_restart_guard_observe(&guard, true, true, false, 0);

    // Act
    bwg_restart_guard_observe(&guard, false, false, true, BWG_RESTART_STABLE_RUN_MS);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 1);
}

static void a_stable_run_that_ends_on_request_clears_the_count(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    bwg_restart_guard_observe(&guard, true, true, false, 0);

    // Act
    bwg_restart_guard_observe(&guard, false, false, false, BWG_RESTART_STABLE_RUN_MS);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 0);
}

static void a_run_just_short_of_stable_keeps_the_count(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    bwg_restart_guard_observe(&guard, true, true, false, 0);

    // Act
    bwg_restart_guard_observe(&guard, false, false, true, BWG_RESTART_STABLE_RUN_MS - 1);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 2);
}

static void time_without_work_is_not_hashing(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    bwg_restart_guard_observe(&guard, true, false, false, 0);

    // Act
    bwg_restart_guard_observe(&guard, false, false, true, BWG_RESTART_STABLE_RUN_MS);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 2);
}

static void hashing_adds_up_across_a_gap_in_one_run(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    const uint64_t half = BWG_RESTART_STABLE_RUN_MS / 2;
    bwg_restart_guard_observe(&guard, true, true, false, 0);
    bwg_restart_guard_observe(&guard, true, false, false, half);
    bwg_restart_guard_observe(&guard, true, true, false, half + 60 * SECOND_MS);

    // Act
    bwg_restart_guard_observe(&guard, false, false, false, half + 60 * SECOND_MS + half);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 0);
}

static void hashing_from_one_run_does_not_carry_into_the_next(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);
    const uint64_t most = BWG_RESTART_STABLE_RUN_MS - 1;
    bwg_restart_guard_observe(&guard, true, true, false, 0);
    bwg_restart_guard_observe(&guard, false, false, false, most);
    bwg_restart_guard_observe(&guard, true, true, false, most);

    // Act
    bwg_restart_guard_observe(&guard, false, false, false, most + 1000);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 1);
}

static void a_failed_start_spends_the_guard_like_a_failed_run(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};

    // Act
    bwg_restart_guard_note_failed_start(&guard, 0);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 1);
    ASSERT_EQ(bwg_restart_guard_verdict(&guard, 30 * SECOND_MS - 1), BWG_RESTART_BACKOFF);
}

static void staying_stopped_ends_no_further_runs(void)
{
    // Arrange
    bwg_restart_guard_t guard = {0};
    end_failed_run(&guard, 0);

    // Act
    bwg_restart_guard_observe(&guard, false, false, true, 1000);
    bwg_restart_guard_observe(&guard, false, false, true, 2000);

    // Assert
    ASSERT_EQ(guard.consecutive_failures, 1);
}

void bwg_tests_restart_guard(void)
{
    BWG_RUN(a_new_guard_allows_a_run);
    BWG_RUN(a_failed_run_backs_off_for_30_seconds);
    BWG_RUN(each_further_failure_doubles_the_backoff_up_to_240_seconds);
    BWG_RUN(the_fifth_failure_holds_however_long_it_waits);
    BWG_RUN(an_operators_resume_ends_the_hold);
    BWG_RUN(a_requested_ending_spends_nothing);
    BWG_RUN(a_requested_ending_keeps_earlier_failures);
    BWG_RUN(a_stable_run_that_then_fails_counts_as_the_first);
    BWG_RUN(a_stable_run_that_ends_on_request_clears_the_count);
    BWG_RUN(a_run_just_short_of_stable_keeps_the_count);
    BWG_RUN(time_without_work_is_not_hashing);
    BWG_RUN(hashing_adds_up_across_a_gap_in_one_run);
    BWG_RUN(hashing_from_one_run_does_not_carry_into_the_next);
    BWG_RUN(a_failed_start_spends_the_guard_like_a_failed_run);
    BWG_RUN(staying_stopped_ends_no_further_runs);
}
