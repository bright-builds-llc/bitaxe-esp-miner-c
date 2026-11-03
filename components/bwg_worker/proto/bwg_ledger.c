#include "bwg_ledger.h"

#include <string.h>

#include "bwg_codec.h"

#define MAX_SAFE_INTEGER 9007199254740991ull
#define ATTEMPT_EXTRA_MS (BWG_ATTEMPT_NORMAL_MS - BWG_ATTEMPT_SHORT_MS)
#define ID_BYTES 16u

static bool allowance_of_last(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger, bwg_allowance_t *out)
{
    *out = (bwg_allowance_t) {
        .kind = kind,
        .schema = {.ptr = kind == BWG_ALLOWANCE_SOAK ? "worker-soak-allowance-v2" : "worker-qualification-attempt-v1"},
        .id = {.ptr = ledger->last_id, .len = strlen(ledger->last_id)},
        .ordinal = ledger->last_ordinal,
        .purpose = ledger->last_purpose,
        .maximum_active_ms = ledger->last_maximum_ms,
    };
    out->schema.len = strlen(out->schema.ptr);
    return bwg_allowance_validate(out);
}

static bool soak_is_valid(const bwg_ledger_t *ledger)
{
    return ledger->total_charged_ms == (uint64_t) ledger->highest_ordinal * BWG_SOAK_MS;
}

static bool attempt_is_valid(const bwg_ledger_t *ledger)
{
    const uint64_t total = ledger->total_charged_ms;
    const uint64_t highest = ledger->highest_ordinal;
    if (total % BWG_ATTEMPT_SHORT_MS != 0) {
        return false;
    }
    if (!ledger->has_last) {
        return true;
    }
    if (total < ledger->last_maximum_ms || total < highest * BWG_ATTEMPT_SHORT_MS ||
        total > highest * BWG_ATTEMPT_NORMAL_MS) {
        return false;
    }
    const uint64_t extra = total - highest * BWG_ATTEMPT_SHORT_MS;
    if (extra % ATTEMPT_EXTRA_MS != 0) {
        return false;
    }
    const uint64_t normals = extra / ATTEMPT_EXTRA_MS;
    return ledger->last_purpose == BWG_ATTEMPT_NORMAL ? normals >= 1 : normals < highest;
}

void bwg_ledger_default(bwg_ledger_t *ledger)
{
    memset(ledger, 0, sizeof(*ledger));
}

bool bwg_ledger_validate(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger)
{
    if (ledger->total_charged_ms > MAX_SAFE_INTEGER) {
        return false;
    }
    if (!ledger->has_last) {
        return ledger->highest_ordinal == 0 && ledger->total_charged_ms == 0;
    }
    bwg_allowance_t last;
    if (!allowance_of_last(kind, ledger, &last) || ledger->last_ordinal != ledger->highest_ordinal) {
        return false;
    }
    return kind == BWG_ALLOWANCE_SOAK ? soak_is_valid(ledger) : attempt_is_valid(ledger);
}

static bool read_u32(yyjson_val *object, const char *key, uint32_t *out)
{
    uint64_t value;
    if (!bwg_json_get_u64(bwg_json_maybe_field(object, key), &value) || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t) value;
    return true;
}

static bool read_last(bwg_allowance_kind_t kind, yyjson_val *last, bwg_ledger_t *ledger)
{
    static const char *const REQUIRED[] = {"allowance", "complete"};
    yyjson_val *complete = bwg_json_maybe_field(last, "complete");
    yyjson_val *allowance_value = bwg_json_maybe_field(last, "allowance");
    bwg_allowance_t allowance = {0};
    const bool parsed = bwg_json_has_exact_fields(last, REQUIRED, 2, NULL, 0) && yyjson_is_bool(complete) &&
                        (kind == BWG_ALLOWANCE_SOAK ? bwg_allowance_parse_soak(allowance_value, &allowance)
                                                    : bwg_allowance_parse_attempt(allowance_value, &allowance));
    if (!parsed || !bwg_allowance_validate(&allowance)) {
        return false;
    }
    ledger->has_last = true;
    ledger->last_complete = yyjson_get_bool(complete);
    memcpy(ledger->last_id, allowance.id.ptr, allowance.id.len);
    ledger->last_ordinal = allowance.ordinal;
    ledger->last_purpose = allowance.purpose;
    ledger->last_maximum_ms = allowance.maximum_active_ms;
    return true;
}

bool bwg_ledger_parse(bwg_allowance_kind_t kind, const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                      bwg_ledger_t *ledger)
{
    static const char *const REQUIRED[] = {"highest_ordinal", "total_charged_ms"};
    static const char *const OPTIONAL[] = {"maybe_last"};
    yyjson_doc *doc;
    bwg_ledger_default(ledger);
    if (bwg_json_parse_strict(allocator, text, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *maybe_last = bwg_json_maybe_field(root, "maybe_last");
    bool ok = bwg_json_has_exact_fields(root, REQUIRED, 2, OPTIONAL, 1) &&
              read_u32(root, "highest_ordinal", &ledger->highest_ordinal) &&
              bwg_json_get_u64(bwg_json_maybe_field(root, "total_charged_ms"), &ledger->total_charged_ms);
    if (ok && maybe_last != NULL && !yyjson_is_null(maybe_last)) {
        ok = read_last(kind, maybe_last, ledger);
    }
    yyjson_doc_free(doc);
    return ok && bwg_ledger_validate(kind, ledger);
}

static void write_str(bwg_jw_t *writer, const char *key, const char *value)
{
    bwg_jw_key(writer, key);
    bwg_jw_cstr(writer, value);
}

static void write_u64(bwg_jw_t *writer, const char *key, uint64_t value)
{
    bwg_jw_key(writer, key);
    bwg_jw_u64(writer, value);
}

static void write_bool(bwg_jw_t *writer, const char *key, bool value)
{
    bwg_jw_key(writer, key);
    bwg_jw_bool(writer, value);
}

bool bwg_ledger_serialize(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger, uint8_t *out, size_t capacity,
                          size_t *len)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, capacity);
    bwg_jw_object_begin(&writer);
    write_u64(&writer, "highest_ordinal", ledger->highest_ordinal);
    write_u64(&writer, "total_charged_ms", ledger->total_charged_ms);
    bwg_jw_key(&writer, "maybe_last");
    if (ledger->has_last) {
        bwg_jw_object_begin(&writer);
        bwg_jw_key(&writer, "allowance");
        bwg_jw_object_begin(&writer);
        write_str(&writer, "id", ledger->last_id);
        write_u64(&writer, "maximumActiveMilliseconds", ledger->last_maximum_ms);
        write_u64(&writer, "ordinal", ledger->last_ordinal);
        if (kind == BWG_ALLOWANCE_SOAK) {
            write_str(&writer, "schema", "worker-soak-allowance-v2");
        } else {
            write_str(&writer, "purpose", bwg_attempt_purpose_label(ledger->last_purpose));
            write_str(&writer, "schema", "worker-qualification-attempt-v1");
        }
        bwg_jw_object_end(&writer);
        write_bool(&writer, "complete", ledger->last_complete);
        bwg_jw_object_end(&writer);
    } else {
        bwg_jw_null(&writer);
    }
    bwg_jw_object_end(&writer);
    *len = writer.len;
    return bwg_jw_ok(&writer);
}

bool bwg_ledger_is_pending(const bwg_ledger_t *ledger)
{
    return ledger->has_last && !ledger->last_complete;
}

bool bwg_ledger_reserve(bwg_allowance_kind_t kind, bwg_ledger_t *ledger, const bwg_allowance_t *allowance)
{
    if (allowance->kind != kind || !bwg_ledger_validate(kind, ledger) || !bwg_allowance_validate(allowance) ||
        bwg_ledger_is_pending(ledger) || ledger->highest_ordinal == UINT32_MAX ||
        allowance->ordinal != ledger->highest_ordinal + 1 ||
        allowance->maximum_active_ms > MAX_SAFE_INTEGER - ledger->total_charged_ms) {
        return false;
    }
    bwg_ledger_t next = *ledger;
    next.highest_ordinal = allowance->ordinal;
    next.total_charged_ms += allowance->maximum_active_ms;
    next.has_last = true;
    next.last_complete = false;
    memset(next.last_id, 0, sizeof(next.last_id));
    memcpy(next.last_id, allowance->id.ptr, allowance->id.len);
    next.last_ordinal = allowance->ordinal;
    next.last_purpose = kind == BWG_ALLOWANCE_ATTEMPT ? allowance->purpose : BWG_ATTEMPT_DIAGNOSTIC;
    next.last_maximum_ms = allowance->maximum_active_ms;
    if (!bwg_ledger_validate(kind, &next)) {
        return false;
    }
    *ledger = next;
    return true;
}

void bwg_ledger_finish(bwg_ledger_t *ledger)
{
    if (ledger->has_last) {
        ledger->last_complete = true;
    }
}

bool bwg_ledger_equal(const bwg_ledger_t *a, const bwg_ledger_t *b)
{
    if (a->highest_ordinal != b->highest_ordinal || a->total_charged_ms != b->total_charged_ms ||
        a->has_last != b->has_last) {
        return false;
    }
    return !a->has_last || (a->last_complete == b->last_complete && a->last_ordinal == b->last_ordinal &&
                            a->last_purpose == b->last_purpose && a->last_maximum_ms == b->last_maximum_ms &&
                            strcmp(a->last_id, b->last_id) == 0);
}

void bwg_ledger_write_review(bwg_allowance_kind_t kind, const bwg_ledger_t *ledger, bwg_jw_t *writer)
{
    const bool pending = bwg_ledger_is_pending(ledger);
    const uint32_t highest = ledger->highest_ordinal;
    bwg_jw_object_begin(writer);
    write_str(writer, "schema", kind == BWG_ALLOWANCE_SOAK ? "worker-soak-ledger-v2" : "worker-qualification-ledger-v1");
    write_u64(writer, "next_ordinal", (uint64_t) highest + 1);
    write_u64(writer, "total_charged_ms", ledger->total_charged_ms);
    write_bool(writer, "pending", pending);
    write_u64(writer, "last_completed_ordinal", pending && highest > 0 ? highest - 1 : highest);
    bwg_jw_object_end(writer);
}

static uint32_t charge_of_mask(uint8_t mask)
{
    uint32_t total = 0;
    for (uint8_t window = 0; window < BWG_CAMPAIGN_WINDOWS; window++) {
        if ((mask & (1u << window)) != 0) {
            total += (uint32_t) bwg_campaign_window_ms(window);
        }
    }
    return total;
}

static bool campaign_id_is_valid(const char *id, size_t len)
{
    if (len != BWG_ALLOWANCE_ID_SIZE - 1) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool bwg_campaign_validate(const bwg_campaign_t *campaign)
{
    if (!campaign_id_is_valid(campaign->campaign_id, strlen(campaign->campaign_id)) || (campaign->reserved & ~7u) != 0 ||
        (campaign->completed & ~campaign->reserved) != 0 || campaign->charged_ms != charge_of_mask(campaign->reserved) ||
        campaign->charged_ms > BWG_CAMPAIGN_TOTAL_MS) {
        return false;
    }
    if (!campaign->has_running) {
        return true;
    }
    const uint8_t bit = (uint8_t) (1u << campaign->running_window);
    return campaign->running_window < BWG_CAMPAIGN_WINDOWS && (campaign->reserved & bit) != 0 &&
           (campaign->completed & bit) == 0;
}

bool bwg_campaign_parse(const bwg_allocator_t *allocator, const uint8_t *text, size_t len, bwg_campaign_t *campaign)
{
    static const char *const REQUIRED[] = {"campaign_id", "reserved", "completed", "charged_ms"};
    static const char *const OPTIONAL[] = {"maybe_running_window"};
    yyjson_doc *doc;
    memset(campaign, 0, sizeof(*campaign));
    if (bwg_json_parse_strict(allocator, text, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bwg_str_t id = {0};
    uint32_t reserved = 0;
    uint32_t completed = 0;
    uint32_t running = 0;
    bool ok = bwg_json_has_exact_fields(root, REQUIRED, 4, OPTIONAL, 1) &&
              bwg_json_get_str(bwg_json_maybe_field(root, "campaign_id"), &id) && id.len == BWG_ALLOWANCE_ID_SIZE - 1 &&
              read_u32(root, "reserved", &reserved) && reserved <= UINT8_MAX &&
              read_u32(root, "completed", &completed) && completed <= UINT8_MAX &&
              read_u32(root, "charged_ms", &campaign->charged_ms);
    yyjson_val *maybe_running = bwg_json_maybe_field(root, "maybe_running_window");
    if (ok && maybe_running != NULL && !yyjson_is_null(maybe_running)) {
        ok = read_u32(root, "maybe_running_window", &running) && running <= UINT8_MAX;
        campaign->has_running = true;
    }
    if (ok) {
        memcpy(campaign->campaign_id, id.ptr, id.len);
        campaign->reserved = (uint8_t) reserved;
        campaign->completed = (uint8_t) completed;
        campaign->running_window = (uint8_t) running;
    }
    yyjson_doc_free(doc);
    return ok && bwg_campaign_validate(campaign);
}

bool bwg_campaign_serialize(const bwg_campaign_t *campaign, uint8_t *out, size_t capacity, size_t *len)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, out, capacity);
    bwg_jw_object_begin(&writer);
    write_str(&writer, "campaign_id", campaign->campaign_id);
    write_u64(&writer, "reserved", campaign->reserved);
    write_u64(&writer, "completed", campaign->completed);
    write_u64(&writer, "charged_ms", campaign->charged_ms);
    bwg_jw_key(&writer, "maybe_running_window");
    if (campaign->has_running) {
        bwg_jw_u64(&writer, campaign->running_window);
    } else {
        bwg_jw_null(&writer);
    }
    bwg_jw_object_end(&writer);
    *len = writer.len;
    return bwg_jw_ok(&writer);
}

bool bwg_campaign_reserve(bwg_campaign_t *campaign, bool is_new, const bwg_allowance_t *allowance)
{
    if (allowance->kind != BWG_ALLOWANCE_CAMPAIGN || !bwg_allowance_validate(allowance) ||
        allowance->id.len != BWG_ALLOWANCE_ID_SIZE - 1) {
        return false;
    }
    bwg_campaign_t next;
    if (is_new) {
        memset(&next, 0, sizeof(next));
        memcpy(next.campaign_id, allowance->id.ptr, allowance->id.len);
    } else {
        if (!bwg_campaign_validate(campaign) || !bwg_str_equals(allowance->id, campaign->campaign_id)) {
            return false;
        }
        next = *campaign;
    }
    const uint8_t bit = (uint8_t) (1u << allowance->window);
    if (next.has_running || (next.reserved & bit) != 0) {
        return false;
    }
    next.reserved |= bit;
    next.charged_ms += (uint32_t) allowance->maximum_active_ms;
    next.has_running = true;
    next.running_window = allowance->window;
    if (!bwg_campaign_validate(&next)) {
        return false;
    }
    *campaign = next;
    return true;
}

void bwg_campaign_finish(bwg_campaign_t *campaign)
{
    if (campaign->has_running) {
        campaign->completed |= (uint8_t) (1u << campaign->running_window);
        campaign->has_running = false;
        campaign->running_window = 0;
    }
}

bool bwg_campaign_is_complete(const bwg_campaign_t *campaign)
{
    return campaign->completed == 7 && campaign->reserved == 7 && !campaign->has_running;
}

bool bwg_campaign_equal(const bwg_campaign_t *a, const bwg_campaign_t *b)
{
    return strcmp(a->campaign_id, b->campaign_id) == 0 && a->reserved == b->reserved && a->completed == b->completed &&
           a->charged_ms == b->charged_ms && a->has_running == b->has_running &&
           (!a->has_running || a->running_window == b->running_window);
}

void bwg_campaign_write_review(const bwg_campaign_t *maybe_campaign, bwg_str_t campaign_id, bwg_jw_t *writer)
{
    const bool match = maybe_campaign != NULL && bwg_str_equals(campaign_id, maybe_campaign->campaign_id);
    bwg_jw_object_begin(writer);
    write_str(writer, "schema", "worker-budget-review-v1");
    write_bool(writer, "campaign_match", match);
    write_u64(writer, "reserved_mask", maybe_campaign != NULL ? maybe_campaign->reserved : 0);
    write_u64(writer, "completed_mask", maybe_campaign != NULL ? maybe_campaign->completed : 0);
    write_u64(writer, "charged_ms", maybe_campaign != NULL ? maybe_campaign->charged_ms : 0);
    write_bool(writer, "pending", maybe_campaign != NULL && maybe_campaign->has_running);
    bwg_jw_object_end(writer);
}
