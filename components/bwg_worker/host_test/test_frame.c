#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_frame.h"
#include "bwg_json.h"
#include "bwg_test.h"
#include "test_suites.h"

static bwg_frame_status_t parse_text(const char *text)
{
    bwg_record_t record;
    return bwg_record_parse((const uint8_t *) text, strlen(text), &record);
}

/** Builds a record whose envelope is well formed around any payload text. */
static size_t build_line(char *out, size_t capacity, const char *kind, const char *payload)
{
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_sha256_text((const uint8_t *) payload, strlen(payload), digest);
    return (size_t) snprintf(out, capacity,
                             "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"%s\","
                             "\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"sequence\":4,\"payloadBytes\":%zu,"
                             "\"payloadSha256\":\"%s\",\"payload\":%s}\n",
                             kind, strlen(payload), digest, payload);
}

static void fixture_frames_parse_to_their_expected_fields(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_val *frames = yyjson_obj_get(yyjson_doc_get_root(doc), "frames");
    yyjson_arr_iter iter = yyjson_arr_iter_with(frames);
    yyjson_val *frame;
    size_t seen = 0;
    while ((frame = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *wire = yyjson_obj_get(frame, "wire");
        yyjson_val *expected = yyjson_obj_get(frame, "frame");
        bwg_record_t record;

        // Act
        bwg_frame_status_t status =
            bwg_record_parse((const uint8_t *) yyjson_get_str(wire), yyjson_get_len(wire), &record);

        // Assert
        ASSERT_EQ(status, BWG_FRAME_OK);
        ASSERT_EQ(record.sequence, yyjson_get_uint(yyjson_obj_get(expected, "sequence")));
        ASSERT_EQ(record.payload_len, yyjson_get_uint(yyjson_obj_get(expected, "payloadBytes")));
        seen++;
    }
    ASSERT_EQ(seen, 4);
    yyjson_doc_free(doc);
}

static void fixture_wire_survives_seven_byte_chunks(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_val *frames = yyjson_obj_get(yyjson_doc_get_root(doc), "frames");
    char *stream = malloc(4096);
    size_t stream_len = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(frames);
    yyjson_val *frame;
    while ((frame = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *wire = yyjson_obj_get(frame, "wire");
        memcpy(stream + stream_len, yyjson_get_str(wire), yyjson_get_len(wire));
        stream_len += yyjson_get_len(wire);
    }
    bwg_accumulator_t accumulator;
    bwg_accumulator_init(&accumulator, &bwg_test_allocator);
    static const uint32_t expected_sequences[] = {0, 1, 2, 3};
    size_t decoded = 0;

    // Act
    for (size_t offset = 0; offset < stream_len; offset += 7) {
        size_t end = offset + 7 < stream_len ? offset + 7 : stream_len;
        for (size_t i = offset; i < end; i++) {
            const uint8_t *line;
            size_t line_len;
            if (bwg_accumulator_push(&accumulator, (uint8_t) stream[i], &line, &line_len) == BWG_ACC_LINE) {
                bwg_record_t record;
                ASSERT_EQ(bwg_record_parse(line, line_len, &record), BWG_FRAME_OK);
                ASSERT_EQ(record.sequence, expected_sequences[decoded]);
                decoded++;
                bwg_accumulator_clear(&accumulator);
            }
        }
    }
    bwg_accumulator_deinit(&accumulator);
    free(stream);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_EQ(decoded, 4);
}

static void every_fixture_frame_refuses_crlf(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "frames"));
    yyjson_val *frame;
    while ((frame = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *wire = yyjson_obj_get(frame, "wire");
        size_t len = yyjson_get_len(wire);
        uint8_t *crlf = malloc(len + 1);
        memcpy(crlf, yyjson_get_str(wire), len - 1);
        crlf[len - 1] = '\r';
        crlf[len] = '\n';
        bwg_record_t record;

        // Act
        bwg_frame_status_t status = bwg_record_parse(crlf, len + 1, &record);
        free(crlf);

        // Assert
        ASSERT_EQ(status, BWG_FRAME_INVALID);
    }
    yyjson_doc_free(doc);
}

static void every_fixture_frame_refuses_an_unknown_field(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "frames"));
    yyjson_val *frame;
    while ((frame = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *wire = yyjson_obj_get(frame, "wire");
        char altered[2048];
        int written = snprintf(altered, sizeof(altered), "{\"extra\":1,%s", yyjson_get_str(wire) + 1);
        ASSERT_TRUE(written > 0);

        // Act
        bwg_frame_status_t status = parse_text(altered);

        // Assert
        ASSERT_EQ(status, BWG_FRAME_INVALID);
    }
    yyjson_doc_free(doc);
}

static void every_fixture_frame_refuses_a_duplicate_sequence(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/fixtures.json");
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "frames"));
    yyjson_val *frame;
    while ((frame = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *wire = yyjson_obj_get(frame, "wire");
        char altered[2048];
        int written = snprintf(altered, sizeof(altered), "{\"sequence\":9,%s", yyjson_get_str(wire) + 1);
        ASSERT_TRUE(written > 0);

        // Act
        bwg_frame_status_t status = parse_text(altered);

        // Assert
        ASSERT_EQ(status, BWG_FRAME_INVALID);
    }
    yyjson_doc_free(doc);
}

static void control_payload_one_byte_too_long_is_oversized(void)
{
    // Arrange: a payload of 65,537 bytes whose hash and length are otherwise right.
    size_t payload_len = BWG_MAX_CONTROL_PAYLOAD_BYTES + 1;
    char *payload = malloc(payload_len + 1);
    memset(payload, ' ', payload_len);
    payload[0] = '{';
    payload[payload_len - 1] = '}';
    payload[payload_len] = '\0';
    char *line = malloc(BWG_MAX_WIRE_FRAME_BYTES);
    size_t line_len = build_line(line, BWG_MAX_WIRE_FRAME_BYTES, "control", payload);
    bwg_record_t record;

    // Act
    bwg_frame_status_t status = bwg_record_parse((const uint8_t *) line, line_len, &record);
    free(line);
    free(payload);

    // Assert
    ASSERT_EQ(status, BWG_FRAME_OVERSIZED);
}

static void control_payload_at_the_limit_is_accepted(void)
{
    // Arrange
    size_t payload_len = BWG_MAX_CONTROL_PAYLOAD_BYTES;
    char *payload = malloc(payload_len + 1);
    memset(payload, ' ', payload_len);
    payload[0] = '{';
    payload[payload_len - 1] = '}';
    payload[payload_len] = '\0';
    char *line = malloc(BWG_MAX_WIRE_FRAME_BYTES);
    size_t line_len = build_line(line, BWG_MAX_WIRE_FRAME_BYTES, "control", payload);
    bwg_record_t record;

    // Act
    bwg_frame_status_t status = bwg_record_parse((const uint8_t *) line, line_len, &record);
    free(line);
    free(payload);

    // Assert
    ASSERT_EQ(status, BWG_FRAME_OK);
    ASSERT_EQ(record.payload_len, payload_len);
}

static void integrity_vectors_encode_to_their_published_digests(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/integrity-vectors.json");
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "vectors"));
    yyjson_val *vector;
    uint8_t *out = malloc(4096);
    size_t seen = 0;
    while ((vector = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *json = yyjson_obj_get(vector, "payloadJson");
        size_t out_len = 0;

        // Act
        bwg_frame_status_t status = bwg_record_encode(BWG_KIND_CONTROL, "AAAAAAAAAAAAAAAAAAAAAA", 2,
                                                      (const uint8_t *) yyjson_get_str(json), yyjson_get_len(json),
                                                      out, 4096, &out_len);
        bwg_record_t record;
        bwg_frame_status_t reparsed = bwg_record_parse(out, out_len, &record);
        char digest[BWG_DIGEST_TEXT_SIZE];
        bwg_sha256_text(record.payload, record.payload_len, digest);

        // Assert
        ASSERT_EQ(status, BWG_FRAME_OK);
        ASSERT_EQ(reparsed, BWG_FRAME_OK);
        ASSERT_EQ(record.payload_len, yyjson_get_uint(yyjson_obj_get(vector, "payloadBytes")));
        ASSERT_STREQ(digest, yyjson_get_str(yyjson_obj_get(vector, "payloadSha256")));
        seen++;
    }
    free(out);
    yyjson_doc_free(doc);
    ASSERT_EQ(seen, 6);
}

static void integrity_vectors_refuse_a_wrong_length_or_digest(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("serial/integrity-vectors.json");
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "vectors"));
    yyjson_val *vector;
    while ((vector = yyjson_arr_iter_next(&iter)) != NULL) {
        const char *payload = yyjson_get_str(yyjson_obj_get(vector, "payloadJson"));
        uint64_t bytes = yyjson_get_uint(yyjson_obj_get(vector, "payloadBytes"));
        const char *digest = yyjson_get_str(yyjson_obj_get(vector, "payloadSha256"));
        char wrong_length[1024];
        char wrong_digest[1024];
        snprintf(wrong_length, sizeof(wrong_length),
                 "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"control\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
                 "\"sequence\":1,\"payloadBytes\":%llu,\"payloadSha256\":\"%s\",\"payload\":%s}\n",
                 (unsigned long long) bytes + 1, digest, payload);
        snprintf(wrong_digest, sizeof(wrong_digest),
                 "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"control\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
                 "\"sequence\":1,\"payloadBytes\":%llu,\"payloadSha256\":\"%s\",\"payload\":%s}\n",
                 (unsigned long long) bytes, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", payload);

        // Act / Assert
        ASSERT_EQ(parse_text(wrong_length), BWG_FRAME_INTEGRITY);
        ASSERT_EQ(parse_text(wrong_digest), BWG_FRAME_INTEGRITY);
    }
    yyjson_doc_free(doc);
}

static void hash_covers_the_raw_payload_bytes(void)
{
    // Arrange: same value as `{}`, different bytes, hashed as transmitted.
    char line[512];
    build_line(line, sizeof(line), "heartbeat", "{ }");
    bwg_record_t record;

    // Act
    bwg_frame_status_t status = bwg_record_parse((const uint8_t *) line, strlen(line), &record);

    // Assert
    ASSERT_EQ(status, BWG_FRAME_OK);
    ASSERT_EQ(record.payload_len, 3);
    ASSERT_MEMEQ(record.payload, "{ }", 3);
}

static void envelope_key_order_and_outer_spaces_are_free(void)
{
    // Arrange
    const char *line =
        " { \"payload\" : {} , \"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payloadBytes\":2,"
        "\"sequence\":7,\"sessionId\":null,\"kind\":\"heartbeat\",\"profile\":\"bwg-worker-serial/0.2\"} \n";
    bwg_record_t record;

    // Act
    bwg_frame_status_t status = bwg_record_parse((const uint8_t *) line, strlen(line), &record);

    // Assert
    ASSERT_EQ(status, BWG_FRAME_OK);
    ASSERT_EQ(record.sequence, 7);
    ASSERT_TRUE(record.maybe_session_id == NULL);
    ASSERT_EQ(record.kind, BWG_KIND_HEARTBEAT);
}

static void escaped_envelope_values_are_invalid(void)
{
    // Arrange: each field is valid once unescaped, but envelopes never escape.
    const char *lines[] = {
        "{\"profile\":\"bwg-worker-serial\\/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbea\\u0074\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"\\u0041AAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
    };
    // Act / Assert
    for (size_t i = 0; i < 3; i++) {
        ASSERT_EQ(parse_text(lines[i]), BWG_FRAME_INVALID);
    }
}

static void envelope_field_shapes_are_checked(void)
{
    // Arrange
    const char *lines[] = {
        "",                                                             // no LF even
        "{}\n",                                                         // missing fields
        "not json\n",                                                   //
        "{\"profile\":\"bwg-worker-serial/0.1\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"ping\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAB\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1.0,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":01,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":-1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":4294967296,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":[]}\n",
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o\",\"payload\":{}} x\n",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        ASSERT_EQ(parse_text(lines[i]), BWG_FRAME_INVALID);
    }
}

static void non_canonical_payload_digest_is_an_integrity_failure(void)
{
    // Arrange: a 43-character digest with nonzero trailing bits.
    const char *line =
        "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"heartbeat\",\"sessionId\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"sequence\":1,\"payloadBytes\":2,\"payloadSha256\":\"RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4p\",\"payload\":{}}\n";

    // Act / Assert
    ASSERT_EQ(parse_text(line), BWG_FRAME_INTEGRITY);
}

static void a_line_past_the_wire_limit_is_oversized(void)
{
    // Arrange
    size_t len = BWG_MAX_WIRE_FRAME_BYTES + 1;
    uint8_t *line = malloc(len);
    memset(line, ' ', len);
    bwg_record_t record;

    // Act
    bwg_frame_status_t status = bwg_record_parse(line, len, &record);
    free(line);

    // Assert
    ASSERT_EQ(status, BWG_FRAME_OVERSIZED);
}

void bwg_tests_frame(void)
{
    BWG_RUN(fixture_frames_parse_to_their_expected_fields);
    BWG_RUN(fixture_wire_survives_seven_byte_chunks);
    BWG_RUN(every_fixture_frame_refuses_crlf);
    BWG_RUN(every_fixture_frame_refuses_an_unknown_field);
    BWG_RUN(every_fixture_frame_refuses_a_duplicate_sequence);
    BWG_RUN(control_payload_one_byte_too_long_is_oversized);
    BWG_RUN(control_payload_at_the_limit_is_accepted);
    BWG_RUN(integrity_vectors_encode_to_their_published_digests);
    BWG_RUN(integrity_vectors_refuse_a_wrong_length_or_digest);
    BWG_RUN(hash_covers_the_raw_payload_bytes);
    BWG_RUN(envelope_key_order_and_outer_spaces_are_free);
    BWG_RUN(escaped_envelope_values_are_invalid);
    BWG_RUN(envelope_field_shapes_are_checked);
    BWG_RUN(non_canonical_payload_digest_is_an_integrity_failure);
    BWG_RUN(a_line_past_the_wire_limit_is_oversized);
}
