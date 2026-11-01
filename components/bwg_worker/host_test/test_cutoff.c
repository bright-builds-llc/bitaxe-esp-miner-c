#include "bwg_cutoff.h"
#include "bwg_test.h"
#include "test_suites.h"

#define RESET_PIN 1u
#define ENABLE_PIN 10u
#define RESET_BIT (1u << RESET_PIN)
#define ENABLE_BIT (1u << ENABLE_PIN)

static void an_active_low_enable_is_set_and_reset_is_cleared(void)
{
    // Act
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, true, ENABLE_PIN, false);

    // Assert
    ASSERT_EQ(plan.set_mask, ENABLE_BIT);
    ASSERT_EQ(plan.clear_mask, RESET_BIT);
}

static void an_active_high_enable_is_cleared_with_reset(void)
{
    // Act
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, true, ENABLE_PIN, true);

    // Assert
    ASSERT_EQ(plan.set_mask, 0);
    ASSERT_EQ(plan.clear_mask, RESET_BIT | ENABLE_BIT);
}

static void a_board_without_an_enable_pin_only_clears_reset(void)
{
    // Act
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, false, ENABLE_PIN, false);

    // Assert
    ASSERT_EQ(plan.set_mask, 0);
    ASSERT_EQ(plan.clear_mask, RESET_BIT);
}

static void outputs_are_off_when_driven_at_the_plan(void)
{
    // Arrange
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, true, ENABLE_PIN, false);

    // Act
    const bool off = bwg_cutoff_outputs_off(plan, RESET_BIT | ENABLE_BIT, ENABLE_BIT);

    // Assert
    ASSERT_TRUE(off);
}

static void a_pin_not_driven_as_an_output_proves_nothing(void)
{
    // Arrange
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, true, ENABLE_PIN, false);

    // Act
    const bool reset_undriven = bwg_cutoff_outputs_off(plan, ENABLE_BIT, ENABLE_BIT);
    const bool enable_undriven = bwg_cutoff_outputs_off(plan, RESET_BIT, ENABLE_BIT);

    // Assert
    ASSERT_FALSE(reset_undriven);
    ASSERT_FALSE(enable_undriven);
}

static void a_powered_or_released_latch_is_not_off(void)
{
    // Arrange
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, true, ENABLE_PIN, false);
    const uint32_t driven = RESET_BIT | ENABLE_BIT;

    // Act
    const bool powered = bwg_cutoff_outputs_off(plan, driven, 0);
    const bool released = bwg_cutoff_outputs_off(plan, driven, RESET_BIT | ENABLE_BIT);

    // Assert
    ASSERT_FALSE(powered);
    ASSERT_FALSE(released);
}

static void other_pins_do_not_affect_the_proof(void)
{
    // Arrange
    const bwg_cutoff_plan_t plan = bwg_cutoff_plan(RESET_PIN, true, ENABLE_PIN, false);

    // Act
    const bool off = bwg_cutoff_outputs_off(plan, UINT32_MAX, ENABLE_BIT | (1u << 4));

    // Assert
    ASSERT_TRUE(off);
}

void bwg_tests_cutoff(void)
{
    BWG_RUN(an_active_low_enable_is_set_and_reset_is_cleared);
    BWG_RUN(an_active_high_enable_is_cleared_with_reset);
    BWG_RUN(a_board_without_an_enable_pin_only_clears_reset);
    BWG_RUN(outputs_are_off_when_driven_at_the_plan);
    BWG_RUN(a_pin_not_driven_as_an_output_proves_nothing);
    BWG_RUN(a_powered_or_released_latch_is_not_off);
    BWG_RUN(other_pins_do_not_affect_the_proof);
}
