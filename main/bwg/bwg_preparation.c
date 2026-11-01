#include "bwg_preparation.h"

#include <stdatomic.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

RTC_NOINIT_ATTR static volatile uint32_t s_slots[BWG_PREP_SLOTS][BWG_PREP_WORDS];

static bwg_prep_snapshot_t s_previous;
static uint64_t s_source_hash;
static uint64_t s_boot_ordinal;
static _Atomic uint32_t s_generation;
// The owner task and the power task both record. Whoever finds the other mid-write drops its record, never waits.
static atomic_flag s_writing = ATOMIC_FLAG_INIT;
static uint32_t s_next_slot;
static _Atomic uint32_t s_sequence;
static uint32_t s_recorded_generation;
static uint32_t s_last_completed;

static void read_slots(bwg_prep_slot_t slots[BWG_PREP_SLOTS])
{
    for (size_t slot = 0; slot < BWG_PREP_SLOTS; slot++) {
        bwg_prep_read_slot(s_slots[slot], slots[slot]);
    }
}

void bwg_preparation_init(uint64_t source_hash, uint64_t boot_ordinal, bool cold_boot)
{
    s_source_hash = source_hash;
    s_boot_ordinal = boot_ordinal;
    if (cold_boot) {
        // Power loss leaves these words undefined, and nothing can be said about a boot before it.
        for (size_t slot = 0; slot < BWG_PREP_SLOTS; slot++) {
            for (size_t word = 0; word < BWG_PREP_WORDS; word++) {
                s_slots[slot][word] = 0;
            }
        }
    }
    bwg_prep_slot_t slots[BWG_PREP_SLOTS];
    read_slots(slots);
    bwg_prep_recover(slots, source_hash, &s_previous);
    s_next_slot = s_previous.slot == 0 ? 1 : 0;
    bwg_prep_snapshot_for_boot(&s_previous, boot_ordinal - 1);
}

void bwg_preparation_begin(uint32_t generation)
{
    atomic_store(&s_generation, generation);
}

void bwg_preparation_end(void)
{
    atomic_store(&s_generation, 0);
}

static void record(uint32_t generation, uint32_t step, bwg_prep_outcome_t outcome, bwg_prep_failure_t failure,
                   const bwg_prep_receipt_t *resources)
{
    if (atomic_flag_test_and_set(&s_writing)) {
        return;
    }
    const uint32_t sequence = atomic_load(&s_sequence) + 1;
    const uint32_t last = s_recorded_generation == generation ? s_last_completed : 0;
    const uint32_t completed = outcome == BWG_PREP_COMPLETED ? step : last;
    bwg_prep_receipt_t receipt = *resources;
    receipt.source_hash = s_source_hash;
    receipt.boot_ordinal = s_boot_ordinal;
    receipt.generation = generation;
    receipt.sequence = sequence;
    receipt.last_completed_step = completed;
    receipt.current_step = step;
    receipt.outcome = outcome;
    receipt.failure = failure;
    bwg_prep_slot_t words;
    if (bwg_prep_encode(&receipt, words)) {
        bwg_prep_commit(words, s_slots[s_next_slot]);
        s_next_slot = 1 - s_next_slot;
        atomic_store(&s_sequence, sequence);
        s_recorded_generation = generation;
        s_last_completed = completed;
    }
    atomic_flag_clear(&s_writing);
}

static void note(uint32_t step, bwg_prep_outcome_t outcome, bwg_prep_failure_t failure)
{
    const uint32_t generation = atomic_load(&s_generation);
    if (generation == 0) {
        return;
    }
    bwg_prep_receipt_t resources = {.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000)};
    // The boundary is committed before the runtime queries, in case what's being diagnosed is the heap.
    record(generation, step, outcome, failure, &resources);
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    resources.has_heap_free = true;
    resources.heap_free = (uint32_t)heap_caps_get_free_size(caps);
    resources.has_heap_largest = true;
    resources.heap_largest = (uint32_t)heap_caps_get_largest_free_block(caps);
    resources.has_stack_free = true;
    // In bytes: this port's stack type is one byte wide.
    resources.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    record(generation, step, outcome, failure, &resources);
}

void bwg_preparation_started(uint32_t step)
{
    note(step, BWG_PREP_STARTED, BWG_PREP_FAILURE_NONE);
}

void bwg_preparation_completed(uint32_t step)
{
    note(step, BWG_PREP_COMPLETED, BWG_PREP_FAILURE_NONE);
}

void bwg_preparation_failed(uint32_t step, bwg_prep_failure_t failure)
{
    note(step, BWG_PREP_FAILED, failure);
}

bool bwg_preparation_format_line(char *out, size_t size, bool previous_boot)
{
    if (previous_boot) {
        return bwg_prep_format(out, size, &s_previous, true);
    }
    bwg_prep_snapshot_t snapshot;
    if (atomic_load(&s_sequence) == 0) {
        bwg_prep_snapshot_unavailable(&snapshot);
    } else {
        bwg_prep_slot_t slots[BWG_PREP_SLOTS];
        read_slots(slots);
        bwg_prep_recover_for_boot(slots, s_source_hash, s_boot_ordinal, &snapshot);
    }
    return bwg_prep_format(out, size, &snapshot, false);
}
