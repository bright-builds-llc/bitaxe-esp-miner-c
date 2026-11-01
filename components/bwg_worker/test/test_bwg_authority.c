#include "unity.h"
#include "bwg_authority.h"

static void assert_moves(bwg_authority_state_t from, bwg_authority_event_t event, bwg_authority_state_t to)
{
    bwg_authority_state_t next = from;
    TEST_ASSERT_TRUE(bwg_authority_transition(from, event, &next));
    TEST_ASSERT_EQUAL_INT(to, next);
}

static void assert_refused(bwg_authority_state_t from, bwg_authority_event_t event)
{
    bwg_authority_state_t next = from;
    TEST_ASSERT_FALSE(bwg_authority_transition(from, event, &next));
    TEST_ASSERT_EQUAL_INT(from, next);
}

TEST_CASE("revoke closes local mining and a lease", "[bwg-authority]")
{
    assert_moves(BWG_AUTHORITY_LOCAL, BWG_AUTHORITY_EVENT_REVOKE, BWG_AUTHORITY_STOPPING);
    assert_moves(BWG_AUTHORITY_LEASED, BWG_AUTHORITY_EVENT_REVOKE, BWG_AUTHORITY_STOPPING);
}

TEST_CASE("revoke does nothing once closed", "[bwg-authority]")
{
    assert_refused(BWG_AUTHORITY_STOPPING, BWG_AUTHORITY_EVENT_REVOKE);
    assert_refused(BWG_AUTHORITY_STOPPED, BWG_AUTHORITY_EVENT_REVOKE);
}

TEST_CASE("only a stop in progress can be confirmed", "[bwg-authority]")
{
    assert_moves(BWG_AUTHORITY_STOPPING, BWG_AUTHORITY_EVENT_STOP_CONFIRMED, BWG_AUTHORITY_STOPPED);
    assert_refused(BWG_AUTHORITY_LOCAL, BWG_AUTHORITY_EVENT_STOP_CONFIRMED);
    assert_refused(BWG_AUTHORITY_LEASED, BWG_AUTHORITY_EVENT_STOP_CONFIRMED);
    assert_refused(BWG_AUTHORITY_STOPPED, BWG_AUTHORITY_EVENT_STOP_CONFIRMED);
}

TEST_CASE("a lease begins only from a confirmed stop", "[bwg-authority]")
{
    assert_moves(BWG_AUTHORITY_STOPPED, BWG_AUTHORITY_EVENT_LEASE_BEGIN, BWG_AUTHORITY_LEASED);
    assert_refused(BWG_AUTHORITY_LOCAL, BWG_AUTHORITY_EVENT_LEASE_BEGIN);
    assert_refused(BWG_AUTHORITY_LEASED, BWG_AUTHORITY_EVENT_LEASE_BEGIN);
    assert_refused(BWG_AUTHORITY_STOPPING, BWG_AUTHORITY_EVENT_LEASE_BEGIN);
}

TEST_CASE("local mining returns only from a confirmed stop", "[bwg-authority]")
{
    assert_moves(BWG_AUTHORITY_STOPPED, BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL, BWG_AUTHORITY_LOCAL);
    assert_refused(BWG_AUTHORITY_LOCAL, BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL);
    assert_refused(BWG_AUTHORITY_LEASED, BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL);
    assert_refused(BWG_AUTHORITY_STOPPING, BWG_AUTHORITY_EVENT_RELEASE_TO_LOCAL);
}

TEST_CASE("work is admitted only in local mining and a lease", "[bwg-authority]")
{
    TEST_ASSERT_TRUE(bwg_authority_admits(BWG_AUTHORITY_LOCAL));
    TEST_ASSERT_TRUE(bwg_authority_admits(BWG_AUTHORITY_LEASED));
    TEST_ASSERT_FALSE(bwg_authority_admits(BWG_AUTHORITY_STOPPING));
    TEST_ASSERT_FALSE(bwg_authority_admits(BWG_AUTHORITY_STOPPED));
}

TEST_CASE("a stop is wanted exactly while work is refused", "[bwg-authority]")
{
    TEST_ASSERT_FALSE(bwg_authority_stop_wanted(BWG_AUTHORITY_LOCAL));
    TEST_ASSERT_FALSE(bwg_authority_stop_wanted(BWG_AUTHORITY_LEASED));
    TEST_ASSERT_TRUE(bwg_authority_stop_wanted(BWG_AUTHORITY_STOPPING));
    TEST_ASSERT_TRUE(bwg_authority_stop_wanted(BWG_AUTHORITY_STOPPED));
}

TEST_CASE("the zero state is local mining", "[bwg-authority]")
{
    TEST_ASSERT_EQUAL_INT(0, BWG_AUTHORITY_LOCAL);
}
