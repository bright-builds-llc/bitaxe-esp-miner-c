#ifndef BWG_REVIEW_H
#define BWG_REVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_lease_auth.h"
#include "bwg_reason.h"

/** How far below the last observed clock the one stimulated sample lands. */
#define BWG_CLOCK_STIMULUS_OFFSET_MS 1000u
/** How long after its reply was delivered a tick may consume the stimulus. */
#define BWG_CLOCK_STIMULUS_ARMED_MS 2000u

typedef enum {
    BWG_AUTH_OPERATION_START = 0,
    BWG_AUTH_OPERATION_RENEW,
} bwg_auth_operation_t;

/** One rejection's metadata. Key IDs, sequences, lease IDs and authorization bytes are never kept. */
typedef struct {
    uint32_t ordinal;
    bwg_auth_operation_t operation;
    bwg_auth_attribution_t attribution;
    bool has_safe_stop;
    bwg_reason_t safe_stop;
} bwg_rejection_record_t;

/** Attribution of this boot's Start and Renew rejections. RAM only, so a reboot clears it. */
typedef struct {
    uint32_t boot_rejections;
    bool has_last;
    bwg_rejection_record_t last;
    bool last_awaits_safe_stop;
    bool high_water_advanced;
} bwg_rejection_log_t;

void bwg_rejection_log_record(bwg_rejection_log_t *log, bwg_auth_operation_t operation,
                              const bwg_auth_attribution_t *attribution);
/** Gives the rejection just recorded the stop it caused. Only once, so a later stop can't rewrite it. */
void bwg_rejection_log_attribute_stop(bwg_rejection_log_t *log, bwg_reason_t reason);
void bwg_rejection_log_mark_advanced(bwg_rejection_log_t *log);

const char *bwg_auth_operation_label(bwg_auth_operation_t operation);
const char *bwg_auth_signature_label(bwg_auth_signature_t signature);
const char *bwg_auth_context_label(bwg_auth_context_t context);
const char *bwg_auth_replay_guard_label(bwg_auth_replay_guard_t replay_guard);

typedef enum {
    BWG_CLOCK_STIMULUS_IDLE = 0,
    BWG_CLOCK_STIMULUS_ARMED,
    BWG_CLOCK_STIMULUS_CONSUMED,
    BWG_CLOCK_STIMULUS_EXPIRED,
} bwg_clock_stimulus_state_t;

/**
 * Makes one controller tick see a clock that went backwards, so the real decreasing-clock path runs.
 * Leaving IDLE can't be undone, which makes it once per boot. The heartbeat, link and lease deadlines
 * read the real clock and never see it.
 */
typedef struct {
    bwg_clock_stimulus_state_t state;
    uint64_t expires_ms;
    bool has_pending_token;
    uint64_t pending_token;
} bwg_clock_stimulus_t;

/** The clock one tick should observe. Unchanged unless armed. */
uint64_t bwg_clock_stimulus_sample(bwg_clock_stimulus_t *stimulus, uint64_t real_now_ms, bool has_last, uint64_t last_ms);
/** True only before this boot's one use and with no unconfirmed reply. */
bool bwg_clock_stimulus_can_prepare(const bwg_clock_stimulus_t *stimulus);
void bwg_clock_stimulus_prepare(bwg_clock_stimulus_t *stimulus, uint64_t token);
/** Takes the unconfirmed reply's token. False when there is none. */
bool bwg_clock_stimulus_take_pending(bwg_clock_stimulus_t *stimulus, uint64_t *token);
void bwg_clock_stimulus_clear_pending(bwg_clock_stimulus_t *stimulus);
/** Arms once the reply was delivered. False unless still idle. */
bool bwg_clock_stimulus_arm(bwg_clock_stimulus_t *stimulus, uint64_t real_now_ms);
/** Any safe stop ends the armed window. This boot's one use stays spent. */
void bwg_clock_stimulus_cancel(bwg_clock_stimulus_t *stimulus);
const char *bwg_clock_stimulus_label(const bwg_clock_stimulus_t *stimulus);

#endif // BWG_REVIEW_H
