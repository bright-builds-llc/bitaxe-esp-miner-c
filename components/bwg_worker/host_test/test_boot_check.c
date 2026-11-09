#include "bwg_boot_check.h"
#include "bwg_test.h"
#include "test_suites.h"

static bwg_boot_facts_t pending(void)
{
    return (bwg_boot_facts_t) {
        .state = BWG_IMAGE_PENDING_VERIFY,
        .link_required = true,
        .link_ready = true,
        .settings_readable = true,
        .elapsed_ms = 1000,
    };
}

static void a_pending_image_without_its_link_by_the_deadline_rolls_back(void)
{
    // Arrange
    bwg_boot_facts_t facts = pending();
    facts.link_ready = false;
    facts.elapsed_ms = BWG_BOOT_CHECK_DEADLINE_MS;

    // Act
    bwg_boot_verdict_t verdict = bwg_boot_check_verdict(&facts);

    // Assert
    ASSERT_EQ(verdict, BWG_BOOT_ROLL_BACK);
}

static void a_pending_image_waits_for_its_link_before_the_deadline(void)
{
    // Arrange
    bwg_boot_facts_t facts = pending();
    facts.link_ready = false;
    facts.elapsed_ms = BWG_BOOT_CHECK_DEADLINE_MS - 1;

    // Act
    bwg_boot_verdict_t verdict = bwg_boot_check_verdict(&facts);

    // Assert
    ASSERT_EQ(verdict, BWG_BOOT_WAIT);
}

static void a_pending_image_with_unreadable_settings_rolls_back_at_once(void)
{
    // Arrange
    bwg_boot_facts_t facts = pending();
    facts.settings_readable = false;

    // Act
    bwg_boot_verdict_t verdict = bwg_boot_check_verdict(&facts);

    // Assert
    ASSERT_EQ(verdict, BWG_BOOT_ROLL_BACK);
}

static void a_pending_image_that_passes_is_marked_valid(void)
{
    // Arrange
    bwg_boot_facts_t facts = pending();

    // Act
    bwg_boot_verdict_t verdict = bwg_boot_check_verdict(&facts);

    // Assert
    ASSERT_EQ(verdict, BWG_BOOT_MARK_VALID);
}

static void without_a_required_link_settings_alone_decide(void)
{
    // Arrange
    bwg_boot_facts_t facts = pending();
    facts.link_required = false;
    facts.link_ready = false;

    // Act
    bwg_boot_verdict_t verdict = bwg_boot_check_verdict(&facts);

    // Assert
    ASSERT_EQ(verdict, BWG_BOOT_MARK_VALID);
}

static void a_new_image_under_an_old_bootloader_is_judged_too(void)
{
    // Arrange
    bwg_boot_facts_t facts = pending();
    facts.state = BWG_IMAGE_NEW;
    facts.link_ready = false;
    facts.elapsed_ms = BWG_BOOT_CHECK_DEADLINE_MS;

    // Act
    bwg_boot_verdict_t verdict = bwg_boot_check_verdict(&facts);

    // Assert
    ASSERT_EQ(verdict, BWG_BOOT_ROLL_BACK);
}

static void a_valid_or_factory_image_is_left_alone(void)
{
    // Arrange
    bwg_boot_facts_t valid = pending();
    valid.state = BWG_IMAGE_VALID;
    valid.settings_readable = false;
    bwg_boot_facts_t factory = valid;
    factory.state = BWG_IMAGE_UNDEFINED;

    // Act
    bwg_boot_verdict_t valid_verdict = bwg_boot_check_verdict(&valid);
    bwg_boot_verdict_t factory_verdict = bwg_boot_check_verdict(&factory);

    // Assert
    ASSERT_EQ(valid_verdict, BWG_BOOT_NOTHING);
    ASSERT_EQ(factory_verdict, BWG_BOOT_NOTHING);
}

void bwg_tests_boot_check(void)
{
    BWG_RUN(a_pending_image_without_its_link_by_the_deadline_rolls_back);
    BWG_RUN(a_pending_image_waits_for_its_link_before_the_deadline);
    BWG_RUN(a_pending_image_with_unreadable_settings_rolls_back_at_once);
    BWG_RUN(a_pending_image_that_passes_is_marked_valid);
    BWG_RUN(without_a_required_link_settings_alone_decide);
    BWG_RUN(a_new_image_under_an_old_bootloader_is_judged_too);
    BWG_RUN(a_valid_or_factory_image_is_left_alone);
}
