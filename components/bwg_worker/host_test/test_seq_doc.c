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

void bwg_tests_seq_doc(void)
{
    BWG_RUN(seq_doc_parses_what_it_encodes);
    BWG_RUN(seq_doc_rejects_invalid_documents);
    BWG_RUN(seq_doc_refuses_a_ninth_key);
}
