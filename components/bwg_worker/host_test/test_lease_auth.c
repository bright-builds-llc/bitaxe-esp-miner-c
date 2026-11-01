#include <stdio.h>
#include <stdlib.h>

#include "bwg_json_writer.h"
#include "bwg_lease_auth.h"
#include "bwg_test.h"
#include "test_jws.h"
#include "test_suites.h"
#include "test_lease_auth_support.h"

static void fixture_digests_reproduce(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    start_case_t c;
    ASSERT_TRUE(auth_start_case_open(&c, "a"));
    char digest[BWG_DIGEST_TEXT_SIZE];

    // Act
    bool ok = bwg_lease_auth_start_digest(&trust, &c.grant, digest);
    auth_start_case_close(&c);

    // Assert
    ASSERT_TRUE(ok);
    ASSERT_STREQ(digest, "tTmJCS9xl-UNY5oCIJ6cywocZs6qZQF71wWEF0FFHng");
}

static void fixture_authorizations_are_accepted_and_committed(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = false};

    // Act
    bwg_auth_result_t start = auth_run_start(&trust, &stub, auth_fixture_compact("deployment-trust/start-authorization.json"), BINDING);
    uint64_t after_start = stub.value;
    bwg_auth_result_t renew = auth_run_renew(&trust, &stub, auth_fixture_compact("deployment-trust/renew-authorization.json"), BINDING);

    // Assert
    ASSERT_EQ(start, BWG_AUTH_ACCEPTED);
    ASSERT_EQ(after_start, 1);
    ASSERT_EQ(renew, BWG_AUTH_ACCEPTED);
    ASSERT_EQ(stub.value, 2);
}

static void rejection_table_never_advances_the_sequence(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = false};
    char start[1024];
    char renew[1024];
    strcpy(start, auth_fixture_compact("deployment-trust/start-authorization.json"));
    strcpy(renew, auth_fixture_compact("deployment-trust/renew-authorization.json"));

    // Act / Assert: accepted once, then replayed.
    ASSERT_EQ(auth_run_start(&trust, &stub, start, BINDING), BWG_AUTH_ACCEPTED);
    ASSERT_EQ(auth_run_start(&trust, &stub, start, BINDING), BWG_AUTH_REPLAY);
    ASSERT_EQ(stub.stores, 1);

    // Old authorization in another context, changed password, flipped signature, oversize.
    ASSERT_EQ(auth_run_renew(&trust, &stub, renew, OTHER_BINDING), BWG_AUTH_INVALID);
    start_case_t changed;
    ASSERT_TRUE(auth_start_case_open(&changed, start));
    changed.grant.password = (bwg_str_t) {.ptr = "changed", .len = 7};
    const bwg_sequence_store_t store = auth_store_of(&stub);
    ASSERT_EQ(bwg_lease_auth_verify_start(&bwg_test_allocator, &trust, &store, &changed.grant, BINDING, NULL), BWG_AUTH_INVALID);
    auth_start_case_close(&changed);
    char flipped[1024];
    strcpy(flipped, renew);
    size_t last = strlen(flipped) - 1;
    flipped[last] = flipped[last] == 'A' ? 'Q' : 'A';
    ASSERT_EQ(auth_run_renew(&trust, &stub, flipped, BINDING), BWG_AUTH_INVALID);
    char oversized[514];
    memset(oversized, 'a', 513);
    oversized[513] = '\0';
    ASSERT_EQ(auth_run_renew(&trust, &stub, oversized, BINDING), BWG_AUTH_INVALID);

    // No rejection moved the high water, so the valid renew still goes through.
    ASSERT_EQ(stub.value, 1);
    ASSERT_EQ(auth_run_renew(&trust, &stub, renew, BINDING), BWG_AUTH_ACCEPTED);
    ASSERT_EQ(auth_run_renew(&trust, &stub, renew, BINDING), BWG_AUTH_REPLAY);
    ASSERT_EQ(stub.value, 2);
}

static void negative_wrong_authority_role(void)
{
    // Arrange: the same key ID, but the lease key really is the update key.
    bwg_lease_trust_t trust;
    char text[1024];
    snprintf(text, sizeof(text),
             "{\"profile\":\"bwg-worker-deployment-trust/0.2\",\"issuer\":\"development-worker-lease-authority\","
             "\"audience\":\"bwg-worker-controller/0.4\",\"role\":\"work_lease_authority\",\"keys\":[{\"kid\":\"fixture-serial-lease\","
             "\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"h2FkuS6pe-kbUYWeO5RZt1OBr1FrsnIJU_GxzXiFShA\",\"alg\":\"Ed25519\","
             "\"use\":\"sig\",\"key_ops\":[\"verify\"]}]}");
    ASSERT_TRUE(bwg_trust_parse_lease_half(&bwg_test_allocator, (const uint8_t *) text, strlen(text), &trust));
    store_stub_t stub = {.present = false};

    // Act
    bwg_auth_result_t result = auth_run_start(&trust, &stub, auth_fixture_compact("deployment-trust/start-authorization.json"), BINDING);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
    ASSERT_FALSE(stub.present);
}

static void negative_changed_request(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = false};
    start_case_t c;
    ASSERT_TRUE(auth_start_case_open(&c, auth_fixture_compact("deployment-trust/start-authorization.json")));
    c.grant.password = (bwg_str_t) {.ptr = "changed", .len = 7};
    const bwg_sequence_store_t store = auth_store_of(&stub);

    // Act
    bwg_auth_result_t result = bwg_lease_auth_verify_start(&bwg_test_allocator, &trust, &store, &c.grant, BINDING, NULL);
    auth_start_case_close(&c);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
}

static void negative_changed_context_and_identity(void)
{
    // Arrange: `changed_context` and `same_nonce_different_identity` both present binding T...T.
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = false};

    // Act
    bwg_auth_result_t result = auth_run_start(&trust, &stub, auth_fixture_compact("deployment-trust/start-authorization.json"), OTHER_BINDING);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_INVALID);
    ASSERT_FALSE(stub.present);
}

static void negative_replayed_sequence(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.present = true, .value = 1};

    // Act
    bwg_auth_result_t result = auth_run_start(&trust, &stub, auth_fixture_compact("deployment-trust/start-authorization.json"), BINDING);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_REPLAY);
}

/** Signs the fixture's Start request under the RFC 8032 key. */
static void sign_start(const bwg_lease_trust_t *trust, const char *binding, const char *sequence, char *out, size_t capacity)
{
    start_case_t c;
    auth_start_case_open(&c, "a");
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_lease_auth_start_digest(trust, &c.grant, digest);
    auth_start_case_close(&c);
    bwg_test_sign_lease(BWG_TEST_RFC8032_SEED, "rfc8032", binding, "start", digest, sequence, out, capacity);
}

static void negative_cross_session_replay(void)
{
    // Arrange: a new context presenting a sequence at the high water.
    const char *binding = "UUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUU";
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_rfc8032_trust(&trust));
    char compact[1024];
    sign_start(&trust, binding, "1", compact, sizeof(compact));
    store_stub_t stub = {.present = true, .value = 1};

    // Act
    bwg_auth_result_t result = auth_run_start(&trust, &stub, compact, binding);

    // Assert
    ASSERT_EQ(result, BWG_AUTH_REPLAY);
}

static void negative_corrupt_high_water(void)
{
    // Arrange: a store that cannot produce a value, as a stored "-1" would.
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_load_trust(&trust));
    store_stub_t stub = {.fail_load = true};
    const char *compact = auth_fixture_compact("deployment-trust/start-authorization.json");

    // Act
    bwg_auth_result_t current = auth_run_start(&trust, &stub, compact, BINDING);
    bwg_auth_result_t other = auth_run_start(&trust, &stub, compact, OTHER_BINDING);

    // Assert
    ASSERT_EQ(current, BWG_AUTH_PERSISTENCE);
    ASSERT_EQ(other, BWG_AUTH_INVALID);
}

static void negative_sequence_overflow(void)
{
    // Arrange
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_rfc8032_trust(&trust));
    char at_max[1024];
    char past_max[1024];
    sign_start(&trust, AAAA_BINDING, "18446744073709551615", at_max, sizeof(at_max));
    sign_start(&trust, AAAA_BINDING, "18446744073709551616", past_max, sizeof(past_max));
    store_stub_t stub = {.present = false};

    // Act
    bwg_auth_result_t past = auth_run_start(&trust, &stub, past_max, AAAA_BINDING);
    bwg_auth_result_t at = auth_run_start(&trust, &stub, at_max, AAAA_BINDING);

    // Assert
    ASSERT_EQ(past, BWG_AUTH_INVALID);
    ASSERT_EQ(at, BWG_AUTH_ACCEPTED);
    ASSERT_EQ(stub.value, UINT64_MAX);
}

static void sequence_text_must_be_canonical(void)
{
    // Arrange
    const char *refused[] = {"0", "01", "", "+1", "1.0", "-1", "1 "};
    bwg_lease_trust_t trust;
    ASSERT_TRUE(auth_rfc8032_trust(&trust));
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char compact[1024];
        sign_start(&trust, AAAA_BINDING, refused[i], compact, sizeof(compact));
        store_stub_t stub = {.present = false};

        // Act
        bwg_auth_result_t result = auth_run_start(&trust, &stub, compact, AAAA_BINDING);

        // Assert
        ASSERT_EQ(result, BWG_AUTH_INVALID);
        ASSERT_FALSE(stub.present);
    }
}

/** Replaces the signature segment with these 64 bytes. */
void bwg_tests_lease_auth(void)
{
    BWG_RUN(fixture_digests_reproduce);
    BWG_RUN(fixture_authorizations_are_accepted_and_committed);
    BWG_RUN(rejection_table_never_advances_the_sequence);
    BWG_RUN(negative_wrong_authority_role);
    BWG_RUN(negative_changed_request);
    BWG_RUN(negative_changed_context_and_identity);
    BWG_RUN(negative_replayed_sequence);
    BWG_RUN(negative_cross_session_replay);
    BWG_RUN(negative_corrupt_high_water);
    BWG_RUN(negative_sequence_overflow);
    BWG_RUN(sequence_text_must_be_canonical);
}
