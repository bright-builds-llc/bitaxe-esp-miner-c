#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_frame.h"
#include "bwg_json.h"
#include "bwg_test.h"
#include "test_suites.h"

static uint8_t *copy_of(const char *text, size_t *len)
{
    *len = strlen(text);
    uint8_t *copy = malloc(*len + 1);
    memcpy(copy, text, *len + 1);
    return copy;
}

static void encode_matches_the_published_wire_bytes(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "frames"));
    yyjson_val *frame;
    while ((frame = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *wire = yyjson_obj_get(frame, "wire");
        size_t wire_len = yyjson_get_len(wire);
        bwg_record_t record;
        ASSERT_EQ(bwg_record_parse((const uint8_t *) yyjson_get_str(wire), wire_len, &record), BWG_FRAME_OK);
        char session_id[BWG_SESSION_ID_TEXT_LEN + 1] = {0};
        if (record.maybe_session_id != NULL) {
            memcpy(session_id, record.maybe_session_id, BWG_SESSION_ID_TEXT_LEN);
        }
        uint8_t out[2048];
        size_t out_len = 0;

        // Act
        bwg_frame_status_t status =
            bwg_record_encode(record.kind, record.maybe_session_id == NULL ? NULL : session_id, record.sequence,
                              record.payload, record.payload_len, out, sizeof(out), &out_len);

        // Assert
        ASSERT_EQ(status, BWG_FRAME_OK);
        ASSERT_EQ(out_len, wire_len);
        ASSERT_MEMEQ(out, yyjson_get_str(wire), wire_len);
    }
    yyjson_doc_free(doc);
}

static void encode_refuses_a_non_object_payload_and_small_buffers(void)
{
    // Arrange
    uint8_t out[300];
    size_t out_len = 0;

    // Act / Assert
    ASSERT_EQ(bwg_record_encode(BWG_KIND_CONTROL, NULL, 0, (const uint8_t *) "[]", 2, out, sizeof(out), &out_len),
              BWG_FRAME_INVALID);
    const char *big = "{\"a\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}";
    ASSERT_EQ(bwg_record_encode(BWG_KIND_CONTROL, NULL, 0, (const uint8_t *) big, strlen(big), out, sizeof(out), &out_len),
              BWG_FRAME_OVERSIZED);
}

static void manifest_matches_the_gate_and_its_digest(void)
{
    // Arrange
    yyjson_doc *fixture = bwg_test_fixture_doc("serial/manifest.json");
    uint8_t *canonical = malloc(4096);
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, canonical, 4096);
    yyjson_doc *ours = yyjson_read(BWG_SERIAL_MANIFEST_JSON, strlen(BWG_SERIAL_MANIFEST_JSON), 0);

    // Act
    bool ok = bwg_json_write_value(&writer, yyjson_doc_get_root(ours), BWG_JSON_ORDER_SORTED);
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_sha256_text(canonical, writer.len, digest);
    bool same = yyjson_equals(yyjson_doc_get_root(fixture), yyjson_doc_get_root(ours));
    free(canonical);
    yyjson_doc_free(ours);
    yyjson_doc_free(fixture);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_TRUE(same);
    ASSERT_STREQ(digest, BWG_SERIAL_MANIFEST_SHA256);
}

static void accumulator_reports_overflow_once_and_recovers_after_lf(void)
{
    // Arrange
    bwg_accumulator_t accumulator;
    bwg_accumulator_init(&accumulator, &bwg_test_allocator);
    const uint8_t *line;
    size_t line_len;
    size_t oversized = 0;
    for (size_t i = 0; i < BWG_MAX_WIRE_FRAME_BYTES - 1; i++) {
        ASSERT_EQ(bwg_accumulator_push(&accumulator, 'x', &line, &line_len), BWG_ACC_NONE);
    }

    // Act
    for (size_t i = 0; i < 5; i++) {
        if (bwg_accumulator_push(&accumulator, 'x', &line, &line_len) == BWG_ACC_OVERSIZED) {
            oversized++;
        }
    }
    bwg_acc_result_t after_lf = bwg_accumulator_push(&accumulator, '\n', &line, &line_len);
    bwg_acc_result_t next = bwg_accumulator_push(&accumulator, '\n', &line, &line_len);
    size_t next_len = line_len;
    bwg_accumulator_deinit(&accumulator);

    // Assert
    ASSERT_EQ(oversized, 1);
    ASSERT_EQ(after_lf, BWG_ACC_NONE);
    ASSERT_EQ(next, BWG_ACC_LINE);
    ASSERT_EQ(next_len, 1);
}

static void accumulator_accepts_a_line_of_exactly_the_limit(void)
{
    // Arrange
    bwg_accumulator_t accumulator;
    bwg_accumulator_init(&accumulator, &bwg_test_allocator);
    const uint8_t *line = NULL;
    size_t line_len = 0;
    for (size_t i = 0; i < BWG_MAX_WIRE_FRAME_BYTES - 1; i++) {
        ASSERT_EQ(bwg_accumulator_push(&accumulator, 'x', &line, &line_len), BWG_ACC_NONE);
    }

    // Act
    bwg_acc_result_t result = bwg_accumulator_push(&accumulator, '\n', &line, &line_len);
    bwg_accumulator_deinit(&accumulator);

    // Assert
    ASSERT_EQ(result, BWG_ACC_LINE);
    ASSERT_EQ(line_len, BWG_MAX_WIRE_FRAME_BYTES);
}

static void accumulator_reports_allocation_failure_and_discards_the_line(void)
{
    // Arrange
    bwg_accumulator_t accumulator;
    bwg_accumulator_init(&accumulator, &bwg_test_allocator);
    bwg_test_alloc_fail_after(0);
    const uint8_t *line;
    size_t line_len;

    // Act
    bwg_acc_result_t first = bwg_accumulator_push(&accumulator, 'x', &line, &line_len);
    bwg_acc_result_t rest = bwg_accumulator_push(&accumulator, 'y', &line, &line_len);
    bwg_acc_result_t lf = bwg_accumulator_push(&accumulator, '\n', &line, &line_len);
    bwg_acc_result_t after = bwg_accumulator_push(&accumulator, '\n', &line, &line_len);
    bwg_accumulator_deinit(&accumulator);

    // Assert
    ASSERT_EQ(first, BWG_ACC_UNAVAILABLE);
    ASSERT_EQ(rest, BWG_ACC_NONE);
    ASSERT_EQ(lf, BWG_ACC_NONE);
    ASSERT_EQ(after, BWG_ACC_LINE);
}

static void accumulator_wipes_partial_content(void)
{
    // Arrange
    bwg_accumulator_t accumulator;
    bwg_accumulator_init(&accumulator, &bwg_test_allocator);
    bwg_test_alloc_watch_secret("secret-partial");
    const uint8_t *line;
    size_t line_len;
    size_t text_len = 0;
    uint8_t *text = copy_of("secret-partial", &text_len);
    for (size_t i = 0; i < text_len; i++) {
        bwg_accumulator_push(&accumulator, text[i], &line, &line_len);
    }

    // Act
    bwg_accumulator_clear(&accumulator);
    size_t len_after = accumulator.len;
    bwg_accumulator_deinit(&accumulator);
    free(text);

    // Assert: the allocator's secret watch fails this test if any block still holds the text.
    ASSERT_EQ(len_after, 0);
}

void bwg_tests_frame_stream(void)
{
    BWG_RUN(encode_matches_the_published_wire_bytes);
    BWG_RUN(encode_refuses_a_non_object_payload_and_small_buffers);
    BWG_RUN(manifest_matches_the_gate_and_its_digest);
    BWG_RUN(accumulator_reports_overflow_once_and_recovers_after_lf);
    BWG_RUN(accumulator_accepts_a_line_of_exactly_the_limit);
    BWG_RUN(accumulator_reports_allocation_failure_and_discards_the_line);
    BWG_RUN(accumulator_wipes_partial_content);
}
