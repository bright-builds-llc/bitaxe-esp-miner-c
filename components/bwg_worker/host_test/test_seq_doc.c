#include "bwg_seq_doc.h"
#include "bwg_test.h"
#include "test_suites.h"

static void seq_doc_parses_what_it_encodes(void)
{
    // Arrange
    bwg_seq_doc_t doc = {0};
    ASSERT_TRUE(bwg_seq_doc_put(&doc, "zeta", 2));
    ASSERT_TRUE(bwg_seq_doc_put(&doc, "alpha", 9));
    uint8_t text[BWG_SEQ_DOC_MAX_BYTES];

    // Act
    const size_t len = bwg_seq_doc_encode(&doc, text, sizeof(text));
    bwg_seq_doc_t parsed;
    const bool ok = bwg_seq_doc_parse(&bwg_test_allocator, text, len, &parsed);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_TRUE(bwg_seq_doc_equal(&doc, &parsed));
    ASSERT_TRUE(len == strlen("{\"alpha\":9,\"zeta\":2}"));
    ASSERT_TRUE(memcmp(text, "{\"alpha\":9,\"zeta\":2}", len) == 0);
}

static void seq_doc_rejects_invalid_documents(void)
{
    // Arrange
    static const char *const invalid[] = {
        "", "[]", "{\"a\":0}", "{\"a\":-1}", "{\"a\":1.5}", "{\"a\":\"1\"}", "{\"a b\":1}", "{\"\":1}", "{\"a\":1,\"a\":2}",
        "{\"123456789012345678901234567890123\":1}",
    };
    bwg_seq_doc_t doc;

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        // Act
        const bool ok = bwg_seq_doc_parse(&bwg_test_allocator, (const uint8_t *)invalid[i], strlen(invalid[i]), &doc);

        // Assert
        ASSERT_TRUE(!ok);
    }
}

static void seq_doc_refuses_a_ninth_key(void)
{
    // Arrange
    bwg_seq_doc_t doc = {0};
    char kid[2] = "a";
    for (size_t i = 0; i < BWG_SEQ_DOC_MAX_KEYS; i++) {
        kid[0] = (char)('a' + i);
        ASSERT_TRUE(bwg_seq_doc_put(&doc, kid, 1));
    }

    // Act
    const bool ninth = bwg_seq_doc_put(&doc, "z", 1);
    const bool replaced = bwg_seq_doc_put(&doc, "a", 5);

    // Assert
    uint64_t value = 0;
    ASSERT_TRUE(!ninth);
    ASSERT_TRUE(replaced);
    ASSERT_TRUE(bwg_seq_doc_get(&doc, "a", &value));
    ASSERT_TRUE(value == 5);
}

static bwg_seq_doc_t doc_with_a_at(uint64_t sequence)
{
    return (bwg_seq_doc_t){.count = 1, .entries = {{.kid = "a", .sequence = sequence}}};
}

static void a_high_water_moves_from_the_one_expected(void)
{
    // Arrange
    const bwg_seq_doc_t doc = doc_with_a_at(4);

    // Act
    const bwg_seq_verdict_t verdict = bwg_seq_doc_judge(&doc, "a", true, 4, 5);

    // Assert
    ASSERT_EQ(verdict, BWG_SEQ_WRITE);
}

static void a_first_high_water_is_written_when_none_was_expected(void)
{
    // Arrange
    const bwg_seq_doc_t doc = {0};

    // Act
    const bwg_seq_verdict_t verdict = bwg_seq_doc_judge(&doc, "a", false, 0, 1);

    // Assert
    ASSERT_EQ(verdict, BWG_SEQ_WRITE);
}

static void a_high_water_already_at_next_is_already_committed(void)
{
    // Arrange: a write landed, then its reply was lost, so the caller still expects the old value.
    const bwg_seq_doc_t doc = doc_with_a_at(5);

    // Act
    const bwg_seq_verdict_t verdict = bwg_seq_doc_judge(&doc, "a", true, 4, 5);

    // Assert
    ASSERT_EQ(verdict, BWG_SEQ_ALREADY_COMMITTED);
}

static void a_high_water_that_moved_elsewhere_is_stale(void)
{
    // Arrange
    const bwg_seq_doc_t doc = doc_with_a_at(7);

    // Act
    const bwg_seq_verdict_t verdict = bwg_seq_doc_judge(&doc, "a", true, 4, 5);

    // Assert
    ASSERT_EQ(verdict, BWG_SEQ_STALE);
}

static void a_high_water_that_appeared_unexpectedly_is_stale(void)
{
    // Arrange
    const bwg_seq_doc_t doc = doc_with_a_at(3);

    // Act
    const bwg_seq_verdict_t verdict = bwg_seq_doc_judge(&doc, "a", false, 0, 5);

    // Assert
    ASSERT_EQ(verdict, BWG_SEQ_STALE);
}

static void an_expected_high_water_that_vanished_is_stale(void)
{
    // Arrange
    const bwg_seq_doc_t doc = {0};

    // Act
    const bwg_seq_verdict_t verdict = bwg_seq_doc_judge(&doc, "a", true, 4, 5);

    // Assert
    ASSERT_EQ(verdict, BWG_SEQ_STALE);
}

void bwg_tests_seq_doc(void)
{
    BWG_RUN(seq_doc_parses_what_it_encodes);
    BWG_RUN(seq_doc_rejects_invalid_documents);
    BWG_RUN(seq_doc_refuses_a_ninth_key);
    BWG_RUN(a_high_water_moves_from_the_one_expected);
    BWG_RUN(a_first_high_water_is_written_when_none_was_expected);
    BWG_RUN(a_high_water_already_at_next_is_already_committed);
    BWG_RUN(a_high_water_that_moved_elsewhere_is_stale);
    BWG_RUN(a_high_water_that_appeared_unexpectedly_is_stale);
    BWG_RUN(an_expected_high_water_that_vanished_is_stale);
}
