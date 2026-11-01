#include <stdio.h>

#include "bwg_json.h"
#include "bwg_scan.h"
#include "bwg_test.h"
#include "test_suites.h"

static bool scan_ok(const char *text)
{
    return bwg_scan_document((const uint8_t *) text, strlen(text));
}

static void scanner_accepts_every_rfc8259_shape(void)
{
    // Arrange
    const char *valid[] = {
        "{}", "[]", "null", "true", "false", "0", "-0", "1.5", "1e2", "-1.5E-3", "\"\"", " { \"a\" : [ 1 , { } ] } ",
        "{\"a\":{\"b\":[[],[{}]]},\"c\":\"\\u00e9\\n\"}", "[1,2,3]", "\"π雪🚀\"",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        ASSERT_TRUE(scan_ok(valid[i]));
    }
}

static void scanner_rejects_malformed_json(void)
{
    // Arrange
    const char *invalid[] = {
        "", " ", "{", "}", "[", "]", "[1,]", "{,}", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}", "01", "1.", ".5",
        "1e", "+1", "--1", "tru", "nul", "\"", "\"\\x\"", "\"\\u12G4\"", "[1 2]", "{\"a\":1 \"b\":2}", "[}", "{]", "1 2",
        "{}{}", "\"a\nb\"",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        ASSERT_FALSE(scan_ok(invalid[i]));
    }
}

static void scanner_rejects_invalid_utf8(void)
{
    // Arrange: lone continuation, overlong, surrogate, past U+10FFFF, truncated.
    const char *invalid[] = {"\"\x80\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"", "\"\xE2\x82\""};
    // Act / Assert
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        ASSERT_FALSE(scan_ok(invalid[i]));
    }
}

static void scanner_reports_escapes(void)
{
    // Arrange
    const uint8_t *text = (const uint8_t *) "\"a\\\"b\"x";
    size_t pos = 0;
    bool has_escape = false;

    // Act
    bool ok = bwg_scan_string(text, 7, &pos, &has_escape);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_TRUE(has_escape);
    ASSERT_EQ(pos, 6);
}

static void scanner_value_stops_at_the_end_of_the_span(void)
{
    // Arrange
    const uint8_t *text = (const uint8_t *) "{\"a\":[1,2]} ,";
    size_t pos = 0;

    // Act
    bool ok = bwg_scan_value(text, 13, &pos);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_EQ(pos, 11);
}

static void scanner_refuses_nesting_past_128(void)
{
    // Arrange
    char accepted[257];
    char refused[259];
    memset(accepted, '[', 128);
    memset(accepted + 128, ']', 128);
    accepted[256] = '\0';
    memset(refused, '[', 129);
    memset(refused + 129, ']', 129);
    refused[258] = '\0';

    // Act / Assert
    ASSERT_TRUE(scan_ok(accepted));
    ASSERT_FALSE(scan_ok(refused));
}

static bwg_json_status_t strict(const char *text, yyjson_doc **doc)
{
    return bwg_json_parse_strict(&bwg_test_allocator, (const uint8_t *) text, strlen(text), doc);
}

static void strict_parse_accepts_unique_keys(void)
{
    // Arrange
    yyjson_doc *doc = NULL;

    // Act
    bwg_json_status_t status = strict("{\"a\":{\"a\":1},\"b\":[{\"a\":2},{\"a\":3}]}", &doc);

    // Assert
    ASSERT_EQ(status, BWG_JSON_OK);
    yyjson_doc_free(doc);
}

static void strict_parse_rejects_duplicates_at_every_depth(void)
{
    // Arrange
    const char *duplicates[] = {
        "{\"a\":1,\"a\":2}",
        "{\"o\":{\"a\":1,\"b\":2,\"a\":3}}",
        "{\"o\":[{\"x\":[{\"y\":1,\"y\":2}]}]}",
        "[[[{\"k\":null,\"k\":null}]]]",
    };
    for (size_t i = 0; i < sizeof(duplicates) / sizeof(duplicates[0]); i++) {
        yyjson_doc *doc = NULL;
        // Act
        bwg_json_status_t status = strict(duplicates[i], &doc);
        // Assert
        ASSERT_EQ(status, BWG_JSON_REJECTED);
        ASSERT_TRUE(doc == NULL);
    }
}

static void strict_parse_compares_decoded_keys(void)
{
    // Arrange: an escape cannot disguise a repeated key.
    yyjson_doc *doc = NULL;

    // Act
    bwg_json_status_t status = strict("{\"a\":1,\"\\u0061\":2}", &doc);

    // Assert
    ASSERT_EQ(status, BWG_JSON_REJECTED);
}

static void strict_parse_rejects_an_oversized_object(void)
{
    // Arrange
    char text[4096];
    size_t at = 0;
    text[at++] = '{';
    for (int i = 0; i <= BWG_JSON_MAX_OBJECT_KEYS; i++) {
        at += (size_t) snprintf(text + at, sizeof(text) - at, "%s\"%d\":0", i == 0 ? "" : ",", i);
    }
    text[at++] = '}';
    text[at] = '\0';
    yyjson_doc *doc = NULL;

    // Act
    bwg_json_status_t status = strict(text, &doc);

    // Assert
    ASSERT_EQ(status, BWG_JSON_REJECTED);
}

static void parse_reports_syntax_errors(void)
{
    // Arrange
    yyjson_doc *doc = NULL;

    // Act
    bwg_json_status_t status = strict("{\"a\":1} trailing", &doc);

    // Assert
    ASSERT_EQ(status, BWG_JSON_SYNTAX);
}

static void parse_rejects_a_lone_surrogate_escape(void)
{
    // Arrange
    yyjson_doc *doc = NULL;

    // Act
    bwg_json_status_t status = strict("{\"a\":\"\\ud800\"}", &doc);

    // Assert
    ASSERT_EQ(status, BWG_JSON_SYNTAX);
}

static void parse_reports_allocation_failure(void)
{
    // Arrange
    yyjson_doc *doc = NULL;
    bwg_test_alloc_fail_after(0);

    // Act
    bwg_json_status_t status = strict("{}", &doc);

    // Assert
    ASSERT_EQ(status, BWG_JSON_NO_MEMORY);
}

static void parsed_documents_are_zeroized_when_freed(void)
{
    // Arrange
    yyjson_doc *doc = NULL;
    bwg_test_alloc_watch_secret("hunter2-secret");

    // Act
    bwg_json_status_t status = strict("{\"password\":\"hunter2-secret\"}", &doc);
    yyjson_doc_free(doc);

    // Assert: the allocator fails this test if the secret survives in a freed block.
    ASSERT_EQ(status, BWG_JSON_OK);
}

static void exact_fields_accepts_required_and_optional(void)
{
    // Arrange
    static const char *const required[] = {"a", "b"};
    static const char *const optional[] = {"c"};
    yyjson_doc *with = yyjson_read("{\"b\":1,\"a\":2,\"c\":3}", 19, 0);
    yyjson_doc *without = yyjson_read("{\"a\":1,\"b\":2}", 13, 0);

    // Act
    bool with_ok = bwg_json_has_exact_fields(yyjson_doc_get_root(with), required, 2, optional, 1);
    bool without_ok = bwg_json_has_exact_fields(yyjson_doc_get_root(without), required, 2, optional, 1);
    yyjson_doc_free(with);
    yyjson_doc_free(without);

    // Assert
    ASSERT_TRUE(with_ok);
    ASSERT_TRUE(without_ok);
}

static void exact_fields_rejects_missing_unknown_and_non_objects(void)
{
    // Arrange
    static const char *const required[] = {"a", "b"};
    const char *shapes[] = {"{\"a\":1}", "{\"a\":1,\"b\":2,\"z\":3}", "[1]", "null"};
    for (size_t i = 0; i < 4; i++) {
        yyjson_doc *doc = yyjson_read(shapes[i], strlen(shapes[i]), 0);
        // Act
        bool ok = bwg_json_has_exact_fields(yyjson_doc_get_root(doc), required, 2, NULL, 0);
        yyjson_doc_free(doc);
        // Assert
        ASSERT_FALSE(ok);
    }
}

static void get_u64_reads_only_non_negative_integers(void)
{
    // Arrange
    const char *refused[] = {"1.0", "1e3", "-1", "-0", "\"1\"", "true", "null", "18446744073709551616"};
    uint64_t value = 0;
    yyjson_doc *accepted = yyjson_read("18446744073709551615", 20, 0);

    // Act
    bool max_ok = bwg_json_get_u64(yyjson_doc_get_root(accepted), &value);
    yyjson_doc_free(accepted);

    // Assert
    ASSERT_TRUE(max_ok);
    ASSERT_EQ(value, UINT64_MAX);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        yyjson_doc *doc = yyjson_read(refused[i], strlen(refused[i]), 0);
        bool ok = bwg_json_get_u64(yyjson_doc_get_root(doc), &value);
        yyjson_doc_free(doc);
        ASSERT_FALSE(ok);
    }
}

static void canonical_write_sorts_keys_recursively(void)
{
    // Arrange
    const char *input = "{\"z\":1,\"a\":{\"y\":[{\"b\":1,\"a\":2}],\"x\":\"t\"},\"Z\":true,\"aa\":null}";
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    uint8_t buffer[128];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bool ok = bwg_json_write_value(&w, yyjson_doc_get_root(doc), BWG_JSON_ORDER_SORTED);
    yyjson_doc_free(doc);

    // Assert
    const char *expected = "{\"Z\":true,\"a\":{\"x\":\"t\",\"y\":[{\"a\":2,\"b\":1}]},\"aa\":null,\"z\":1}";
    ASSERT_TRUE(ok);
    ASSERT_EQ(w.len, strlen(expected));
    ASSERT_MEMEQ(buffer, expected, w.len);
}

static void document_order_write_keeps_order_and_strips_whitespace(void)
{
    // Arrange
    const char *input = "{ \"z\" : 1,\n \"a\" : [ true ] }";
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    uint8_t buffer[64];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bool ok = bwg_json_write_value(&w, yyjson_doc_get_root(doc), BWG_JSON_ORDER_DOCUMENT);
    yyjson_doc_free(doc);

    // Assert
    const char *expected = "{\"z\":1,\"a\":[true]}";
    ASSERT_TRUE(ok);
    ASSERT_EQ(w.len, strlen(expected));
    ASSERT_MEMEQ(buffer, expected, w.len);
}

static void canonical_write_refuses_non_integer_numbers(void)
{
    // Arrange
    yyjson_doc *doc = yyjson_read("{\"a\":1.5}", 9, 0);
    uint8_t buffer[64];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bool ok = bwg_json_write_value(&w, yyjson_doc_get_root(doc), BWG_JSON_ORDER_SORTED);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_FALSE(ok);
}

void bwg_tests_json(void)
{
    BWG_RUN(scanner_accepts_every_rfc8259_shape);
    BWG_RUN(scanner_rejects_malformed_json);
    BWG_RUN(scanner_rejects_invalid_utf8);
    BWG_RUN(scanner_reports_escapes);
    BWG_RUN(scanner_value_stops_at_the_end_of_the_span);
    BWG_RUN(scanner_refuses_nesting_past_128);
    BWG_RUN(strict_parse_accepts_unique_keys);
    BWG_RUN(strict_parse_rejects_duplicates_at_every_depth);
    BWG_RUN(strict_parse_compares_decoded_keys);
    BWG_RUN(strict_parse_rejects_an_oversized_object);
    BWG_RUN(parse_reports_syntax_errors);
    BWG_RUN(parse_rejects_a_lone_surrogate_escape);
    BWG_RUN(parse_reports_allocation_failure);
    BWG_RUN(parsed_documents_are_zeroized_when_freed);
    BWG_RUN(exact_fields_accepts_required_and_optional);
    BWG_RUN(exact_fields_rejects_missing_unknown_and_non_objects);
    BWG_RUN(get_u64_reads_only_non_negative_integers);
    BWG_RUN(canonical_write_sorts_keys_recursively);
    BWG_RUN(document_order_write_keeps_order_and_strips_whitespace);
    BWG_RUN(canonical_write_refuses_non_integer_numbers);
}
