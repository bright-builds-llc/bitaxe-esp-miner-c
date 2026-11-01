#include "unity.h"
#include "bwg_link_presence.h"

TEST_CASE("a link with no activity holds nothing", "[bwg-link-presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};

    // Act and Assert
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 1000));
}

TEST_CASE("activity holds local mining until the link is quiet", "[bwg-link-presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};

    // Act
    bwg_link_presence_note_activity(&presence, 1000);

    // Assert
    TEST_ASSERT_TRUE(bwg_link_presence_holds_local(&presence, 1000 + BWG_LINK_QUIET_MS - 1));
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 1000 + BWG_LINK_QUIET_MS));
}

TEST_CASE("a refused Hello holds local mining for the handover hold", "[bwg-link-presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};

    // Act
    bwg_link_presence_note_hello(&presence, 1000, false);

    // Assert
    TEST_ASSERT_TRUE(bwg_link_presence_holds_local(&presence, 1000 + BWG_HANDOVER_HOLD_MS - 1));
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 1000 + BWG_HANDOVER_HOLD_MS));
}

TEST_CASE("an admitted Hello ends the handover hold", "[bwg-link-presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};
    bwg_link_presence_note_hello(&presence, 1000, false);

    // Act
    bwg_link_presence_note_hello(&presence, 2000, true);

    // Assert
    TEST_ASSERT_FALSE(bwg_link_presence_holds_local(&presence, 2000 + BWG_LINK_QUIET_MS));
}

TEST_CASE("a clock behind the stamp still holds", "[bwg-link-presence]")
{
    // Arrange
    bwg_link_presence_t presence = {0};
    bwg_link_presence_note_activity(&presence, 5000);

    // Act and Assert
    TEST_ASSERT_TRUE(bwg_link_presence_holds_local(&presence, 4000));
}
