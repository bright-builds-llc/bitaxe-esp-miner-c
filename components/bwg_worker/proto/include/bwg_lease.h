#ifndef BWG_LEASE_H
#define BWG_LEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

#include "bwg_allowance.h"
#include "bwg_json.h"
#include "bwg_json_writer.h"

#define BWG_CONTROLLER_PROTOCOL "bwg-worker-controller/0.4"
#define BWG_LEASE_MAX_SECRET_BYTES 512u
#define BWG_LEASE_MAX_DURATION_MS 60000u
#define BWG_LEASE_MAX_RENEW_AFTER_MS 20000u
#define BWG_STRATUM_V2_AUTHORITY_KEY_BYTES 32u
#define BWG_STRATUM_V2_MAX_USER_BYTES 255u

typedef enum {
    BWG_STRATUM_V1 = 0,
    BWG_STRATUM_V2,
} bwg_stratum_kind_t;

/**
 * A Start input, shape-checked, borrowing from its parsed document, so it holds the same secrets
 * that document does. `maybe_stratum_v2` and the three `maybe_` objects exist so the signing input
 * can include them. `allowance` is the budget, kind NONE for an unlimited lease.
 */
typedef struct {
    bwg_str_t protocol_version;
    bwg_str_t lease_id;
    bwg_str_t challenge_id;
    bwg_str_t authorization;
    uint64_t duration_ms;
    uint64_t renew_after_ms;
    bwg_stratum_kind_t stratum_kind;
    bwg_str_t endpoint;
    bwg_str_t username;
    bwg_str_t password;
    bool has_suggested_difficulty;
    uint16_t suggested_difficulty;
    bwg_str_t authority_public_key;
    bwg_str_t user_identity;
    yyjson_val *maybe_stratum_v2;
    yyjson_val *maybe_acceptance_campaign;
    yyjson_val *maybe_qualification_attempt;
    yyjson_val *maybe_soak_allowance;
    bwg_allowance_t allowance;
} bwg_lease_grant_t;

/** A Renew input, likewise. */
typedef struct {
    bwg_str_t protocol_version;
    bwg_str_t lease_id;
    bwg_str_t authorization;
    uint64_t duration_ms;
    uint64_t renew_after_ms;
} bwg_lease_renewal_t;

/**
 * Exact fields and types only. False is `invalid_request`. Stratum shapes are closed: a null
 * `suggestedDifficulty`, an unknown or null `profile`, or mixed V1 and V2 fields all return false.
 * So are the allowances: a malformed one returns false here, before admission is considered.
 */
bool bwg_lease_grant_parse(yyjson_val *payload, bwg_lease_grant_t *grant);

/**
 * The value rules: protocol, identifiers, window, endpoint, credential lengths, and at most one
 * valid allowance. A soak must use the exact 60,000/20,000 window. A NUL in a pool string returns
 * false too, since the device passes these as C strings. A V2 grant needs a private literal IPv4
 * endpoint, a canonical 32-byte authority key and a user identity of 1 to 255 bytes.
 * False is `invalid_request`.
 */
bool bwg_lease_grant_validate(const bwg_lease_grant_t *grant);

/** Exact fields and types only. False is `invalid_request`. */
bool bwg_lease_renewal_parse(yyjson_val *payload, bwg_lease_renewal_t *renewal);

/** The value rules. False is `invalid_request`. */
bool bwg_lease_renewal_validate(const bwg_lease_renewal_t *renewal);

/**
 * Splits `stratum+tcp://host:port/`. Refuses `@`, `?`, `#` or `/` in the authority, an empty host,
 * and a port that is zero, over 65535 or not in its shortest form.
 */
bool bwg_stratum_endpoint_parse(bwg_str_t endpoint, bwg_str_t *host, uint16_t *port);

/**
 * The closed V2 value, for a grant and for a channel start: profile, endpoint, authority key and user
 * identity, with no other field. Borrows from its document.
 */
bool bwg_stratum_v2_parse(yyjson_val *stratum, bwg_str_t *endpoint, bwg_str_t *authority_public_key, bwg_str_t *user_identity);

/** The V2 value rules: a private literal endpoint, a canonical key and a bounded user identity. */
bool bwg_stratum_v2_is_valid(bwg_str_t endpoint, bwg_str_t authority_public_key, bwg_str_t user_identity);

/** Whether `text` is an RFC 1918 IPv4 literal in its one canonical spelling. */
bool bwg_private_ipv4_parse(bwg_str_t text, uint8_t octets[4]);

/** Writes the Start signing request: everything but `authorization`, with sorted keys at every level. */
void bwg_lease_write_start_request(bwg_jw_t *writer, const bwg_lease_grant_t *grant);

/** Writes the Renew signing request. */
void bwg_lease_write_renew_request(bwg_jw_t *writer, const bwg_lease_renewal_t *renewal);

#endif // BWG_LEASE_H
