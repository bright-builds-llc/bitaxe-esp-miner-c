#ifndef BWG_LEASE_AUTH_H
#define BWG_LEASE_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"
#include "bwg_codec.h"
#include "bwg_lease.h"
#include "bwg_trust.h"

typedef enum {
    BWG_AUTH_ACCEPTED = 0,
    BWG_AUTH_INVALID,
    BWG_AUTH_REPLAY,
    BWG_AUTH_PERSISTENCE,
} bwg_auth_result_t;

typedef enum {
    BWG_STORE_COMMITTED = 0,
    /** The stored value was not `expected`. */
    BWG_STORE_STALE,
    /** `next` is already stored. */
    BWG_STORE_ALREADY_COMMITTED,
    BWG_STORE_ERROR,
} bwg_store_result_t;

typedef enum {
    /** Rejected before the signature check: malformed, unknown key or a changed request. */
    BWG_AUTH_SIGNATURE_NOT_EVALUATED = 0,
    BWG_AUTH_SIGNATURE_VALID,
    BWG_AUTH_SIGNATURE_INVALID,
} bwg_auth_signature_t;

typedef enum {
    /** The request presented no binding equal to the current admission. */
    BWG_AUTH_CONTEXT_MISMATCH = 0,
    BWG_AUTH_CONTEXT_CURRENT,
    /** An admission of this generation exists but is at least 60 s old. */
    BWG_AUTH_CONTEXT_EXPIRED,
    /** No admission of this generation exists. */
    BWG_AUTH_CONTEXT_ABSENT,
} bwg_auth_context_t;

typedef enum {
    BWG_AUTH_REPLAY_NOT_EVALUATED = 0,
    BWG_AUTH_REPLAY_FRESH,
    BWG_AUTH_REPLAY_AT_OR_BELOW,
    BWG_AUTH_REPLAY_UNAVAILABLE,
} bwg_auth_replay_guard_t;

/** What a verification found, for the rejection review. All zero is "nothing evaluated". */
typedef struct {
    bwg_auth_signature_t signature;
    bwg_auth_context_t context;
    bwg_auth_replay_guard_t replay_guard;
} bwg_auth_attribution_t;

/** Durable per-key high water. Compare-and-store must be one atomic transaction, verified before it returns. */
typedef struct {
    void *ctx;
    /** Reads the high water for `kid`. False means the store is corrupt or unavailable. */
    bool (*load)(void *ctx, const char *kid, bool *present, uint64_t *value);
    bwg_store_result_t (*compare_and_store)(void *ctx, const char *kid, bool has_expected, uint64_t expected,
                                            uint64_t next);
} bwg_sequence_store_t;

/**
 * Verifies a Start authorization, in the protocol's order, against the control-session `binding`.
 * Only BWG_AUTH_ACCEPTED advances the sequence, and it has been committed durably when this
 * returns, so commit precedes any effect. Running out of memory is BWG_AUTH_INVALID with no state
 * change. Needs about 3 KiB of stack inside libsodium and yyjson's allocator for its working space.
 * `maybe_attribution`, when given, receives what the check found, which only a rejection needs.
 */
bwg_auth_result_t bwg_lease_auth_verify_start(const bwg_allocator_t *allocator, const bwg_lease_trust_t *trust,
                                              const bwg_sequence_store_t *store, const bwg_lease_grant_t *grant,
                                              const char *binding, bwg_auth_attribution_t *maybe_attribution);

/** Likewise for a Renew. `active_challenge_id` is the active lease's challenge. */
bwg_auth_result_t bwg_lease_auth_verify_renew(const bwg_allocator_t *allocator, const bwg_lease_trust_t *trust,
                                              const bwg_sequence_store_t *store, const bwg_lease_renewal_t *renewal,
                                              const char *active_challenge_id, const char *binding,
                                              bwg_auth_attribution_t *maybe_attribution);

/** The `requestSha256` a Start authorization must claim. False if the input cannot be written canonically. */
bool bwg_lease_auth_start_digest(const bwg_lease_trust_t *trust, const bwg_lease_grant_t *grant,
                                 char digest[BWG_DIGEST_TEXT_SIZE]);

/** The `requestSha256` a Renew authorization must claim. */
bool bwg_lease_auth_renew_digest(const bwg_lease_trust_t *trust, const bwg_lease_renewal_t *renewal,
                                 const char *active_challenge_id, char digest[BWG_DIGEST_TEXT_SIZE]);

#endif // BWG_LEASE_AUTH_H
