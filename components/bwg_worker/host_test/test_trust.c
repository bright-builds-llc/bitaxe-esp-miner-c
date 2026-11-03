#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_test.h"
#include "bwg_trust.h"
#include "test_suites.h"

#define LEASE_X "oqVLcb9xO7-KjOQMBCbEVDyi_nZHuAnDoKtx0GMZhak"
#define UPDATE_X "h2FkuS6pe-kbUYWeO5RZt1OBr1FrsnIJU_GxzXiFShA"

static bool parse_with_replacement(const char *from, const char *to)
{
    size_t len = 0;
    yyjson_doc *doc = bwg_test_fixture_doc("deployment-trust/trust.json");
    char *text = bwg_test_json_line(yyjson_doc_get_root(doc), &len);
    yyjson_doc_free(doc);
    char *changed = malloc(len + strlen(to) + 1);
    const char *hit = strstr(text, from);
    if (hit == NULL) {
        bwg_test_fail(__FILE__, __LINE__, "the text to replace is absent");
        free(changed);
        free(text);
        return true;
    }
    size_t head = (size_t) (hit - text);
    memcpy(changed, text, head);
    memcpy(changed + head, to, strlen(to));
    strcpy(changed + head + strlen(to), hit + strlen(from));
    bwg_lease_trust_t trust;
    bool ok = bwg_trust_parse(&bwg_test_allocator, (const uint8_t *) changed, strlen(changed), &trust);
    free(changed);
    free(text);
    return ok;
}

static void fixture_trust_parses_to_its_lease_half(void)
{
    // Arrange
    size_t len = 0;
    char *text = bwg_test_read_fixture("deployment-trust/trust.json", &len);
    bwg_lease_trust_t trust;

    // Act
    bool ok = bwg_trust_parse(&bwg_test_allocator, (const uint8_t *) text, len, &trust);
    bwg_test_free_fixture(text);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_STREQ(trust.issuer, "development-worker-lease-authority");
    ASSERT_EQ(trust.key_count, 1);
    ASSERT_STREQ(trust.keys[0].kid, "fixture-serial-lease");
    ASSERT_TRUE(bwg_trust_maybe_find_key(&trust, "fixture-serial-lease", 20) == &trust.keys[0]);
    ASSERT_TRUE(bwg_trust_maybe_find_key(&trust, "fixture-serial-update", 21) == NULL);
}

static void every_lease_half_vector_parses(void)
{
    // Arrange
    const char *files[] = {
        "controller/difficulty-hint-vectors.json",
        "controller/qualification-attempt-vectors.json",
        "controller/soak-allowance-vectors.json",
    };
    for (size_t i = 0; i < 3; i++) {
        yyjson_doc *doc = bwg_test_fixture_doc(files[i]);
        size_t len = 0;
        char *text = bwg_test_json_line(yyjson_obj_get(yyjson_doc_get_root(doc), "trust"), &len);
        bwg_lease_trust_t trust;

        // Act
        bool ok = bwg_trust_parse_lease_half(&bwg_test_allocator, (const uint8_t *) text, len, &trust);
        free(text);
        yyjson_doc_free(doc);

        // Assert
        ASSERT_TRUE(ok);
        ASSERT_STREQ(trust.keys[0].kid, "rfc8032");
    }
}

static void each_changed_field_is_rejected(void)
{
    // Arrange
    static const char *const changes[][2] = {
        {"\"profile\":\"bwg-worker-deployment-trust/0.2\",\"updateAuthority\"", "\"profile\":\"x\",\"updateAuthority\""},
        {"\"role\":\"update_authority\"", "\"role\":\"work_lease_authority\""},
        {"\"role\":\"work_lease_authority\"", "\"role\":\"update_authority\""},
        {"bwg-reference-firmware-capability/0.2", "bwg-worker-controller/0.4"},
        {"\"audience\":\"bwg-worker-controller/0.4\"", "\"audience\":\"other\""},
        {"\"issuer\":\"development-worker-lease-authority\"", "\"issuer\":\"has space\""},
        {"\"issuer\":\"development-update-authority\"", "\"issuer\":\"\""},
        {"\"kid\":\"fixture-serial-lease\"", "\"kid\":\"has space\""},
        {"\"kid\":\"fixture-serial-lease\"", "\"kid\":\"123456789012345678901234567890123\""},
        {"\"kty\":\"OKP\"", "\"kty\":\"EC\""},
        {"\"crv\":\"Ed25519\"", "\"crv\":\"X25519\""},
        {"\"alg\":\"Ed25519\"", "\"alg\":\"none\""},
        {"\"use\":\"sig\"", "\"use\":\"enc\""},
        {"[\"verify\"]", "[\"sign\"]"},
        {"[\"verify\"]", "[\"verify\",\"sign\"]"},
        {"[\"verify\"]", "[]"},
        {"\"x\":\"" LEASE_X "\"", "\"x\":\"short\""},
        {"\"kid\":\"fixture-serial-lease\"", "\"kid\":\"fixture-serial-lease\",\"extra\":1"},
        {"\"kid\":\"fixture-serial-lease\"", "\"kid\":\"a\",\"kid\":\"b\""},
        {"\"workLeaseAuthority\"", "\"other\""},
    };
    // Act / Assert
    ASSERT_TRUE(parse_with_replacement("development-update-authority", "development-update-authority"));
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        ASSERT_FALSE(parse_with_replacement(changes[i][0], changes[i][1]));
    }
}

static void a_key_id_or_key_shared_between_roles_is_rejected(void)
{
    // Act / Assert
    ASSERT_FALSE(parse_with_replacement("\"kid\":\"fixture-serial-lease\"", "\"kid\":\"fixture-serial-update\""));
    ASSERT_FALSE(parse_with_replacement(LEASE_X, UPDATE_X));
}

static void weak_and_non_canonical_keys_are_rejected(void)
{
    // Arrange: small order, invalid sign of y = 1, off curve, and a mixed-order point
    // (the basepoint plus an order 8 point) that decompresses but is not torsion free.
    const char *keys[] = {
        "AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIA",
        "AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "mFGerfNbmVIztRtc0j6cxaKLY5taSvDskDy5YNgbeBk",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        ASSERT_FALSE(parse_with_replacement(LEASE_X, keys[i]));
    }
}

static void a_non_canonical_y_above_the_field_prime_is_rejected(void)
{
    // Arrange: y = p + 1 encoded in 32 bytes.
    uint8_t bytes[32];
    memset(bytes, 0xff, sizeof(bytes));
    bytes[0] = 0xee;
    bytes[31] = 0x7f;
    char x[64];
    bwg_b64url_encode(bytes, 32, x, sizeof(x), NULL);

    // Act / Assert
    ASSERT_FALSE(parse_with_replacement(LEASE_X, x));
}

static void trailing_bits_in_a_key_are_rejected(void)
{
    // Arrange: last character changed to one carrying nonzero trailing bits.
    char key[44];
    memcpy(key, LEASE_X, 44);
    key[42] = 'l';

    // Act / Assert
    ASSERT_FALSE(parse_with_replacement(LEASE_X, key));
}

static void key_counts_are_bounded(void)
{
    // Arrange
    char keys[4096];
    size_t at = 0;
    at += (size_t) snprintf(keys + at, sizeof(keys) - at, "[");
    for (int i = 0; i < 9; i++) {
        at += (size_t) snprintf(keys + at, sizeof(keys) - at,
                                "%s{\"kid\":\"k%d\",\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"" LEASE_X
                                "\",\"alg\":\"Ed25519\",\"use\":\"sig\",\"key_ops\":[\"verify\"]}",
                                i == 0 ? "" : ",", i);
    }
    snprintf(keys + at, sizeof(keys) - at, "]");

    // Act / Assert
    ASSERT_FALSE(parse_with_replacement(
        "[{\"kid\":\"fixture-serial-lease\",\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"" LEASE_X
        "\",\"alg\":\"Ed25519\",\"use\":\"sig\",\"key_ops\":[\"verify\"]}]",
        keys));
    ASSERT_FALSE(parse_with_replacement(
        "[{\"kid\":\"fixture-serial-lease\",\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"" LEASE_X
        "\",\"alg\":\"Ed25519\",\"use\":\"sig\",\"key_ops\":[\"verify\"]}]",
        "[]"));
}

void bwg_tests_trust(void)
{
    BWG_RUN(fixture_trust_parses_to_its_lease_half);
    BWG_RUN(every_lease_half_vector_parses);
    BWG_RUN(each_changed_field_is_rejected);
    BWG_RUN(a_key_id_or_key_shared_between_roles_is_rejected);
    BWG_RUN(weak_and_non_canonical_keys_are_rejected);
    BWG_RUN(a_non_canonical_y_above_the_field_prime_is_rejected);
    BWG_RUN(trailing_bits_in_a_key_are_rejected);
    BWG_RUN(key_counts_are_bounded);
}
