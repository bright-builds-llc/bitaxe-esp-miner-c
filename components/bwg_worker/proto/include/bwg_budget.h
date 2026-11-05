#ifndef BWG_BUDGET_H
#define BWG_BUDGET_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_ledger.h"

#define BWG_BUDGET_KEY_ATTEMPTS "qual_attempts"
#define BWG_BUDGET_KEY_CAMPAIGN "accept_budget"
#define BWG_BUDGET_KEY_SOAK "soak_ledger2"

typedef enum {
    BWG_BUDGET_LOAD_ABSENT = 0,
    BWG_BUDGET_LOAD_OK,
    BWG_BUDGET_LOAD_FAILED,
} bwg_budget_load_t;

/** Where ledgers live. A store commits before it returns. */
typedef struct {
    void *ctx;
    bwg_budget_load_t (*load)(void *ctx, const char *key, uint8_t *out, size_t capacity, size_t *len);
    bool (*store)(void *ctx, const char *key, const uint8_t *data, size_t len);
} bwg_budget_store_t;

/** What a soak needs of the acceptance campaign, which differs by board. */
typedef enum {
    BWG_SOAK_NEEDS_COMPLETE_CAMPAIGN = 0,
    BWG_SOAK_NEEDS_NO_UNFINISHED_CAMPAIGN,
} bwg_soak_rule_t;

/**
 * The device's work budgets over one store. Owner task only, except the two cached acceptance
 * values, which a diagnostic line reads without touching flash.
 */
typedef struct {
    const bwg_allocator_t *allocator;
    bwg_budget_store_t store;
    bwg_soak_rule_t soak_rule;
    bool soak_recovery_failed;

    bwg_allowance_kind_t pending_kind;
    uint32_t pending_generation;
    bool campaign_had_previous;
    bwg_campaign_t campaign_previous;
    bwg_campaign_t campaign_reserved;
    bwg_ledger_t ledger_previous;
    bwg_ledger_t ledger_reserved;

    bwg_allowance_kind_t observed_kind;
    uint32_t observed_generation;
    uint32_t observed_ordinal;
    bwg_attempt_purpose_t observed_purpose;
    uint32_t observed_maximum_ms;
    bool observed_complete;

    _Atomic uint32_t campaign_reserved_ms;
    _Atomic bool campaign_complete;
} bwg_budget_t;

void bwg_budget_init(bwg_budget_t *budget, const bwg_allocator_t *allocator, const bwg_budget_store_t *store,
                     bwg_soak_rule_t soak_rule);

/**
 * Completes whatever an interrupted boot left pending, with no refund, and loads the cached values.
 * A soak ledger that can't be recovered only refuses soaks. False means another ledger can't be, and
 * the device must not take part in the Gate.
 */
bool bwg_budget_recover_boot(bwg_budget_t *budget);

/**
 * Charges the lease's budget before any work and gives the active limit, with 0 for an unlimited
 * lease. False refuses the lease and sets `*refusal` to a closed label. A failed write refuses too,
 * and stays pending, so bwg_budget_finish still charges it.
 */
bool bwg_budget_admit(bwg_budget_t *budget, uint32_t generation, const bwg_allowance_t *allowance, uint32_t *limit_ms,
                      const char **refusal);

/** Completes this generation's reservation durably. True when nothing is pending. A failure retries on the next call. */
bool bwg_budget_finish(bwg_budget_t *budget, uint32_t generation);

/** Writes a read-only review. False means the ledger is unreadable or invalid. It never creates a key. */
bool bwg_budget_write_ledger_review(bwg_budget_t *budget, bwg_allowance_kind_t kind, bwg_jw_t *writer);
bool bwg_budget_write_campaign_review(bwg_budget_t *budget, bwg_str_t campaign_id, bwg_jw_t *writer);

/** Writes the `attempt` or `soak` key of this generation's status, if it has one. */
void bwg_budget_write_observation(const bwg_budget_t *budget, uint32_t generation, uint32_t active_ms, bwg_jw_t *writer);

uint32_t bwg_budget_campaign_reserved_ms(const bwg_budget_t *budget);
bool bwg_budget_campaign_complete(const bwg_budget_t *budget);

/** A lease's active time and what remains of its budget, from one set of clock readings. */
typedef struct {
    uint32_t active_ms;
    bool has_remaining;
    uint32_t remaining_ms;
} bwg_budget_timing_t;

/** A clock reading as a stamp, which is never 0: 0 means unset, so a reading taken as the clock wraps counts 1 ms later. */
uint32_t bwg_budget_stamp(uint32_t clock_ms);

/**
 * `first_work_ms` and `closed_ms` are 0 when unset, and clocks wrap, so times compare by signed
 * difference. A `limit_ms` of 0 means none. Active time never reads past the limit.
 */
bwg_budget_timing_t bwg_budget_timing(uint32_t first_work_ms, uint32_t closed_ms, uint32_t limit_ms, uint32_t deadline_ms,
                                      uint32_t now_ms);

#endif // BWG_BUDGET_H
