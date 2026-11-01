#ifndef TEST_LEASE_AUTH_SUPPORT_H
#define TEST_LEASE_AUTH_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>

#include "bwg_json_writer.h"
#include "bwg_lease_auth.h"
#include "bwg_test.h"
#include "test_jws.h"
#include "test_suites.h"

#define BINDING "jEc1ro90lL2fX0ZozUbY0XpmxrC46gEkEUKKTyGj0Zs"

#define OTHER_BINDING "TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"

#define CHALLENGE "challenge_00000000000000000000000000000001"

#define AAAA_BINDING "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

typedef struct {
    bool present;
    uint64_t value;
    bool fail_load;
    bool has_forced_result;
    bwg_store_result_t forced_result;
    int stores;
} store_stub_t;

typedef struct {
    yyjson_doc *fixture;
    yyjson_doc *doc;
    bwg_lease_grant_t grant;
} start_case_t;

bwg_sequence_store_t auth_store_of(store_stub_t *stub);
bool auth_load_trust(bwg_lease_trust_t *trust);
bool auth_rfc8032_trust(bwg_lease_trust_t *trust);
const char *auth_fixture_compact(const char *path);
yyjson_doc *auth_doc_with_authorization(yyjson_val *request, const char *authorization);
bool auth_start_case_open(start_case_t *c, const char *authorization);
void auth_start_case_close(start_case_t *c);
bwg_auth_result_t auth_run_start(const bwg_lease_trust_t *trust, store_stub_t *stub, const char *authorization,
                                   const char *binding);
bwg_auth_result_t auth_run_renew(const bwg_lease_trust_t *trust, store_stub_t *stub, const char *authorization,
                                   const char *binding);

#endif // TEST_LEASE_AUTH_SUPPORT_H
