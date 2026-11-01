#include <string.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "bwg_writer.h"
#include "test_suites.h"
#include "test_writer_support.h"

/** Moves the fixture to the closing state a validated Close leaves behind. */
static void close_with_receipt(fixture_t *fixture, uint32_t previous, uint32_t received, uint64_t now_ms)
{
    bwg_credit_publish(&fixture->shared, previous);
    bwg_link_shared_revoke(&fixture->shared, 1);
    bwg_credit_retain_terminal(&fixture->shared, 1, true, previous, received, now_ms);
}

static void the_closing_epoch_writes_one_final_credit(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    close_with_receipt(&fixture, 99, 100, 10);

    // Act
    writer_drain(&fixture, 20);
    size_t after_final = fixture.state.len;
    writer_drain(&fixture, 30);

    // Assert
    bwg_record_t records[3];
    ASSERT_EQ(writer_parse_output(&fixture, records, 3), 2);
    const char *expected = "{\"op\":\"receive_credit\",\"receivedBytes\":100}";
    ASSERT_MEMEQ(records[1].payload, expected, strlen(expected));
    ASSERT_EQ(fixture.state.len, after_final);
    uint32_t bytes;
    ASSERT_FALSE(bwg_credit_terminal(&fixture.shared, 1, 30, &bytes));
    writer_fixture_deinit(&fixture);
}

static void an_expired_terminal_receipt_writes_nothing(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    size_t after_ack = fixture.state.len;
    close_with_receipt(&fixture, 99, 100, 10);

    // Act
    writer_drain(&fixture, 2010);

    // Assert
    ASSERT_EQ(fixture.state.len, after_ack);
    writer_fixture_deinit(&fixture);
}

static void a_partial_line_blocks_the_terminal_credit(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    fixture.writer.partial_output = true;
    size_t after_ack = fixture.state.len;
    close_with_receipt(&fixture, 99, 100, 10);

    // Act
    writer_drain(&fixture, 20);

    // Assert
    ASSERT_EQ(fixture.state.len, after_ack);
    uint32_t bytes;
    ASSERT_FALSE(bwg_credit_terminal(&fixture.shared, 1, 20, &bytes));
    writer_fixture_deinit(&fixture);
}

static void a_record_still_flushing_at_close_keeps_the_final_credit(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_credit_publish(&fixture.shared, 50);
    fixture.state.hold_flush = true;
    bwg_writer_next(&fixture.writer, &fixture.sink, 1);
    close_with_receipt(&fixture, 99, 100, 10);
    fixture.state.hold_flush = false;

    // Act
    bwg_writer_result_t aborted = bwg_writer_next(&fixture.writer, &fixture.sink, 11);
    writer_drain(&fixture, 12);

    // Assert
    ASSERT_EQ(aborted.failure, BWG_WRITER_FAILED_REVOKED);
    ASSERT_TRUE(aborted.failed_flushing);
    ASSERT_EQ(aborted.failed_queued_bytes, aborted.failed_record_bytes);
    ASSERT_FALSE(fixture.writer.partial_output);
    bwg_record_t records[4];
    size_t count = writer_parse_output(&fixture, records, 4);
    ASSERT_EQ(count, 3);
    const char *expected = "{\"op\":\"receive_credit\",\"receivedBytes\":100}";
    ASSERT_MEMEQ(records[2].payload, expected, strlen(expected));
    writer_fixture_deinit(&fixture);
}

static void a_record_revoked_on_its_last_chunk_keeps_the_final_credit(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 9);
    fixture.state.maybe_revoke_shared = &fixture.shared;
    fixture.state.revoke_epoch = 1;

    // Act
    bwg_writer_result_t aborted = bwg_writer_next(&fixture.writer, &fixture.sink, 1);
    bwg_credit_publish(&fixture.shared, 99);
    bwg_credit_retain_terminal(&fixture.shared, 1, true, 99, 100, 2);
    writer_drain(&fixture, 3);

    // Assert
    ASSERT_EQ(aborted.failure, BWG_WRITER_FAILED_REVOKED);
    ASSERT_EQ(aborted.failed_queued_bytes, aborted.failed_record_bytes);
    ASSERT_FALSE(fixture.writer.partial_output);
    bwg_record_t records[4];
    ASSERT_EQ(writer_parse_output(&fixture, records, 4), 3);
    const char *expected = "{\"op\":\"receive_credit\",\"receivedBytes\":100}";
    ASSERT_MEMEQ(records[2].payload, expected, strlen(expected));
    writer_fixture_deinit(&fixture);
}

static void revoking_a_closed_epoch_keeps_its_receipt(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    close_with_receipt(&fixture, 99, 100, 10);

    // Act
    bwg_link_shared_revoke(&fixture.shared, 1);

    // Assert
    uint32_t bytes = 0;
    ASSERT_TRUE(bwg_credit_terminal(&fixture.shared, 1, 20, &bytes));
    ASSERT_EQ(bytes, 100);
    writer_fixture_deinit(&fixture);
}

static void a_final_credit_that_cannot_be_framed_ends_the_receipt(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    close_with_receipt(&fixture, 99, 100, 10);
    fixture.writer.sequence = UINT32_MAX;

    // Act
    bwg_writer_result_t result = bwg_writer_next(&fixture.writer, &fixture.sink, 20);

    // Assert
    ASSERT_EQ(result.failure, BWG_WRITER_FAILED_SEQUENCE_EXHAUSTED);
    uint32_t bytes;
    ASSERT_FALSE(bwg_credit_terminal(&fixture.shared, 1, 20, &bytes));
    writer_fixture_deinit(&fixture);
}

void bwg_tests_writer_close(void)
{
    BWG_RUN(the_closing_epoch_writes_one_final_credit);
    BWG_RUN(an_expired_terminal_receipt_writes_nothing);
    BWG_RUN(a_partial_line_blocks_the_terminal_credit);
    BWG_RUN(a_record_still_flushing_at_close_keeps_the_final_credit);
    BWG_RUN(a_record_revoked_on_its_last_chunk_keeps_the_final_credit);
    BWG_RUN(revoking_a_closed_epoch_keeps_its_receipt);
    BWG_RUN(a_final_credit_that_cannot_be_framed_ends_the_receipt);
}
