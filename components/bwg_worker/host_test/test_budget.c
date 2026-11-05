#include <string.h>

#include "bwg_budget.h"
#include "bwg_test.h"
#include "test_suites.h"

#define ID "AAAAAAAAAAAAAAAAAAAAAA"

typedef struct {
    char key[16];
    uint8_t data[BWG_LEDGER_MAX_BYTES];
    size_t len;
    bool present;
} fake_blob_t;

/** Flash that outlives a budget, as it outlives a boot. */
typedef struct {
    fake_blob_t blobs[3];
    int store_failures;
    bool store_lands_anyway;
    bool load_fails;
    int stores;
} fake_flash_t;

static fake_blob_t *blob_for(fake_flash_t *flash, const char *key)
{
    for (size_t i = 0; i < 3; i++) {
        if (flash->blobs[i].key[0] == '\0') {
            strcpy(flash->blobs[i].key, key);
        }
        if (strcmp(flash->blobs[i].key, key) == 0) {
            return &flash->blobs[i];
        }
    }
    return NULL;
}

static bwg_budget_load_t fake_load(void *ctx, const char *key, uint8_t *out, size_t capacity, size_t *len)
{
    fake_flash_t *flash = ctx;
    if (flash->load_fails) {
        return BWG_BUDGET_LOAD_FAILED;
    }
    const fake_blob_t *blob = blob_for(flash, key);
    if (!blob->present) {
        return BWG_BUDGET_LOAD_ABSENT;
    }
    if (blob->len > capacity) {
        return BWG_BUDGET_LOAD_FAILED;
    }
    memcpy(out, blob->data, blob->len);
    *len = blob->len;
    return BWG_BUDGET_LOAD_OK;
}

static bool fake_store(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    fake_flash_t *flash = ctx;
    const bool fails = flash->store_failures > 0;
    if (fails) {
        flash->store_failures--;
        if (!flash->store_lands_anyway) {
            return false;
        }
    }
    fake_blob_t *blob = blob_for(flash, key);
    memcpy(blob->data, data, len);
    blob->len = len;
    blob->present = true;
    flash->stores++;
    return !fails;
}

static void boot(bwg_budget_t *budget, fake_flash_t *flash)
{
    const bwg_budget_store_t store = {.ctx = flash, .load = fake_load, .store = fake_store};
    bwg_budget_init(budget, &bwg_test_allocator, &store, BWG_SOAK_NEEDS_COMPLETE_CAMPAIGN);
}

static void flash_put(fake_flash_t *flash, const char *key, const char *text)
{
    fake_blob_t *blob = blob_for(flash, key);
    blob->len = strlen(text);
    memcpy(blob->data, text, blob->len);
    blob->present = true;
}

static bool flash_holds(fake_flash_t *flash, const char *key, const char *needle)
{
    const fake_blob_t *blob = blob_for(flash, key);
    char text[BWG_LEDGER_MAX_BYTES + 1];
    if (!blob->present) {
        return false;
    }
    memcpy(text, blob->data, blob->len);
    text[blob->len] = '\0';
    return strstr(text, needle) != NULL;
}

static const char COMPLETE_CAMPAIGN[] =
    "{\"campaign_id\":\"" ID "\",\"reserved\":7,\"completed\":7,\"charged_ms\":240000,\"maybe_running_window\":null}";
static const char PARTIAL_CAMPAIGN[] =
    "{\"campaign_id\":\"" ID "\",\"reserved\":1,\"completed\":1,\"charged_ms\":180000,\"maybe_running_window\":null}";

static bwg_allowance_t attempt(uint32_t ordinal)
{
    return (bwg_allowance_t) {
        .kind = BWG_ALLOWANCE_ATTEMPT,
        .schema = {.ptr = "worker-qualification-attempt-v1", .len = 31},
        .id = {.ptr = ID, .len = 22},
        .ordinal = ordinal,
        .purpose = BWG_ATTEMPT_DIAGNOSTIC,
        .maximum_active_ms = BWG_ATTEMPT_SHORT_MS,
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

static bwg_allowance_t window(uint8_t number)
{
    return (bwg_allowance_t) {
        .kind = BWG_ALLOWANCE_CAMPAIGN,
        .id = {.ptr = ID, .len = 22},
        .window = number,
        .maximum_active_ms = bwg_campaign_window_ms(number),
    };
}

static void an_unlimited_lease_on_a_fresh_device_writes_nothing(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t none = {.kind = BWG_ALLOWANCE_NONE};
    uint32_t limit = 1;
    const char *refusal;

    // Act
    const bool admitted = bwg_budget_admit(&budget, 1, &none, &limit, &refusal);

    // Assert
    ASSERT_TRUE(admitted);
    ASSERT_EQ(limit, 0);
    ASSERT_EQ(flash.stores, 0);
}

static void a_partly_run_campaign_refuses_an_unlimited_lease(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, PARTIAL_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t none = {.kind = BWG_ALLOWANCE_NONE};
    uint32_t limit;
    const char *refusal;

    // Act
    const bool admitted = bwg_budget_admit(&budget, 1, &none, &limit, &refusal);

    // Assert
    ASSERT_FALSE(admitted);
    ASSERT_STREQ(refusal, "acceptance_budget=unfinished_campaign");
}

static void a_campaign_window_charges_before_work_and_caches_at_once(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t first = window(0);
    uint32_t limit;
    const char *refusal;

    // Act
    const bool admitted = bwg_budget_admit(&budget, 1, &first, &limit, &refusal);

    // Assert
    ASSERT_TRUE(admitted);
    ASSERT_EQ(limit, 180000);
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_CAMPAIGN, "\"maybe_running_window\":0"));
    ASSERT_EQ(bwg_budget_campaign_reserved_ms(&budget), 180000);
    ASSERT_FALSE(bwg_budget_campaign_complete(&budget));
}

static void finishing_all_three_windows_reports_complete(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t budget;
    boot(&budget, &flash);
    uint32_t limit;
    const char *refusal;

    for (uint8_t number = 0; number < 3; number++) {
        const bwg_allowance_t next = window(number);
        ASSERT_TRUE(bwg_budget_admit(&budget, number + 1u, &next, &limit, &refusal));
        // Act
        ASSERT_TRUE(bwg_budget_finish(&budget, number + 1u));
    }

    // Assert
    ASSERT_TRUE(bwg_budget_campaign_complete(&budget));
    ASSERT_EQ(bwg_budget_campaign_reserved_ms(&budget), 240000);
}

static void a_failed_reservation_write_refuses_yet_still_charges_at_cleanup(void)
{
    // Arrange
    fake_flash_t flash = {.store_failures = 1};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = attempt(1);
    uint32_t limit;
    const char *refusal;

    // Act
    const bool admitted = bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal);
    const bool finished = bwg_budget_finish(&budget, 1);

    // Assert
    ASSERT_FALSE(admitted);
    ASSERT_STREQ(refusal, "qualification_budget=storage");
    ASSERT_TRUE(finished);
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_ATTEMPTS, "\"total_charged_ms\":30000"));
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_ATTEMPTS, "\"complete\":true"));
}

static void an_ambiguous_write_that_landed_is_finished_without_refund(void)
{
    // Arrange
    fake_flash_t flash = {.store_failures = 1, .store_lands_anyway = true};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = attempt(1);
    uint32_t limit;
    const char *refusal;

    // Act
    const bool admitted = bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal);
    const bool finished = bwg_budget_finish(&budget, 1);

    // Assert
    ASSERT_FALSE(admitted);
    ASSERT_TRUE(finished);
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_ATTEMPTS, "\"complete\":true"));
}

static void a_failed_cleanup_write_keeps_pending_and_the_retry_finishes(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = attempt(1);
    uint32_t limit;
    const char *refusal;
    ASSERT_TRUE(bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal));
    flash.store_failures = 1;

    // Act
    const bool first = bwg_budget_finish(&budget, 1);
    const bool second = bwg_budget_finish(&budget, 1);

    // Assert
    ASSERT_FALSE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_ATTEMPTS, "\"complete\":true"));
}

static void a_ledger_changed_underneath_is_not_overwritten(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = attempt(1);
    uint32_t limit;
    const char *refusal;
    ASSERT_TRUE(bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal));
    flash_put(&flash, BWG_BUDGET_KEY_ATTEMPTS,
              "{\"highest_ordinal\":2,\"total_charged_ms\":60000,\"maybe_last\":{\"allowance\":{\"id\":\"" ID
              "\",\"maximumActiveMilliseconds\":30000,\"ordinal\":2,\"purpose\":\"diagnostic\","
              "\"schema\":\"worker-qualification-attempt-v1\"},\"complete\":true}}");

    // Act
    const bool finished = bwg_budget_finish(&budget, 1);

    // Assert
    ASSERT_FALSE(finished);
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_ATTEMPTS, "\"highest_ordinal\":2"));
}

static void an_attempt_needs_the_exhausted_campaign(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, PARTIAL_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = attempt(1);
    uint32_t limit;
    const char *refusal;

    // Act
    const bool admitted = bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal);

    // Assert
    ASSERT_FALSE(admitted);
    ASSERT_STREQ(refusal, "qualification_budget=original_incomplete");
    ASSERT_EQ(flash.stores, 0);
}

static void a_soak_follows_the_rule_of_its_board(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_budget_store_t store = {.ctx = &flash, .load = fake_load, .store = fake_store};
    bwg_budget_t no_campaign_needed;
    bwg_budget_init(&no_campaign_needed, &bwg_test_allocator, &store, BWG_SOAK_NEEDS_NO_UNFINISHED_CAMPAIGN);
    const bwg_allowance_t allowance = soak(1);
    uint32_t limit;
    const char *refusal;

    // Act
    const bool strict = bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal);
    const bool lenient = bwg_budget_admit(&no_campaign_needed, 1, &allowance, &limit, &refusal);

    // Assert
    ASSERT_FALSE(strict);
    ASSERT_TRUE(lenient);
    ASSERT_EQ(limit, BWG_SOAK_MS);
}

static void a_pending_soak_refuses_a_campaign_window_and_an_attempt(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t running = soak(1);
    const bwg_allowance_t next = attempt(1);
    uint32_t limit;
    const char *refusal;
    ASSERT_TRUE(bwg_budget_admit(&budget, 1, &running, &limit, &refusal));

    // Act
    const bool admitted = bwg_budget_admit(&budget, 2, &next, &limit, &refusal);

    // Assert
    ASSERT_FALSE(admitted);
    ASSERT_STREQ(refusal, "qualification_budget=soak_pending");
}

static void boot_recovery_completes_pending_work_without_refund(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t before;
    boot(&before, &flash);
    const bwg_allowance_t first = window(0);
    uint32_t limit;
    const char *refusal;
    ASSERT_TRUE(bwg_budget_admit(&before, 1, &first, &limit, &refusal));
    bwg_budget_t after;
    boot(&after, &flash);

    // Act
    const bool recovered = bwg_budget_recover_boot(&after);

    // Assert
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_CAMPAIGN, "\"completed\":1"));
    ASSERT_TRUE(flash_holds(&flash, BWG_BUDGET_KEY_CAMPAIGN, "\"charged_ms\":180000"));
    ASSERT_EQ(bwg_budget_campaign_reserved_ms(&after), 180000);
}

static void boot_recovery_on_a_fresh_device_creates_nothing(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t budget;
    boot(&budget, &flash);

    // Act
    const bool recovered = bwg_budget_recover_boot(&budget);

    // Assert
    ASSERT_TRUE(recovered);
    ASSERT_EQ(flash.stores, 0);
}

static void a_corrupt_soak_ledger_blocks_only_soaks(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_SOAK, "{corrupt");
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = soak(1);
    uint32_t limit;
    const char *refusal;

    // Act
    const bool recovered = bwg_budget_recover_boot(&budget);
    const bool admitted = bwg_budget_admit(&budget, 1, &allowance, &limit, &refusal);

    // Assert
    ASSERT_TRUE(recovered);
    ASSERT_FALSE(admitted);
    ASSERT_STREQ(refusal, "soak_budget=recovery_failed");
}

static void a_corrupt_attempt_ledger_fails_recovery_with_no_repair(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_ATTEMPTS, "{corrupt");
    bwg_budget_t budget;
    boot(&budget, &flash);

    // Act
    const bool recovered = bwg_budget_recover_boot(&budget);

    // Assert
    ASSERT_FALSE(recovered);
    ASSERT_EQ(flash.stores, 0);
}

static void a_review_never_creates_a_ledger(void)
{
    // Arrange
    fake_flash_t flash = {0};
    bwg_budget_t budget;
    boot(&budget, &flash);
    uint8_t out[256];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, sizeof(out));
    const bwg_str_t id = {.ptr = ID, .len = 22};

    // Act
    const bool reviewed = bwg_budget_write_campaign_review(&budget, id, &writer);

    // Assert
    ASSERT_TRUE(reviewed);
    ASSERT_EQ(flash.stores, 0);
}

static void a_review_of_a_corrupt_ledger_fails_without_repair(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_SOAK, "{corrupt");
    bwg_budget_t budget;
    boot(&budget, &flash);
    uint8_t out[256];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, sizeof(out));

    // Act
    const bool reviewed = bwg_budget_write_ledger_review(&budget, BWG_ALLOWANCE_SOAK, &writer);

    // Assert
    ASSERT_FALSE(reviewed);
    ASSERT_EQ(flash.stores, 0);
}

static void an_observation_reports_only_its_own_generation(void)
{
    // Arrange
    fake_flash_t flash = {0};
    flash_put(&flash, BWG_BUDGET_KEY_CAMPAIGN, COMPLETE_CAMPAIGN);
    bwg_budget_t budget;
    boot(&budget, &flash);
    const bwg_allowance_t allowance = attempt(1);
    uint32_t limit;
    const char *refusal;
    ASSERT_TRUE(bwg_budget_admit(&budget, 4, &allowance, &limit, &refusal));
    uint8_t own[256];
    bwg_jw_t own_writer;
    bwg_jw_init_buffer(&own_writer, own, sizeof(own));
    uint8_t other[256];
    bwg_jw_t other_writer;
    bwg_jw_init_buffer(&other_writer, other, sizeof(other));

    // Act
    bwg_jw_object_begin(&own_writer);
    bwg_budget_write_observation(&budget, 4, 12, &own_writer);
    bwg_jw_object_end(&own_writer);
    bwg_jw_object_begin(&other_writer);
    bwg_budget_write_observation(&budget, 5, 12, &other_writer);
    bwg_jw_object_end(&other_writer);

    // Assert
    const char *expected = "{\"attempt\":{\"schema\":\"worker-qualification-observation-v1\",\"ordinal\":1,"
                           "\"purpose\":\"diagnostic\",\"maximum_active_ms\":30000,\"reserved_ms\":30000,"
                           "\"complete\":false,\"active_ms\":12}}";
    ASSERT_EQ(own_writer.len, strlen(expected));
    ASSERT_MEMEQ(own, expected, own_writer.len);
    ASSERT_EQ(other_writer.len, 2);
}

static void active_time_stops_at_the_limit_and_remaining_reaches_zero(void)
{
    // Arrange, Act
    const bwg_budget_timing_t running = bwg_budget_timing(1000, 0, 30000, 31000, 11000);
    const bwg_budget_timing_t past = bwg_budget_timing(1000, 0, 30000, 31000, 40000);
    const bwg_budget_timing_t closed = bwg_budget_timing(1000, 31000, 30000, 31000, 90000);

    // Assert
    ASSERT_EQ(running.active_ms, 10000);
    ASSERT_TRUE(running.has_remaining);
    ASSERT_EQ(running.remaining_ms, 20000);
    ASSERT_EQ(past.active_ms, 30000);
    ASSERT_EQ(past.remaining_ms, 0);
    ASSERT_EQ(closed.active_ms, 30000);
    ASSERT_TRUE(closed.has_remaining);
    ASSERT_EQ(closed.remaining_ms, 0);
}

static void no_limit_or_no_work_has_nothing_remaining(void)
{
    // Arrange, Act
    const bwg_budget_timing_t unlimited = bwg_budget_timing(1000, 0, 0, 0, 5000);
    const bwg_budget_timing_t unstarted = bwg_budget_timing(0, 0, 30000, 0, 5000);

    // Assert
    ASSERT_EQ(unlimited.active_ms, 4000);
    ASSERT_FALSE(unlimited.has_remaining);
    ASSERT_EQ(unstarted.active_ms, 0);
    ASSERT_FALSE(unstarted.has_remaining);
}

static void timing_survives_the_clock_wrapping(void)
{
    // Arrange
    const uint32_t first = UINT32_MAX - 1000;
    const uint32_t deadline = first + 30000;
    const uint32_t now = first + 11000;

    // Act
    const bwg_budget_timing_t timing = bwg_budget_timing(first, 0, 30000, deadline, now);

    // Assert
    ASSERT_EQ(timing.active_ms, 11000);
    ASSERT_EQ(timing.remaining_ms, 19000);
}

static void a_stamp_keeps_a_nonzero_reading(void)
{
    // Act / Assert
    ASSERT_EQ(bwg_budget_stamp(12345), 12345u);
}

static void a_stamp_at_the_wrap_is_never_unset(void)
{
    // Act / Assert
    ASSERT_EQ(bwg_budget_stamp(0), 1u);
}

static void a_lease_closed_as_the_clock_wraps_still_reads_as_closed(void)
{
    // Arrange
    const uint32_t first = UINT32_MAX - 999;
    const uint32_t closed = bwg_budget_stamp(0);

    // Act
    const bwg_budget_timing_t timing = bwg_budget_timing(first, closed, 30000, first + 30000, first + 20000);

    // Assert
    ASSERT_EQ(timing.active_ms, 1001u);
    ASSERT_EQ(timing.remaining_ms, 0u);
}

void bwg_tests_budget(void)
{
    BWG_RUN(an_unlimited_lease_on_a_fresh_device_writes_nothing);
    BWG_RUN(a_partly_run_campaign_refuses_an_unlimited_lease);
    BWG_RUN(a_campaign_window_charges_before_work_and_caches_at_once);
    BWG_RUN(finishing_all_three_windows_reports_complete);
    BWG_RUN(a_failed_reservation_write_refuses_yet_still_charges_at_cleanup);
    BWG_RUN(an_ambiguous_write_that_landed_is_finished_without_refund);
    BWG_RUN(a_failed_cleanup_write_keeps_pending_and_the_retry_finishes);
    BWG_RUN(a_ledger_changed_underneath_is_not_overwritten);
    BWG_RUN(an_attempt_needs_the_exhausted_campaign);
    BWG_RUN(a_soak_follows_the_rule_of_its_board);
    BWG_RUN(a_pending_soak_refuses_a_campaign_window_and_an_attempt);
    BWG_RUN(boot_recovery_completes_pending_work_without_refund);
    BWG_RUN(boot_recovery_on_a_fresh_device_creates_nothing);
    BWG_RUN(a_corrupt_soak_ledger_blocks_only_soaks);
    BWG_RUN(a_corrupt_attempt_ledger_fails_recovery_with_no_repair);
    BWG_RUN(a_review_never_creates_a_ledger);
    BWG_RUN(a_review_of_a_corrupt_ledger_fails_without_repair);
    BWG_RUN(an_observation_reports_only_its_own_generation);
    BWG_RUN(active_time_stops_at_the_limit_and_remaining_reaches_zero);
    BWG_RUN(no_limit_or_no_work_has_nothing_remaining);
    BWG_RUN(timing_survives_the_clock_wrapping);
    BWG_RUN(a_stamp_keeps_a_nonzero_reading);
    BWG_RUN(a_stamp_at_the_wrap_is_never_unset);
    BWG_RUN(a_lease_closed_as_the_clock_wraps_still_reads_as_closed);
}
