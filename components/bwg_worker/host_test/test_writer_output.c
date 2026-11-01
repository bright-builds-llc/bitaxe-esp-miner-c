#include <stdio.h>
#include <stdlib.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "bwg_writer.h"
#include "test_suites.h"
#include "test_writer_support.h"

static uint8_t *object_of(size_t len)
{
    uint8_t *payload = malloc(len);
    memset(payload, ' ', len);
    payload[0] = '{';
    payload[len - 1] = '}';
    return payload;
}

static void a_long_control_record_gets_a_fresh_heartbeat_first(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    uint8_t *payload = object_of(BWG_LONG_RECORD_HEARTBEAT_BYTES + 1);
    bwg_writer_offer_control(&fixture.writer, 1, payload, BWG_LONG_RECORD_HEARTBEAT_BYTES + 1, 1);

    // Act
    writer_drain(&fixture, 100);

    // Assert
    bwg_record_t records[3];
    ASSERT_EQ(writer_parse_output(&fixture, records, 3), 3);
    ASSERT_EQ(records[1].kind, BWG_KIND_HEARTBEAT);
    ASSERT_EQ(records[2].kind, BWG_KIND_CONTROL);
    ASSERT_EQ(fixture.writer.last_heartbeat_ms, 100);
    free(payload);
    writer_fixture_deinit(&fixture);
}

static void a_record_at_the_threshold_needs_no_extra_heartbeat(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    uint8_t *payload = object_of(BWG_LONG_RECORD_HEARTBEAT_BYTES);
    bwg_writer_offer_control(&fixture.writer, 1, payload, BWG_LONG_RECORD_HEARTBEAT_BYTES, 1);

    // Act
    writer_drain(&fixture, 100);

    // Assert
    bwg_record_t records[3];
    ASSERT_EQ(writer_parse_output(&fixture, records, 3), 2);
    ASSERT_EQ(records[1].kind, BWG_KIND_CONTROL);
    free(payload);
    writer_fixture_deinit(&fixture);
}

static void chunks_stay_within_the_write_limit(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    uint8_t *payload = object_of(3000);
    bwg_writer_offer_control(&fixture.writer, 1, payload, 3000, 1);

    // Act
    writer_drain(&fixture, 100);

    // Assert
    bwg_record_t records[3];
    ASSERT_EQ(writer_parse_output(&fixture, records, 3), 2);
    ASSERT_TRUE(fixture.state.largest_chunk <= BWG_WRITE_CHUNK_BYTES);
    free(payload);
    writer_fixture_deinit(&fixture);
}

static void short_writes_resume_where_they_stopped(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    fixture.state.accept_limit = 3;
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 1);

    // Act
    writer_drain(&fixture, 100);

    // Assert
    bwg_record_t records[3];
    ASSERT_EQ(writer_parse_output(&fixture, records, 3), 2);
    ASSERT_MEMEQ(records[1].payload, "{\"c\":1}", 7);
    writer_fixture_deinit(&fixture);
}

static void a_full_sink_blocks_without_losing_the_record(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);
    fixture.state.accept_limit = 0;

    // Act
    bwg_writer_result_t blocked = bwg_writer_next(&fixture.writer, &fixture.sink, 100);
    fixture.state.accept_limit = 1 << 20;
    bwg_writer_result_t finished = bwg_writer_next(&fixture.writer, &fixture.sink, 200);

    // Assert
    ASSERT_EQ(blocked.status, BWG_WRITER_BLOCKED);
    ASSERT_TRUE(finished.control_finished);
    ASSERT_TRUE(finished.control_sent);
    ASSERT_EQ(finished.control_token, 9);
    writer_fixture_deinit(&fixture);
}

static void send_is_confirmed_only_after_the_flush(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);
    fixture.state.hold_flush = true;

    // Act
    bwg_writer_result_t queued = bwg_writer_next(&fixture.writer, &fixture.sink, 100);
    bwg_writer_result_t waiting = bwg_writer_next(&fixture.writer, &fixture.sink, 101);
    fixture.state.hold_flush = false;
    bwg_writer_result_t flushed = bwg_writer_next(&fixture.writer, &fixture.sink, 102);

    // Assert
    ASSERT_FALSE(queued.control_finished);
    ASSERT_EQ(waiting.status, BWG_WRITER_BLOCKED);
    ASSERT_TRUE(flushed.control_finished);
    ASSERT_TRUE(flushed.control_sent);
    writer_fixture_deinit(&fixture);
}

static void missing_the_record_deadline_aborts_and_revokes(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);
    fixture.state.budget = 3;
    bwg_writer_next(&fixture.writer, &fixture.sink, 0);
    bwg_writer_next(&fixture.writer, &fixture.sink, 1);

    // Act
    bwg_writer_result_t before = bwg_writer_next(&fixture.writer, &fixture.sink, 1999);
    bwg_writer_result_t after = bwg_writer_next(&fixture.writer, &fixture.sink, 2000);

    // Assert
    ASSERT_EQ(before.status, BWG_WRITER_BLOCKED);
    ASSERT_EQ(after.failure, BWG_WRITER_FAILED_DEADLINE);
    ASSERT_EQ(after.failed_elapsed_ms, 2000);
    ASSERT_EQ(after.failed_queued_bytes, 3);
    ASSERT_TRUE(after.failed_record_bytes > 3);
    ASSERT_FALSE(after.failed_flushing);
    ASSERT_TRUE(after.control_finished);
    ASSERT_FALSE(after.control_sent);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    ASSERT_TRUE(fixture.writer.partial_output);
    writer_fixture_deinit(&fixture);
}

static void the_next_hello_terminates_an_interrupted_line(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);
    fixture.state.budget = 3;
    bwg_writer_next(&fixture.writer, &fixture.sink, 0);
    bwg_link_shared_revoke(&fixture.shared, 1);
    bwg_writer_next(&fixture.writer, &fixture.sink, 1);
    fixture.state.budget = SIZE_MAX;
    size_t before_hello = fixture.state.len;

    // Act
    writer_open_session(&fixture, 2, 10);

    // Assert
    ASSERT_EQ(fixture.state.bytes[before_hello], '\n');
    ASSERT_FALSE(fixture.writer.partial_output);
    bwg_record_t record;
    const uint8_t *line = fixture.state.bytes + before_hello + 1;
    ASSERT_EQ(bwg_record_parse(line, fixture.state.len - before_hello - 1, &record), BWG_FRAME_OK);
    ASSERT_EQ(record.sequence, 0);
    writer_fixture_deinit(&fixture);
}

static void sink_failure_revokes_and_fails_the_reply(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);
    fixture.state.accept_limit = -1;

    // Act
    bwg_writer_result_t result = bwg_writer_next(&fixture.writer, &fixture.sink, 1);

    // Assert
    ASSERT_EQ(result.failure, BWG_WRITER_FAILED_SINK);
    ASSERT_TRUE(result.control_finished);
    ASSERT_FALSE(result.control_sent);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    writer_fixture_deinit(&fixture);
}

static void revocation_mid_record_stops_output(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    uint8_t *payload = object_of(2000);
    bwg_writer_offer_control(&fixture.writer, 1, payload, 2000, 9);
    fixture.state.maybe_revoke_shared = &fixture.shared;
    fixture.state.revoke_epoch = 1;
    size_t before = fixture.state.len;

    // Act
    bwg_writer_result_t result = bwg_writer_next(&fixture.writer, &fixture.sink, 1);

    // Assert
    ASSERT_EQ(result.failure, BWG_WRITER_FAILED_REVOKED);
    ASSERT_TRUE(result.control_finished);
    ASSERT_FALSE(result.control_sent);
    ASSERT_EQ(fixture.state.len - before, BWG_WRITE_CHUNK_BYTES);
    ASSERT_TRUE(fixture.writer.partial_output);
    free(payload);
    writer_fixture_deinit(&fixture);
}

static void a_reply_for_an_old_epoch_fails_without_output(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 2, 0);
    size_t after_ack = fixture.state.len;
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);

    // Act
    bwg_writer_result_t result = bwg_writer_next(&fixture.writer, &fixture.sink, 1);

    // Assert
    ASSERT_TRUE(result.control_finished);
    ASSERT_FALSE(result.control_sent);
    ASSERT_EQ(fixture.state.len, after_ack);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 2);
    writer_fixture_deinit(&fixture);
}

static void reply_allocation_failure_revokes(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    uint8_t *payload = object_of(3000);
    bwg_writer_offer_control(&fixture.writer, 1, payload, 3000, 9);
    bwg_test_alloc_fail_after(0);

    // Act
    bwg_writer_result_t result = bwg_writer_next(&fixture.writer, &fixture.sink, 1);

    // Assert
    ASSERT_EQ(result.failure, BWG_WRITER_FAILED_ALLOCATION);
    ASSERT_TRUE(result.control_finished);
    ASSERT_FALSE(result.control_sent);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    free(payload);
    writer_fixture_deinit(&fixture);
}

static void sequence_exhaustion_revokes_before_any_output(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    fixture.writer.sequence = UINT32_MAX;
    size_t after_ack = fixture.state.len;

    // Act
    bwg_writer_result_t result = bwg_writer_next(&fixture.writer, &fixture.sink, 1000);

    // Assert
    ASSERT_EQ(result.failure, BWG_WRITER_FAILED_SEQUENCE_EXHAUSTED);
    ASSERT_EQ(bwg_link_shared_current_epoch(&fixture.shared), 0);
    ASSERT_EQ(fixture.state.len, after_ack);
    writer_fixture_deinit(&fixture);
}

static void no_session_output_follows_revocation(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    size_t after_ack = fixture.state.len;
    bwg_credit_publish(&fixture.shared, 10);
    bwg_link_shared_revoke(&fixture.shared, 1);

    // Act
    writer_drain(&fixture, 5000);

    // Assert
    ASSERT_EQ(fixture.state.len, after_ack);
    writer_fixture_deinit(&fixture);
}

static void offers_are_bounded(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    char *long_line = malloc(BWG_DIAGNOSTIC_MAX_BYTES + 2);
    memset(long_line, 'x', BWG_DIAGNOSTIC_MAX_BYTES + 1);
    long_line[BWG_DIAGNOSTIC_MAX_BYTES + 1] = '\0';

    // Act
    bool too_long = bwg_writer_offer_diagnostic(&fixture.writer, long_line);
    bool first = bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{}", 2, 1);
    bool second = bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{}", 2, 2);
    free(long_line);

    // Assert
    ASSERT_FALSE(too_long);
    ASSERT_TRUE(first);
    ASSERT_FALSE(second);
    writer_fixture_deinit(&fixture);
}

void bwg_tests_writer_output(void)
{
    BWG_RUN(a_long_control_record_gets_a_fresh_heartbeat_first);
    BWG_RUN(a_record_at_the_threshold_needs_no_extra_heartbeat);
    BWG_RUN(chunks_stay_within_the_write_limit);
    BWG_RUN(short_writes_resume_where_they_stopped);
    BWG_RUN(a_full_sink_blocks_without_losing_the_record);
    BWG_RUN(send_is_confirmed_only_after_the_flush);
    BWG_RUN(missing_the_record_deadline_aborts_and_revokes);
    BWG_RUN(the_next_hello_terminates_an_interrupted_line);
    BWG_RUN(sink_failure_revokes_and_fails_the_reply);
    BWG_RUN(revocation_mid_record_stops_output);
    BWG_RUN(a_reply_for_an_old_epoch_fails_without_output);
    BWG_RUN(reply_allocation_failure_revokes);
    BWG_RUN(sequence_exhaustion_revokes_before_any_output);
    BWG_RUN(no_session_output_follows_revocation);
    BWG_RUN(offers_are_bounded);
}
