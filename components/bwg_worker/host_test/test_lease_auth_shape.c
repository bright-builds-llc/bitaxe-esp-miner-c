#include <stdio.h>
#include <stdlib.h>

#include "bwg_json_writer.h"
#include "bwg_lease_auth.h"
#include "bwg_test.h"
#include "test_jws.h"
#include "test_suites.h"
#include "test_lease_auth_support.h"

static void with_signature(const char *compact, const uint8_t signature[64], char *out, size_t capacity)
{
    const char *last_dot = strrchr(compact, '.');
    size_t head = (size_t) (last_dot - compact) + 1;
    memcpy(out, compact, head);
    bwg_b64url_encode(signature, 64, out + head, capacity - head, NULL);
}

static bwg_auth_result_t run_with_signature(const uint8_t signature[64])
{
    bwg_lease_trust_t trust;
    auth_load_trust(&trust);
    char compact[1024];
    with_signature(auth_fixture_compact("deployment-trust/start-authorization.json"), signature, compact, sizeof(compact));
    store_stub_t stub = {.present = false};
    return auth_run_start(&trust, &stub, compact, BINDING);
}

static void original_signature(uint8_t signature[64])
{
    const char *compact = auth_fixture_compact("deployment-trust/start-authorization.json");
    const char *text = strrchr(compact, '.') + 1;
    size_t written = 0;
    bwg_b64url_decode(text, strlen(text), signature, 64, &written);
}

static void signature_with_s_above_the_group_order_is_rejected(void)
{
    // Arrange: S + L verifies under a malleable verifier. L is little endian.
    static const uint8_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                  0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    uint8_t signature[64];
    original_signature(signature);
    ASSERT_EQ(run_with_signature(signature), BWG_AUTH_ACCEPTED);
    unsigned carry = 0;
    for (size_t i = 0; i < 32; i++) {
        unsigned sum = (unsigned) signature[32 + i] + L[i] + carry;
        signature[32 + i] = (uint8_t) sum;
        carry = sum >> 8;
    }

    // Act
    bwg_auth_result_t result = run_with_signature(signature);

    // Assert
    ASSERT_EQ(carry, 0);
    ASSERT_EQ(result, BWG_AUTH_INVALID);
}

static void signature_with_a_non_canonical_r_is_rejected(void)
{
    // Arrange: R encoded as y = p + 1, the identity point spelled out of range, with S = 0.
    uint8_t signature[64] = {0};
    memset(signature, 0xff, 32);
    signature[0] = 0xee;
    signature[31] = 0x7f;

    // Act
    bwg_auth_result_t result = run_with_signature(signature);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
}

static void signature_with_a_small_order_r_is_rejected(void)
{
    // Arrange
    uint8_t signature[64] = {0};
    signature[0] = 1;

    // Act
    bwg_auth_result_t result = run_with_signature(signature);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
}

static void all_zero_signature_is_rejected(void)
{
    // Arrange
    uint8_t signature[64] = {0};

    // Act / Assert
    ASSERT_EQ(run_with_signature(signature), BWG_AUTH_INVALID);
}

static void trailing_bits_in_the_signature_text_are_rejected(void)
{
    // Arrange: 86 characters encode 64 bytes with four spare bits.
    char compact[1024];
    strcpy(compact, auth_fixture_compact("deployment-trust/start-authorization.json"));
    compact[strlen(compact) - 1] = 'B';
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = false};

    // Act
    bwg_auth_result_t result = auth_run_start(&trust, &stub, compact, BINDING);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
}

static void compact_shapes_are_rejected(void)
{
    // Arrange
    const char *refused[] = {"", "a", "a.b", "a.b.c.d", ".b.c", "a..c", "a.b.", "YQ==.YQ.YQ"};
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = false};

    // Act / Assert
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_EQ(auth_run_start(&trust, &stub, refused[i], BINDING), BWG_AUTH_INVALID);
    }
}

static void header_and_claim_shapes_are_rejected(void)
{
    // Arrange: every one validly signed, so only the shape can refuse it.
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_rfc8032_trust(&trust));
    start_case_t c;
    ASSERT_TRUE(auth_start_case_open(&c, "a"));
    char digest[BWG_DIGEST_TEXT_SIZE];
    ASSERT_TRUE(bwg_lease_auth_start_digest(&trust, &c.grant, digest));
    auth_start_case_close(&c);
    char good_claims[256];
    snprintf(good_claims, sizeof(good_claims),
             "{\"controlSessionBindingSha256\":\"" AAAA_BINDING "\",\"operation\":\"start\",\"requestSha256\":\"%s\",\"sequence\":\"1\"}", digest);
    const char *good_header = "{\"alg\":\"Ed25519\",\"kid\":\"rfc8032\",\"typ\":\"bwg-worker-lease-authorization+jws\"}";
    char spaced_claims[256];
    snprintf(spaced_claims, sizeof(spaced_claims), "%s ", good_claims);
    char number_sequence[256];
    snprintf(number_sequence, sizeof(number_sequence),
             "{\"controlSessionBindingSha256\":\"" AAAA_BINDING "\",\"operation\":\"start\",\"requestSha256\":\"%s\",\"sequence\":1}", digest);
    char renew_operation[256];
    snprintf(renew_operation, sizeof(renew_operation),
             "{\"controlSessionBindingSha256\":\"" AAAA_BINDING "\",\"operation\":\"renew\",\"requestSha256\":\"%s\",\"sequence\":\"1\"}", digest);
    char reordered[256];
    snprintf(reordered, sizeof(reordered),
             "{\"operation\":\"start\",\"controlSessionBindingSha256\":\"" AAAA_BINDING "\",\"requestSha256\":\"%s\",\"sequence\":\"1\"}", digest);
    char extra[300];
    snprintf(extra, sizeof(extra),
             "{\"controlSessionBindingSha256\":\"" AAAA_BINDING "\",\"extra\":\"x\",\"operation\":\"start\",\"requestSha256\":\"%s\",\"sequence\":\"1\"}", digest);
    const char *wrong_digest =
        "{\"controlSessionBindingSha256\":\"" AAAA_BINDING "\",\"operation\":\"start\",\"requestSha256\":\"" AAAA_BINDING "\",\"sequence\":\"1\"}";
    const struct {
        const char *header;
        const char *claims;
    } cases[] = {
        {"{\"alg\":\"none\",\"kid\":\"rfc8032\",\"typ\":\"bwg-worker-lease-authorization+jws\"}", good_claims},
        {"{\"alg\":\"Ed25519\",\"kid\":\"rfc8032\",\"typ\":\"JWT\"}", good_claims},
        {"{\"alg\":\"Ed25519\",\"kid\":\"unknown\",\"typ\":\"bwg-worker-lease-authorization+jws\"}", good_claims},
        {"{\"alg\":\"Ed25519\",\"kid\":\"has space\",\"typ\":\"bwg-worker-lease-authorization+jws\"}", good_claims},
        {"{\"kid\":\"rfc8032\",\"alg\":\"Ed25519\",\"typ\":\"bwg-worker-lease-authorization+jws\"}", good_claims},
        {"{\"alg\":\"Ed25519\",\"kid\":\"rfc8032\",\"typ\":\"bwg-worker-lease-authorization+jws\",\"x\":1}", good_claims},
        {"{\"alg\":\"Ed25519\",\"alg\":\"Ed25519\",\"kid\":\"rfc8032\",\"typ\":\"bwg-worker-lease-authorization+jws\"}", good_claims},
        {"{\"alg\":\"Ed25519\",\"kid\":\"rfc8032\",\"typ\":\"bwg-worker-lease-authorization\\u002bjws\"}", good_claims},
        {good_header, spaced_claims},
        {good_header, number_sequence},
        {good_header, renew_operation},
        {good_header, reordered},
        {good_header, extra},
        {good_header, wrong_digest},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char compact[1024];
        bwg_test_sign_raw(BWG_TEST_RFC8032_SEED, cases[i].header, cases[i].claims, compact, sizeof(compact));
        store_stub_t stub = {.present = false};

        // Act
        bwg_auth_result_t result = auth_run_start(&trust, &stub, compact, AAAA_BINDING);

        // Assert
        ASSERT_EQ(result, BWG_AUTH_INVALID);
        ASSERT_FALSE(stub.present);
    }
    // The same helper accepts the unaltered shape, so the refusals above mean what they say.
    char compact[1024];
    bwg_test_sign_raw(BWG_TEST_RFC8032_SEED, good_header, good_claims, compact, sizeof(compact));
    store_stub_t stub = {.present = false};
    ASSERT_EQ(auth_run_start(&trust, &stub, compact, AAAA_BINDING), BWG_AUTH_ACCEPTED);
}

static void compare_and_store_outcomes_map_correctly(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    const char *compact = auth_fixture_compact("deployment-trust/start-authorization.json");
    const struct {
        bwg_store_result_t store;
        bwg_auth_result_t expected;
    } cases[] = {
        {BWG_STORE_STALE, BWG_AUTH_REPLAY},
        {BWG_STORE_ALREADY_COMMITTED, BWG_AUTH_REPLAY},
        {BWG_STORE_ERROR, BWG_AUTH_PERSISTENCE},
    };
    for (size_t i = 0; i < 3; i++) {
        store_stub_t stub = {.has_forced_result = true, .forced_result = cases[i].store};

        // Act
        bwg_auth_result_t result = auth_run_start(&trust, &stub, compact, BINDING);

        // Assert
        ASSERT_EQ(result, cases[i].expected);
    }
}

static void out_of_memory_is_invalid_and_changes_nothing(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    start_case_t c;
    ASSERT_TRUE(auth_start_case_open(&c, auth_fixture_compact("deployment-trust/start-authorization.json")));
    store_stub_t stub = {.present = false};
    const bwg_sequence_store_t store = auth_store_of(&stub);
    bwg_test_alloc_fail_after(0);

    // Act
    bwg_auth_result_t result = bwg_lease_auth_verify_start(&bwg_test_allocator, &trust, &store, &c.grant, BINDING, NULL);
    bwg_test_alloc_fail_after(-1);
    auth_start_case_close(&c);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
    ASSERT_FALSE(stub.present);
}

/** Verifies a signed vector whose grant carries its own authorization. */
static bwg_auth_result_t verify_vector_grant(const bwg_lease_trust_t *trust, yyjson_val *grant_value, const char *binding)
{
    bwg_lease_grant_t grant;
    if (!bwg_lease_grant_parse(grant_value, &grant)) {
        return BWG_AUTH_INVALID;
    }
    store_stub_t stub = {.present = false};
    const bwg_sequence_store_t store = auth_store_of(&stub);
    return bwg_lease_auth_verify_start(&bwg_test_allocator, trust, &store, &grant, binding, NULL);
}

static bool load_vector_trust(yyjson_val *root, bwg_lease_trust_t *trust)
{
    size_t len = 0;
    char *text = bwg_test_json_line(yyjson_obj_get(root, "trust"), &len);
    bool ok = bwg_trust_parse_lease_half(&bwg_test_allocator, (const uint8_t *) text, len, trust);
    free(text);
    return ok;
}

static void difficulty_hint_vectors_verify_and_match_their_canonical_request(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("controller/difficulty-hint-vectors.json");
    yyjson_val *root = yyjson_doc_get_root(doc);
    bwg_lease_trust_t trust;
    ASSERT_TRUE(load_vector_trust(root, &trust));
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(root, "vectors"));
    yyjson_val *vector;
    size_t seen = 0;
    while ((vector = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *input = yyjson_obj_get(vector, "input");
        const char *authorization = yyjson_get_str(yyjson_obj_get(vector, "authorization"));
        yyjson_doc *grant_doc = auth_doc_with_authorization(yyjson_obj_get(input, "request"), authorization);
        bwg_lease_grant_t grant;
        ASSERT_TRUE(bwg_lease_grant_parse(yyjson_doc_get_root(grant_doc), &grant));
        uint8_t canonical[2048];
        bwg_jw_t writer;
        bwg_jw_init_buffer(&writer, canonical, sizeof(canonical));

        // Act
        bwg_lease_write_start_request(&writer, &grant);
        store_stub_t stub = {.present = false};
        const bwg_sequence_store_t store = auth_store_of(&stub);
        bwg_auth_result_t result = bwg_lease_auth_verify_start(
            &bwg_test_allocator, &trust, &store, &grant, yyjson_get_str(yyjson_obj_get(input, "controlSessionBindingSha256")), NULL);

        // Assert
        yyjson_val *expected = yyjson_obj_get(vector, "canonical_request");
        ASSERT_TRUE(bwg_jw_ok(&writer));
        ASSERT_EQ(result, BWG_AUTH_ACCEPTED);
        ASSERT_EQ(writer.len, yyjson_get_len(expected));
        ASSERT_MEMEQ(canonical, yyjson_get_str(expected), writer.len);
        yyjson_doc_free(grant_doc);
        seen++;
    }
    yyjson_doc_free(doc);
    ASSERT_EQ(seen, 4);
}

static void signed_extension_vectors_verify_through_the_builder(void)
{
    // Arrange
    const char *files[] = {"controller/qualification-attempt-vectors.json", "controller/soak-allowance-vectors.json"};
    size_t expected_counts[] = {4, 2};
    for (size_t f = 0; f < 2; f++) {
        yyjson_doc *doc = bwg_test_fixture_doc(files[f]);
        yyjson_val *root = yyjson_doc_get_root(doc);
        bwg_lease_trust_t trust;
        ASSERT_TRUE(load_vector_trust(root, &trust));
        yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(root, "vectors"));
        yyjson_val *vector;
        size_t seen = 0;
        while ((vector = yyjson_arr_iter_next(&iter)) != NULL) {
            const char *binding = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(vector, "input"), "controlSessionBindingSha256"));

            // Act
            bwg_auth_result_t result = verify_vector_grant(&trust, yyjson_obj_get(vector, "grant"), binding);

            // Assert
            ASSERT_EQ(result, BWG_AUTH_ACCEPTED);
            seen++;
        }
        ASSERT_EQ(seen, expected_counts[f]);
        yyjson_doc_free(doc);
    }
}

static void pool_strings_escape_as_the_gate_does(void)
{
    // Arrange: a password with a quote, a backslash, a control character, an LF and non-ASCII,
    // whose digest was pinned from the Gate's JSON.stringify.
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    const char *text =
        "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"lease_fixture_01\","
        "\"challengeId\":\"challenge_00000000000000000000000000000001\",\"authorization\":\"a\","
        "\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":20000,\"stratum\":{\"endpoint\":\"stratum+tcp://127.0.0.1:3333/\","
        "\"username\":\"fixture-session-user\",\"password\":\"pa\\\"ss\\\\w\\u0001rd\\nπ雪🚀/\"}}";
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    bwg_lease_grant_t grant;
    ASSERT_TRUE(bwg_lease_grant_parse(yyjson_doc_get_root(doc), &grant));
    char digest[BWG_DIGEST_TEXT_SIZE];

    // Act
    bool ok = bwg_lease_auth_start_digest(&trust, &grant, digest);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_STREQ(digest, "DsT-i1_Ben_1Nk5jx9ursMNXeuTlu3JPWHtitMFOCp4");
}

void bwg_tests_lease_auth_shape(void)
{
    BWG_RUN(signature_with_s_above_the_group_order_is_rejected);
    BWG_RUN(signature_with_a_non_canonical_r_is_rejected);
    BWG_RUN(signature_with_a_small_order_r_is_rejected);
    BWG_RUN(all_zero_signature_is_rejected);
    BWG_RUN(trailing_bits_in_the_signature_text_are_rejected);
    BWG_RUN(compact_shapes_are_rejected);
    BWG_RUN(header_and_claim_shapes_are_rejected);
    BWG_RUN(compare_and_store_outcomes_map_correctly);
    BWG_RUN(out_of_memory_is_invalid_and_changes_nothing);
    BWG_RUN(difficulty_hint_vectors_verify_and_match_their_canonical_request);
    BWG_RUN(signed_extension_vectors_verify_through_the_builder);
    BWG_RUN(pool_strings_escape_as_the_gate_does);
}
