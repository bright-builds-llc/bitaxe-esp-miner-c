#ifndef BWG_TRUST_H
#define BWG_TRUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"

#define BWG_TRUST_MAX_KEYS 8u
#define BWG_TRUST_KID_MAX_LEN 32u
#define BWG_TRUST_ISSUER_MAX_LEN 128u

typedef struct {
    char kid[BWG_TRUST_KID_MAX_LEN + 1];
    uint8_t public_key[32];
} bwg_trust_key_t;

/** The Work Lease half of the deployment trust. Self-contained: it keeps nothing from the input text. */
typedef struct {
    char issuer[BWG_TRUST_ISSUER_MAX_LEN + 1];
    size_t key_count;
    bwg_trust_key_t keys[BWG_TRUST_MAX_KEYS];
} bwg_lease_trust_t;

/**
 * Parses a whole deployment trust document strictly: exact fields at every level, profiles, roles,
 * audiences, issuer labels, 1 to 8 keys per role, and for each key a canonical encoding of a valid
 * point in the main subgroup. No key ID or key may repeat, within a role or across the two roles.
 * False must disable leases. Allocates only while parsing.
 */
bool bwg_trust_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, bwg_lease_trust_t *trust);

/** Same checks for a standalone `workLeaseAuthority` object, as the conformance vectors carry. */
bool bwg_trust_parse_lease_half(const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                                bwg_lease_trust_t *trust);

/** The key with this ID, or NULL. */
const bwg_trust_key_t *bwg_trust_maybe_find_key(const bwg_lease_trust_t *trust, const char *kid, size_t kid_len);

#endif // BWG_TRUST_H
