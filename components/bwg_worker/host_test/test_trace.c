#include <stdlib.h>
#include <string.h>

#include "bwg_alloc.h"
#include "bwg_test.h"
#include "bwg_trace.h"
#include "test_suites.h"
#include "yyjson.h"

static uint64_t s_now_ms;

static uint64_t fake_now(void)
{
    return s_now_ms++;
}

static yyjson_doc *review(bwg_trace_t *trace, char *text, size_t capacity)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, (uint8_t *)text, capacity);
    bwg_trace_write(trace, &writer);
    return bwg_jw_ok(&writer) ? yyjson_read(text, writer.len, 0) : NULL;
}

static uint64_t field(yyjson_val *object, const char *key)
{
    return yyjson_get_uint(yyjson_obj_get(object, key));
}

static void an_empty_trace_reports_no_snapshot(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    char text[1024];

    // Act
    yyjson_doc *doc = review(&trace, text, sizeof(text));

    // Assert
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(root, "snapshotAvailable")));
    ASSERT_TRUE(yyjson_is_null(yyjson_obj_get(root, "previous")));
    ASSERT_EQ(field(yyjson_obj_get(root, "current"), "epoch"), 0u);
    yyjson_doc_free(doc);
}

static void events_are_contiguous_within_the_window(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    static char text[BWG_TRACE_REVIEW_BYTES];

    // Act
    bwg_trace_note(&trace, 3, 1, BWG_TRACE_VALIDATED, 100, 0);
    bwg_trace_note(&trace, 3, 1, BWG_TRACE_WRITER_QUEUED, 200, 200);
    yyjson_doc *doc = review(&trace, text, sizeof(text));

    // Assert
    yyjson_val *current = yyjson_obj_get(yyjson_doc_get_root(doc), "current");
    ASSERT_EQ(field(current, "firstEventOrdinal"), 1u);
    ASSERT_EQ(field(current, "nextEventOrdinal"), 3u);
    ASSERT_EQ(yyjson_arr_size(yyjson_obj_get(current, "events")), 2u);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(current, "events"), 1), "stage")), "writer_queued");
    yyjson_doc_free(doc);
}

static void a_full_window_keeps_the_newest_and_counts_the_rest(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    static char text[BWG_TRACE_REVIEW_BYTES];

    // Act
    for (uint32_t i = 0; i < BWG_TRACE_CAPACITY + 5; i++) {
        bwg_trace_note(&trace, 1, i, BWG_TRACE_ENQUEUED, 1, 0);
    }
    yyjson_doc *doc = review(&trace, text, sizeof(text));

    // Assert
    yyjson_val *current = yyjson_obj_get(yyjson_doc_get_root(doc), "current");
    yyjson_val *events = yyjson_obj_get(current, "events");
    ASSERT_EQ(field(current, "firstEventOrdinal"), 6u);
    ASSERT_EQ(field(current, "overwrittenEvents"), 5u);
    ASSERT_EQ(yyjson_arr_size(events), BWG_TRACE_CAPACITY);
    ASSERT_EQ(field(yyjson_arr_get(events, 0), "ordinal"), 6u);
    ASSERT_EQ(field(yyjson_arr_get(events, BWG_TRACE_CAPACITY - 1), "ordinal"), BWG_TRACE_CAPACITY + 5u);
    yyjson_doc_free(doc);
}

static void a_new_epoch_keeps_the_last_one_as_previous(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    static char text[BWG_TRACE_REVIEW_BYTES];

    // Act
    bwg_trace_note(&trace, 1, 0, BWG_TRACE_HELLO, 0, 0);
    bwg_trace_note(&trace, 2, 0, BWG_TRACE_HELLO, 0, 0);
    yyjson_doc *doc = review(&trace, text, sizeof(text));

    // Assert
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_EQ(field(yyjson_obj_get(root, "current"), "epoch"), 2u);
    ASSERT_EQ(field(yyjson_obj_get(root, "previous"), "epoch"), 1u);
    yyjson_doc_free(doc);
}

static void an_older_epochs_event_is_dropped(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    static char text[BWG_TRACE_REVIEW_BYTES];
    bwg_trace_note(&trace, 2, 0, BWG_TRACE_HELLO, 0, 0);

    // Act
    bwg_trace_note(&trace, 1, 7, BWG_TRACE_WRITER_ABANDONED, 10, 0);
    yyjson_doc *doc = review(&trace, text, sizeof(text));

    // Assert
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_EQ(field(root, "droppedEvents"), 1u);
    ASSERT_EQ(field(yyjson_obj_get(root, "current"), "nextEventOrdinal"), 2u);
    yyjson_doc_free(doc);
}

static void more_queued_than_written_is_dropped(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    static char text[BWG_TRACE_REVIEW_BYTES];

    // Act
    bwg_trace_note(&trace, 1, 0, BWG_TRACE_WRITER_QUEUED, 10, 11);
    yyjson_doc *doc = review(&trace, text, sizeof(text));

    // Assert
    ASSERT_EQ(field(yyjson_doc_get_root(doc), "droppedEvents"), 1u);
    yyjson_doc_free(doc);
}

static void a_busy_trace_drops_a_note_rather_than_waiting(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    atomic_flag_test_and_set(&trace.busy);

    // Act
    bwg_trace_note(&trace, 1, 0, BWG_TRACE_HELLO, 0, 0);

    // Assert
    ASSERT_EQ(atomic_load(&trace.dropped), 1u);
}

static void a_full_review_fits_its_reply(void)
{
    // Arrange
    bwg_trace_t trace;
    bwg_trace_init(&trace, fake_now);
    static char text[BWG_TRACE_REVIEW_BYTES];
    for (uint32_t epoch = 1; epoch <= 2; epoch++) {
        for (uint32_t i = 0; i < BWG_TRACE_CAPACITY; i++) {
            bwg_trace_note(&trace, epoch, UINT32_MAX, BWG_TRACE_VALIDATION_REJECTED, 66560, 66560);
        }
    }
    s_now_ms = 9000000000000ull;

    // Act
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, (uint8_t *)text, sizeof(text));
    bwg_trace_write(&trace, &writer);

    // Assert
    ASSERT_TRUE(bwg_jw_ok(&writer));
}

void bwg_tests_trace(void)
{
    BWG_RUN(an_empty_trace_reports_no_snapshot);
    BWG_RUN(events_are_contiguous_within_the_window);
    BWG_RUN(a_full_window_keeps_the_newest_and_counts_the_rest);
    BWG_RUN(a_new_epoch_keeps_the_last_one_as_previous);
    BWG_RUN(an_older_epochs_event_is_dropped);
    BWG_RUN(more_queued_than_written_is_dropped);
    BWG_RUN(a_busy_trace_drops_a_note_rather_than_waiting);
    BWG_RUN(a_full_review_fits_its_reply);
}
