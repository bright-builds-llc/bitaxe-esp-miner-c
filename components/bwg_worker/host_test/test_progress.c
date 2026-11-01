#include <string.h>

#include "bwg_progress.h"
#include "bwg_test.h"
#include "test_suites.h"

static size_t write_progress(uint8_t *out, size_t capacity)
{
    bwg_jw_t json;
    bwg_jw_init_buffer(&json, out, capacity);
    bwg_progress_write(&json, 7, 1234);
    return bwg_jw_ok(&json) ? json.len : 0;
}

static void the_shape_matches_the_gates_parser(void)
{
    // Arrange
    bwg_progress_begin();
    bwg_progress_note(BWG_PROGRESS_POLL_REQUESTED);
    bwg_progress_note(BWG_PROGRESS_POLL_NONCE);
    bwg_progress_note(BWG_PROGRESS_BELOW_POOL_TARGET);
    uint8_t out[1024];

    // Act
    size_t len = write_progress(out, sizeof(out));

    // Assert
    const char *expected =
        "{\"schema\":\"worker-mining-progress-v2\",\"generation\":7,\"observed_at_ms\":\"1234\","
        "\"poll_requested\":\"1\",\"poll_idle\":\"0\",\"poll_nonce\":\"1\",\"poll_register\":\"0\","
        "\"stale_completion\":\"0\",\"qualified_candidates\":\"0\",\"below_pool_target\":\"1\","
        "\"duplicate_candidates\":\"0\","
        "\"discards\":{\"invalid_length\":\"0\",\"invalid_preamble\":\"0\",\"invalid_crc\":\"0\",\"job_lookup\":\"0\","
        "\"core\":\"0\",\"address_interval\":\"0\",\"register_response\":\"0\",\"parser_invariant\":\"0\"},"
        "\"blocked\":{\"wrong_session\":\"0\",\"job_lookup\":\"0\",\"work_stale\":\"0\",\"target_mismatch\":\"0\","
        "\"other\":\"0\"},"
        "\"expected_filter\":\"bm1366-ticket-256-leading-zero-40-v1\","
        "\"expected_filter_matches\":\"0\",\"expected_filter_misses\":\"0\"}";
    ASSERT_EQ(len, strlen(expected));
    ASSERT_MEMEQ(out, expected, len);
}

static void an_unexplained_empty_poll_counts_as_a_register_discard(void)
{
    // Arrange
    bwg_progress_begin();
    bwg_progress_note(BWG_PROGRESS_POLL_EMPTY);
    bwg_progress_note(BWG_PROGRESS_POLL_EMPTY);
    bwg_progress_note(BWG_PROGRESS_POLL_IDLE);
    uint8_t out[1024];

    // Act
    size_t len = write_progress(out, sizeof(out));

    // Assert
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *) out, "\"register_response\":\"1\"") != NULL);
}

static void a_frozen_count_ignores_later_events(void)
{
    // Arrange
    bwg_progress_begin();
    bwg_progress_note(BWG_PROGRESS_POLL_NONCE);
    bwg_progress_freeze();
    bwg_progress_note(BWG_PROGRESS_POLL_NONCE);
    uint8_t out[1024];

    // Act
    size_t len = write_progress(out, sizeof(out));

    // Assert
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *) out, "\"poll_nonce\":\"1\"") != NULL);
}

static void a_new_lease_counts_from_zero(void)
{
    // Arrange
    bwg_progress_begin();
    bwg_progress_note(BWG_PROGRESS_POLL_NONCE);
    uint8_t out[1024];

    // Act
    bwg_progress_begin();
    size_t len = write_progress(out, sizeof(out));

    // Assert
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *) out, "\"poll_nonce\":\"0\"") != NULL);
}

static void the_last_hash_under_two_to_the_216_meets_the_expected_filter(void)
{
    // Arrange
    bwg_progress_begin();
    uint8_t hash[32];
    memset(hash, 0xff, sizeof(hash));
    memset(&hash[27], 0, 5);
    uint8_t out[1024];

    // Act
    bwg_progress_note_expected_filter(hash);
    size_t len = write_progress(out, sizeof(out));

    // Assert
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *) out, "\"expected_filter_matches\":\"1\",\"expected_filter_misses\":\"0\"") != NULL);
}

static void any_high_filter_bit_misses_the_expected_filter(void)
{
    // Arrange
    bwg_progress_begin();
    uint8_t hash[32] = {0};
    hash[27] = 1;
    uint8_t out[1024];

    // Act
    bwg_progress_note_expected_filter(hash);
    size_t len = write_progress(out, sizeof(out));

    // Assert
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *) out, "\"expected_filter_matches\":\"0\",\"expected_filter_misses\":\"1\"") != NULL);
}

void bwg_tests_progress(void)
{
    BWG_RUN(the_last_hash_under_two_to_the_216_meets_the_expected_filter);
    BWG_RUN(any_high_filter_bit_misses_the_expected_filter);
    BWG_RUN(the_shape_matches_the_gates_parser);
    BWG_RUN(an_unexplained_empty_poll_counts_as_a_register_discard);
    BWG_RUN(a_frozen_count_ignores_later_events);
    BWG_RUN(a_new_lease_counts_from_zero);
}
