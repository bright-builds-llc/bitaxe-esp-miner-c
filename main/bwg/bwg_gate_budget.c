#include "bwg_gate_budget.h"

#include <string.h>

#include "bwg_store.h"
#include "esp_log.h"

static const char *TAG = "bwg_budget";

// Static, so its atomics stay in internal RAM.
static bwg_budget_t s_budget;

static bwg_budget_load_t load_blob(void *ctx, const char *key, uint8_t *out, size_t capacity, size_t *len)
{
    bool present = false;
    if (!bwg_store_ledger_load(key, out, capacity, len, &present)) {
        return BWG_BUDGET_LOAD_FAILED;
    }
    return present ? BWG_BUDGET_LOAD_OK : BWG_BUDGET_LOAD_ABSENT;
}

static bool store_blob(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    return bwg_store_ledger_store(key, data, len);
}

bool bwg_gate_budget_start(const bwg_allocator_t *allocator, const GlobalState *GLOBAL_STATE)
{
    const bwg_budget_store_t store = {.load = load_blob, .store = store_blob};
    const bool is_205 = strcmp(GLOBAL_STATE->DEVICE_CONFIG.board_version, "205") == 0;
    bwg_budget_init(&s_budget, allocator, &store, is_205 ? BWG_SOAK_NEEDS_COMPLETE_CAMPAIGN : BWG_SOAK_NEEDS_NO_UNFINISHED_CAMPAIGN);
    const bool recovered = bwg_budget_recover_boot(&s_budget);
    if (!recovered) {
        ESP_LOGE(TAG, "Budget recovery failed");
    }
    return recovered;
}

bool bwg_gate_budget_admit(uint32_t generation, const bwg_allowance_t *allowance, uint32_t *limit_ms)
{
    const char *refusal = "";
    const bool admitted = bwg_budget_admit(&s_budget, generation, allowance, limit_ms, &refusal);
    if (!admitted) {
        ESP_LOGW(TAG, "worker_start=refused category=budget reason=%s", refusal);
    }
    return admitted;
}

bool bwg_gate_budget_finish(uint32_t generation)
{
    return bwg_budget_finish(&s_budget, generation);
}

bwg_evidence_t bwg_gate_budget_review(bwg_allowance_kind_t kind, bwg_str_t campaign_id, bwg_jw_t *writer)
{
    const bool written = kind == BWG_ALLOWANCE_CAMPAIGN ? bwg_budget_write_campaign_review(&s_budget, campaign_id, writer)
                                                        : bwg_budget_write_ledger_review(&s_budget, kind, writer);
    return written ? BWG_EVIDENCE_WRITTEN : BWG_EVIDENCE_SESSION_FAILED;
}

void bwg_gate_budget_write_observation(uint32_t generation, uint32_t active_ms, bwg_jw_t *writer)
{
    bwg_budget_write_observation(&s_budget, generation, active_ms, writer);
}

uint32_t bwg_gate_budget_campaign_reserved_ms(void)
{
    return bwg_budget_campaign_reserved_ms(&s_budget);
}

bool bwg_gate_budget_campaign_complete(void)
{
    return bwg_budget_campaign_complete(&s_budget);
}
