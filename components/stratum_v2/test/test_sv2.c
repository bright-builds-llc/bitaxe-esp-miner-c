#include "unity.h"
#include "sv2_protocol.h"
#include "sv2_noise.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

TEST_CASE("SV2 parse open extended channel extranonce bounds", "[sv2]")
{
    // Frame structure:
    // request_id(4) + channel_id(4) + target(32) + extranonce_size(2) + B0_32(1+N) + group_channel_id(4)
    uint8_t payload[128];
    uint32_t req_id, chan_id, group_id;
    uint8_t target[32];
    uint16_t extranonce_size;
    uint8_t prefix[32];
    uint8_t prefix_len;

    // 1. Valid case: prefix_len = 8, extranonce_size = 8 (total 16 <= 32)
    memset(payload, 0, sizeof(payload));
    int pos = 0;
    // req_id = 1, chan_id = 2
    payload[pos++] = 1; pos += 3;
    payload[pos++] = 2; pos += 3;
    pos += 32; // target
    payload[pos++] = 8; payload[pos++] = 0; // extranonce_size = 8
    payload[pos++] = 8; // prefix_len = 8
    pos += 8; // prefix bytes
    payload[pos++] = 3; pos += 3; // group_channel_id = 3

    TEST_ASSERT_EQUAL(0, sv2_parse_open_extended_channel_success(
        payload, pos, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));
    TEST_ASSERT_EQUAL_UINT16(8, extranonce_size);
    TEST_ASSERT_EQUAL_UINT8(8, prefix_len);

    // 2. Invalid case: extranonce_size = 33 (> 32)
    payload[40] = 33; payload[41] = 0;
    TEST_ASSERT_EQUAL(-1, sv2_parse_open_extended_channel_success(
        payload, pos, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));

    // 3. Invalid case: prefix_len = 20, extranonce_size = 16 (sum 36 > 32)
    payload[40] = 16; payload[41] = 0; // size = 16
    payload[42] = 20; // prefix_len = 20
    uint32_t len_36 = 40 + 2 + 1 + 20 + 4;
    TEST_ASSERT_EQUAL(-1, sv2_parse_open_extended_channel_success(
        payload, len_36, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));

    // 4. Exact boundary case: prefix_len = 16, extranonce_size = 16 (sum 32)
    payload[40] = 16; payload[41] = 0; // size = 16
    payload[42] = 16; // prefix_len = 16
    uint32_t len_32 = 40 + 2 + 1 + 16 + 4;
    TEST_ASSERT_EQUAL(0, sv2_parse_open_extended_channel_success(
        payload, len_32, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));
    TEST_ASSERT_EQUAL_UINT16(16, extranonce_size);
    TEST_ASSERT_EQUAL_UINT8(16, prefix_len);

    // 5. Invalid case: extranonce_size < 2 (e.g. 0 and 1)
    payload[40] = 0; payload[41] = 0;
    TEST_ASSERT_EQUAL(-1, sv2_parse_open_extended_channel_success(
        payload, len_32, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));
    payload[40] = 1; payload[41] = 0;
    TEST_ASSERT_EQUAL(-1, sv2_parse_open_extended_channel_success(
        payload, len_32, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));

    // 6. Framing checks: trailing byte or truncated byte rejected
    payload[40] = 16; payload[41] = 0; // restore valid size
    TEST_ASSERT_EQUAL(-1, sv2_parse_open_extended_channel_success(
        payload, len_32 + 1, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));
    TEST_ASSERT_EQUAL(-1, sv2_parse_open_extended_channel_success(
        payload, len_32 - 1, &req_id, &chan_id, target, &extranonce_size, prefix, &prefix_len, &group_id));
}

TEST_CASE("SV2 parse submit shares error", "[sv2]")
{
    uint8_t payload[64];
    memset(payload, 0, sizeof(payload));
    payload[0] = 1; // channel_id = 1
    payload[4] = 42; // seq_num = 42
    payload[8] = 9; // strlen = 9
    memcpy(payload + 9, "stale-job", 9);

    uint32_t chan_id, seq;
    char error_code[32];
    TEST_ASSERT_EQUAL(0, sv2_parse_submit_shares_error(payload, 9 + 9, &chan_id, &seq, error_code, sizeof(error_code)));
    TEST_ASSERT_EQUAL_UINT32(1, chan_id);
    TEST_ASSERT_EQUAL_UINT32(42, seq);
    TEST_ASSERT_EQUAL_STRING("stale-job", error_code);
}

TEST_CASE("SV2 parse new extended mining job direct decoding", "[sv2]")
{
    uint8_t payload[256];
    memset(payload, 0, sizeof(payload));
    int pos = 0;

    // channel_id = 5
    payload[pos++] = 5; pos += 3;
    // job_id = 12
    payload[pos++] = 12; pos += 3;
    // min_ntime option flag = 1, min_ntime = 0x647025b5
    payload[pos++] = 1;
    payload[pos++] = 0xb5; payload[pos++] = 0x25; payload[pos++] = 0x70; payload[pos++] = 0x64;
    // version = 0x20000000
    payload[pos++] = 0x00; payload[pos++] = 0x00; payload[pos++] = 0x00; payload[pos++] = 0x20;
    // version_rolling_allowed = 1
    payload[pos++] = 1;
    // merkle_count = 1
    payload[pos++] = 1;
    memset(payload + pos, 0xab, 32);
    pos += 32;

    // prefix: len = 4, data = 0x01020304
    payload[pos++] = 4; payload[pos++] = 0;
    payload[pos++] = 1; payload[pos++] = 2; payload[pos++] = 3; payload[pos++] = 4;

    // suffix: len = 3, data = 0x050607
    payload[pos++] = 3; payload[pos++] = 0;
    payload[pos++] = 5; payload[pos++] = 6; payload[pos++] = 7;

    uint8_t p_buf[1024];
    uint8_t s_buf[2048];
    miner_job_t job;
    memset(&job, 0, sizeof(job));
    job.coinbase_prefix = p_buf;
    job.coinbase_suffix = s_buf;

    uint32_t channel_id = 0;
    bool has_min_ntime = false;
    bool version_rolling_allowed = false;
    int res = sv2_parse_new_extended_mining_job(payload, pos, &channel_id, &job, &has_min_ntime, &version_rolling_allowed);

    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL_UINT32(5, channel_id);
    TEST_ASSERT_TRUE(has_min_ntime);
    TEST_ASSERT_TRUE(version_rolling_allowed);
    TEST_ASSERT_EQUAL_STRING("12", job.job_id);
    TEST_ASSERT_EQUAL_HEX32(0x20000000, job.version);
    TEST_ASSERT_EQUAL_HEX32(0x647025b5, job.ntime);
    TEST_ASSERT_EQUAL(1, job.merkle_path_count);
    TEST_ASSERT_EQUAL_HEX8(0xab, job.merkle_path[0][0]);
    TEST_ASSERT_EQUAL(4, job.coinbase_prefix_len);
    TEST_ASSERT_EQUAL_HEX8(1, job.coinbase_prefix[0]);
    TEST_ASSERT_EQUAL_HEX8(4, job.coinbase_prefix[3]);
    TEST_ASSERT_EQUAL(3, job.coinbase_suffix_len);
    TEST_ASSERT_EQUAL_HEX8(5, job.coinbase_suffix[0]);
    TEST_ASSERT_EQUAL_HEX8(7, job.coinbase_suffix[2]);
}

TEST_CASE("SV2 setup requires version rolling", "[sv2]")
{
    uint32_t standard_flags = sv2_setup_flags_for_channel(SV2_CHANNEL_STANDARD);
    uint32_t extended_flags = sv2_setup_flags_for_channel(SV2_CHANNEL_EXTENDED);

    TEST_ASSERT_BITS_HIGH(SV2_SETUP_FLAGS_REQUIRES_STANDARD_JOBS, standard_flags);
    TEST_ASSERT_BITS_HIGH(SV2_SETUP_FLAGS_REQUIRES_VERSION_ROLLING, standard_flags);
    TEST_ASSERT_BITS_LOW(SV2_SETUP_FLAGS_REQUIRES_STANDARD_JOBS, extended_flags);
    TEST_ASSERT_BITS_HIGH(SV2_SETUP_FLAGS_REQUIRES_VERSION_ROLLING, extended_flags);
    TEST_ASSERT_EQUAL_HEX32(0x01, SV2_SETUP_SUCCESS_FLAGS_REQUIRES_FIXED_VERSION);
    TEST_ASSERT_EQUAL_HEX32(0x02, SV2_SETUP_SUCCESS_FLAGS_REQUIRES_EXT_CHANNELS);
    TEST_ASSERT_TRUE(sv2_setup_success_allows_version_rolling(0));
    TEST_ASSERT_TRUE(sv2_setup_success_allows_version_rolling(
        SV2_SETUP_SUCCESS_FLAGS_REQUIRES_EXT_CHANNELS));
    TEST_ASSERT_FALSE(sv2_setup_success_allows_version_rolling(
        SV2_SETUP_SUCCESS_FLAGS_REQUIRES_FIXED_VERSION));
    TEST_ASSERT_FALSE(sv2_setup_success_allows_version_rolling(
        SV2_SETUP_SUCCESS_FLAGS_REQUIRES_FIXED_VERSION | SV2_SETUP_SUCCESS_FLAGS_REQUIRES_EXT_CHANNELS));
}

TEST_CASE("SV2 extended job reports fixed-version jobs", "[sv2]")
{
    uint8_t payload[256] = {0};
    int pos = 0;
    payload[pos++] = 5; pos += 3; // channel_id
    payload[pos++] = 12; pos += 3; // job_id
    payload[pos++] = 0; // min_ntime option flag = false
    payload[pos++] = 0x00; payload[pos++] = 0x00; payload[pos++] = 0x00; payload[pos++] = 0x20; // version
    payload[pos++] = 0; // version_rolling_allowed = false
    payload[pos++] = 0; // merkle_count = 0
    payload[pos++] = 0; payload[pos++] = 0; // prefix len = 0
    payload[pos++] = 0; payload[pos++] = 0; // suffix len = 0

    uint8_t p_buf[128];
    uint8_t s_buf[128];
    miner_job_t job;
    memset(&job, 0, sizeof(job));
    job.coinbase_prefix = p_buf;
    job.coinbase_suffix = s_buf;

    bool version_rolling_allowed = true;
    int res = sv2_parse_new_extended_mining_job(payload, pos, NULL, &job, NULL, &version_rolling_allowed);

    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_FALSE(version_rolling_allowed);
    TEST_ASSERT_EQUAL_HEX32(0, job.version_mask);
}

TEST_CASE("SV2 accepts direct and group channel broadcasts", "[sv2]")
{
    TEST_ASSERT_TRUE(sv2_channel_or_group_matches(7, 7, 11));
    TEST_ASSERT_TRUE(sv2_channel_or_group_matches(11, 7, 11));
    TEST_ASSERT_FALSE(sv2_channel_or_group_matches(12, 7, 11));
    TEST_ASSERT_TRUE(sv2_channel_or_group_matches(7, 7, 0));
    TEST_ASSERT_FALSE(sv2_channel_or_group_matches(0, 7, 0));
}

TEST_CASE("SV2 certificate holds only inside its window", "[sv2]")
{
    // Arrange
    const uint32_t valid_from = 1800000000u;
    const uint32_t not_valid_after = 1800003600u;

    // Act / Assert
    TEST_ASSERT_TRUE(sv2_noise_certificate_time_valid(valid_from, not_valid_after, valid_from + 1));
    TEST_ASSERT_TRUE(sv2_noise_certificate_time_valid(valid_from, not_valid_after, valid_from - 10));
    TEST_ASSERT_TRUE(sv2_noise_certificate_time_valid(valid_from, not_valid_after, not_valid_after + 10));
    TEST_ASSERT_FALSE(sv2_noise_certificate_time_valid(valid_from, not_valid_after, valid_from - 11));
    TEST_ASSERT_FALSE(sv2_noise_certificate_time_valid(valid_from, not_valid_after, not_valid_after + 11));
}

TEST_CASE("SV2 certificate can't hold before the clock is set", "[sv2]")
{
    // Act / Assert
    TEST_ASSERT_FALSE(sv2_noise_certificate_time_valid(0, UINT32_MAX, 12345));
}

// A pool that stays quiet for a while, then delivers a frame.
static struct {
    int quiet_polls;
    int polls;
    int polls_at_read_begin;
    bool stalled;
    sv2_noise_ctx_t *send_during_read;
    esp_transport_handle_t transport;
    int stop_after_checks;
    int checks;
    int begins;
    int ends;
    sv2_noise_op_t begun[8];
    sv2_noise_op_t ended[8];
    bool last_end_failed;
} s_pool;

static int pool_poll_read(esp_transport_handle_t transport, int timeout_ms)
{
    return ++s_pool.polls > s_pool.quiet_polls ? 1 : 0;
}

static int pool_read(esp_transport_handle_t transport, char *buffer, int len, int timeout_ms)
{
    if (s_pool.stalled) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    }
    if (s_pool.send_during_read != NULL) {
        const uint8_t frame[6] = {0};
        TEST_ASSERT_EQUAL(-1, sv2_noise_send(s_pool.send_during_read, s_pool.transport, frame, sizeof(frame)));
    }
    memset(buffer, 0, len);
    return len;
}

static void pool_begin(sv2_noise_op_t op)
{
    if (op == SV2_NOISE_OP_FRAME_READ && s_pool.polls_at_read_begin < 0) {
        s_pool.polls_at_read_begin = s_pool.polls;
    }
    s_pool.begun[s_pool.begins++] = op;
}

static void pool_end(sv2_noise_op_t op, bool failed)
{
    s_pool.last_end_failed = failed;
    s_pool.ended[s_pool.ends++] = op;
}

static bool pool_keep_going(void)
{
    return s_pool.stop_after_checks == 0 || ++s_pool.checks < s_pool.stop_after_checks;
}

static bool pool_frame(const uint8_t header[6], const uint8_t *payload, int payload_len)
{
    return true;
}

static const sv2_noise_observer_t POOL_OBSERVER = {
    .begin = pool_begin,
    .end = pool_end,
    .keep_going = pool_keep_going,
    .frame = pool_frame,
};

static sv2_noise_ctx_t *pool_session(esp_transport_handle_t *transport)
{
    memset(&s_pool, 0, sizeof(s_pool));
    s_pool.polls_at_read_begin = -1;
    *transport = esp_transport_init();
    TEST_ASSERT_NOT_NULL(*transport);
    esp_transport_set_func(*transport, NULL, pool_read, NULL, NULL, pool_poll_read, NULL, NULL);
    sv2_noise_ctx_t *ctx = sv2_noise_create();
    TEST_ASSERT_NOT_NULL(ctx);
    sv2_noise_test_skip_handshake(ctx);
    sv2_noise_set_observer(ctx, &POOL_OBSERVER);
    return ctx;
}

static int pool_receive(sv2_noise_ctx_t *ctx, esp_transport_handle_t transport)
{
    uint8_t header[6];
    uint8_t payload[16];
    int payload_len = 0;
    return sv2_noise_recv(ctx, transport, header, payload, sizeof(payload), &payload_len);
}

TEST_CASE("SV2 observed frame read begins at the first byte", "[sv2]")
{
    // Arrange
    esp_transport_handle_t transport;
    sv2_noise_ctx_t *ctx = pool_session(&transport);
    s_pool.quiet_polls = 3;

    // Act
    pool_receive(ctx, transport);

    // Assert
    TEST_ASSERT_EQUAL(4, s_pool.polls_at_read_begin);
    TEST_ASSERT_EQUAL(SV2_NOISE_OP_FRAME_READ, s_pool.begun[0]);

    sv2_noise_destroy(ctx);
    esp_transport_destroy(transport);
}

TEST_CASE("SV2 observed idle wait that stops opens no read", "[sv2]")
{
    // Arrange
    esp_transport_handle_t transport;
    sv2_noise_ctx_t *ctx = pool_session(&transport);
    s_pool.quiet_polls = 1000;
    s_pool.stop_after_checks = 3;

    // Act
    const int result = pool_receive(ctx, transport);

    // Assert
    TEST_ASSERT_EQUAL(-1, result);
    TEST_ASSERT_EQUAL(0, s_pool.begins);
    TEST_ASSERT_EQUAL(SV2_NOISE_FAILURE_TIMEOUT, sv2_noise_last_failure(ctx));

    sv2_noise_destroy(ctx);
    esp_transport_destroy(transport);
}

TEST_CASE("SV2 observed frame read gives up at its deadline", "[sv2]")
{
    // Arrange
    esp_transport_handle_t transport;
    sv2_noise_ctx_t *ctx = pool_session(&transport);
    s_pool.stalled = true;
    const int64_t started_us = esp_timer_get_time();

    // Act
    const int result = pool_receive(ctx, transport);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;

    // Assert
    TEST_ASSERT_EQUAL(-1, result);
    TEST_ASSERT_TRUE(s_pool.last_end_failed);
    TEST_ASSERT_EQUAL(SV2_NOISE_OP_FRAME_READ, s_pool.ended[0]);
    TEST_ASSERT_EQUAL(SV2_NOISE_FAILURE_TIMEOUT, sv2_noise_last_failure(ctx));
    TEST_ASSERT_TRUE(elapsed_us >= 10000000 && elapsed_us < 11000000);

    sv2_noise_destroy(ctx);
    esp_transport_destroy(transport);
}

TEST_CASE("SV2 observed send leaves a read in progress open", "[sv2]")
{
    // Arrange
    esp_transport_handle_t transport;
    sv2_noise_ctx_t *ctx = pool_session(&transport);
    s_pool.send_during_read = ctx;
    s_pool.transport = transport;

    // Act
    pool_receive(ctx, transport);

    // Assert
    TEST_ASSERT_EQUAL(SV2_NOISE_OP_FRAME_READ, s_pool.begun[0]);
    TEST_ASSERT_EQUAL(SV2_NOISE_OP_FRAME_ENCRYPT, s_pool.begun[1]);
    TEST_ASSERT_EQUAL(SV2_NOISE_OP_FRAME_ENCRYPT, s_pool.ended[0]);
    TEST_ASSERT_EQUAL(SV2_NOISE_OP_FRAME_READ, s_pool.ended[1]);

    sv2_noise_destroy(ctx);
    esp_transport_destroy(transport);
}
