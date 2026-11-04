#include <string.h>

#include "bwg_ctrl_lifecycle.h"
#include "bwg_test.h"
#include "test_suites.h"

static const bwg_lease_deadlines_t DEADLINES = {.renew_at_ms = 20000, .expires_at_ms = 60000};

static bwg_str_t str_of(const char *text)
{
    return (bwg_str_t){.ptr = text, .len = strlen(text)};
}

static bwg_ctrl_lifecycle_t leased(void)
{
    bwg_ctrl_lifecycle_t lifecycle = {0};
    bwg_ctrl_lifecycle_start_lease(&lifecycle, str_of("lease-1"), str_of("challenge-1"), DEADLINES, true);
    return lifecycle;
}

static bwg_ctrl_lifecycle_t cooling(void)
{
    bwg_ctrl_lifecycle_t lifecycle = {0};
    bwg_ctrl_lifecycle_hold_cooling(&lifecycle);
    return lifecycle;
}

static bwg_ctrl_lifecycle_t rebooted(void)
{
    bwg_ctrl_lifecycle_t lifecycle = {0};
    bwg_ctrl_lifecycle_find_reboot(&lifecycle);
    return lifecycle;
}

static void a_zeroed_lifecycle_is_quiet_with_nothing_to_restore(void)
{
    // Arrange
    const bwg_ctrl_lifecycle_t lifecycle = {0};

    // Act
    const bwg_ctrl_restoration_t restoration = bwg_ctrl_lifecycle_restoration(&lifecycle);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_is_quiet(&lifecycle));
    ASSERT_FALSE(bwg_ctrl_lifecycle_holds_effect(&lifecycle));
    ASSERT_EQ(restoration.status, BWG_RESTORATION_NOT_REQUIRED);
}

static void a_started_lease_is_held_and_running(void)
{
    // Arrange / Act
    const bwg_ctrl_lifecycle_t lifecycle = leased();

    // Assert
    const bwg_ctrl_lease_t *maybe_lease = bwg_ctrl_lifecycle_maybe_running_lease(&lifecycle);
    ASSERT_TRUE(maybe_lease != NULL);
    ASSERT_STREQ(maybe_lease->lease_id, "lease-1");
    ASSERT_STREQ(maybe_lease->challenge_id, "challenge-1");
    ASSERT_EQ(maybe_lease->deadlines.expires_at_ms, 60000u);
    ASSERT_TRUE(maybe_lease->exact_window);
    ASSERT_EQ(bwg_ctrl_lifecycle_restoration(&lifecycle).status, BWG_RESTORATION_PENDING);
}

static void an_overlong_lease_id_is_cut_to_fit(void)
{
    // Arrange
    char long_id[200];
    memset(long_id, 'a', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    bwg_ctrl_lifecycle_t lifecycle = {0};

    // Act
    bwg_ctrl_lifecycle_start_lease(&lifecycle, str_of(long_id), str_of("c"), DEADLINES, false);

    // Assert
    ASSERT_EQ(strlen(bwg_ctrl_lifecycle_maybe_lease(&lifecycle)->lease_id), BWG_CTRL_LEASE_ID_SIZE - 1);
}

static void a_renewal_moves_the_lease_deadlines(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = leased();
    const bwg_lease_deadlines_t later = {.renew_at_ms = 80000, .expires_at_ms = 120000};

    // Act
    bwg_ctrl_lifecycle_renew_lease(&lifecycle, later);

    // Assert
    ASSERT_EQ(bwg_ctrl_lifecycle_maybe_lease(&lifecycle)->deadlines.expires_at_ms, 120000u);
}

static void a_renewal_without_a_lease_changes_nothing(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = cooling();

    // Act
    bwg_ctrl_lifecycle_renew_lease(&lifecycle, DEADLINES);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_cooling_held(&lifecycle));
    ASSERT_TRUE(bwg_ctrl_lifecycle_maybe_lease(&lifecycle) == NULL);
}

static void a_failing_lease_stop_keeps_the_lease_but_stops_running_it(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = leased();
    bwg_reason_t reason = BWG_REASON_PAUSED;

    // Act
    bwg_ctrl_lifecycle_begin_stop(&lifecycle, BWG_REASON_LEASE_EXPIRED);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_stop_pending(&lifecycle, &reason));
    ASSERT_EQ(reason, BWG_REASON_LEASE_EXPIRED);
    ASSERT_TRUE(bwg_ctrl_lifecycle_maybe_lease(&lifecycle) != NULL);
    ASSERT_TRUE(bwg_ctrl_lifecycle_maybe_running_lease(&lifecycle) == NULL);
    ASSERT_STREQ(bwg_ctrl_lifecycle_maybe_lease(&lifecycle)->lease_id, "lease-1");
}

static void a_later_stop_replaces_the_pending_reason(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = leased();
    bwg_ctrl_lifecycle_begin_stop(&lifecycle, BWG_REASON_LEASE_EXPIRED);
    bwg_reason_t reason = BWG_REASON_PAUSED;

    // Act
    bwg_ctrl_lifecycle_begin_stop(&lifecycle, BWG_REASON_CONNECTIVITY_LOST);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_stop_pending(&lifecycle, &reason));
    ASSERT_EQ(reason, BWG_REASON_CONNECTIVITY_LOST);
}

static void a_failing_cooling_stop_is_pending_and_no_longer_held(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = cooling();
    bwg_reason_t reason = BWG_REASON_PAUSED;

    // Act
    bwg_ctrl_lifecycle_begin_stop(&lifecycle, BWG_REASON_CANCELLED);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_stop_pending(&lifecycle, &reason));
    ASSERT_EQ(reason, BWG_REASON_CANCELLED);
    ASSERT_FALSE(bwg_ctrl_lifecycle_cooling_held(&lifecycle));
    ASSERT_TRUE(bwg_ctrl_lifecycle_holds_effect(&lifecycle));
}

static void a_stop_while_idle_has_nothing_pending(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = {0};

    // Act
    bwg_ctrl_lifecycle_begin_stop(&lifecycle, BWG_REASON_CANCELLED);

    // Assert
    ASSERT_FALSE(bwg_ctrl_lifecycle_stop_pending(&lifecycle, NULL));
    ASSERT_TRUE(bwg_ctrl_lifecycle_is_quiet(&lifecycle));
}

static void a_finished_stop_is_restored_with_its_reason(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = leased();
    bwg_ctrl_lifecycle_begin_stop(&lifecycle, BWG_REASON_PAUSED);

    // Act
    bwg_ctrl_lifecycle_finish_stop(&lifecycle, BWG_REASON_PAUSED);

    // Assert
    const bwg_ctrl_restoration_t restoration = bwg_ctrl_lifecycle_restoration(&lifecycle);
    ASSERT_TRUE(bwg_ctrl_lifecycle_is_quiet(&lifecycle));
    ASSERT_TRUE(bwg_ctrl_lifecycle_maybe_lease(&lifecycle) == NULL);
    ASSERT_EQ(restoration.status, BWG_RESTORATION_CONFIRMED);
    ASSERT_EQ(restoration.reason, BWG_REASON_PAUSED);
}

static void an_idle_stop_before_any_effect_is_restored(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = {0};

    // Act
    bwg_ctrl_lifecycle_finish_stop(&lifecycle, BWG_REASON_CANCELLED);

    // Assert
    const bwg_ctrl_restoration_t restoration = bwg_ctrl_lifecycle_restoration(&lifecycle);
    ASSERT_EQ(restoration.status, BWG_RESTORATION_CONFIRMED);
    ASSERT_EQ(restoration.reason, BWG_REASON_CANCELLED);
}

static void a_held_fan_is_pending_restoration(void)
{
    // Arrange / Act
    const bwg_ctrl_lifecycle_t lifecycle = cooling();

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_cooling_held(&lifecycle));
    ASSERT_TRUE(bwg_ctrl_lifecycle_maybe_lease(&lifecycle) == NULL);
    ASSERT_EQ(bwg_ctrl_lifecycle_restoration(&lifecycle).status, BWG_RESTORATION_PENDING);
}

static void a_found_reboot_is_due_and_reported_as_reboot(void)
{
    // Arrange / Act
    const bwg_ctrl_lifecycle_t lifecycle = rebooted();

    // Assert
    const bwg_ctrl_restoration_t restoration = bwg_ctrl_lifecycle_restoration(&lifecycle);
    ASSERT_TRUE(bwg_ctrl_lifecycle_reboot_due(&lifecycle));
    ASSERT_FALSE(bwg_ctrl_lifecycle_is_quiet(&lifecycle));
    ASSERT_FALSE(bwg_ctrl_lifecycle_holds_effect(&lifecycle));
    ASSERT_EQ(restoration.status, BWG_RESTORATION_CONFIRMED);
    ASSERT_EQ(restoration.reason, BWG_REASON_REBOOT);
}

static void an_idle_stop_keeps_a_due_reboot_reported(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = rebooted();

    // Act
    bwg_ctrl_lifecycle_finish_stop(&lifecycle, BWG_REASON_CONNECTIVITY_LOST);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_reboot_due(&lifecycle));
    ASSERT_EQ(bwg_ctrl_lifecycle_restoration(&lifecycle).reason, BWG_REASON_REBOOT);
}

static void a_reboot_report_counts_only_in_its_session(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = rebooted();

    // Act
    const bool noted = bwg_ctrl_lifecycle_note_reboot_reported(&lifecycle, 7);

    // Assert
    ASSERT_TRUE(noted);
    ASSERT_TRUE(bwg_ctrl_lifecycle_reboot_reported_in(&lifecycle, 7));
    ASSERT_FALSE(bwg_ctrl_lifecycle_reboot_reported_in(&lifecycle, 8));
}

static void a_reboot_report_without_a_reboot_is_refused(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = {0};

    // Act
    const bool noted = bwg_ctrl_lifecycle_note_reboot_reported(&lifecycle, 7);

    // Assert
    ASSERT_FALSE(noted);
    ASSERT_TRUE(bwg_ctrl_lifecycle_is_quiet(&lifecycle));
}

static void an_ended_session_forgets_its_reboot_report(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = rebooted();
    ASSERT_TRUE(bwg_ctrl_lifecycle_note_reboot_reported(&lifecycle, 7));

    // Act
    bwg_ctrl_lifecycle_forget_reboot_report(&lifecycle);

    // Assert
    ASSERT_FALSE(bwg_ctrl_lifecycle_reboot_reported_in(&lifecycle, 7));
    ASSERT_TRUE(bwg_ctrl_lifecycle_reboot_due(&lifecycle));
}

static void an_acknowledged_reboot_reports_the_reason_kept_for_it(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = rebooted();
    bwg_ctrl_lifecycle_finish_stop(&lifecycle, BWG_REASON_CONNECTIVITY_LOST);
    ASSERT_TRUE(bwg_ctrl_lifecycle_note_reboot_reported(&lifecycle, 7));

    // Act
    bwg_ctrl_lifecycle_acknowledge_reboot(&lifecycle);

    // Assert
    const bwg_ctrl_restoration_t restoration = bwg_ctrl_lifecycle_restoration(&lifecycle);
    ASSERT_TRUE(bwg_ctrl_lifecycle_is_quiet(&lifecycle));
    ASSERT_EQ(restoration.status, BWG_RESTORATION_CONFIRMED);
    ASSERT_EQ(restoration.reason, BWG_REASON_CONNECTIVITY_LOST);
}

static void an_unreported_reboot_is_not_acknowledged(void)
{
    // Arrange
    bwg_ctrl_lifecycle_t lifecycle = rebooted();

    // Act
    bwg_ctrl_lifecycle_acknowledge_reboot(&lifecycle);

    // Assert
    ASSERT_TRUE(bwg_ctrl_lifecycle_reboot_due(&lifecycle));
}

void bwg_tests_ctrl_lifecycle(void)
{
    BWG_RUN(a_zeroed_lifecycle_is_quiet_with_nothing_to_restore);
    BWG_RUN(a_started_lease_is_held_and_running);
    BWG_RUN(an_overlong_lease_id_is_cut_to_fit);
    BWG_RUN(a_renewal_moves_the_lease_deadlines);
    BWG_RUN(a_renewal_without_a_lease_changes_nothing);
    BWG_RUN(a_failing_lease_stop_keeps_the_lease_but_stops_running_it);
    BWG_RUN(a_later_stop_replaces_the_pending_reason);
    BWG_RUN(a_failing_cooling_stop_is_pending_and_no_longer_held);
    BWG_RUN(a_stop_while_idle_has_nothing_pending);
    BWG_RUN(a_finished_stop_is_restored_with_its_reason);
    BWG_RUN(an_idle_stop_before_any_effect_is_restored);
    BWG_RUN(a_held_fan_is_pending_restoration);
    BWG_RUN(a_found_reboot_is_due_and_reported_as_reboot);
    BWG_RUN(an_idle_stop_keeps_a_due_reboot_reported);
    BWG_RUN(a_reboot_report_counts_only_in_its_session);
    BWG_RUN(a_reboot_report_without_a_reboot_is_refused);
    BWG_RUN(an_ended_session_forgets_its_reboot_report);
    BWG_RUN(an_acknowledged_reboot_reports_the_reason_kept_for_it);
    BWG_RUN(an_unreported_reboot_is_not_acknowledged);
}
