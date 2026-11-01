#include <stdlib.h>

#include "bwg_possession.h"
#include "bwg_test.h"
#include "test_suites.h"

static const uint8_t RFC8032_SEED[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                         0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                         0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
#define COMMIT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ELF "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void reproduce_vector(const char *name, bool check_binding)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("possession/fixtures.json");
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *vector = yyjson_obj_get(root, name);
    bwg_identity_t identity;
    bwg_identity_from_seed(&identity, RFC8032_SEED);
    bwg_possession_request_t request;
    ASSERT_TRUE(bwg_possession_parse_request(yyjson_obj_get(vector, "request"), &request));
    bwg_buffer_t response = {0};
    char binding[BWG_DIGEST_TEXT_SIZE];

    // Act
    bool ok = bwg_possession_prove(&bwg_test_allocator, &identity, &request, COMMIT, ELF, &response, binding);

    // Assert
    ASSERT_TRUE(ok);
    yyjson_doc *ours = yyjson_read((const char *) response.data, response.len, 0);
    ASSERT_TRUE(ours != NULL);
    ASSERT_TRUE(yyjson_equals(yyjson_doc_get_root(ours), yyjson_obj_get(vector, "response")));
    if (check_binding) {
        ASSERT_STREQ(binding, yyjson_get_str(yyjson_obj_get(root, "controlSessionBindingSha256")));
    }
    yyjson_doc_free(ours);
    bwg_buffer_release(&bwg_test_allocator, &response);
    bwg_identity_wipe(&identity);
    yyjson_doc_free(doc);
}

static void initial_admission_reproduces_the_exact_jws_and_binding(void)
{
    reproduce_vector("initialAdmission", true);
}

static void reacquisition_reproduces_the_exact_jws(void)
{
    reproduce_vector("reacquisition", false);
}

static void identity_fingerprint_is_the_digest_of_the_public_key(void)
{
    // Arrange
    bwg_identity_t identity;

    // Act
    bwg_identity_from_seed(&identity, RFC8032_SEED);

    // Assert
    ASSERT_STREQ(identity.x, "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo");
    uint8_t digest[32];
    crypto_hash_sha256(digest, identity.public_key, 32);
    char hex[65];
    bwg_hex_encode(digest, 32, hex);
    ASSERT_STREQ(identity.fingerprint_hex, hex);
    bwg_identity_wipe(&identity);
}

static void wiping_the_identity_clears_the_secret_key(void)
{
    // Arrange
    bwg_identity_t identity;
    bwg_identity_from_seed(&identity, RFC8032_SEED);

    // Act
    bwg_identity_wipe(&identity);

    // Assert
    uint8_t zero[crypto_sign_SECRETKEYBYTES] = {0};
    ASSERT_MEMEQ(identity.secret_key, zero, sizeof(zero));
}

/** Takes the fixture request with one change applied to its text. */
static bool parses_after_replacing(const char *from, const char *to)
{
    yyjson_doc *doc = bwg_test_fixture_doc("possession/fixtures.json");
    size_t len = 0;
    char *text = bwg_test_json_line(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "initialAdmission"), "request"), &len);
    char changed[2048];
    const char *hit = strstr(text, from);
    size_t head = (size_t) (hit - text);
    memcpy(changed, text, head);
    size_t to_len = strlen(to);
    memcpy(changed + head, to, to_len);
    strcpy(changed + head + to_len, hit + strlen(from));
    yyjson_doc *altered = yyjson_read(changed, strlen(changed), 0);
    bwg_possession_request_t request;
    bool ok = bwg_possession_parse_request(yyjson_doc_get_root(altered), &request);
    yyjson_doc_free(altered);
    free(text);
    yyjson_doc_free(doc);
    return ok;
}

static void parse_rejects_each_bad_shape(void)
{
    // Arrange
    const char *from[] = {
        "\"bwg-worker-possession/0.2\"", "\"pos_initial_01\"", "\"pos_initial_01\"", "\"prove_possession\"",
        "\"initial_admission\"", "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"", "\"AgICAgICAgICAgICAgICAg\"",
        "\"AwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwM\"", "\"BAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ\"",
        "\"purpose\"",
    };
    const char *to[] = {
        "\"bwg-worker-possession/0.1\"", "\"serial_1\"", "\"pos_a b\"", "\"status\"", "\"other\"", "\"short\"",
        "\"AgICAgICAgICAgICAgICAh\"", "\"AwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwN\"",
        "\"BAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAR\"", "\"purposes\"",
    };
    // Act / Assert
    ASSERT_TRUE(parses_after_replacing("\"pos_initial_01\"", "\"pos_x\""));
    for (size_t i = 0; i < sizeof(from) / sizeof(from[0]); i++) {
        ASSERT_FALSE(parses_after_replacing(from[i], to[i]));
    }
}

static void parse_rejects_an_identifier_over_128_characters(void)
{
    // Arrange
    char id[140];
    id[0] = '"';
    memcpy(id + 1, "pos_", 4);
    memset(id + 5, 'x', 125);
    id[130] = '"';
    id[131] = '\0';

    // Act / Assert
    ASSERT_FALSE(parses_after_replacing("\"pos_initial_01\"", id));
    id[129] = '"';
    id[130] = '\0';
    ASSERT_TRUE(parses_after_replacing("\"pos_initial_01\"", id));
}

static void matches_checks_every_bound_value(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("possession/fixtures.json");
    yyjson_val *vector = yyjson_obj_get(yyjson_doc_get_root(doc), "initialAdmission");
    bwg_possession_request_t request;
    ASSERT_TRUE(bwg_possession_parse_request(yyjson_obj_get(vector, "request"), &request));
    const char *capability = "OUAcmoLu-Sjv6nq6fqavz8_3Ev18z7ziNXuEN3FVUrg";
    const char *manifest = "pDwsfWH5X8bHMTqyB0SgAxTKA0_KpzBqdSLxMpKVNhU";
    bwg_session_binding_t binding = {
        .session_id = "AgICAgICAgICAgICAgICAg",
        .host_nonce = "AwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwM",
        .device_nonce = "BAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ",
    };

    // Act / Assert
    ASSERT_TRUE(bwg_possession_matches(&request, capability, manifest, &binding));
    ASSERT_FALSE(bwg_possession_matches(&request, manifest, manifest, &binding));
    ASSERT_FALSE(bwg_possession_matches(&request, capability, capability, &binding));
    bwg_session_binding_t other = binding;
    other.session_id[0] = 'B';
    ASSERT_FALSE(bwg_possession_matches(&request, capability, manifest, &other));
    other = binding;
    other.host_nonce[0] = 'B';
    ASSERT_FALSE(bwg_possession_matches(&request, capability, manifest, &other));
    other = binding;
    other.device_nonce[0] = 'C';
    ASSERT_FALSE(bwg_possession_matches(&request, capability, manifest, &other));
    yyjson_doc_free(doc);
}

static void prove_reports_allocation_failure_at_each_step(void)
{
    // Arrange
    yyjson_doc *doc = bwg_test_fixture_doc("possession/fixtures.json");
    yyjson_val *vector = yyjson_obj_get(yyjson_doc_get_root(doc), "initialAdmission");
    bwg_identity_t identity;
    bwg_identity_from_seed(&identity, RFC8032_SEED);
    bwg_possession_request_t request;
    ASSERT_TRUE(bwg_possession_parse_request(yyjson_obj_get(vector, "request"), &request));
    for (long step = 0; step < 3; step++) {
        bwg_test_alloc_fail_after(step);
        bwg_buffer_t response = {0};
        char binding[BWG_DIGEST_TEXT_SIZE];

        // Act
        bool ok = bwg_possession_prove(&bwg_test_allocator, &identity, &request, COMMIT, ELF, &response, binding);

        // Assert
        ASSERT_FALSE(ok);
        ASSERT_TRUE(response.data == NULL);
        ASSERT_EQ(bwg_test_alloc_outstanding(), 0);
    }
    bwg_test_alloc_fail_after(-1);
    bwg_identity_wipe(&identity);
    yyjson_doc_free(doc);
}

void bwg_tests_possession(void)
{
    BWG_RUN(initial_admission_reproduces_the_exact_jws_and_binding);
    BWG_RUN(reacquisition_reproduces_the_exact_jws);
    BWG_RUN(identity_fingerprint_is_the_digest_of_the_public_key);
    BWG_RUN(wiping_the_identity_clears_the_secret_key);
    BWG_RUN(parse_rejects_each_bad_shape);
    BWG_RUN(parse_rejects_an_identifier_over_128_characters);
    BWG_RUN(matches_checks_every_bound_value);
    BWG_RUN(prove_reports_allocation_failure_at_each_step);
}
