#include "bwg_preparation_receipt.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define VALID 0x42585053u
#define WRITING 0x42585057u
#define VERSION 1u
#define METRIC_TEXT_BYTES 12
#define MAX_FAILURE BWG_PREP_FAILURE_COOLING_PROOF_REQUIRED

static void put_u64(bwg_prep_slot_t words, size_t index, uint64_t value)
{
    words[index] = (uint32_t)value;
    words[index + 1] = (uint32_t)(value >> 32);
}

static uint64_t get_u64(const bwg_prep_slot_t words, size_t index)
{
    return (uint64_t)words[index] | ((uint64_t)words[index + 1] << 32);
}

static uint32_t checksum(const bwg_prep_slot_t words)
{
    uint32_t crc = ~0u;
    for (size_t index = 1; index < 17; index++) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            crc ^= (words[index] >> shift) & 0xffu;
            for (unsigned bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
        }
    }
    return ~crc;
}

bool bwg_prep_encode(const bwg_prep_receipt_t *receipt, bwg_prep_slot_t words)
{
    const bool failed = receipt->outcome == BWG_PREP_FAILED;
    if (failed != (receipt->failure != BWG_PREP_FAILURE_NONE) || receipt->source_hash == 0 ||
        receipt->boot_ordinal == 0 || receipt->generation == 0 || receipt->sequence == 0 ||
        receipt->current_step < 1 || receipt->current_step > BWG_PREP_LAST_STEP ||
        receipt->last_completed_step > BWG_PREP_LAST_STEP ||
        (receipt->outcome == BWG_PREP_COMPLETED && receipt->last_completed_step != receipt->current_step) ||
        (receipt->outcome == BWG_PREP_STARTED && receipt->last_completed_step >= receipt->current_step) ||
        (failed && receipt->last_completed_step > receipt->current_step)) {
        return false;
    }
    memset(words, 0, sizeof(bwg_prep_slot_t));
    words[0] = VALID;
    words[1] = VERSION;
    put_u64(words, 2, receipt->source_hash);
    put_u64(words, 4, receipt->boot_ordinal);
    words[6] = receipt->generation;
    words[7] = receipt->sequence;
    put_u64(words, 8, receipt->uptime_ms);
    words[10] = receipt->last_completed_step;
    words[11] = receipt->current_step;
    words[12] = (uint32_t)receipt->outcome | ((uint32_t)receipt->failure << 8);
    if (receipt->has_heap_free) {
        words[13] |= 1u;
        words[14] = receipt->heap_free;
    }
    if (receipt->has_heap_largest) {
        words[13] |= 2u;
        words[15] = receipt->heap_largest;
    }
    if (receipt->has_stack_free) {
        words[13] |= 4u;
        words[16] = receipt->stack_free;
    }
    words[17] = checksum(words);
    return true;
}

static bwg_prep_status_t decode(const bwg_prep_slot_t words, bwg_prep_receipt_t *receipt)
{
    uint32_t any = 0;
    for (size_t index = 0; index < BWG_PREP_WORDS; index++) {
        any |= words[index];
    }
    if (any == 0) {
        return BWG_PREP_STATUS_UNAVAILABLE;
    }
    if (words[0] == 0 || words[0] == WRITING) {
        return BWG_PREP_STATUS_INCOMPLETE;
    }
    if (words[0] != VALID || words[1] != VERSION || words[17] != checksum(words) || (words[13] & ~7u) != 0) {
        return BWG_PREP_STATUS_CORRUPT;
    }
    for (unsigned bit = 0; bit < 3; bit++) {
        if ((words[13] & (1u << bit)) == 0 && words[14 + bit] != 0) {
            return BWG_PREP_STATUS_CORRUPT;
        }
    }
    const uint32_t outcome = words[12] & 0xffu;
    const uint32_t failure = words[12] >> 8;
    if (outcome < BWG_PREP_STARTED || outcome > BWG_PREP_FAILED || failure > MAX_FAILURE) {
        return BWG_PREP_STATUS_CORRUPT;
    }
    *receipt = (bwg_prep_receipt_t) {
        .source_hash = get_u64(words, 2),
        .boot_ordinal = get_u64(words, 4),
        .generation = words[6],
        .sequence = words[7],
        .uptime_ms = get_u64(words, 8),
        .last_completed_step = words[10],
        .current_step = words[11],
        .outcome = (bwg_prep_outcome_t)outcome,
        .failure = (bwg_prep_failure_t)failure,
        .has_heap_free = (words[13] & 1u) != 0,
        .has_heap_largest = (words[13] & 2u) != 0,
        .has_stack_free = (words[13] & 4u) != 0,
        .heap_free = words[14],
        .heap_largest = words[15],
        .stack_free = words[16],
    };
    bwg_prep_slot_t reencoded;
    return bwg_prep_encode(receipt, reencoded) ? BWG_PREP_STATUS_VALID : BWG_PREP_STATUS_CORRUPT;
}

void bwg_prep_read_slot(const volatile uint32_t *slot, bwg_prep_slot_t words)
{
    const uint32_t before = slot[0];
    for (size_t index = 1; index < BWG_PREP_WORDS; index++) {
        words[index] = slot[index];
    }
    const uint32_t after = slot[0];
    words[0] = before == after ? before : WRITING;
}

void bwg_prep_commit(const bwg_prep_slot_t words, volatile uint32_t *slot)
{
    slot[0] = WRITING;
    for (size_t index = 1; index < BWG_PREP_WORDS; index++) {
        slot[index] = words[index];
    }
    slot[0] = VALID;
}

void bwg_prep_snapshot_unavailable(bwg_prep_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->status = BWG_PREP_STATUS_UNAVAILABLE;
    snapshot->slot = -1;
}

static bool is_interrupted(bwg_prep_status_t status)
{
    return status == BWG_PREP_STATUS_INCOMPLETE || status == BWG_PREP_STATUS_CORRUPT;
}

/** Two valid receipts of one boot that can't both be true: one sequence with two contents, or a generation going back. */
static bool contradict(const bwg_prep_slot_t left_words, const bwg_prep_slot_t right_words,
                       const bwg_prep_receipt_t *left, const bwg_prep_receipt_t *right)
{
    if (left->boot_ordinal != right->boot_ordinal) {
        return false;
    }
    return (left->sequence == right->sequence && memcmp(left_words, right_words, sizeof(bwg_prep_slot_t)) != 0) ||
           (left->sequence < right->sequence && left->generation > right->generation) ||
           (right->sequence < left->sequence && right->generation > left->generation);
}

static bool newer(const bwg_prep_receipt_t *receipt, const bwg_prep_receipt_t *old)
{
    return receipt->boot_ordinal > old->boot_ordinal ||
           (receipt->boot_ordinal == old->boot_ordinal && receipt->sequence > old->sequence);
}

void bwg_prep_recover(const bwg_prep_slot_t slots[BWG_PREP_SLOTS], uint64_t expected_source,
                      bwg_prep_snapshot_t *snapshot)
{
    bwg_prep_snapshot_unavailable(snapshot);
    bwg_prep_receipt_t receipts[BWG_PREP_SLOTS];
    const bwg_prep_status_t left = decode(slots[0], &receipts[0]);
    const bwg_prep_status_t right = decode(slots[1], &receipts[1]);
    const bool left_valid = left == BWG_PREP_STATUS_VALID;
    const bool right_valid = right == BWG_PREP_STATUS_VALID;
    if (left_valid && right_valid && contradict(slots[0], slots[1], &receipts[0], &receipts[1])) {
        snapshot->status = BWG_PREP_STATUS_CORRUPT;
        return;
    }
    if (!left_valid && !right_valid) {
        snapshot->status = left == BWG_PREP_STATUS_CORRUPT || right == BWG_PREP_STATUS_CORRUPT
                               ? BWG_PREP_STATUS_CORRUPT
                           : left == BWG_PREP_STATUS_INCOMPLETE || right == BWG_PREP_STATUS_INCOMPLETE
                               ? BWG_PREP_STATUS_INCOMPLETE
                               : BWG_PREP_STATUS_UNAVAILABLE;
        return;
    }
    const int latest = left_valid && (!right_valid || !newer(&receipts[1], &receipts[0])) ? 0 : 1;
    snapshot->slot = latest;
    if (receipts[latest].source_hash != expected_source) {
        snapshot->status = BWG_PREP_STATUS_WRONG_FIRMWARE;
        return;
    }
    snapshot->status = BWG_PREP_STATUS_VALID;
    snapshot->interrupted = is_interrupted(latest == 0 ? right : left);
    snapshot->has_receipt = true;
    snapshot->receipt = receipts[latest];
}

void bwg_prep_recover_for_boot(const bwg_prep_slot_t slots[BWG_PREP_SLOTS], uint64_t expected_source, uint64_t boot,
                               bwg_prep_snapshot_t *snapshot)
{
    bwg_prep_slot_t scoped[BWG_PREP_SLOTS];
    memcpy(scoped, slots, sizeof(scoped));
    for (size_t slot = 0; slot < BWG_PREP_SLOTS; slot++) {
        bwg_prep_receipt_t receipt;
        if (decode(slots[slot], &receipt) == BWG_PREP_STATUS_VALID && receipt.boot_ordinal != boot) {
            memset(scoped[slot], 0, sizeof(bwg_prep_slot_t));
        }
    }
    bwg_prep_recover(scoped, expected_source, snapshot);
}

void bwg_prep_snapshot_for_boot(bwg_prep_snapshot_t *snapshot, uint64_t boot)
{
    if (snapshot->has_receipt && snapshot->receipt.boot_ordinal != boot) {
        bwg_prep_snapshot_unavailable(snapshot);
    }
}

static const char *status_label(bwg_prep_status_t status)
{
    switch (status) {
    case BWG_PREP_STATUS_VALID:
        return "valid";
    case BWG_PREP_STATUS_INCOMPLETE:
        return "incomplete";
    case BWG_PREP_STATUS_CORRUPT:
        return "corrupt";
    case BWG_PREP_STATUS_WRONG_FIRMWARE:
        return "wrong_firmware";
    case BWG_PREP_STATUS_UNAVAILABLE:
        return "unavailable";
    }
    return "unavailable";
}

static const char *outcome_label(bwg_prep_outcome_t outcome)
{
    switch (outcome) {
    case BWG_PREP_STARTED:
        return "started";
    case BWG_PREP_COMPLETED:
        return "completed";
    case BWG_PREP_FAILED:
        return "failed";
    }
    return "failed";
}

static const char *failure_label(bwg_prep_failure_t failure)
{
    switch (failure) {
    case BWG_PREP_FAILURE_NONE:
        return "none";
    case BWG_PREP_FAILURE_CANCELLED:
        return "cancelled";
    case BWG_PREP_FAILURE_SAFETY_UNAVAILABLE:
        return "safety_unavailable";
    case BWG_PREP_FAILURE_OWNER_UNAVAILABLE:
        return "owner_unavailable";
    case BWG_PREP_FAILURE_QUEUE_FULL:
        return "queue_full";
    case BWG_PREP_FAILURE_REPLY_TIMEOUT:
        return "reply_timeout";
    case BWG_PREP_FAILURE_HARDWARE_WRITE_FAILED:
        return "hardware_write_failed";
    case BWG_PREP_FAILURE_FAN_TIMEOUT:
        return "fan_timeout";
    case BWG_PREP_FAILURE_UNSUPPORTED_PROFILE:
        return "unsupported_profile";
    case BWG_PREP_FAILURE_ASIC_FAILED:
        return "asic_failed";
    case BWG_PREP_FAILURE_ASIC_PLAN_INVALID:
        return "asic_plan_invalid";
    case BWG_PREP_FAILURE_COOLING_TIMEOUT:
        return "cooling_timeout";
    case BWG_PREP_FAILURE_COOLING_PROOF_REQUIRED:
        return "cooling_proof_required";
    }
    return "none";
}

static const char *metric(char text[METRIC_TEXT_BYTES], bool present, uint32_t value)
{
    if (!present) {
        return "unavailable";
    }
    snprintf(text, METRIC_TEXT_BYTES, "%" PRIu32, value);
    return text;
}

bool bwg_prep_format(char *out, size_t size, const bwg_prep_snapshot_t *snapshot, bool previous_boot)
{
    const char *origin = previous_boot ? "previous_boot" : "current_boot";
    int written;
    if (!snapshot->has_receipt) {
        written = snprintf(out, size, "worker_preparation_receipt schema=v1 origin=%s status=%s redacted=true", origin,
                           status_label(snapshot->status));
    } else {
        const bwg_prep_receipt_t *r = &snapshot->receipt;
        char heap_free[METRIC_TEXT_BYTES];
        char heap_largest[METRIC_TEXT_BYTES];
        char stack_free[METRIC_TEXT_BYTES];
        written = snprintf(out, size,
                           "worker_preparation_receipt schema=v1 origin=%s status=%s interrupted=%s source_hash=%016" PRIx64
                           " boot_ordinal=%" PRIu64 " generation=%" PRIu32 " sequence=%" PRIu32 " uptime_ms=%" PRIu64
                           " last_completed_step=%" PRIu32 " current_step=%" PRIu32
                           " outcome=%s failure=%s heap_free=%s heap_largest=%s stack_free=%s redacted=true",
                           origin, status_label(snapshot->status), snapshot->interrupted ? "true" : "false",
                           r->source_hash, r->boot_ordinal, r->generation, r->sequence, r->uptime_ms,
                           r->last_completed_step, r->current_step, outcome_label(r->outcome),
                           failure_label(r->failure), metric(heap_free, r->has_heap_free, r->heap_free),
                           metric(heap_largest, r->has_heap_largest, r->heap_largest),
                           metric(stack_free, r->has_stack_free, r->stack_free));
    }
    return written > 0 && (size_t)written < size;
}
