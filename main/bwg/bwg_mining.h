#ifndef BWG_MINING_H
#define BWG_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_lease.h"
#include "bwg_limits.h"
#include "bwg_revocation.h"
#include "global_state.h"

/**
 * A lease's pool. The strings are copied into RAM and that copy is wiped when the lease
 * ends. The pool client's working copies on its stack and heap are released unwiped; nothing reads them.
 */
typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    // Zero sends no hint and never consults the saved setting.
    uint16_t suggested_difficulty;
    // V2 has an authority key where V1 has a password. Null means V1.
    const uint8_t *maybe_authority_key;
} bwg_lease_pool_t;

void bwg_mining_init(GlobalState *GLOBAL_STATE);

/**
 * Opens authority for a lease and waits for the chip and the pool's first job, so the reply can't
 * claim work that never started. False means the lease didn't start and the caller must stop.
 */
bool bwg_mining_start(const bwg_lease_pool_t *pool, uint32_t duration_ms, uint32_t active_limit_ms);

/** Silences what the pool tags log, as a lease does, for a channel that reaches a pool without one. */
void bwg_mining_quiet_pool_logs(bool quiet);

/** Copies a V2 lease's authority key. False when no lease has one, and the pool's own setting applies. */
bool bwg_mining_lease_authority_key(uint8_t key[BWG_STRATUM_V2_AUTHORITY_KEY_BYTES]);

/** The device's own refusals of a lease, checked before a budget is charged for it. */
bool bwg_mining_may_start(void);

/** Restarts the lease deadline. Only a verified renewal may call this. */
bool bwg_mining_renew(uint32_t duration_ms);

/** Closes authority if open, waits for the ordered stop to finish, and wipes the grant's pool. */
bool bwg_mining_safe_stop(bwg_stop_reason_t reason);

/** Returns a stopped device to upstream's own mining, with no pool failure left over from before. */
bool bwg_mining_release_to_local(void);

/** Whether an operator's pause holds, or will return when a lease gives the device back. */
bool bwg_mining_operator_paused(const GlobalState *GLOBAL_STATE);

/** Why the device paused itself, or NULL. An operator's own pause has no reason. */
const char *bwg_mining_maybe_pause_reason(const GlobalState *GLOBAL_STATE);

/** One of paused, active, safe_blocked or stopping. */
const char *bwg_mining_activity(const GlobalState *GLOBAL_STATE);

/** The pool the stratum task should use: the grant's during a lease, otherwise `saved`. */
PoolConfig *bwg_mining_pool_or(PoolConfig *saved);

/**
 * Whether the pool has to wait for the chip. The job task applies a job's version mask only to a
 * running chip, so a lease's first job has to arrive after the chip starts or it would lose it.
 */
bool bwg_mining_holds_pool(const GlobalState *GLOBAL_STATE);

/** Whether a lease's pool is in force, which rules out any fallback. */
bool bwg_mining_lease_pool_active(void);

/** Records this poll's sample, and revokes a lease that has had no safe one for a second. Called from the power task's loop. */
void bwg_mining_check_limits(GlobalState *GLOBAL_STATE);

/** Upstream's overheat check tripped during a lease, on the chip or else the regulator, at this reading. */
void bwg_mining_end_lease_on_overheat(bool chip, float celsius);

/**
 * Shares the pool answered during the current or last lease, across its connections. The session's
 * own counters go back to zero when a connection is dropped, and the Gate reads these after the stop.
 * Power task only, and the last call belongs before the stop drops the pool.
 */
void bwg_mining_latch_shares(const GlobalState *GLOBAL_STATE);
void bwg_mining_lease_shares(uint32_t *accepted, uint32_t *rejected);

/** The latest sample and its age in ms. False before the first one. */
bool bwg_mining_latest_sample(bwg_safety_sample_t *sample, uint64_t *age_ms);

void bwg_mining_note_sample(const bwg_safety_sample_t *sample);

/** Whether the board is paused with its chip already off, so a handover has nothing to stop. */
bool bwg_mining_local_is_stopped(void);

/** The oldest sample that still describes the board now. */
#define BWG_SAMPLE_FRESH_MS 1000u

#endif // BWG_MINING_H
