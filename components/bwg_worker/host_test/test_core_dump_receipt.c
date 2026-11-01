#include <string.h>

#include "bwg_core_dump_receipt.h"
#include "bwg_test.h"
#include "test_suites.h"

#define SOURCE 0x0123456789abcdefull
#define BOOT 7ull
#define CAPACITY 974848u

static void the_source_hash_is_fnv1a_64(void)
{
    // Act
    const uint64_t empty = bwg_source_hash("");
    const uint64_t letter = bwg_source_hash("a");

    // Assert
    ASSERT_TRUE(empty == 0xcbf29ce484222325ull);
    ASSERT_TRUE(letter == 0xaf63dc4c8601ec8cull);
}

static void a_ready_receipt_is_valid_with_no_results(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);
    char line[512];

    // Act
    const bool formatted = bwg_core_dump_format(line, sizeof(line), words, SOURCE, BOOT, false);

    // Assert
    ASSERT_TRUE(formatted);
    ASSERT_STREQ(line,
                 "core_dump_store_receipt schema=v1 origin=current_boot status=valid source_hash=0123456789abcdef "
                 "boot_ordinal=7 stage=ready capacity_bytes=974848 requested_bytes=unavailable "
                 "prepared_bytes=unavailable init_result=unavailable prepare_result=unavailable "
                 "start_result=unavailable end_result=unavailable store_result=unavailable self_test_marked=false "
                 "redacted=true");
}

static void a_full_store_names_every_value(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);
    bwg_core_dump_checkpoint(words, 0, BWG_CORE_DUMP_FIELD_SELF_TEST, 1, 0);
    bwg_core_dump_checkpoint(words, 1, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 2, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 3, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 4, BWG_CORE_DUMP_FIELD_REQUESTED, 88704, 0);
    bwg_core_dump_checkpoint(words, 5, BWG_CORE_DUMP_FIELD_PREPARED, 89088, 0);
    bwg_core_dump_checkpoint(words, 6, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 7, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 8, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 9, BWG_CORE_DUMP_FIELD_NONE, 0, -1);
    bwg_core_dump_checkpoint(words, 10, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    char line[512];

    // Act
    const bool formatted = bwg_core_dump_format(line, sizeof(line), words, SOURCE, BOOT, true);

    // Assert
    ASSERT_TRUE(formatted);
    ASSERT_STREQ(line,
                 "core_dump_store_receipt schema=v1 origin=previous_boot status=valid source_hash=0123456789abcdef "
                 "boot_ordinal=7 stage=store_returned capacity_bytes=974848 requested_bytes=88704 "
                 "prepared_bytes=89088 init_result=0 prepare_result=0 start_result=0 end_result=-1 store_result=0 "
                 "self_test_marked=true redacted=true");
}

static void an_interrupted_store_keeps_its_last_stage(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);
    bwg_core_dump_checkpoint(words, 1, BWG_CORE_DUMP_FIELD_NONE, 0, 0);
    bwg_core_dump_checkpoint(words, 4, BWG_CORE_DUMP_FIELD_REQUESTED, 88704, 0);
    char line[512];

    // Act
    bwg_core_dump_format(line, sizeof(line), words, SOURCE, BOOT, true);

    // Assert
    ASSERT_TRUE(strstr(line, "stage=prepare_entered") != NULL);
    ASSERT_TRUE(strstr(line, "requested_bytes=88704 prepared_bytes=unavailable") != NULL);
}

static void all_zero_is_unavailable(void)
{
    // Arrange
    const uint32_t words[BWG_CORE_DUMP_WORDS] = {0};

    // Act
    const bwg_core_dump_status_t status = bwg_core_dump_classify(words, SOURCE, BOOT);

    // Assert
    ASSERT_EQ(status, BWG_CORE_DUMP_UNAVAILABLE);
}

static void any_changed_word_is_corrupt(void)
{
    for (size_t index = 0; index < BWG_CORE_DUMP_WORDS; index++) {
        // Arrange
        uint32_t words[BWG_CORE_DUMP_WORDS];
        bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);
        words[index] ^= 1u << 4;

        // Act
        const bwg_core_dump_status_t status = bwg_core_dump_classify(words, SOURCE, BOOT);

        // Assert
        ASSERT_EQ(status, BWG_CORE_DUMP_CORRUPT);
    }
}

static void another_firmware_is_named_as_such(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);

    // Act
    const bwg_core_dump_status_t status = bwg_core_dump_classify(words, SOURCE + 1, BOOT);

    // Assert
    ASSERT_EQ(status, BWG_CORE_DUMP_WRONG_FIRMWARE);
}

static void another_boot_is_named_as_such(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);

    // Act
    const bwg_core_dump_status_t wrong = bwg_core_dump_classify(words, SOURCE, BOOT + 1);
    const bwg_core_dump_status_t none = bwg_core_dump_classify(words, SOURCE, 0);

    // Assert
    ASSERT_EQ(wrong, BWG_CORE_DUMP_WRONG_BOOT);
    ASSERT_EQ(none, BWG_CORE_DUMP_WRONG_BOOT);
}

static void an_invalid_receipt_shows_only_its_status(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);
    char line[512];

    // Act
    bwg_core_dump_format(line, sizeof(line), words, SOURCE, BOOT + 1, true);

    // Assert
    ASSERT_STREQ(line, "core_dump_store_receipt schema=v1 origin=previous_boot status=wrong_boot redacted=true");
}

static void a_line_that_does_not_fit_is_refused(void)
{
    // Arrange
    uint32_t words[BWG_CORE_DUMP_WORDS];
    bwg_core_dump_ready(words, SOURCE, BOOT, CAPACITY);
    char line[32];

    // Act
    const bool formatted = bwg_core_dump_format(line, sizeof(line), words, SOURCE, BOOT, false);

    // Assert
    ASSERT_FALSE(formatted);
}

void bwg_tests_core_dump_receipt(void)
{
    BWG_RUN(the_source_hash_is_fnv1a_64);
    BWG_RUN(a_ready_receipt_is_valid_with_no_results);
    BWG_RUN(a_full_store_names_every_value);
    BWG_RUN(an_interrupted_store_keeps_its_last_stage);
    BWG_RUN(all_zero_is_unavailable);
    BWG_RUN(any_changed_word_is_corrupt);
    BWG_RUN(another_firmware_is_named_as_such);
    BWG_RUN(another_boot_is_named_as_such);
    BWG_RUN(an_invalid_receipt_shows_only_its_status);
    BWG_RUN(a_line_that_does_not_fit_is_refused);
}
