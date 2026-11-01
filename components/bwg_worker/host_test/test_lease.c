#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_lease.h"
#include "bwg_test.h"
#include "test_suites.h"

static bool parse_grant_text(const char *text, bwg_lease_grant_t *grant, yyjson_doc **doc)
{
    *doc = yyjson_read(text, strlen(text), 0);
    return bwg_lease_grant_parse(yyjson_doc_get_root(*doc), grant);
}

static void grant_text(char *out, size_t capacity, const char *stratum, const char *extra)
{
    snprintf(out, capacity,
             "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"lease_1\",\"challengeId\":\"challenge_1\","
             "\"authorization\":\"a.b.c\",\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":20000,"
             "\"stratum\":%s%s}",
             stratum, extra);
}

#define V1_STRATUM "{\"endpoint\":\"stratum+tcp://127.0.0.1:3333/\",\"username\":\"u\",\"password\":\"p\"}"

static bool stratum_parses(const char *stratum)
{
    char text[1024];
    grant_text(text, sizeof(text), stratum, "");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;
    bool ok = parse_grant_text(text, &grant, &doc);
    yyjson_doc_free(doc);
    return ok;
}

static void plain_v1_grant_parses_and_validates(void)
{
    // Arrange
    char text[1024];
    grant_text(text, sizeof(text), V1_STRATUM, "");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;

    // Act
    bool parsed = parse_grant_text(text, &grant, &doc);
    bool valid = bwg_lease_grant_validate(&grant);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(valid);
    ASSERT_EQ(grant.stratum_kind, BWG_STRATUM_V1);
    ASSERT_EQ(grant.duration_ms, 60000);
    ASSERT_FALSE(grant.has_suggested_difficulty);
}

static void stratum_shapes_are_closed(void)
{
    // Arrange
    const char *refused[] = {
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"profile\":null}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"profile\":\"unknown\"}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"suggestedDifficulty\":null}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"suggestedDifficulty\":65536}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"suggestedDifficulty\":1.0}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"suggestedDifficulty\":-1}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"userIdentity\":\"i\"}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"authorityPublicKey\":\"k\"}",
        "{\"endpoint\":\"e\",\"username\":\"u\",\"password\":\"p\",\"hardwareProfile\":{}}",
        "{\"endpoint\":\"e\",\"username\":\"u\"}",
        "{\"endpoint\":\"e\",\"password\":\"p\"}",
        "{\"username\":\"u\",\"password\":\"p\"}",
        "{\"endpoint\":\"e\",\"profile\":\"bwg-worker-stratum-v2-standard/0.1\",\"userIdentity\":\"i\"}",
        "{\"endpoint\":\"e\",\"profile\":\"bwg-worker-stratum-v2-standard/0.1\",\"authorityPublicKey\":\"k\"}",
        "{\"endpoint\":\"e\",\"profile\":\"bwg-worker-stratum-v2-standard/0.1\",\"authorityPublicKey\":\"k\",\"userIdentity\":\"i\",\"username\":\"u\"}",
        "[]",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_FALSE(stratum_parses(refused[i]));
    }
}

static void explicit_zero_difficulty_is_kept(void)
{
    // Arrange
    char text[1024];
    grant_text(text, sizeof(text), "{\"endpoint\":\"stratum+tcp://h:1/\",\"username\":\"u\",\"password\":\"p\",\"suggestedDifficulty\":0}", "");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;

    // Act
    bool parsed = parse_grant_text(text, &grant, &doc);
    bool valid = bwg_lease_grant_validate(&grant);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(valid);
    ASSERT_TRUE(grant.has_suggested_difficulty);
    ASSERT_EQ(grant.suggested_difficulty, 0);
}

#define V2_KEY "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA"

static void v2_stratum_text(char *out, size_t capacity, const char *endpoint, const char *key, const char *identity)
{
    snprintf(out, capacity,
             "{\"endpoint\":\"%s\",\"profile\":\"bwg-worker-stratum-v2-standard/0.1\","
             "\"authorityPublicKey\":\"%s\",\"userIdentity\":\"%s\"}",
             endpoint, key, identity);
}

static bool v2_grant_validates(const char *endpoint, const char *key, const char *identity)
{
    char stratum[1024];
    char text[2048];
    v2_stratum_text(stratum, sizeof(stratum), endpoint, key, identity);
    grant_text(text, sizeof(text), stratum, "");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;
    const bool ok = parse_grant_text(text, &grant, &doc) && bwg_lease_grant_validate(&grant);
    yyjson_doc_free(doc);
    return ok;
}

static void a_v2_grant_with_private_endpoint_validates(void)
{
    // Arrange
    char stratum[1024];
    char text[2048];
    v2_stratum_text(stratum, sizeof(stratum), "stratum+tcp://192.168.1.2:3336/", V2_KEY, "bc1qexample.worker");
    grant_text(text, sizeof(text), stratum, "");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;

    // Act
    bool parsed = parse_grant_text(text, &grant, &doc);
    bool valid = bwg_lease_grant_validate(&grant);

    // Assert
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(valid);
    ASSERT_EQ(grant.stratum_kind, BWG_STRATUM_V2);
    ASSERT_TRUE(bwg_str_equals(grant.authority_public_key, V2_KEY));
    ASSERT_TRUE(bwg_str_equals(grant.user_identity, "bc1qexample.worker"));
    yyjson_doc_free(doc);
}

static void a_v2_grant_refuses_public_or_named_endpoints(void)
{
    // Arrange
    const char *refused[] = {
        "stratum+tcp://8.8.8.8:3336/",      "stratum+tcp://pool.example.com:3336/", "stratum+tcp://127.0.0.1:3336/",
        "stratum+tcp://172.32.0.1:3336/",   "stratum+tcp://192.168.1.02:3336/",     "stratum+tcp://192.168.1:3336/",
        "stratum+tcp://192.168.1.2.3:3336/", "stratum+tcp://192.168.1.256:3336/",   "stratum+tcp://10.0.0.1:0/",
        "stratum+tcp://10.0.0.1:03336/",    "stratum+tcp://10.0.0.1:3336",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_FALSE(v2_grant_validates(refused[i], V2_KEY, "user"));
    }
}

static void a_v2_grant_needs_a_canonical_authority_key(void)
{
    // Arrange
    const char *refused[] = {
        "k",
        "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyB",
        "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA=",
        "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHw",
    };
    // Act / Assert
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_FALSE(v2_grant_validates("stratum+tcp://10.0.0.1:3336/", refused[i], "user"));
    }
}

static void a_v2_grant_bounds_its_user_identity(void)
{
    // Arrange
    char longest[BWG_STRATUM_V2_MAX_USER_BYTES + 1];
    char too_long[BWG_STRATUM_V2_MAX_USER_BYTES + 2];
    memset(longest, 'u', sizeof(longest) - 1);
    longest[sizeof(longest) - 1] = '\0';
    memset(too_long, 'u', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    // Act / Assert
    ASSERT_TRUE(v2_grant_validates("stratum+tcp://10.0.0.1:3336/", V2_KEY, longest));
    ASSERT_FALSE(v2_grant_validates("stratum+tcp://10.0.0.1:3336/", V2_KEY, too_long));
    ASSERT_FALSE(v2_grant_validates("stratum+tcp://10.0.0.1:3336/", V2_KEY, ""));
    ASSERT_FALSE(v2_grant_validates("stratum+tcp://10.0.0.1:3336/", V2_KEY, "a\\u0000b"));
}

static void a_malformed_allowance_fails_at_parse(void)
{
    // Arrange
    const char *extras[] = {
        ",\"qualificationAttempt\":{\"schema\":\"s\"}",
        ",\"soakAllowance\":{\"schema\":\"s\"}",
        ",\"acceptanceCampaign\":null",
        ",\"acceptanceCampaign\":{\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"window\":256,\"maximumActiveMilliseconds\":180000}",
    };
    for (size_t i = 0; i < 4; i++) {
        char text[1024];
        grant_text(text, sizeof(text), V1_STRATUM, extras[i]);
        bwg_lease_grant_t grant;
        yyjson_doc *doc;

        // Act
        bool parsed = parse_grant_text(text, &grant, &doc);
        yyjson_doc_free(doc);

        // Assert
        ASSERT_FALSE(parsed);
    }
}

static void an_impossible_allowance_fails_validation(void)
{
    // Arrange
    const char *extras[] = {
        ",\"acceptanceCampaign\":{\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"window\":3,\"maximumActiveMilliseconds\":30000}",
        ",\"acceptanceCampaign\":{\"id\":\"AAAAAAAAAAAAAAAAAAAAAB\",\"window\":0,\"maximumActiveMilliseconds\":180000}",
        ",\"acceptanceCampaign\":{\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"window\":1,\"maximumActiveMilliseconds\":180000}",
        ",\"qualificationAttempt\":{\"schema\":\"worker-qualification-attempt-v1\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"ordinal\":1,\"purpose\":\"diagnostic\",\"maximumActiveMilliseconds\":180000}",
        ",\"qualificationAttempt\":{\"schema\":\"worker-qualification-attempt-v1\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"ordinal\":0,\"purpose\":\"diagnostic\",\"maximumActiveMilliseconds\":30000}",
        ",\"soakAllowance\":{\"schema\":\"worker-soak-allowance-v1\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"ordinal\":1,"
        "\"maximumActiveMilliseconds\":600000}",
    };
    for (size_t i = 0; i < 6; i++) {
        char text[1024];
        grant_text(text, sizeof(text), V1_STRATUM, extras[i]);
        bwg_lease_grant_t grant;
        yyjson_doc *doc;

        // Act
        bool parsed = parse_grant_text(text, &grant, &doc);
        bool valid = parsed && bwg_lease_grant_validate(&grant);
        yyjson_doc_free(doc);

        // Assert
        ASSERT_TRUE(parsed);
        ASSERT_FALSE(valid);
    }
}

static void each_allowance_kind_validates(void)
{
    // Arrange
    const struct {
        const char *extra;
        bwg_allowance_kind_t kind;
    } cases[] = {
        {",\"acceptanceCampaign\":{\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"window\":2,\"maximumActiveMilliseconds\":30000}",
         BWG_ALLOWANCE_CAMPAIGN},
        {",\"qualificationAttempt\":{\"schema\":\"worker-qualification-attempt-v1\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\","
         "\"ordinal\":2,\"purpose\":\"normal\",\"maximumActiveMilliseconds\":180000}",
         BWG_ALLOWANCE_ATTEMPT},
        {",\"soakAllowance\":{\"schema\":\"worker-soak-allowance-v2\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"ordinal\":1,"
         "\"maximumActiveMilliseconds\":600000}",
         BWG_ALLOWANCE_SOAK},
    };
    for (size_t i = 0; i < 3; i++) {
        char text[1024];
        grant_text(text, sizeof(text), V1_STRATUM, cases[i].extra);
        bwg_lease_grant_t grant;
        yyjson_doc *doc;

        // Act
        bool parsed = parse_grant_text(text, &grant, &doc);
        bool valid = parsed && bwg_lease_grant_validate(&grant);
        yyjson_doc_free(doc);

        // Assert
        ASSERT_TRUE(valid);
        ASSERT_EQ(grant.allowance.kind, cases[i].kind);
    }
}

static void two_allowances_never_validate(void)
{
    // Arrange
    char text[1024];
    grant_text(text, sizeof(text), V1_STRATUM,
               ",\"acceptanceCampaign\":{\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"window\":2,\"maximumActiveMilliseconds\":30000}"
               ",\"qualificationAttempt\":{\"schema\":\"worker-qualification-attempt-v1\",\"id\":\"AAAAAAAAAAAAAAAAAAAAAA\","
               "\"ordinal\":2,\"purpose\":\"diagnostic\",\"maximumActiveMilliseconds\":30000}");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;

    // Act
    bool parsed = parse_grant_text(text, &grant, &doc);
    bool valid = parsed && bwg_lease_grant_validate(&grant);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_TRUE(parsed);
    ASSERT_FALSE(valid);
}

static void unknown_grant_fields_are_refused(void)
{
    // Arrange
    char text[1024];
    grant_text(text, sizeof(text), V1_STRATUM, ",\"hardwareProfile\":{}");
    bwg_lease_grant_t grant;
    yyjson_doc *doc;

    // Act
    bool parsed = parse_grant_text(text, &grant, &doc);
    yyjson_doc_free(doc);

    // Assert
    ASSERT_FALSE(parsed);
}

static bool window_validates(uint64_t duration, uint64_t renew_after)
{
    char text[1024];
    snprintf(text, sizeof(text),
             "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"l\",\"challengeId\":\"c\",\"authorization\":\"a\","
             "\"durationMilliseconds\":%llu,\"renewAfterMilliseconds\":%llu,\"stratum\":" V1_STRATUM "}",
             (unsigned long long) duration, (unsigned long long) renew_after);
    bwg_lease_grant_t grant;
    yyjson_doc *doc;
    bool ok = parse_grant_text(text, &grant, &doc) && bwg_lease_grant_validate(&grant);
    yyjson_doc_free(doc);
    return ok;
}

static void window_limits_are_exact(void)
{
    // Act / Assert
    ASSERT_TRUE(window_validates(60000, 20000));
    ASSERT_TRUE(window_validates(2, 1));
    ASSERT_FALSE(window_validates(60001, 20000));
    ASSERT_FALSE(window_validates(60000, 20001));
    ASSERT_FALSE(window_validates(1000, 1000));
    ASSERT_FALSE(window_validates(0, 0));
    ASSERT_FALSE(window_validates(1000, 0));
}

static bool endpoint_ok(const char *text, char *host_out, uint16_t *port)
{
    bwg_str_t host;
    bwg_str_t endpoint = {.ptr = text, .len = strlen(text)};
    bool ok = bwg_stratum_endpoint_parse(endpoint, &host, port);
    if (ok && host_out != NULL) {
        memcpy(host_out, host.ptr, host.len);
        host_out[host.len] = '\0';
    }
    return ok;
}

static void endpoints_follow_the_exact_rule(void)
{
    // Arrange
    const char *refused[] = {
        "stratum+tcp://h:1",        "stratum+ssl://h:1/",   "stratum+tcp://h/",        "stratum+tcp://:1/",
        "stratum+tcp://h:0/",       "stratum+tcp://h:65536/", "stratum+tcp://h:01/",   "stratum+tcp://h:+1/",
        "stratum+tcp://u@h:1/",     "stratum+tcp://h:1/?x", "stratum+tcp://h:1/#",     "stratum+tcp://h/x:1/",
        "stratum+tcp://h:/",        "stratum+tcp://",       "",
    };
    char host[64];
    uint16_t port = 0;

    // Act / Assert
    ASSERT_TRUE(endpoint_ok("stratum+tcp://pool.example.test:3333/", host, &port));
    ASSERT_STREQ(host, "pool.example.test");
    ASSERT_EQ(port, 3333);
    ASSERT_TRUE(endpoint_ok("stratum+tcp://h:65535/", host, &port));
    ASSERT_EQ(port, 65535);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_FALSE(endpoint_ok(refused[i], NULL, &port));
    }
}

static bool grant_validates_with(const char *lease_id, const char *authorization, const char *username, const char *password)
{
    char *text = malloc(4096);
    snprintf(text, 4096,
             "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"%s\",\"challengeId\":\"c\",\"authorization\":\"%s\","
             "\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":20000,\"stratum\":"
             "{\"endpoint\":\"stratum+tcp://h:1/\",\"username\":\"%s\",\"password\":\"%s\"}}",
             lease_id, authorization, username, password);
    bwg_lease_grant_t grant;
    yyjson_doc *doc;
    bool ok = parse_grant_text(text, &grant, &doc) && bwg_lease_grant_validate(&grant);
    yyjson_doc_free(doc);
    free(text);
    return ok;
}

static void credentials_and_identifiers_are_bounded(void)
{
    // Arrange
    char at_512[513];
    char at_513[514];
    char id_128[129];
    char id_129[130];
    memset(at_512, 'x', 512);
    at_512[512] = '\0';
    memset(at_513, 'x', 513);
    at_513[513] = '\0';
    memset(id_128, 'x', 128);
    id_128[128] = '\0';
    memset(id_129, 'x', 129);
    id_129[129] = '\0';

    // Act / Assert
    ASSERT_TRUE(grant_validates_with(id_128, at_512, at_512, at_512));
    ASSERT_FALSE(grant_validates_with(id_129, "a", "u", "p"));
    ASSERT_FALSE(grant_validates_with("has space", "a", "u", "p"));
    ASSERT_FALSE(grant_validates_with("l", at_513, "u", "p"));
    ASSERT_FALSE(grant_validates_with("l", "", "u", "p"));
    ASSERT_FALSE(grant_validates_with("l", "a", at_513, "p"));
    ASSERT_FALSE(grant_validates_with("l", "a", "", "p"));
    ASSERT_FALSE(grant_validates_with("l", "a", "u", at_513));
    ASSERT_FALSE(grant_validates_with("l", "a", "u", ""));
}

static void a_nul_in_a_pool_string_is_refused(void)
{
    // Act / Assert
    ASSERT_FALSE(grant_validates_with("l", "a", "u\\u0000x", "p"));
    ASSERT_FALSE(grant_validates_with("l", "a", "u", "p\\u0000x"));
}

static void renewal_rules_apply(void)
{
    // Arrange
    const char *valid = "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"l\",\"authorization\":\"a\","
                        "\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":20000}";
    const char *refused_shape = "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"l\",\"authorization\":\"a\","
                                "\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":20000,\"challengeId\":\"c\"}";
    const char *refused_value = "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"l\",\"authorization\":\"a\","
                                "\"durationMilliseconds\":60000,\"renewAfterMilliseconds\":60000}";
    yyjson_doc *a = yyjson_read(valid, strlen(valid), 0);
    yyjson_doc *b = yyjson_read(refused_shape, strlen(refused_shape), 0);
    yyjson_doc *c = yyjson_read(refused_value, strlen(refused_value), 0);
    bwg_lease_renewal_t renewal;

    // Act
    bool valid_ok = bwg_lease_renewal_parse(yyjson_doc_get_root(a), &renewal) && bwg_lease_renewal_validate(&renewal);
    bool shape_ok = bwg_lease_renewal_parse(yyjson_doc_get_root(b), &renewal);
    bool value_parsed = bwg_lease_renewal_parse(yyjson_doc_get_root(c), &renewal);
    bool value_ok = value_parsed && bwg_lease_renewal_validate(&renewal);
    yyjson_doc_free(a);
    yyjson_doc_free(b);
    yyjson_doc_free(c);

    // Assert
    ASSERT_TRUE(valid_ok);
    ASSERT_FALSE(shape_ok);
    ASSERT_TRUE(value_parsed);
    ASSERT_FALSE(value_ok);
}

void bwg_tests_lease(void)
{
    BWG_RUN(plain_v1_grant_parses_and_validates);
    BWG_RUN(stratum_shapes_are_closed);
    BWG_RUN(explicit_zero_difficulty_is_kept);
    BWG_RUN(a_v2_grant_with_private_endpoint_validates);
    BWG_RUN(a_v2_grant_refuses_public_or_named_endpoints);
    BWG_RUN(a_v2_grant_needs_a_canonical_authority_key);
    BWG_RUN(a_v2_grant_bounds_its_user_identity);
    BWG_RUN(a_malformed_allowance_fails_at_parse);
    BWG_RUN(an_impossible_allowance_fails_validation);
    BWG_RUN(each_allowance_kind_validates);
    BWG_RUN(two_allowances_never_validate);
    BWG_RUN(unknown_grant_fields_are_refused);
    BWG_RUN(window_limits_are_exact);
    BWG_RUN(endpoints_follow_the_exact_rule);
    BWG_RUN(credentials_and_identifiers_are_bounded);
    BWG_RUN(a_nul_in_a_pool_string_is_refused);
    BWG_RUN(renewal_rules_apply);
}
