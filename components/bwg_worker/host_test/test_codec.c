#include <sodium.h>

#include "bwg_codec.h"
#include "bwg_json_writer.h"
#include "bwg_test.h"
#include "test_suites.h"

static void encodes_rfc4648_vectors_without_padding(void)
{
    // Arrange
    const char *inputs[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *expected[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    char out[16];
    for (size_t i = 0; i < 7; i++) {
        // Act
        size_t written = 99;
        bool ok = bwg_b64url_encode((const uint8_t *) inputs[i], strlen(inputs[i]), out, sizeof(out), &written);
        // Assert
        ASSERT_TRUE(ok);
        ASSERT_STREQ(out, expected[i]);
        ASSERT_EQ(written, strlen(expected[i]));
    }
}

static void encode_uses_the_url_alphabet(void)
{
    // Arrange
    const uint8_t input[] = {0xfb, 0xff};
    char out[8];

    // Act
    bwg_b64url_encode(input, sizeof(input), out, sizeof(out), NULL);

    // Assert
    ASSERT_STREQ(out, "-_8");
}

static void encode_refuses_a_buffer_without_room_for_the_nul(void)
{
    // Arrange
    char out[4];

    // Act
    bool ok = bwg_b64url_encode((const uint8_t *) "foo", 3, out, sizeof(out), NULL);

    // Assert
    ASSERT_FALSE(ok);
}

static void decode_round_trips(void)
{
    // Arrange
    uint8_t out[16];
    size_t written = 0;

    // Act
    bool ok = bwg_b64url_decode("Zm9vYmE", 7, out, sizeof(out), &written);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_EQ(written, 5);
    ASSERT_MEMEQ(out, "fooba", 5);
}

static void decode_rejects_nonzero_trailing_bits(void)
{
    // Arrange: "Zg" is the canonical one-byte form; "Zh" leaves a trailing bit set.
    uint8_t out[4];
    size_t written = 0;

    // Act
    bool canonical = bwg_b64url_decode("Zg", 2, out, sizeof(out), &written);
    bool altered = bwg_b64url_decode("Zh", 2, out, sizeof(out), &written);

    // Assert
    ASSERT_TRUE(canonical);
    ASSERT_FALSE(altered);
}

static void decode_rejects_padding(void)
{
    // Arrange
    uint8_t out[4];
    size_t written = 0;

    // Act
    bool ok = bwg_b64url_decode("Zg==", 4, out, sizeof(out), &written);

    // Assert
    ASSERT_FALSE(ok);
}

static void decode_rejects_the_standard_alphabet(void)
{
    // Arrange
    uint8_t out[4];
    size_t written = 0;

    // Act
    bool plus = bwg_b64url_decode("+_8", 3, out, sizeof(out), &written);
    bool slash = bwg_b64url_decode("-/8", 3, out, sizeof(out), &written);

    // Assert
    ASSERT_FALSE(plus);
    ASSERT_FALSE(slash);
}

static void decode_rejects_a_single_trailing_character(void)
{
    // Arrange
    uint8_t out[8];
    size_t written = 0;

    // Act
    bool ok = bwg_b64url_decode("Zm9vY", 5, out, sizeof(out), &written);

    // Assert
    ASSERT_FALSE(ok);
}

static void decode_refuses_a_short_buffer(void)
{
    // Arrange
    uint8_t out[2];
    size_t written = 0;

    // Act
    bool ok = bwg_b64url_decode("Zm9v", 4, out, sizeof(out), &written);

    // Assert
    ASSERT_FALSE(ok);
}

static void canonical_check_matches_the_session_id_regex_end(void)
{
    // Arrange: a 16-byte value ends in one of A, Q, g, w.
    const char *good = "AAAAAAAAAAAAAAAAAAAAAQ";
    const char *bad = "AAAAAAAAAAAAAAAAAAAAAB";

    // Act / Assert
    ASSERT_TRUE(bwg_b64url_is_canonical(good, strlen(good), 16));
    ASSERT_FALSE(bwg_b64url_is_canonical(bad, strlen(bad), 16));
    ASSERT_FALSE(bwg_b64url_is_canonical(good, strlen(good), 32));
}

static void hex_is_lowercase(void)
{
    // Arrange
    const uint8_t input[] = {0x00, 0xab, 0xff};
    char out[7];

    // Act
    bwg_hex_encode(input, sizeof(input), out);

    // Assert
    ASSERT_STREQ(out, "00abff");
}

static void hex_check_rejects_uppercase_and_wrong_length(void)
{
    // Act / Assert
    ASSERT_TRUE(bwg_hex_is_lower("00abff", 6, 6));
    ASSERT_FALSE(bwg_hex_is_lower("00ABFF", 6, 6));
    ASSERT_FALSE(bwg_hex_is_lower("00ab", 4, 6));
    ASSERT_FALSE(bwg_hex_is_lower("00abfg", 6, 6));
}

static void digest_text_checks_only_alphabet_and_length(void)
{
    // Arrange
    char digest[44];
    memset(digest, 'T', 43);
    digest[43] = '\0';

    // Act / Assert
    ASSERT_TRUE(bwg_digest_text_is_valid(digest, 43));
    ASSERT_FALSE(bwg_digest_text_is_valid(digest, 42));
    digest[3] = '+';
    ASSERT_FALSE(bwg_digest_text_is_valid(digest, 43));
}

static void sha256_text_matches_the_empty_payload_vector(void)
{
    // Arrange
    char digest[BWG_DIGEST_TEXT_SIZE];

    // Act
    bwg_sha256_text((const uint8_t *) "{}", 2, digest);

    // Assert
    ASSERT_STREQ(digest, "RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o");
}

static void writer_emits_nested_values_without_whitespace(void)
{
    // Arrange
    uint8_t buffer[128];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bwg_jw_object_begin(&w);
    bwg_jw_key(&w, "a");
    bwg_jw_u64(&w, 18446744073709551615ULL);
    bwg_jw_key(&w, "b");
    bwg_jw_array_begin(&w);
    bwg_jw_cstr(&w, "x");
    bwg_jw_bool(&w, true);
    bwg_jw_null(&w);
    bwg_jw_i64(&w, -7);
    bwg_jw_object_begin(&w);
    bwg_jw_object_end(&w);
    bwg_jw_array_end(&w);
    bwg_jw_key(&w, "c");
    bwg_jw_raw_value(&w, (const uint8_t *) "{\"d\":1}", 7);
    bwg_jw_object_end(&w);

    // Assert
    const char *expected = "{\"a\":18446744073709551615,\"b\":[\"x\",true,null,-7,{}],\"c\":{\"d\":1}}";
    ASSERT_TRUE(bwg_jw_ok(&w));
    ASSERT_EQ(w.len, strlen(expected));
    ASSERT_MEMEQ(buffer, expected, w.len);
}

static void writer_escapes_quote_backslash_and_controls_like_serde_json(void)
{
    // Arrange
    const char input[] = "\"\\\b\f\n\r\t\x01\x1f\x7f/";
    uint8_t buffer[128];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bwg_jw_string(&w, input, sizeof(input) - 1);

    // Assert: 0x7f and the slash stay raw, as serde_json and JSON.stringify both write them.
    const char *expected = "\"\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u001f\x7f/\"";
    ASSERT_EQ(w.len, strlen(expected));
    ASSERT_MEMEQ(buffer, expected, w.len);
}

static void writer_keeps_non_ascii_raw(void)
{
    // Arrange
    const char *input = "π雪🚀";
    uint8_t buffer[32];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bwg_jw_cstr(&w, input);

    // Assert
    ASSERT_EQ(w.len, strlen(input) + 2);
    ASSERT_MEMEQ(buffer + 1, input, strlen(input));
}

static void writer_escapes_nul_inside_a_counted_string(void)
{
    // Arrange
    uint8_t buffer[16];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bwg_jw_string(&w, "a\0b", 3);

    // Assert
    ASSERT_EQ(w.len, 10);
    ASSERT_MEMEQ(buffer, "\"a\\u0000b\"", 10);
}

static void writer_latches_overflow(void)
{
    // Arrange
    uint8_t buffer[4];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    bwg_jw_cstr(&w, "too long");
    bwg_jw_u64(&w, 1);

    // Assert
    ASSERT_FALSE(bwg_jw_ok(&w));
    ASSERT_TRUE(w.len <= sizeof(buffer));
}

static void writer_rejects_depth_past_its_limit(void)
{
    // Arrange
    uint8_t buffer[128];
    bwg_jw_t w;
    bwg_jw_init_buffer(&w, buffer, sizeof(buffer));

    // Act
    for (int i = 0; i <= BWG_JW_MAX_DEPTH; i++) {
        bwg_jw_array_begin(&w);
    }

    // Assert
    ASSERT_FALSE(bwg_jw_ok(&w));
}

static void writer_hash_mode_equals_hashing_the_buffered_text(void)
{
    // Arrange
    uint8_t buffer[64];
    bwg_jw_t buffered;
    bwg_jw_init_buffer(&buffered, buffer, sizeof(buffer));
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    bwg_jw_t hashed;
    bwg_jw_init_hash(&hashed, &state);

    // Act
    bwg_jw_t *writers[] = {&buffered, &hashed};
    for (size_t i = 0; i < 2; i++) {
        bwg_jw_object_begin(writers[i]);
        bwg_jw_key(writers[i], "k\"ey");
        bwg_jw_cstr(writers[i], "v\\alue");
        bwg_jw_object_end(writers[i]);
    }
    uint8_t from_hash[32];
    uint8_t from_buffer[32];
    crypto_hash_sha256_final(&state, from_hash);
    crypto_hash_sha256(from_buffer, buffer, buffered.len);

    // Assert
    ASSERT_MEMEQ(from_hash, from_buffer, 32);
}

void bwg_tests_codec(void)
{
    BWG_RUN(encodes_rfc4648_vectors_without_padding);
    BWG_RUN(encode_uses_the_url_alphabet);
    BWG_RUN(encode_refuses_a_buffer_without_room_for_the_nul);
    BWG_RUN(decode_round_trips);
    BWG_RUN(decode_rejects_nonzero_trailing_bits);
    BWG_RUN(decode_rejects_padding);
    BWG_RUN(decode_rejects_the_standard_alphabet);
    BWG_RUN(decode_rejects_a_single_trailing_character);
    BWG_RUN(decode_refuses_a_short_buffer);
    BWG_RUN(canonical_check_matches_the_session_id_regex_end);
    BWG_RUN(hex_is_lowercase);
    BWG_RUN(hex_check_rejects_uppercase_and_wrong_length);
    BWG_RUN(digest_text_checks_only_alphabet_and_length);
    BWG_RUN(sha256_text_matches_the_empty_payload_vector);
    BWG_RUN(writer_emits_nested_values_without_whitespace);
    BWG_RUN(writer_escapes_quote_backslash_and_controls_like_serde_json);
    BWG_RUN(writer_keeps_non_ascii_raw);
    BWG_RUN(writer_escapes_nul_inside_a_counted_string);
    BWG_RUN(writer_latches_overflow);
    BWG_RUN(writer_rejects_depth_past_its_limit);
    BWG_RUN(writer_hash_mode_equals_hashing_the_buffered_text);
}
