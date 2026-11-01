#include <stdio.h>
#include <string.h>

#include "bwg_ledger.h"
#include "bwg_test.h"
#include "test_suites.h"

#define ID "AAAAAAAAAAAAAAAAAAAAAA"
#define OTHER_ID "BBBBBBBBBBBBBBBBBBBBBA"

static bwg_allowance_t attempt(uint32_t ordinal, bwg_attempt_purpose_t purpose)
{
    return (bwg_allowance_t) {
        .kind = BWG_ALLOWANCE_ATTEMPT,
        .schema = {.ptr = "worker-qualification-attempt-v1", .len = 31},
        .id = {.ptr = ID, .len = 22},
        .ordinal = ordinal,
        .purpose = purpose,
        .maximum_active_ms = bwg_attempt_limit_ms(purpose),
    };
}

static bwg_allowance_t soak(uint32_t ordinal)
{
    return (bwg_allowance_t) {
        .kind = BWG_ALLOWANCE_SOAK,
        .schema = {.ptr = "worker-soak-allowance-v2", .len = 24},
        .id = {.ptr = ID, .len = 22},
        .ordinal = ordinal,
        .maximum_active_ms = BWG_SOAK_MS,
    };
}

static bwg_allowance_t window(const char *id, uint8_t number)
{
    return (bwg_allowance_t) {
        .kind = BWG_ALLOWANCE_CAMPAIGN,
        .id = {.ptr = id, .len = 22},
        .window = number,
        .maximum_active_ms = bwg_campaign_window_ms(number),
    };
}

static void assert_ledger_bytes(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger, const char *expected)
{
    uint8_t out[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    ASSERT_TRUE(bwg_ledger_serialize(kind, ledger, out, sizeof(out), &len));
    ASSERT_EQ(len, strlen(expected));
    ASSERT_MEMEQ(out, expected, len);
}

static bool parse_ledger_text(bwg_allowance_kind_t kind, const char *text, bwg_ledger_t *ledger)
{
    return bwg_ledger_parse(kind, &bwg_test_allocator, (const uint8_t *) text, strlen(text), ledger);
}

static void the_default_attempt_ledger_is_rusts_bytes(void)
{
    // Arrange
    bwg_ledger_t ledger;

    // Act
    bwg_ledger_default(&ledger);

    // Assert
    assert_ledger_bytes(BWG_ALLOWANCE_ATTEMPT, &ledger, "{\"highest_ordinal\":0,\"total_charged_ms\":0,\"maybe_last\":null}");
}

static void a_reserved_attempt_is_rusts_bytes(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t allowance = attempt(1, BWG_ATTEMPT_DIAGNOSTIC);

    // Act
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &allowance));

    // Assert
    assert_ledger_bytes(BWG_ALLOWANCE_ATTEMPT, &ledger,
                        "{\"highest_ordinal\":1,\"total_charged_ms\":30000,\"maybe_last\":{\"allowance\":{\"id\":\"" ID
                        "\",\"maximumActiveMilliseconds\":30000,\"ordinal\":1,\"purpose\":\"diagnostic\","
                        "\"schema\":\"worker-qualification-attempt-v1\"},\"complete\":false}}");
}

static void a_reserved_soak_is_rusts_bytes(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t allowance = soak(1);

    // Act
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_SOAK, &ledger, &allowance));

    // Assert
    assert_ledger_bytes(BWG_ALLOWANCE_SOAK, &ledger,
                        "{\"highest_ordinal\":1,\"total_charged_ms\":600000,\"maybe_last\":{\"allowance\":{\"id\":\"" ID
                        "\",\"maximumActiveMilliseconds\":600000,\"ordinal\":1,\"schema\":\"worker-soak-allowance-v2\"},"
                        "\"complete\":false}}");
}

static void a_campaign_is_rusts_bytes_before_and_after_finish(void)
{
    // Arrange
    bwg_campaign_t campaign;
    const bwg_allowance_t allowance = window(ID, 0);
    uint8_t out[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    memset(&campaign, 0, sizeof(campaign));

    // Act
    ASSERT_TRUE(bwg_campaign_reserve(&campaign, true, &allowance));
    ASSERT_TRUE(bwg_campaign_serialize(&campaign, out, sizeof(out), &len));
    out[len] = '\0';
    char reserved[BWG_LEDGER_MAX_BYTES + 1];
    strcpy(reserved, (const char *) out);
    bwg_campaign_finish(&campaign);
    ASSERT_TRUE(bwg_campaign_serialize(&campaign, out, sizeof(out), &len));
    out[len] = '\0';

    // Assert
    ASSERT_STREQ(reserved, "{\"campaign_id\":\"" ID "\",\"reserved\":1,\"completed\":0,\"charged_ms\":180000,\"maybe_running_window\":0}");
    ASSERT_STREQ((const char *) out, "{\"campaign_id\":\"" ID "\",\"reserved\":1,\"completed\":1,\"charged_ms\":180000,\"maybe_running_window\":null}");
}

static void a_stored_ledger_round_trips(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t allowance = attempt(1, BWG_ATTEMPT_NORMAL);
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &allowance));
    uint8_t out[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    ASSERT_TRUE(bwg_ledger_serialize(BWG_ALLOWANCE_ATTEMPT, &ledger, out, sizeof(out), &len));
    bwg_ledger_t parsed;

    // Act
    const bool ok = bwg_ledger_parse(BWG_ALLOWANCE_ATTEMPT, &bwg_test_allocator, out, len, &parsed);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_TRUE(bwg_ledger_equal(&ledger, &parsed));
}

static void a_reservation_charges_at_once_and_finish_never_refunds(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t allowance = attempt(1, BWG_ATTEMPT_DIAGNOSTIC);
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &allowance));
    ASSERT_TRUE(bwg_ledger_is_pending(&ledger));

    // Act
    bwg_ledger_finish(&ledger);
    bwg_ledger_finish(&ledger);

    // Assert
    ASSERT_FALSE(bwg_ledger_is_pending(&ledger));
    ASSERT_EQ(ledger.total_charged_ms, 30000);
}

static void ordinals_must_be_contiguous_and_never_replay(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t first = attempt(1, BWG_ATTEMPT_DIAGNOSTIC);
    const bwg_allowance_t gap = attempt(3, BWG_ATTEMPT_DIAGNOSTIC);

    // Act
    ASSERT_FALSE(bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &gap));
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &first));
    bwg_ledger_finish(&ledger);
    const bool replayed = bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &first);

    // Assert
    ASSERT_FALSE(replayed);
}

static void a_pending_reservation_blocks_the_next(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t first = soak(1);
    const bwg_allowance_t second = soak(2);
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_SOAK, &ledger, &first));

    // Act
    const bool reserved = bwg_ledger_reserve(BWG_ALLOWANCE_SOAK, &ledger, &second);

    // Assert
    ASSERT_FALSE(reserved);
}

static void impossible_attempt_totals_fail_closed(void)
{
    // Arrange
    const struct {
        uint32_t ordinal;
        const char *purpose;
        uint64_t total;
    } cases[] = {{1, "diagnostic", 180000}, {2, "diagnostic", 90000}, {2, "diagnostic", 360000}};
    for (size_t i = 0; i < 3; i++) {
        char text[BWG_LEDGER_MAX_BYTES];
        snprintf(text, sizeof(text),
                 "{\"highest_ordinal\":%u,\"total_charged_ms\":%llu,\"maybe_last\":{\"allowance\":{\"id\":\"" ID
                 "\",\"maximumActiveMilliseconds\":30000,\"ordinal\":%u,\"purpose\":\"%s\","
                 "\"schema\":\"worker-qualification-attempt-v1\"},\"complete\":true}}",
                 cases[i].ordinal, (unsigned long long) cases[i].total, cases[i].ordinal, cases[i].purpose);
        bwg_ledger_t ledger;

        // Act
        const bool parsed = parse_ledger_text(BWG_ALLOWANCE_ATTEMPT, text, &ledger);

        // Assert
        ASSERT_FALSE(parsed);
    }
}

static void a_soak_total_that_disagrees_with_its_ordinal_is_refused(void)
{
    // Arrange
    const char *text = "{\"highest_ordinal\":1,\"total_charged_ms\":1200000,\"maybe_last\":{\"allowance\":{\"id\":\"" ID
                       "\",\"maximumActiveMilliseconds\":600000,\"ordinal\":1,\"schema\":\"worker-soak-allowance-v2\"},"
                       "\"complete\":true}}";
    bwg_ledger_t ledger;

    // Act
    const bool parsed = parse_ledger_text(BWG_ALLOWANCE_SOAK, text, &ledger);

    // Assert
    ASSERT_FALSE(parsed);
}

static void unknown_and_repeated_keys_are_refused(void)
{
    // Arrange
    const char *unknown = "{\"highest_ordinal\":0,\"total_charged_ms\":0,\"maybe_last\":null,\"extra\":1}";
    const char *repeated = "{\"highest_ordinal\":0,\"highest_ordinal\":0,\"total_charged_ms\":0,\"maybe_last\":null}";
    bwg_ledger_t ledger;

    // Act
    const bool unknown_parsed = parse_ledger_text(BWG_ALLOWANCE_ATTEMPT, unknown, &ledger);
    const bool repeated_parsed = parse_ledger_text(BWG_ALLOWANCE_ATTEMPT, repeated, &ledger);

    // Assert
    ASSERT_FALSE(unknown_parsed);
    ASSERT_FALSE(repeated_parsed);
}

static void any_key_order_and_a_missing_option_are_read(void)
{
    // Arrange
    const char *text = "{ \"total_charged_ms\": 0, \"highest_ordinal\": 0 }";
    bwg_ledger_t ledger;

    // Act
    const bool parsed = parse_ledger_text(BWG_ALLOWANCE_ATTEMPT, text, &ledger);

    // Assert
    ASSERT_TRUE(parsed);
    ASSERT_FALSE(ledger.has_last);
}

static void a_review_reports_the_next_and_completed_ordinals(void)
{
    // Arrange
    bwg_ledger_t ledger;
    bwg_ledger_default(&ledger);
    const bwg_allowance_t allowance = attempt(1, BWG_ATTEMPT_DIAGNOSTIC);
    ASSERT_TRUE(bwg_ledger_reserve(BWG_ALLOWANCE_ATTEMPT, &ledger, &allowance));
    uint8_t out[256];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, sizeof(out));

    // Act
    bwg_ledger_write_review(BWG_ALLOWANCE_ATTEMPT, &ledger, &writer);

    // Assert
    const char *expected = "{\"schema\":\"worker-qualification-ledger-v1\",\"next_ordinal\":2,\"total_charged_ms\":30000,"
                           "\"pending\":true,\"last_completed_ordinal\":0}";
    ASSERT_EQ(writer.len, strlen(expected));
    ASSERT_MEMEQ(out, expected, writer.len);
}

static void three_windows_exhaust_the_campaign_only_after_finishing(void)
{
    // Arrange
    bwg_campaign_t campaign;
    memset(&campaign, 0, sizeof(campaign));

    for (uint8_t number = 0; number < BWG_CAMPAIGN_WINDOWS; number++) {
        ASSERT_FALSE(bwg_campaign_is_complete(&campaign));
        const bwg_allowance_t allowance = window(ID, number);
        // Act
        ASSERT_TRUE(bwg_campaign_reserve(&campaign, number == 0, &allowance));
        bwg_campaign_finish(&campaign);
    }

    // Assert
    ASSERT_TRUE(bwg_campaign_is_complete(&campaign));
    ASSERT_EQ(campaign.charged_ms, BWG_CAMPAIGN_TOTAL_MS);
}

static void a_campaign_refuses_another_id_a_repeat_and_a_concurrent_window(void)
{
    // Arrange
    bwg_campaign_t campaign;
    memset(&campaign, 0, sizeof(campaign));
    const bwg_allowance_t first = window(ID, 0);
    const bwg_allowance_t next = window(ID, 1);
    const bwg_allowance_t stranger = window(OTHER_ID, 1);
    ASSERT_TRUE(bwg_campaign_reserve(&campaign, true, &first));

    // Act
    const bool concurrent = bwg_campaign_reserve(&campaign, false, &next);
    bwg_campaign_finish(&campaign);
    const bool repeated = bwg_campaign_reserve(&campaign, false, &first);
    const bool other = bwg_campaign_reserve(&campaign, false, &stranger);

    // Assert
    ASSERT_FALSE(concurrent);
    ASSERT_FALSE(repeated);
    ASSERT_FALSE(other);
}

static void the_conformance_review_shape_is_written_without_echoing_an_id(void)
{
    // Arrange
    bwg_campaign_t campaign;
    memset(&campaign, 0, sizeof(campaign));
    const bwg_allowance_t first = window(ID, 0);
    ASSERT_TRUE(bwg_campaign_reserve(&campaign, true, &first));
    bwg_campaign_finish(&campaign);
    uint8_t out[256];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, sizeof(out));
    const bwg_str_t id = {.ptr = ID, .len = 22};

    // Act
    bwg_campaign_write_review(&campaign, id, &writer);

    // Assert
    const char *expected = "{\"schema\":\"worker-budget-review-v1\",\"campaign_match\":true,\"reserved_mask\":1,"
                           "\"completed_mask\":1,\"charged_ms\":180000,\"pending\":false}";
    ASSERT_EQ(writer.len, strlen(expected));
    ASSERT_MEMEQ(out, expected, writer.len);
}

static void no_campaign_reviews_as_empty(void)
{
    // Arrange
    uint8_t out[256];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, sizeof(out));
    const bwg_str_t id = {.ptr = ID, .len = 22};

    // Act
    bwg_campaign_write_review(NULL, id, &writer);

    // Assert
    const char *expected = "{\"schema\":\"worker-budget-review-v1\",\"campaign_match\":false,\"reserved_mask\":0,"
                           "\"completed_mask\":0,\"charged_ms\":0,\"pending\":false}";
    ASSERT_EQ(writer.len, strlen(expected));
    ASSERT_MEMEQ(out, expected, writer.len);
}

void bwg_tests_ledger(void)
{
    BWG_RUN(the_default_attempt_ledger_is_rusts_bytes);
    BWG_RUN(a_reserved_attempt_is_rusts_bytes);
    BWG_RUN(a_reserved_soak_is_rusts_bytes);
    BWG_RUN(a_campaign_is_rusts_bytes_before_and_after_finish);
    BWG_RUN(a_stored_ledger_round_trips);
    BWG_RUN(a_reservation_charges_at_once_and_finish_never_refunds);
    BWG_RUN(ordinals_must_be_contiguous_and_never_replay);
    BWG_RUN(a_pending_reservation_blocks_the_next);
    BWG_RUN(impossible_attempt_totals_fail_closed);
    BWG_RUN(a_soak_total_that_disagrees_with_its_ordinal_is_refused);
    BWG_RUN(unknown_and_repeated_keys_are_refused);
    BWG_RUN(any_key_order_and_a_missing_option_are_read);
    BWG_RUN(a_review_reports_the_next_and_completed_ordinals);
    BWG_RUN(three_windows_exhaust_the_campaign_only_after_finishing);
    BWG_RUN(a_campaign_refuses_another_id_a_repeat_and_a_concurrent_window);
    BWG_RUN(the_conformance_review_shape_is_written_without_echoing_an_id);
    BWG_RUN(no_campaign_reviews_as_empty);
}
