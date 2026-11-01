#ifndef BWG_REVOCATION_H
#define BWG_REVOCATION_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_authority.h"
#include "sdkconfig.h"

#define BWG_HEARTBEAT_CUTOFF_MS 2800
#define BWG_LEASE_MAX_MS 60000
/** The most active time a budget may allow: the whole campaign, or exactly one soak. */
#define BWG_ACTIVE_LIMIT_MAX_MS 240000
#define BWG_ACTIVE_LIMIT_SOAK_MS 600000

typedef enum {
    BWG_STOP_REASON_NONE = 0,
    BWG_STOP_REASON_HEARTBEAT_TIMEOUT,
    BWG_STOP_REASON_LEASE_EXPIRED,
    BWG_STOP_REASON_LINK_CLOSED,
    BWG_STOP_REASON_CONTROL_FAILED,
    BWG_STOP_REASON_UNSAFE_OBSERVATION,
    BWG_STOP_REASON_LOCAL_STOP,
    BWG_STOP_REASON_LOCAL_HANDOVER,
    BWG_STOP_REASON_RESTORATION_REQUESTED,
} bwg_stop_reason_t;

/** Timings in ms since boot, 0 when not yet recorded. Holds no pool or device values. */
typedef struct {
    bwg_authority_state_t state;
    bwg_stop_reason_t reason;
    uint32_t last_heartbeat_ms;
    uint32_t closed_ms;
    // How the last lease ended. A later local handover moves closed_ms and stop_started_ms but not these.
    uint32_t lease_closed_ms;
    uint32_t lease_stop_started_ms;
    bwg_stop_reason_t lease_reason;
    uint32_t stop_started_ms;
    uint32_t stopped_ms;
    uint32_t last_work_ms;
    uint32_t last_share_ms;
    uint32_t refused_work;
    uint32_t refused_shares;
    // The current or last lease, reset when one begins. Local mining counts toward none of these.
    uint32_t activated_ms;
    uint32_t first_work_ms;
    uint32_t admitted_work;
    uint32_t admitted_shares;
    uint32_t nonces;
    uint32_t active_limit_ms;
    uint32_t budget_deadline_ms;
} bwg_revocation_snapshot_t;

/** Whether a job may go to the ASIC now. Called from the single dispatch point. */
bool bwg_revocation_admit_work(void);

/** Whether a share may go to the pool now. Called from the single submission point. */
bool bwg_revocation_admit_share(void);

/** Counts a nonce the chip returned for a job it was given. */
void bwg_revocation_note_nonce(void);

/** Closes authority from LOCAL or LEASED. The first reason stays; later calls do nothing. */
void bwg_revocation_revoke(bwg_stop_reason_t reason);

/** As bwg_revocation_revoke, but does nothing outside a lease, so local mining is unaffected. */
void bwg_revocation_revoke_lease(bwg_stop_reason_t reason);

bool bwg_revocation_stop_wanted(void);
void bwg_revocation_note_stop_started(void);

/** Reports that the ASIC is stopped and held. Authority can reopen only after this. */
void bwg_revocation_note_stopped(void);

/** Opens authority for a lease from STOPPED and starts both deadlines. */
bool bwg_revocation_begin_lease(uint32_t duration_ms);

/**
 * The same, with a budget of active time: 0 means none. It starts at the first work admitted, is
 * independent of the heartbeat, and no renewal extends it. Its end closes the lease as an expiry.
 */
bool bwg_revocation_begin_limited_lease(uint32_t duration_ms, uint32_t active_limit_ms);

/** Restarts the heartbeat deadline only. A heartbeat never extends a lease. */
bool bwg_revocation_feed_heartbeat(void);

/** The same, for a heartbeat observed earlier, in milliseconds of `esp_timer_get_time`. */
bool bwg_revocation_feed_heartbeat_at(uint32_t observed_ms);

/** Restarts the lease deadline. Only a verified renewal may call this. */
bool bwg_revocation_renew_lease(uint32_t duration_ms);

/** Returns from STOPPED to upstream's own mining. */
bool bwg_revocation_release_to_local(void);

bwg_revocation_snapshot_t bwg_revocation_snapshot(void);

/** State and reason as one word, readable on the fatal path with the flash caches off. */
uint32_t bwg_revocation_panic_word(void);

const char *bwg_stop_reason_label(bwg_stop_reason_t reason);

#ifdef CONFIG_BWG_REVOCATION_PROOF
void bwg_revocation_proof_start(void);
#endif

#endif // BWG_REVOCATION_H
