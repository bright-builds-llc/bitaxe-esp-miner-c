#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_link.h"
#include "bwg_test.h"
#include "test_suites.h"
#include "test_link_support.h"

static void every_byte_earns_credit_except_the_lf_until_classified(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    uint32_t after_hello = link_published_credit(&fixture);
    const uint8_t partial[] = "{\"profile\"";

    // Act
    bwg_link_feed(&fixture.link, partial, sizeof(partial) - 1, 20);
    uint32_t after_partial = link_published_credit(&fixture);
    bwg_link_read_failed(&fixture.link, 30);
    link_feed_hello(&fixture, 40);
    size_t line = link_feed_record(&fixture, "control", SESSION_ID, 1, "{}", 50);
    uint32_t after_record = link_published_credit(&fixture);

    // Assert
    ASSERT_EQ(after_hello, 0);
    ASSERT_EQ(after_partial, sizeof(partial) - 1);
    ASSERT_EQ(after_record, line);
    link_fixture_deinit(&fixture);
}

static void bytes_after_hello_in_the_same_read_belong_to_the_new_epoch(void)
{
    // Arrange: build a Hello line then a partial tail in one read.
    static fixture_t fixture;
    link_fixture_init(&fixture);
    const char *payload = "{\"op\":\"hello\",\"hostNonce\":\"" HOST_NONCE "\"}";
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_sha256_text((const uint8_t *) payload, strlen(payload), digest);
    char batch[512];
    int len = snprintf(batch, sizeof(batch),
                       "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"session\",\"sessionId\":null,\"sequence\":0,"
                       "\"payloadBytes\":%zu,\"payloadSha256\":\"%s\",\"payload\":%s}\nabcde",
                       strlen(payload), digest, payload);

    // Act
    bwg_link_feed(&fixture.link, (const uint8_t *) batch, (size_t) len, 10);

    // Assert
    ASSERT_EQ(link_published_credit(&fixture), 5);
    link_fixture_deinit(&fixture);
}

static void receive_counter_exhaustion_closes(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    fixture.link.received = UINT32_MAX;
    const uint8_t byte = 'x';

    // Act
    bwg_link_feed(&fixture.link, &byte, 1, 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_RECEIVE_COUNTER);
    ASSERT_EQ(fixture.link.received, UINT32_MAX);
    link_fixture_deinit(&fixture);
}

static void unauthenticated_heartbeats_do_not_refresh_the_clock(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 1, "{}", 3000);

    // Act
    bwg_link_supervise(&fixture.link, 3799);
    int failures_before = fixture.stub.failures;
    bwg_link_supervise(&fixture.link, 3800);

    // Assert
    ASSERT_EQ(failures_before, 0);
    ASSERT_EQ(fixture.stub.heartbeats, 0);
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    link_fixture_deinit(&fixture);
}

static void authenticated_heartbeats_refresh_link_and_generation(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);
    bwg_link_shared_authenticate(&fixture.shared, 1);

    // Act
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 1, "{}", 3000);
    bwg_link_supervise(&fixture.link, 5799);

    // Assert
    ASSERT_EQ(fixture.stub.heartbeats, 1);
    ASSERT_EQ(fixture.stub.failures, 0);
    link_fixture_deinit(&fixture);
}

static void heartbeat_after_the_cutoff_times_out(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);
    bwg_link_shared_authenticate(&fixture.shared, 1);

    // Act
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 1, "{}", 3800);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT);
    ASSERT_EQ(fixture.stub.heartbeats, 0);
    link_fixture_deinit(&fixture);
}

static void heartbeat_payload_allows_only_whitespace_around_braces(void)
{
    // Arrange
    const char *refused[] = {"{\"a\":1}", "{\"x\":{}}"};
    for (size_t i = 0; i < 2; i++) {
        static fixture_t fixture;
        link_fixture_init(&fixture);
        link_feed_hello(&fixture, 10);

        // Act
        link_feed_record(&fixture, "heartbeat", SESSION_ID, 1, refused[i], 20);

        // Assert
        ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_HEARTBEAT_PAYLOAD);
        link_fixture_deinit(&fixture);
    }
    static fixture_t spaced;
    link_fixture_init(&spaced);
    link_feed_hello(&spaced, 10);
    link_feed_record(&spaced, "heartbeat", SESSION_ID, 1, "{ \t }", 20);
    ASSERT_EQ(spaced.stub.failures, 0);
    link_fixture_deinit(&spaced);
}

static void clock_running_backwards_closes(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);

    // Act
    bwg_link_supervise(&fixture.link, 999);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT);
    link_fixture_deinit(&fixture);
}

static void revoked_epoch_closes_with_session_revoked(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);
    bwg_link_shared_revoke(&fixture.shared, 1);

    // Act
    bwg_link_supervise(&fixture.link, 1001);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_SESSION_REVOKED);
    ASSERT_EQ(fixture.stub.generations_closed, 1);
    link_fixture_deinit(&fixture);
}

static void read_failure_drops_the_link_and_partial_record(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    const uint8_t partial[] = "partial";
    bwg_link_feed(&fixture.link, partial, 7, 20);

    // Act
    bwg_link_read_failed(&fixture.link, 30);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_READ);
    ASSERT_EQ(fixture.link.accumulator.len, 0);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    link_fixture_deinit(&fixture);
}

static void control_payload_reaches_the_owner_with_an_lf(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);

    // Act
    link_feed_record(&fixture, "control", SESSION_ID, 3, "{ \"a\":1 }", 20);

    // Assert
    ASSERT_EQ(fixture.stub.controls, 1);
    ASSERT_EQ(fixture.stub.control_sequence, 3);
    ASSERT_EQ(fixture.stub.control.len, 10);
    ASSERT_MEMEQ(fixture.stub.control.data, "{ \"a\":1 }\n", 10);
    link_fixture_deinit(&fixture);
}

static void control_queue_full_closes_and_wipes(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    fixture.stub.refuse_control = true;
    bwg_test_alloc_watch_secret("queue-secret");

    // Act
    link_feed_record(&fixture, "control", SESSION_ID, 3, "{\"a\":\"queue-secret\"}", 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_CONTROL_QUEUE);
    link_fixture_deinit(&fixture);
}

static void control_allocation_failure_closes(void)
{
    // Arrange: only the payload copy allocates once the frame buffer exists.
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    bwg_test_alloc_fail_after(0);

    // Act
    link_feed_record(&fixture, "control", SESSION_ID, 3, "{}", 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_CONTROL_ALLOCATION);
    link_fixture_deinit(&fixture);
}

static void clean_close_revokes_and_retains_one_newer_receipt(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);

    // Act
    size_t line = link_feed_record(&fixture, "session", SESSION_ID, 1, "{\"op\":\"close\",\"reason\":\"tab_closed\"}", 1100);
    uint32_t terminal = 0;
    bool present = bwg_credit_terminal(&fixture.shared, 1, 3099, &terminal);
    uint32_t after_window = 0;
    bool expired = !bwg_credit_terminal(&fixture.shared, 1, 3100, &after_window);

    // Assert
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    ASSERT_EQ(fixture.stub.generations_closed, 1);
    ASSERT_EQ(fixture.stub.failures, 0);
    ASSERT_TRUE(present);
    ASSERT_EQ(terminal, line);
    ASSERT_TRUE(expired);
    link_fixture_deinit(&fixture);
}

static void a_newer_hello_discards_the_terminal_receipt(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 1000);
    link_feed_record(&fixture, "session", SESSION_ID, 1, "{\"op\":\"close\",\"reason\":\"tab_closed\"}", 1100);

    // Act
    link_feed_hello(&fixture, 1200);
    uint32_t terminal = 0;

    // Assert
    ASSERT_FALSE(bwg_credit_terminal(&fixture.shared, 1, 1201, &terminal));
    ASSERT_EQ(link_published_credit(&fixture), 0);
    link_fixture_deinit(&fixture);
}

static void other_session_records_and_diagnostics_are_unexpected(void)
{
    // Arrange
    const char *kinds[] = {"session", "session", "session", "diagnostic"};
    const char *payloads[] = {
        "{\"op\":\"close\",\"reason\":\"because\"}",
        "{\"op\":\"close\",\"reason\":\"paused\",\"x\":1}",
        "{\"op\":\"receive_credit\",\"receivedBytes\":1}",
        "{\"line\":\"x\"}",
    };
    for (size_t i = 0; i < 4; i++) {
        static fixture_t fixture;
        link_fixture_init(&fixture);
        link_feed_hello(&fixture, 10);

        // Act
        link_feed_record(&fixture, kinds[i], SESSION_ID, 1, payloads[i], 20);

        // Assert
        ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_UNEXPECTED_KIND);
        link_fixture_deinit(&fixture);
    }
}

static void failure_labels_are_pinned(void)
{
    // Arrange
    static const char *const labels[] = {
        "read", "framing", "envelope", "sequence", "heartbeat_payload", "heartbeat_timeout", "control_allocation",
        "control_queue", "unexpected_kind", "session_revoked", "integrity", "receive_counter", "frame_allocation",
    };
    // Act / Assert
    for (int code = 1; code <= 13; code++) {
        ASSERT_STREQ(bwg_link_failure_label((bwg_link_failure_t) code), labels[code - 1]);
    }
}

static void init_refuses_malformed_build_identity(void)
{
    // Arrange
    static bwg_link_t link;
    bwg_link_shared_t shared;
    bwg_link_shared_init(&shared);
    const bwg_link_ops_t ops = {0};

    // Act
    bool ok = bwg_link_init(&link, &bwg_test_allocator, &shared, &ops, "ABC", "def");

    // Assert
    ASSERT_FALSE(ok);
}

void bwg_tests_link_session(void)
{
    BWG_RUN(every_byte_earns_credit_except_the_lf_until_classified);
    BWG_RUN(bytes_after_hello_in_the_same_read_belong_to_the_new_epoch);
    BWG_RUN(receive_counter_exhaustion_closes);
    BWG_RUN(unauthenticated_heartbeats_do_not_refresh_the_clock);
    BWG_RUN(authenticated_heartbeats_refresh_link_and_generation);
    BWG_RUN(heartbeat_after_the_cutoff_times_out);
    BWG_RUN(heartbeat_payload_allows_only_whitespace_around_braces);
    BWG_RUN(clock_running_backwards_closes);
    BWG_RUN(revoked_epoch_closes_with_session_revoked);
    BWG_RUN(read_failure_drops_the_link_and_partial_record);
    BWG_RUN(control_payload_reaches_the_owner_with_an_lf);
    BWG_RUN(control_queue_full_closes_and_wipes);
    BWG_RUN(control_allocation_failure_closes);
    BWG_RUN(clean_close_revokes_and_retains_one_newer_receipt);
    BWG_RUN(a_newer_hello_discards_the_terminal_receipt);
    BWG_RUN(other_session_records_and_diagnostics_are_unexpected);
    BWG_RUN(failure_labels_are_pinned);
    BWG_RUN(init_refuses_malformed_build_identity);
}
