#ifndef BWG_LEDGER_H
#define BWG_LEDGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"
#include "bwg_allowance.h"
#include "bwg_json_writer.h"

/** The most a stored ledger may hold. The longest real one is under 300 bytes. */
#define BWG_LEDGER_MAX_BYTES 512u

/**
 * The attempt and soak ledgers, which share a shape. A charge is the whole allowance, taken at
 * reservation, and nothing refunds it. Two values compare equal by bwg_ledger_equal only.
 */
typedef struct {
    uint32_t highest_ordinal;
    uint64_t total_charged_ms;
    bool has_last;
    bool last_complete;
    char last_id[BWG_ALLOWANCE_ID_SIZE];
    uint32_t last_ordinal;
    bwg_attempt_purpose_t last_purpose;
    uint64_t last_maximum_ms;
} bwg_ledger_t;

/** The one acceptance campaign a device ever runs: three windows under a single identifier. */
typedef struct {
    char campaign_id[BWG_ALLOWANCE_ID_SIZE];
    uint8_t reserved;
    uint8_t completed;
    uint32_t charged_ms;
    bool has_running;
    uint8_t running_window;
} bwg_campaign_t;

/** `kind` is ATTEMPT or SOAK throughout. */
void bwg_ledger_default(bwg_ledger_t *ledger);
bool bwg_ledger_validate(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger);
/** Accepts any key order, and refuses an unknown key, a repeated key or a ledger that fails to validate. */
bool bwg_ledger_parse(bwg_allowance_kind_t kind, const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                      bwg_ledger_t *ledger);
/** Writes compact JSON in the stored order, which is not sorted. False when it does not fit. */
bool bwg_ledger_serialize(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger, uint8_t *out, size_t capacity,
                          size_t *len);
/** Charges the next contiguous ordinal. Refuses a pending reservation, a gap, a replay or overflow. */
bool bwg_ledger_reserve(bwg_allowance_kind_t kind, bwg_ledger_t *ledger, const bwg_allowance_t *allowance);
/** Completes the reservation, keeping its charge. A ledger with nothing pending is unchanged. */
void bwg_ledger_finish(bwg_ledger_t *ledger);
bool bwg_ledger_is_pending(const bwg_ledger_t *ledger);
bool bwg_ledger_equal(const bwg_ledger_t *a, const bwg_ledger_t *b);
/** Writes the `qualification_attempt_review` or `soak_allowance_review` result. */
void bwg_ledger_write_review(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger, bwg_jw_t *writer);

bool bwg_campaign_validate(const bwg_campaign_t *campaign);
bool bwg_campaign_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, bwg_campaign_t *campaign);
bool bwg_campaign_serialize(const bwg_campaign_t *campaign, uint8_t *out, size_t capacity, size_t *len);
/** `is_new` starts a ledger for this identifier. Otherwise the identifier must match the stored one. */
bool bwg_campaign_reserve(bwg_campaign_t *campaign, bool is_new, const bwg_allowance_t *allowance);
/** Completes the running window, keeping its charge. No running window leaves it unchanged. */
void bwg_campaign_finish(bwg_campaign_t *campaign);
bool bwg_campaign_is_complete(const bwg_campaign_t *campaign);
bool bwg_campaign_equal(const bwg_campaign_t *a, const bwg_campaign_t *b);
/** Writes the `acceptance_budget_review` result. A NULL ledger is a device with none. Neither ID is echoed. */
void bwg_campaign_write_review(const bwg_campaign_t *maybe_campaign, bwg_str_t campaign_id, bwg_jw_t *writer);

#endif // BWG_LEDGER_H
