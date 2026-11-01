#include <math.h>

#include "unity.h"

#include "bwg_limits.h"
#include "bwg_link_presence.h"

static bwg_board_limits_t limits_205(void)
{
    bwg_board_limits_t limits = {0};
    TEST_ASSERT_TRUE(bwg_board_limits_for("205", 12, &limits));
    return limits;
}

static bwg_safety_sample_t safe_sample(void)
{
    return (bwg_safety_sample_t) {
        .input_volts = 5.1f,
        .power_watts = 12.5f,
        .chip_celsius = 60.0f,
        .fan_rpm = 4000,
    };
}

TEST_CASE("Only the Gate's closed board set gets limits", "[bwg_limits]")
{
    // Arrange
    bwg_board_limits_t limits = {0};

    // Act / Assert
    TEST_ASSERT_TRUE(bwg_board_limits_for("205", 12, &limits));
    TEST_ASSERT_TRUE(bwg_board_limits_for("601", 19, &limits));
    TEST_ASSERT_FALSE(bwg_board_limits_for("204", 12, &limits));
}

TEST_CASE("Saved settings outside the catalog range are refused", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();

    // Act / Assert
    TEST_ASSERT_TRUE(bwg_limits_profile_admissible(&limits, 485.0f, 1200));
    TEST_ASSERT_FALSE(bwg_limits_profile_admissible(&limits, 600.0f, 1200));
    TEST_ASSERT_FALSE(bwg_limits_profile_admissible(&limits, 485.0f, 1050));
}

TEST_CASE("A safe sample keeps a lease running", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    const bwg_safety_sample_t sample = safe_sample();

    // Act
    const bool safe = bwg_limits_sample_safe(&limits, &sample);

    // Assert
    TEST_ASSERT_TRUE(safe);
}

TEST_CASE("A safe sample has no unsafe fact", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    const bwg_safety_sample_t sample = safe_sample();

    // Act / Assert
    TEST_ASSERT_EQUAL(BWG_SAFETY_FACT_NONE, bwg_limits_first_unsafe_fact(&limits, &sample));
}

TEST_CASE("The first unsafe fact is named with its reading", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    bwg_safety_sample_t hot = safe_sample();
    hot.chip_celsius = 75.5f;
    hot.fan_rpm = 0;
    int32_t value_milli = 0;

    // Act
    const bwg_safety_fact_t fact = bwg_limits_first_unsafe_fact(&limits, &hot);

    // Assert
    TEST_ASSERT_EQUAL(BWG_SAFETY_FACT_CHIP_TEMPERATURE, fact);
    TEST_ASSERT_EQUAL_STRING("chip_temperature", bwg_limits_fact_label(fact));
    TEST_ASSERT_TRUE(bwg_limits_fact_value_milli(&hot, fact, &value_milli));
    TEST_ASSERT_EQUAL_INT32(75500, value_milli);
}

TEST_CASE("A reading that isn't a number has no value to report", "[bwg_limits]")
{
    // Arrange
    bwg_safety_sample_t failed_read = safe_sample();
    failed_read.power_watts = NAN;
    int32_t value_milli = 0;

    // Act / Assert
    TEST_ASSERT_FALSE(bwg_limits_fact_value_milli(&failed_read, BWG_SAFETY_FACT_POWER, &value_milli));
}

TEST_CASE("Each breached fact makes a sample unsafe", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    bwg_safety_sample_t low_input = safe_sample();
    low_input.input_volts = 4.4f;
    bwg_safety_sample_t high_power = safe_sample();
    high_power.power_watts = 15.5f;
    bwg_safety_sample_t hot = safe_sample();
    hot.chip_celsius = 75.0f;
    bwg_safety_sample_t failed_read = safe_sample();
    failed_read.chip_celsius = -1.0f;
    bwg_safety_sample_t stopped_fan = safe_sample();
    stopped_fan.fan_rpm = 0;

    // Act / Assert
    TEST_ASSERT_FALSE(bwg_limits_sample_safe(&limits, &low_input));
    TEST_ASSERT_FALSE(bwg_limits_sample_safe(&limits, &high_power));
    TEST_ASSERT_FALSE(bwg_limits_sample_safe(&limits, &hot));
    TEST_ASSERT_FALSE(bwg_limits_sample_safe(&limits, &failed_read));
    TEST_ASSERT_FALSE(bwg_limits_sample_safe(&limits, &stopped_fan));
}

TEST_CASE("A board sensor proves cooling at 45 C with the fan turning", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    const bwg_cooling_sample_t cool = {.chip_celsius = 45.0f, .fan_rpm = 6000};
    const bwg_cooling_sample_t warm = {.chip_celsius = 45.5f, .fan_rpm = 6000};
    const bwg_cooling_sample_t no_fan = {.chip_celsius = 40.0f};

    // Act / Assert
    TEST_ASSERT_TRUE(bwg_limits_cooling_proven(&limits, &cool));
    TEST_ASSERT_FALSE(bwg_limits_cooling_proven(&limits, &warm));
    TEST_ASSERT_FALSE(bwg_limits_cooling_proven(&limits, &no_fan));
}

TEST_CASE("A diode board proves cooling by regulator and time", "[bwg_limits]")
{
    // Arrange
    bwg_board_limits_t limits = {0};
    TEST_ASSERT_TRUE(bwg_board_limits_for("601", 19, &limits));
    const bwg_cooling_sample_t early = {.regulator_celsius = 60.0f, .fan_rpm = 6000, .cooling_elapsed_ms = 29999};
    const bwg_cooling_sample_t ready = {.regulator_celsius = 95.0f, .fan_rpm = 6000, .cooling_elapsed_ms = 30000};
    const bwg_cooling_sample_t hot = {.regulator_celsius = 96.0f, .fan_rpm = 6000, .cooling_elapsed_ms = 90000};

    // Act / Assert
    TEST_ASSERT_FALSE(bwg_limits_cooling_proven(&limits, &early));
    TEST_ASSERT_TRUE(bwg_limits_cooling_proven(&limits, &ready));
    TEST_ASSERT_FALSE(bwg_limits_cooling_proven(&limits, &hot));
}

TEST_CASE("No link holds nothing", "[bwg_link_presence]")
{
    // Arrange
    const bwg_link_presence_t presence = {0};

    // Act / Assert
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 0));
}

TEST_CASE("Heartbeats hold local mining until the link is quiet", "[bwg_link_presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};

    // Act
    bwg_link_presence_note_activity(&presence, 1000);

    // Assert
    TEST_ASSERT_TRUE(bwg_link_presence_holds_local(&presence, 1000 + BWG_LINK_QUIET_MS - 1));
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 1000 + BWG_LINK_QUIET_MS));
}

TEST_CASE("A refused Hello holds local mining for the next Connect", "[bwg_link_presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};

    // Act
    bwg_link_presence_note_hello(&presence, 1000, false);

    // Assert
    TEST_ASSERT_TRUE(bwg_link_presence_holds_local(&presence, 1000 + BWG_HANDOVER_HOLD_MS - 1));
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 1000 + BWG_HANDOVER_HOLD_MS));
}

TEST_CASE("An admitted Hello ends the handover hold", "[bwg_link_presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};
    bwg_link_presence_note_hello(&presence, 1000, false);

    // Act
    bwg_link_presence_note_hello(&presence, 2000, true);

    // Assert
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 2000 + BWG_LINK_QUIET_MS));
}

static bwg_safety_sample_t idle_sample_205(float board_celsius)
{
    const bwg_safety_sample_t sample = {
        .input_volts = 5.0f,
        .power_watts = 2.0f,
        .chip_celsius = board_celsius,
        .regulator_celsius = 40.0f,
        .fan_rpm = 3000,
    };
    return sample;
}

TEST_CASE("A warm board sensor keeps the fan up", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    const bwg_safety_sample_t sample = idle_sample_205(46.0f);

    // Act
    const bool safe = bwg_limits_baseline_safe(&limits, &sample);

    // Assert
    TEST_ASSERT_FALSE(safe);
}

TEST_CASE("A cool board sensor lets the fan return to baseline", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    const bwg_safety_sample_t sample = idle_sample_205(45.0f);

    // Act
    const bool safe = bwg_limits_baseline_safe(&limits, &sample);

    // Assert
    TEST_ASSERT_TRUE(safe);
}

TEST_CASE("A stopped fan is never a safe baseline", "[bwg_limits]")
{
    // Arrange
    const bwg_board_limits_t limits = limits_205();
    bwg_safety_sample_t sample = idle_sample_205(30.0f);
    sample.fan_rpm = 0;

    // Act
    const bool safe = bwg_limits_baseline_safe(&limits, &sample);

    // Assert
    TEST_ASSERT_FALSE(safe);
}
