#include <stdio.h>
#include <stdlib.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "bwg_writer.h"
#include "test_suites.h"
#include "test_writer_support.h"

static void reproduces_the_published_records(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_val *frames = yyjson_obj_get(yyjson_doc_get_root(doc), "frames");
    const char *heartbeat_wire = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(frames, 1), "wire"));
    const char *discover_wire = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(frames, 2), "wire"));
    const char *credit_wire = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(frames, 3), "wire"));
    bwg_record_t discover;
    ASSERT_EQ(bwg_record_parse((const uint8_t *) discover_wire, strlen(discover_wire), &discover), BWG_FRAME_OK);
    writer_open_session(&fixture, 1, 0);
    size_t after_ack = fixture.state.len;

    // Act
    writer_drain(&fixture, 1000);
    size_t after_heartbeat = fixture.state.len;
    bwg_writer_offer_control(&fixture.writer, 1, discover.payload, discover.payload_len, 7);
    writer_drain(&fixture, 1100);
    size_t after_control = fixture.state.len;
    bwg_credit_publish(&fixture.shared, 1024);
    writer_drain(&fixture, 1200);

    // Assert
    ASSERT_EQ(after_heartbeat - after_ack, strlen(heartbeat_wire));
    ASSERT_MEMEQ(fixture.state.bytes + after_ack, heartbeat_wire, strlen(heartbeat_wire));
    ASSERT_EQ(after_control - after_heartbeat, strlen(discover_wire));
    ASSERT_MEMEQ(fixture.state.bytes + after_heartbeat, discover_wire, strlen(discover_wire));
    ASSERT_EQ(fixture.state.len - after_control, strlen(credit_wire));
    ASSERT_MEMEQ(fixture.state.bytes + after_control, credit_wire, strlen(credit_wire));
    yyjson_doc_free(doc);
    writer_fixture_deinit(&fixture);
}

static void acknowledgement_is_sequence_zero(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);

    // Act
    writer_open_session(&fixture, 1, 0);

    // Assert
    bwg_record_t records[2];
    ASSERT_EQ(writer_parse_output(&fixture, records, 2), 1);
    ASSERT_EQ(records[0].sequence, 0);
    ASSERT_EQ(records[0].kind, BWG_KIND_SESSION);
    ASSERT_EQ(records[0].payload_len, strlen(ACK));
    writer_fixture_deinit(&fixture);
}

static void a_stale_acknowledgement_is_dropped(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    bwg_writer_begin_session(&fixture.writer, 1, SESSION_ID, (const uint8_t *) ACK, strlen(ACK));

    // Act
    writer_drain(&fixture, 0);

    // Assert
    ASSERT_EQ(fixture.state.len, 0);
    writer_fixture_deinit(&fixture);
}

static void priority_is_heartbeat_then_credit_then_control_then_diagnostic(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_credit_publish(&fixture.shared, 10);
    bwg_writer_offer_control(&fixture.writer, 1, (const uint8_t *) "{\"c\":1}", 7, 1);
    bwg_writer_offer_diagnostic(&fixture.writer, "note");

    // Act
    writer_drain(&fixture, 1000);

    // Assert
    bwg_record_t records[6];
    ASSERT_EQ(writer_parse_output(&fixture, records, 6), 5);
    ASSERT_EQ(records[1].kind, BWG_KIND_HEARTBEAT);
    ASSERT_EQ(records[2].kind, BWG_KIND_SESSION);
    ASSERT_EQ(records[3].kind, BWG_KIND_CONTROL);
    ASSERT_EQ(records[4].kind, BWG_KIND_DIAGNOSTIC);
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_EQ(records[i].sequence, i);
    }
    writer_fixture_deinit(&fixture);
}

static void diagnostic_is_framed_as_a_line_in_a_session(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_writer_offer_diagnostic(&fixture.writer, "say \"hi\"");

    // Act
    writer_drain(&fixture, 1);

    // Assert
    bwg_record_t records[2];
    ASSERT_EQ(writer_parse_output(&fixture, records, 2), 2);
    const char *expected = "{\"line\":\"say \\\"hi\\\"\"}";
    ASSERT_EQ(records[1].payload_len, strlen(expected));
    ASSERT_MEMEQ(records[1].payload, expected, strlen(expected));
    writer_fixture_deinit(&fixture);
}

static void diagnostic_is_plain_text_outside_a_session(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    bwg_writer_offer_diagnostic(&fixture.writer, "booting");

    // Act
    writer_drain(&fixture, 1);

    // Assert
    ASSERT_EQ(fixture.state.len, 8);
    ASSERT_MEMEQ(fixture.state.bytes, "booting\n", 8);
    writer_fixture_deinit(&fixture);
}

static void diagnostic_for_a_session_the_writer_has_not_opened_is_dropped(void)
{
    // Arrange: an epoch is current but its acknowledgement has not been queued yet.
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    atomic_store(&fixture.shared.current_epoch, 1);
    bwg_writer_offer_diagnostic(&fixture.writer, "booting");

    // Act
    writer_drain(&fixture, 1);

    // Assert
    ASSERT_EQ(fixture.state.len, 0);
    ASSERT_FALSE(fixture.writer.diagnostic_pending);
    writer_fixture_deinit(&fixture);
}

static void credit_coalesces_and_never_repeats(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    bwg_credit_publish(&fixture.shared, 5);
    bwg_credit_publish(&fixture.shared, 9);

    // Act
    writer_drain(&fixture, 1);
    size_t after_first = fixture.state.len;
    writer_drain(&fixture, 2);

    // Assert
    bwg_record_t records[3];
    ASSERT_EQ(writer_parse_output(&fixture, records, 3), 2);
    const char *expected = "{\"op\":\"receive_credit\",\"receivedBytes\":9}";
    ASSERT_MEMEQ(records[1].payload, expected, strlen(expected));
    ASSERT_EQ(fixture.state.len, after_first);
    writer_fixture_deinit(&fixture);
}

static void no_credit_when_nothing_was_received(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    size_t after_ack = fixture.state.len;

    // Act
    writer_drain(&fixture, 1);

    // Assert
    ASSERT_EQ(fixture.state.len, after_ack);
    writer_fixture_deinit(&fixture);
}

static void heartbeats_follow_their_cadence(void)
{
    // Arrange
    static fixture_t fixture;
    writer_fixture_init(&fixture);
    writer_open_session(&fixture, 1, 0);
    size_t after_ack = fixture.state.len;

    // Act
    writer_drain(&fixture, 999);
    size_t before_due = fixture.state.len;
    writer_drain(&fixture, 1000);
    size_t at_due = fixture.state.len;
    writer_drain(&fixture, 1999);
    size_t before_next = fixture.state.len;
    writer_drain(&fixture, 2000);

    // Assert
    ASSERT_EQ(before_due, after_ack);
    ASSERT_TRUE(at_due > before_due);
    ASSERT_EQ(before_next, at_due);
    ASSERT_TRUE(fixture.state.len > before_next);
    writer_fixture_deinit(&fixture);
}

void bwg_tests_writer(void)
{
    BWG_RUN(reproduces_the_published_records);
    BWG_RUN(acknowledgement_is_sequence_zero);
    BWG_RUN(a_stale_acknowledgement_is_dropped);
    BWG_RUN(priority_is_heartbeat_then_credit_then_control_then_diagnostic);
    BWG_RUN(diagnostic_is_framed_as_a_line_in_a_session);
    BWG_RUN(diagnostic_is_plain_text_outside_a_session);
    BWG_RUN(diagnostic_for_a_session_the_writer_has_not_opened_is_dropped);
    BWG_RUN(credit_coalesces_and_never_repeats);
    BWG_RUN(no_credit_when_nothing_was_received);
    BWG_RUN(heartbeats_follow_their_cadence);
}
