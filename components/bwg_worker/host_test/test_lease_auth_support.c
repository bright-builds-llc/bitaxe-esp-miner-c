#include "test_lease_auth_support.h"

#include <stdio.h>
#include <stdlib.h>

#include "bwg_json_writer.h"
#include "bwg_lease_auth.h"
#include "bwg_test.h"
#include "test_jws.h"
#include "test_suites.h"

static bool stub_load(void *ctx, const char *kid, bool *present, uint64_t *value)
{
    (void) kid;
    store_stub_t *stub = ctx;
    if (stub->fail_load) {
        return false;
    }
    *present = stub->present;
    *value = stub->value;
    return true;
}

static bwg_store_result_t stub_store(void *ctx, const char *kid, bool has_expected, uint64_t expected, uint64_t next)
{
    (void) kid;
    store_stub_t *stub = ctx;
    if (stub->has_forced_result) {
        return stub->forced_result;
    }
    if (has_expected != stub->present || (has_expected && expected != stub->value)) {
        return BWG_STORE_STALE;
    }
    stub->present = true;
    stub->value = next;
    stub->stores++;
    return BWG_STORE_COMMITTED;
}

bwg_sequence_store_t auth_store_of(store_stub_t *stub)
{
    return (bwg_sequence_store_t) {.ctx = stub, .load = stub_load, .compare_and_store = stub_store};
}

bool auth_load_trust(bwg_lease_trust_t *trust)
{
    size_t len = 0;
    char *text = bwg_test_read_fixture("deployment-trust/trust.json", &len);
    bool ok = bwg_trust_parse(&bwg_test_allocator, (const uint8_t *) text, len, trust);
    bwg_test_free_fixture(text);
    return ok;
}

bool auth_rfc8032_trust(bwg_lease_trust_t *trust)
{
    char text[1024];
    bwg_test_lease_half_text(BWG_TEST_RFC8032_SEED, "rfc8032", "fixture-qualification-attempt", text, sizeof(text));
    return bwg_trust_parse_lease_half(&bwg_test_allocator, (const uint8_t *) text, strlen(text), trust);
}

/** The compact authorization string of a fixture file. Static storage: one at a time per name. */
const char *auth_fixture_compact(const char *path)
{
    static char copy[2][1024];
    static int next = 0;
    yyjson_doc *doc = bwg_test_fixture_doc(path);
    char *slot = copy[next++ % 2];
    strcpy(slot, yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "compactJws")));
    yyjson_doc_free(doc);
    return slot;
}

/** Parses `request` from a fixture with its authorization added, by text. Caller frees the doc. */
yyjson_doc *auth_doc_with_authorization(yyjson_val *request, const char *authorization)
{
    size_t len = 0;
    char *line = bwg_test_json_line(request, &len);
    size_t capacity = len + strlen(authorization) + 64;
    char *text = malloc(capacity);
    // Drop `}` and the LF, then append the field.
    snprintf(text, capacity, "%.*s,\"authorization\":\"%s\"}", (int) (len - 2), line, authorization);
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    free(line);
    free(text);
    return doc;
}

bool auth_start_case_open(start_case_t *c, const char *authorization)
{
    c->fixture = bwg_test_fixture_doc("deployment-trust/start-input.json");
    c->doc = auth_doc_with_authorization(yyjson_obj_get(yyjson_doc_get_root(c->fixture), "request"), authorization);
    return bwg_lease_grant_parse(yyjson_doc_get_root(c->doc), &c->grant);
}

void auth_start_case_close(start_case_t *c)
{
    yyjson_doc_free(c->doc);
    yyjson_doc_free(c->fixture);
}

bwg_auth_result_t auth_run_start(const bwg_lease_trust_t *trust, store_stub_t *stub, const char *authorization,
                                   const char *binding)
{
    start_case_t c;
    if (!auth_start_case_open(&c, authorization)) {
        auth_start_case_close(&c);
        return BWG_AUTH_INVALID;
    }
    const bwg_sequence_store_t store = auth_store_of(stub);
    bwg_auth_result_t result = bwg_lease_auth_verify_start(&bwg_test_allocator, trust, &store, &c.grant, binding, NULL);
    auth_start_case_close(&c);
    return result;
}

bwg_auth_result_t auth_run_renew(const bwg_lease_trust_t *trust, store_stub_t *stub, const char *authorization,
                                   const char *binding)
{
    yyjson_doc *fixture = bwg_test_fixture_doc("deployment-trust/renew-input.json");
    yyjson_doc *doc = auth_doc_with_authorization(yyjson_obj_get(yyjson_doc_get_root(fixture), "request"), authorization);
    bwg_lease_renewal_t renewal;
    bwg_auth_result_t result = BWG_AUTH_INVALID;
    if (bwg_lease_renewal_parse(yyjson_doc_get_root(doc), &renewal)) {
        const bwg_sequence_store_t store = auth_store_of(stub);
        result = bwg_lease_auth_verify_renew(&bwg_test_allocator, trust, &store, &renewal, CHALLENGE, binding, NULL);
    }
    yyjson_doc_free(doc);
    yyjson_doc_free(fixture);
    return result;
}
