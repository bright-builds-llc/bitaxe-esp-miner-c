#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_link.h"
#include "bwg_test.h"
#include "test_suites.h"
#include "test_link_support.h"

static void hello_gets_an_exact_acknowledgement(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);

    // Act
    link_feed_hello(&fixture, 10);

    // Assert
    char expected[1280];
    int len = snprintf(expected, sizeof(expected),
                       "{\"op\":\"hello_ack\",\"hostNonce\":\"" HOST_NONCE "\","
                       "\"deviceNonce\":\"%s\",\"serialManifest\":%s,"
                       "\"firmwareSourceCommit\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                       "\"appElfSha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
                       "\"receiveWindowBytes\":2048,\"receivedBytes\":0}",
                       "ERERERERERERERERERERERERERERERERERERERERERE", BWG_SERIAL_MANIFEST_JSON);
    ASSERT_EQ(fixture.stub.acks, 1);
    ASSERT_EQ(fixture.stub.ack_len, (size_t) len);
    ASSERT_MEMEQ(fixture.stub.ack, expected, fixture.stub.ack_len);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 1);
    ASSERT_EQ(fixture.stub.failure_clears, 1);
    ASSERT_STREQ(fixture.link.binding.session_id, SESSION_ID);
    link_fixture_deinit(&fixture);
}

static void refused_generation_gets_no_reply(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    fixture.stub.refuse_generation = true;

    // Act
    link_feed_hello(&fixture, 10);

    // Assert
    ASSERT_EQ(fixture.stub.acks, 0);
    ASSERT_EQ(fixture.stub.sessions, 0);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    ASSERT_EQ(bwg_link_epoch(&fixture.link), 0);
    link_fixture_deinit(&fixture);
}

static void entropy_failure_gets_no_reply(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    fixture.stub.refuse_random = true;

    // Act
    link_feed_hello(&fixture, 10);

    // Assert
    ASSERT_EQ(fixture.stub.acks, 0);
    ASSERT_EQ(fixture.stub.generations_begun, 0);
    link_fixture_deinit(&fixture);
}

static void exhausted_epochs_get_no_reply(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    fixture.link.next_epoch = UINT32_MAX;

    // Act
    link_feed_hello(&fixture, 10);

    // Assert
    ASSERT_EQ(fixture.stub.acks, 0);
    link_fixture_deinit(&fixture);
}

static void a_new_hello_closes_the_old_link_first(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);

    // Act
    link_feed_hello(&fixture, 20);

    // Assert
    ASSERT_EQ(fixture.stub.generations_closed, 1);
    ASSERT_EQ(fixture.stub.acks, 2);
    ASSERT_EQ(fixture.stub.last_ack_epoch, 2);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 2);
    link_fixture_deinit(&fixture);
}

static void oversized_hello_closes_silently(void)
{
    // Arrange: a syntactically valid Hello whose line is over 2,048 bytes.
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    char payload[2200];
    int prefix = snprintf(payload, sizeof(payload), "{\"op\":\"hello\",\"hostNonce\":\"" HOST_NONCE "\"");
    memset(payload + prefix, ' ', 2000);
    payload[prefix + 2000] = '}';
    payload[prefix + 2001] = '\0';

    // Act
    link_feed_record(&fixture, "session", NULL, 0, payload, 20);

    // Assert
    ASSERT_EQ(fixture.stub.acks, 1);
    ASSERT_EQ(fixture.stub.generations_closed, 1);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    ASSERT_EQ(fixture.stub.failures, 0);
    link_fixture_deinit(&fixture);
}

static void malformed_hello_payloads_get_no_reply(void)
{
    // Arrange
    const char *payloads[] = {
        "{\"op\":\"hello\"}",
        "{\"op\":\"hi\",\"hostNonce\":\"" HOST_NONCE "\"}",
        "{\"op\":\"hello\",\"hostNonce\":\"" HOST_NONCE "\",\"extra\":1}",
        "{\"op\":\"hello\",\"hostNonce\":\"" HOST_NONCE "\",\"op\":\"hello\"}",
        "{\"op\":\"hello\",\"hostNonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB\"}",
        "{\"op\":\"hello\",\"hostNonce\":\"short\"}",
    };
    for (size_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
        static fixture_t fixture;
        link_fixture_init(&fixture);

        // Act
        link_feed_record(&fixture, "session", NULL, 0, payloads[i], 10);

        // Assert
        ASSERT_EQ(fixture.stub.acks, 0);
        link_fixture_deinit(&fixture);
    }
}

static void failure_after_publication_closes_the_generation(void)
{
    // Arrange
    bool refuse_session[] = {true, false};
    for (size_t i = 0; i < 2; i++) {
        static fixture_t fixture;
        link_fixture_init(&fixture);
        fixture.stub.refuse_session = refuse_session[i];
        fixture.stub.refuse_ack = !refuse_session[i];

        // Act
        link_feed_hello(&fixture, 10);

        // Assert
        ASSERT_EQ(fixture.stub.generations_closed, 1);
        ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
        ASSERT_EQ(bwg_link_epoch(&fixture.link), 0);
        link_fixture_deinit(&fixture);
    }
}

static void boot_text_before_hello_is_ignored(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    const char *text = "I (120) boot: not protocol\n\n{\"valid\":\"json\"}\n";

    // Act
    bwg_link_feed(&fixture.link, (const uint8_t *) text, strlen(text), 1);
    link_feed_hello(&fixture, 10);

    // Assert
    ASSERT_EQ(fixture.stub.failures, 0);
    ASSERT_EQ(fixture.stub.acks, 1);
    link_fixture_deinit(&fixture);
}

static void a_hello_after_an_oversized_line_still_answers(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    uint8_t *noise = malloc(BWG_MAX_WIRE_FRAME_BYTES + 100);
    memset(noise, 'x', BWG_MAX_WIRE_FRAME_BYTES + 99);
    noise[BWG_MAX_WIRE_FRAME_BYTES + 99] = '\n';

    // Act
    bwg_link_feed(&fixture.link, noise, BWG_MAX_WIRE_FRAME_BYTES + 100, 1);
    link_feed_hello(&fixture, 10);
    free(noise);

    // Assert
    ASSERT_EQ(fixture.stub.acks, 1);
    link_fixture_deinit(&fixture);
}

static void corrupt_input_closes_an_established_link(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    const char *text = "not json\n";

    // Act
    bwg_link_feed(&fixture.link, (const uint8_t *) text, strlen(text), 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_ENVELOPE);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    link_fixture_deinit(&fixture);
}

static void integrity_failure_closes_with_its_own_stage(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    const char *line =
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"" SESSION_ID "\","
        "\"sequence\":1,\"payloadBytes\":3,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n";

    // Act
    bwg_link_feed(&fixture.link, (const uint8_t *) line, strlen(line), 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_INTEGRITY);
    link_fixture_deinit(&fixture);
}

static void line_overflow_closes_with_framing(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    uint8_t *noise = malloc(BWG_MAX_WIRE_FRAME_BYTES);
    memset(noise, 'x', BWG_MAX_WIRE_FRAME_BYTES);

    // Act
    bwg_link_feed(&fixture.link, noise, BWG_MAX_WIRE_FRAME_BYTES, 20);
    free(noise);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_FRAMING);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    link_fixture_deinit(&fixture);
}

static void frame_buffer_allocation_failure_closes(void)
{
    // Arrange: the Hello's bytes keep the buffer; a fresh link needs a new one.
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    bwg_accumulator_deinit(&fixture.link.accumulator);
    bwg_test_alloc_fail_size(BWG_MAX_WIRE_FRAME_BYTES);
    const uint8_t byte = 'x';

    // Act
    bwg_link_feed(&fixture.link, &byte, 1, 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_FRAME_ALLOCATION);
    link_fixture_deinit(&fixture);
}

static void wrong_session_closes_with_sequence(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);

    // Act
    link_feed_record(&fixture, "heartbeat", "AAAAAAAAAAAAAAAAAAAAAA", 1, "{}", 20);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_SEQUENCE);
    link_fixture_deinit(&fixture);
}

static void repeated_sequence_closes(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 5, "{}", 20);
    ASSERT_EQ(fixture.stub.failures, 0);

    // Act
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 5, "{}", 30);

    // Assert
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_SEQUENCE);
    link_fixture_deinit(&fixture);
}

static void sequence_gaps_are_allowed_up_to_the_maximum(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);
    link_feed_hello(&fixture, 10);

    // Act
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 4294967294u, "{}", 20);
    int failures_before = fixture.stub.failures;
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 4294967295u, "{}", 30);

    // Assert
    ASSERT_EQ(failures_before, 0);
    ASSERT_EQ(fixture.stub.last_failure, BWG_LINK_FAILURE_SEQUENCE);
    link_fixture_deinit(&fixture);
}

static void records_without_a_link_are_dropped(void)
{
    // Arrange
    static fixture_t fixture;
    link_fixture_init(&fixture);

    // Act
    link_feed_record(&fixture, "heartbeat", SESSION_ID, 1, "{}", 20);

    // Assert
    ASSERT_EQ(fixture.stub.failures, 0);
    ASSERT_EQ(fixture.stub.heartbeats, 0);
    link_fixture_deinit(&fixture);
}

void bwg_tests_link(void)
{
    BWG_RUN(hello_gets_an_exact_acknowledgement);
    BWG_RUN(refused_generation_gets_no_reply);
    BWG_RUN(entropy_failure_gets_no_reply);
    BWG_RUN(exhausted_epochs_get_no_reply);
    BWG_RUN(a_new_hello_closes_the_old_link_first);
    BWG_RUN(oversized_hello_closes_silently);
    BWG_RUN(malformed_hello_payloads_get_no_reply);
    BWG_RUN(failure_after_publication_closes_the_generation);
    BWG_RUN(boot_text_before_hello_is_ignored);
    BWG_RUN(a_hello_after_an_oversized_line_still_answers);
    BWG_RUN(corrupt_input_closes_an_established_link);
    BWG_RUN(integrity_failure_closes_with_its_own_stage);
    BWG_RUN(line_overflow_closes_with_framing);
    BWG_RUN(frame_buffer_allocation_failure_closes);
    BWG_RUN(wrong_session_closes_with_sequence);
    BWG_RUN(repeated_sequence_closes);
    BWG_RUN(sequence_gaps_are_allowed_up_to_the_maximum);
    BWG_RUN(records_without_a_link_are_dropped);
}
