#include "bwg_budget.h"

#include <string.h>

typedef enum {
    READ_ABSENT = 0,
    READ_OK,
    READ_FAILED,
} read_result_t;

static const char *ledger_key(bwg_allowance_kind_t kind)
{
    return kind == BWG_ALLOWANCE_SOAK ? BWG_BUDGET_KEY_SOAK : BWG_BUDGET_KEY_ATTEMPTS;
}

static read_result_t read_ledger(bwg_budget_t *budget, bwg_allowance_kind_t kind, bwg_ledger_t *ledger)
{
    uint8_t text[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    bwg_ledger_default(ledger);
    const bwg_budget_load_t loaded = budget->store.load(budget->store.ctx, ledger_key(kind), text, sizeof(text), &len);
    if (loaded == BWG_BUDGET_LOAD_ABSENT) {
        return READ_ABSENT;
    }
    if (loaded != BWG_BUDGET_LOAD_OK || len == 0 || !bwg_ledger_parse(kind, budget->allocator, text, len, ledger)) {
        return READ_FAILED;
    }
    return READ_OK;
}

static read_result_t read_campaign(bwg_budget_t *budget, bwg_campaign_t *campaign)
{
    uint8_t text[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    memset(campaign, 0, sizeof(*campaign));
    const bwg_budget_load_t loaded = budget->store.load(budget->store.ctx, BWG_BUDGET_KEY_CAMPAIGN, text, sizeof(text), &len);
    if (loaded == BWG_BUDGET_LOAD_ABSENT) {
        return READ_ABSENT;
    }
    if (loaded != BWG_BUDGET_LOAD_OK || len == 0 || !bwg_campaign_parse(budget->allocator, text, len, campaign)) {
        return READ_FAILED;
    }
    return READ_OK;
}

/** Stored and read back through the whole read path, so nothing later trusts a write that didn't land. */
static bool store_ledger(bwg_budget_t *budget, bwg_allowance_kind_t kind, const bwg_ledger_t *ledger)
{
    uint8_t text[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    if (!bwg_ledger_validate(kind, ledger) || !bwg_ledger_serialize(kind, ledger, text, sizeof(text), &len) ||
        !budget->store.store(budget->store.ctx, ledger_key(kind), text, len)) {
        return false;
    }
    bwg_ledger_t stored;
    return read_ledger(budget, kind, &stored) == READ_OK && bwg_ledger_equal(&stored, ledger);
}

static bool store_campaign(bwg_budget_t *budget, const bwg_campaign_t *campaign)
{
    uint8_t text[BWG_LEDGER_MAX_BYTES];
    size_t len = 0;
    if (!bwg_campaign_validate(campaign) || !bwg_campaign_serialize(campaign, text, sizeof(text), &len) ||
        !budget->store.store(budget->store.ctx, BWG_BUDGET_KEY_CAMPAIGN, text, len)) {
        return false;
    }
    bwg_campaign_t stored;
    return read_campaign(budget, &stored) == READ_OK && bwg_campaign_equal(&stored, campaign);
}

static void cache_campaign(bwg_budget_t *budget, const bwg_campaign_t *campaign)
{
    atomic_store(&budget->campaign_reserved_ms, campaign->charged_ms);
    atomic_store(&budget->campaign_complete, bwg_campaign_is_complete(campaign));
}

void bwg_budget_init(bwg_budget_t *budget, const bwg_allocator_t *allocator, const bwg_budget_store_t *store,
                     bwg_soak_rule_t soak_rule)
{
    memset(budget, 0, sizeof(*budget));
    budget->allocator = allocator;
    budget->store = *store;
    budget->soak_rule = soak_rule;
}

static bool recover_ledger(bwg_budget_t *budget, bwg_allowance_kind_t kind)
{
    bwg_ledger_t ledger;
    const read_result_t result = read_ledger(budget, kind, &ledger);
    if (result == READ_ABSENT) {
        return true;
    }
    if (result != READ_OK) {
        return false;
    }
    if (!bwg_ledger_is_pending(&ledger)) {
        return true;
    }
    bwg_ledger_finish(&ledger);
    return store_ledger(budget, kind, &ledger);
}

bool bwg_budget_recover_boot(bwg_budget_t *budget)
{
    budget->soak_recovery_failed = !recover_ledger(budget, BWG_ALLOWANCE_SOAK);
    if (!recover_ledger(budget, BWG_ALLOWANCE_ATTEMPT)) {
        return false;
    }
    bwg_campaign_t campaign;
    const read_result_t result = read_campaign(budget, &campaign);
    if (result == READ_ABSENT) {
        return true;
    }
    if (result != READ_OK) {
        return false;
    }
    if (campaign.has_running) {
        bwg_campaign_finish(&campaign);
        if (!store_campaign(budget, &campaign)) {
            return false;
        }
    }
    cache_campaign(budget, &campaign);
    return true;
}

static void observe(bwg_budget_t *budget, uint32_t generation, const bwg_allowance_t *allowance)
{
    budget->observed_kind = allowance->kind;
    budget->observed_ordinal = allowance->ordinal;
    budget->observed_purpose = allowance->purpose;
    budget->observed_maximum_ms = (uint32_t) allowance->maximum_active_ms;
    budget->observed_complete = false;
    budget->observed_generation = generation;
}

/** The original campaign, finished and exactly fully charged. */
static bool campaign_is_exhausted(const bwg_campaign_t *campaign, read_result_t result)
{
    return result == READ_OK && bwg_campaign_is_complete(campaign) && campaign->charged_ms == BWG_CAMPAIGN_TOTAL_MS;
}

static bool admit_ledger(bwg_budget_t *budget, uint32_t generation, const bwg_allowance_t *allowance, const char **refusal)
{
    const bwg_allowance_kind_t kind = allowance->kind;
    const bool soak = kind == BWG_ALLOWANCE_SOAK;
    const bwg_allowance_kind_t other = soak ? BWG_ALLOWANCE_ATTEMPT : BWG_ALLOWANCE_SOAK;
    if (soak && budget->soak_recovery_failed) {
        *refusal = "soak_budget=recovery_failed";
        return false;
    }
    bwg_campaign_t campaign;
    const read_result_t campaign_read = read_campaign(budget, &campaign);
    const bool campaign_ok =
        soak && budget->soak_rule == BWG_SOAK_NEEDS_NO_UNFINISHED_CAMPAIGN
            ? campaign_read == READ_ABSENT || (campaign_read == READ_OK && bwg_campaign_is_complete(&campaign))
            : campaign_is_exhausted(&campaign, campaign_read);
    if (!campaign_ok) {
        *refusal = soak ? "soak_budget=original_incomplete" : "qualification_budget=original_incomplete";
        return false;
    }
    bwg_ledger_t other_ledger;
    if (read_ledger(budget, other, &other_ledger) == READ_FAILED || bwg_ledger_is_pending(&other_ledger)) {
        *refusal = soak ? "soak_budget=qualification_pending" : "qualification_budget=soak_pending";
        return false;
    }
    bwg_ledger_t previous;
    if (read_ledger(budget, kind, &previous) == READ_FAILED) {
        *refusal = soak ? "soak_budget=invalid_storage" : "qualification_budget=invalid_storage";
        return false;
    }
    bwg_ledger_t reserved = previous;
    if (!bwg_ledger_reserve(kind, &reserved, allowance)) {
        *refusal = soak ? "soak_budget=rejected" : "qualification_budget=rejected";
        return false;
    }
    budget->ledger_previous = previous;
    budget->ledger_reserved = reserved;
    budget->pending_kind = kind;
    budget->pending_generation = generation;
    observe(budget, generation, allowance);
    if (!store_ledger(budget, kind, &reserved)) {
        *refusal = soak ? "soak_budget=storage" : "qualification_budget=storage";
        return false;
    }
    return true;
}

static bool admit_campaign(bwg_budget_t *budget, uint32_t generation, const bwg_allowance_t *allowance, const char **refusal)
{
    bwg_campaign_t previous;
    const read_result_t read = read_campaign(budget, &previous);
    if (read == READ_FAILED) {
        *refusal = "acceptance_budget=invalid_storage";
        return false;
    }
    bwg_ledger_t soak;
    if (read_ledger(budget, BWG_ALLOWANCE_SOAK, &soak) == READ_FAILED || bwg_ledger_is_pending(&soak)) {
        *refusal = "acceptance_budget=soak_pending";
        return false;
    }
    bwg_campaign_t reserved = previous;
    if (!bwg_campaign_reserve(&reserved, read == READ_ABSENT, allowance)) {
        *refusal = "acceptance_budget=rejected";
        return false;
    }
    budget->campaign_had_previous = read == READ_OK;
    budget->campaign_previous = previous;
    budget->campaign_reserved = reserved;
    budget->pending_kind = BWG_ALLOWANCE_CAMPAIGN;
    budget->pending_generation = generation;
    atomic_store(&budget->campaign_reserved_ms, reserved.charged_ms);
    atomic_store(&budget->campaign_complete, false);
    if (!store_campaign(budget, &reserved)) {
        *refusal = "acceptance_budget=storage";
        return false;
    }
    return true;
}

/** A partly run campaign holds the device for its remaining windows, so an ordinary lease can't overtake it. */
static bool admit_unlimited(bwg_budget_t *budget, const char **refusal)
{
    bwg_campaign_t campaign;
    const read_result_t read = read_campaign(budget, &campaign);
    if (read == READ_FAILED) {
        *refusal = "acceptance_budget=invalid_storage";
        return false;
    }
    if (read == READ_OK && !bwg_campaign_is_complete(&campaign)) {
        *refusal = "acceptance_budget=unfinished_campaign";
        return false;
    }
    return true;
}

bool bwg_budget_admit(bwg_budget_t *budget, uint32_t generation, const bwg_allowance_t *allowance, uint32_t *limit_ms,
                      const char **refusal)
{
    *limit_ms = 0;
    *refusal = "";
    bool admitted;
    switch (allowance->kind) {
    case BWG_ALLOWANCE_NONE:
        return admit_unlimited(budget, refusal);
    case BWG_ALLOWANCE_CAMPAIGN:
        admitted = admit_campaign(budget, generation, allowance, refusal);
        break;
    default:
        admitted = admit_ledger(budget, generation, allowance, refusal);
        break;
    }
    if (admitted) {
        *limit_ms = (uint32_t) allowance->maximum_active_ms;
    }
    return admitted;
}

/** Stores the stopped form whatever the original write did. The stored value must still be one this lease made. */
static bool finish_ledger(bwg_budget_t *budget)
{
    const bwg_allowance_kind_t kind = budget->pending_kind;
    bwg_ledger_t stopped = budget->ledger_reserved;
    bwg_ledger_finish(&stopped);
    bwg_ledger_t stored;
    if (read_ledger(budget, kind, &stored) == READ_FAILED) {
        return false;
    }
    if (!bwg_ledger_equal(&stored, &budget->ledger_previous) && !bwg_ledger_equal(&stored, &budget->ledger_reserved) &&
        !bwg_ledger_equal(&stored, &stopped)) {
        return false;
    }
    return store_ledger(budget, kind, &stopped);
}

static bool finish_campaign(bwg_budget_t *budget)
{
    bwg_campaign_t stopped = budget->campaign_reserved;
    bwg_campaign_finish(&stopped);
    bwg_campaign_t stored;
    const read_result_t read = read_campaign(budget, &stored);
    if (read == READ_FAILED) {
        return false;
    }
    const bool was_previous = read == READ_ABSENT ? !budget->campaign_had_previous
                                                  : budget->campaign_had_previous &&
                                                        bwg_campaign_equal(&stored, &budget->campaign_previous);
    if (!was_previous && (read == READ_ABSENT || (!bwg_campaign_equal(&stored, &budget->campaign_reserved) &&
                                                  !bwg_campaign_equal(&stored, &stopped)))) {
        return false;
    }
    if (!store_campaign(budget, &stopped)) {
        return false;
    }
    cache_campaign(budget, &stopped);
    return true;
}

bool bwg_budget_finish(bwg_budget_t *budget, uint32_t generation)
{
    if (budget->pending_kind == BWG_ALLOWANCE_NONE || budget->pending_generation != generation) {
        return true;
    }
    const bool finished = budget->pending_kind == BWG_ALLOWANCE_CAMPAIGN ? finish_campaign(budget) : finish_ledger(budget);
    if (!finished) {
        return false;
    }
    if (budget->observed_generation == generation) {
        budget->observed_complete = true;
    }
    budget->pending_kind = BWG_ALLOWANCE_NONE;
    return true;
}

bool bwg_budget_write_ledger_review(bwg_budget_t *budget, bwg_allowance_kind_t kind, bwg_jw_t *writer)
{
    bwg_ledger_t ledger;
    if (read_ledger(budget, kind, &ledger) == READ_FAILED) {
        return false;
    }
    bwg_ledger_write_review(kind, &ledger, writer);
    return true;
}

bool bwg_budget_write_campaign_review(bwg_budget_t *budget, bwg_str_t campaign_id, bwg_jw_t *writer)
{
    bwg_campaign_t campaign;
    const read_result_t read = read_campaign(budget, &campaign);
    if (read == READ_FAILED) {
        return false;
    }
    bwg_campaign_write_review(read == READ_OK ? &campaign : NULL, campaign_id, writer);
    return true;
}

void bwg_budget_write_observation(const bwg_budget_t *budget, uint32_t generation, uint32_t active_ms, bwg_jw_t *writer)
{
    if (generation == 0 || budget->observed_generation != generation) {
        return;
    }
    const bool soak = budget->observed_kind == BWG_ALLOWANCE_SOAK;
    bwg_jw_key(writer, soak ? "soak" : "attempt");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, soak ? "worker-soak-observation-v1" : "worker-qualification-observation-v1");
    bwg_jw_key(writer, "ordinal");
    bwg_jw_u64(writer, budget->observed_ordinal);
    if (!soak) {
        bwg_jw_key(writer, "purpose");
        bwg_jw_cstr(writer, bwg_attempt_purpose_label(budget->observed_purpose));
    }
    bwg_jw_key(writer, "maximum_active_ms");
    bwg_jw_u64(writer, budget->observed_maximum_ms);
    bwg_jw_key(writer, "reserved_ms");
    bwg_jw_u64(writer, budget->observed_maximum_ms);
    bwg_jw_key(writer, "complete");
    bwg_jw_bool(writer, budget->observed_complete);
    bwg_jw_key(writer, "active_ms");
    bwg_jw_u64(writer, active_ms);
    bwg_jw_object_end(writer);
}

uint32_t bwg_budget_campaign_reserved_ms(const bwg_budget_t *budget)
{
    return atomic_load(&budget->campaign_reserved_ms);
}

bool bwg_budget_campaign_complete(const bwg_budget_t *budget)
{
    return atomic_load(&budget->campaign_complete);
}

bwg_budget_timing_t bwg_budget_timing(uint32_t first_work_ms, uint32_t closed_ms, uint32_t limit_ms, uint32_t deadline_ms,
                                      uint32_t now_ms)
{
    bwg_budget_timing_t timing = {0};
    const bool started = first_work_ms != 0;
    const bool closed = closed_ms != 0;
    if (started) {
        timing.active_ms = (closed ? closed_ms : now_ms) - first_work_ms;
        if (limit_ms != 0 && timing.active_ms > limit_ms) {
            timing.active_ms = limit_ms;
        }
    }
    if (limit_ms == 0 || !started) {
        return timing;
    }
    timing.has_remaining = true;
    const int32_t remaining = (int32_t) (deadline_ms - now_ms);
    if (!closed && remaining > 0) {
        timing.remaining_ms = (uint32_t) remaining > limit_ms ? limit_ms : (uint32_t) remaining;
    }
    return timing;
}
