#include "unity.h"
#include "bwg_revocation.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Authority is one process-wide state, so each test leaves it back at LOCAL.
static void return_to_local(void)
{
    bwg_revocation_revoke(BWG_STOP_REASON_CONTROL_FAILED);
    bwg_revocation_note_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

static void enter_stopped(void)
{
    bwg_revocation_revoke(BWG_STOP_REASON_LOCAL_HANDOVER);
    bwg_revocation_note_stopped();
}

TEST_CASE("local mining admits work and shares", "[bwg-revocation]")
{
    // Act and Assert
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    TEST_ASSERT_TRUE(bwg_revocation_admit_share());
    TEST_ASSERT_FALSE(bwg_revocation_stop_wanted());
}

TEST_CASE("revoking refuses work and shares at once", "[bwg-revocation]")
{
    // Arrange
    const bwg_revocation_snapshot_t before = bwg_revocation_snapshot();

    // Act
    bwg_revocation_revoke(BWG_STOP_REASON_LINK_CLOSED);

    // Assert
    TEST_ASSERT_FALSE(bwg_revocation_admit_work());
    TEST_ASSERT_FALSE(bwg_revocation_admit_share());
    TEST_ASSERT_TRUE(bwg_revocation_stop_wanted());
    const bwg_revocation_snapshot_t after = bwg_revocation_snapshot();
    TEST_ASSERT_EQUAL_UINT32(before.refused_work + 1, after.refused_work);
    TEST_ASSERT_EQUAL_UINT32(before.refused_shares + 1, after.refused_shares);
    return_to_local();
}

TEST_CASE("the first revocation reason stays", "[bwg-revocation]")
{
    // Arrange
    bwg_revocation_revoke(BWG_STOP_REASON_UNSAFE_OBSERVATION);

    // Act
    bwg_revocation_revoke(BWG_STOP_REASON_LINK_CLOSED);

    // Assert
    TEST_ASSERT_EQUAL_INT(BWG_STOP_REASON_UNSAFE_OBSERVATION, bwg_revocation_snapshot().reason);
    return_to_local();
}

TEST_CASE("the reason survives the stop being confirmed", "[bwg-revocation]")
{
    // Arrange
    bwg_revocation_revoke(BWG_STOP_REASON_UNSAFE_OBSERVATION);

    // Act
    bwg_revocation_note_stopped();

    // Assert
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    TEST_ASSERT_EQUAL_INT(BWG_AUTHORITY_STOPPED, snapshot.state);
    TEST_ASSERT_EQUAL_INT(BWG_STOP_REASON_UNSAFE_OBSERVATION, snapshot.reason);
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

TEST_CASE("revoking a lease leaves local mining alone", "[bwg-revocation]")
{
    // Act
    bwg_revocation_revoke_lease(BWG_STOP_REASON_UNSAFE_OBSERVATION);

    // Assert
    TEST_ASSERT_EQUAL_INT(BWG_AUTHORITY_LOCAL, bwg_revocation_snapshot().state);
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
}

TEST_CASE("a lease cannot begin before the stop is confirmed", "[bwg-revocation]")
{
    // Arrange
    bwg_revocation_revoke(BWG_STOP_REASON_LOCAL_HANDOVER);

    // Act
    const bool began = bwg_revocation_begin_lease(10000);

    // Assert
    TEST_ASSERT_FALSE(began);
    TEST_ASSERT_FALSE(bwg_revocation_admit_work());
    return_to_local();
}

TEST_CASE("a lease beyond the limit is refused", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();

    // Act
    const bool too_long = bwg_revocation_begin_lease(BWG_LEASE_MAX_MS + 1);
    const bool empty = bwg_revocation_begin_lease(0);

    // Assert
    TEST_ASSERT_FALSE(too_long);
    TEST_ASSERT_FALSE(empty);
    TEST_ASSERT_EQUAL_INT(BWG_AUTHORITY_STOPPED, bwg_revocation_snapshot().state);
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

TEST_CASE("a beginning lease admits work", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();

    // Act
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(10000));

    // Assert
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    TEST_ASSERT_TRUE(bwg_revocation_admit_share());
    return_to_local();
}

TEST_CASE("local mining adds nothing to a lease's counts", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(10000));
    return_to_local();

    // Act
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    TEST_ASSERT_TRUE(bwg_revocation_admit_share());
    bwg_revocation_note_nonce();

    // Assert
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.first_work_ms);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.admitted_work);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.admitted_shares);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.nonces);
}

TEST_CASE("a handover after a lease keeps how the lease ended", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(10000));
    bwg_revocation_revoke_lease(BWG_STOP_REASON_LEASE_EXPIRED);
    bwg_revocation_note_stop_started();
    bwg_revocation_note_stopped();
    const bwg_revocation_snapshot_t ended = bwg_revocation_snapshot();
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
    vTaskDelay(pdMS_TO_TICKS(20));

    // Act
    bwg_revocation_revoke(BWG_STOP_REASON_LOCAL_HANDOVER);
    bwg_revocation_note_stop_started();
    bwg_revocation_note_stopped();

    // Assert
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    TEST_ASSERT_EQUAL_INT(BWG_STOP_REASON_LEASE_EXPIRED, snapshot.lease_reason);
    TEST_ASSERT_EQUAL_UINT32(ended.closed_ms, snapshot.lease_closed_ms);
    TEST_ASSERT_EQUAL_UINT32(ended.stop_started_ms, snapshot.lease_stop_started_ms);
    TEST_ASSERT_GREATER_THAN_UINT32(ended.closed_ms, snapshot.closed_ms);
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

TEST_CASE("heartbeats and renewals are refused without a lease", "[bwg-revocation]")
{
    // Act and Assert
    TEST_ASSERT_FALSE(bwg_revocation_feed_heartbeat());
    TEST_ASSERT_FALSE(bwg_revocation_renew_lease(10000));
}

TEST_CASE("a missing heartbeat closes authority at the cutoff", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(BWG_LEASE_MAX_MS));

    // Act
    vTaskDelay(pdMS_TO_TICKS(BWG_HEARTBEAT_CUTOFF_MS - 300));
    const bool open_before = bwg_revocation_admit_work();
    vTaskDelay(pdMS_TO_TICKS(600));

    // Assert
    TEST_ASSERT_TRUE(open_before);
    TEST_ASSERT_FALSE(bwg_revocation_admit_work());
    TEST_ASSERT_EQUAL_INT(BWG_STOP_REASON_HEARTBEAT_TIMEOUT, bwg_revocation_snapshot().reason);
    bwg_revocation_note_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

TEST_CASE("the reported gap from a heartbeat to the close is at least the cutoff", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(BWG_LEASE_MAX_MS));
    vTaskDelay(pdMS_TO_TICKS(7));
    TEST_ASSERT_TRUE(bwg_revocation_feed_heartbeat());

    // Act
    vTaskDelay(pdMS_TO_TICKS(BWG_HEARTBEAT_CUTOFF_MS + 300));

    // Assert
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    TEST_ASSERT_EQUAL_INT(BWG_STOP_REASON_HEARTBEAT_TIMEOUT, snapshot.reason);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(BWG_HEARTBEAT_CUTOFF_MS, snapshot.closed_ms - snapshot.last_heartbeat_ms);
    bwg_revocation_note_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

TEST_CASE("a heartbeat is reported from when the link observed it", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(BWG_LEASE_MAX_MS));
    const uint32_t observed = bwg_revocation_snapshot().last_heartbeat_ms + 3;
    vTaskDelay(pdMS_TO_TICKS(7));

    // Act
    TEST_ASSERT_TRUE(bwg_revocation_feed_heartbeat_at(observed));

    // Assert
    TEST_ASSERT_EQUAL_UINT32(observed, bwg_revocation_snapshot().last_heartbeat_ms);
    return_to_local();
}

TEST_CASE("heartbeats hold authority past the cutoff", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(BWG_LEASE_MAX_MS));

    // Act
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        TEST_ASSERT_TRUE(bwg_revocation_feed_heartbeat());
    }

    // Assert
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    return_to_local();
}

TEST_CASE("heartbeats do not extend a lease", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(1500));

    // Act
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        bwg_revocation_feed_heartbeat();
    }

    // Assert
    TEST_ASSERT_FALSE(bwg_revocation_admit_work());
    TEST_ASSERT_EQUAL_INT(BWG_STOP_REASON_LEASE_EXPIRED, bwg_revocation_snapshot().reason);
    bwg_revocation_note_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}

TEST_CASE("a renewal extends a lease", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(1000));

    // Act
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_ASSERT_TRUE(bwg_revocation_renew_lease(2000));
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Assert
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    return_to_local();
}

TEST_CASE("a closed lease leaves no deadline running", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(1000));
    bwg_revocation_revoke_lease(BWG_STOP_REASON_LINK_CLOSED);
    bwg_revocation_note_stopped();

    // Act
    TEST_ASSERT_TRUE(bwg_revocation_begin_lease(BWG_LEASE_MAX_MS));
    bwg_revocation_feed_heartbeat();
    vTaskDelay(pdMS_TO_TICKS(1300));

    // Assert
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    return_to_local();
}

TEST_CASE("a budget beyond its limits is refused", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();

    // Act
    const bool over_campaign = bwg_revocation_begin_limited_lease(1000, BWG_ACTIVE_LIMIT_MAX_MS + 1);
    const bool under_soak = bwg_revocation_begin_limited_lease(1000, BWG_ACTIVE_LIMIT_SOAK_MS - 1);

    // Assert
    TEST_ASSERT_FALSE(over_campaign);
    TEST_ASSERT_FALSE(under_soak);
    TEST_ASSERT_EQUAL(BWG_AUTHORITY_STOPPED, bwg_revocation_snapshot().state);
    return_to_local();
}

TEST_CASE("preparation spends none of a budget", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_limited_lease(10000, 300));

    // Act
    vTaskDelay(pdMS_TO_TICKS(500));
    const bwg_authority_state_t state = bwg_revocation_snapshot().state;

    // Assert
    TEST_ASSERT_EQUAL(BWG_AUTHORITY_LEASED, state);
    bwg_revocation_revoke_lease(BWG_STOP_REASON_CONTROL_FAILED);
    enter_stopped();
    return_to_local();
}

TEST_CASE("heartbeats and renewals do not extend a budget", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_limited_lease(10000, 300));
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());

    // Act
    for (int step = 0; step < 8; step++) {
        bwg_revocation_feed_heartbeat();
        bwg_revocation_renew_lease(10000);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();

    // Assert
    TEST_ASSERT_EQUAL(BWG_AUTHORITY_STOPPING, snapshot.state);
    TEST_ASSERT_EQUAL(BWG_STOP_REASON_LEASE_EXPIRED, snapshot.reason);
    TEST_ASSERT_FALSE(bwg_revocation_admit_work());
    enter_stopped();
    return_to_local();
}

TEST_CASE("work past a budget's deadline is refused without its timer", "[bwg-revocation]")
{
    // Arrange
    enter_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_begin_limited_lease(10000, 100));
    TEST_ASSERT_TRUE(bwg_revocation_admit_work());
    const bwg_revocation_snapshot_t before = bwg_revocation_snapshot();

    // Act
    vTaskDelay(pdMS_TO_TICKS(200));
    const bool admitted = bwg_revocation_admit_work();

    // Assert
    TEST_ASSERT_EQUAL_UINT32(before.first_work_ms + 100, before.budget_deadline_ms);
    TEST_ASSERT_FALSE(admitted);
    enter_stopped();
    return_to_local();
}
