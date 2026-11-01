#ifndef BWG_PREPARATION_RECEIPT_H
#define BWG_PREPARATION_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Integrity-checked progress of a lease's Start that survives a reset. Evidence, never authority. */
#define BWG_PREP_WORDS 18u
#define BWG_PREP_SLOTS 2u
#define BWG_PREP_LAST_STEP 9u

typedef enum {
    BWG_PREP_STARTED = 1,
    BWG_PREP_COMPLETED = 2,
    BWG_PREP_FAILED = 3,
} bwg_prep_outcome_t;

typedef enum {
    BWG_PREP_FAILURE_NONE = 0,
    BWG_PREP_FAILURE_CANCELLED = 1,
    BWG_PREP_FAILURE_SAFETY_UNAVAILABLE = 2,
    BWG_PREP_FAILURE_OWNER_UNAVAILABLE = 3,
    BWG_PREP_FAILURE_QUEUE_FULL = 4,
    BWG_PREP_FAILURE_REPLY_TIMEOUT = 5,
    BWG_PREP_FAILURE_HARDWARE_WRITE_FAILED = 6,
    BWG_PREP_FAILURE_FAN_TIMEOUT = 7,
    BWG_PREP_FAILURE_UNSUPPORTED_PROFILE = 8,
    BWG_PREP_FAILURE_ASIC_FAILED = 9,
    BWG_PREP_FAILURE_ASIC_PLAN_INVALID = 10,
    BWG_PREP_FAILURE_COOLING_TIMEOUT = 11,
    BWG_PREP_FAILURE_COOLING_PROOF_REQUIRED = 12,
} bwg_prep_failure_t;

typedef enum {
    BWG_PREP_STATUS_VALID,
    BWG_PREP_STATUS_INCOMPLETE,
    BWG_PREP_STATUS_CORRUPT,
    BWG_PREP_STATUS_UNAVAILABLE,
    BWG_PREP_STATUS_WRONG_FIRMWARE,
} bwg_prep_status_t;

typedef struct {
    uint64_t source_hash;
    uint64_t boot_ordinal;
    uint32_t generation;
    uint32_t sequence;
    uint64_t uptime_ms;
    uint32_t last_completed_step;
    uint32_t current_step;
    bwg_prep_outcome_t outcome;
    bwg_prep_failure_t failure;
    bool has_heap_free;
    bool has_heap_largest;
    bool has_stack_free;
    uint32_t heap_free;
    uint32_t heap_largest;
    uint32_t stack_free;
} bwg_prep_receipt_t;

typedef struct {
    bwg_prep_status_t status;
    bool interrupted;
    bool has_receipt;
    bwg_prep_receipt_t receipt;
    /** The slot holding the receipt, or -1. */
    int slot;
} bwg_prep_snapshot_t;

typedef uint32_t bwg_prep_slot_t[BWG_PREP_WORDS];

/** False for a receipt whose step, outcome and failure can't describe one moment of a Start. */
bool bwg_prep_encode(const bwg_prep_receipt_t *receipt, bwg_prep_slot_t words);

/** Reads the validity word twice, so an old commit can't vouch for fields a new write has begun to change. */
void bwg_prep_read_slot(const volatile uint32_t *slot, bwg_prep_slot_t words);

/** Publishes the words last-valid, so a reset part way leaves a slot that reads as incomplete. */
void bwg_prep_commit(const bwg_prep_slot_t words, volatile uint32_t *slot);

/** Selects only committed, verified progress. A partial write never invents a newer step. */
void bwg_prep_recover(const bwg_prep_slot_t slots[BWG_PREP_SLOTS], uint64_t expected_source,
                      bwg_prep_snapshot_t *snapshot);

/** As recover, but no older boot's committed receipt can stand as this boot's. */
void bwg_prep_recover_for_boot(const bwg_prep_slot_t slots[BWG_PREP_SLOTS], uint64_t expected_source, uint64_t boot,
                               bwg_prep_snapshot_t *snapshot);

/** Drops a receipt that isn't from this boot. */
void bwg_prep_snapshot_for_boot(bwg_prep_snapshot_t *snapshot, uint64_t boot);

void bwg_prep_snapshot_unavailable(bwg_prep_snapshot_t *snapshot);

bool bwg_prep_format(char *out, size_t size, const bwg_prep_snapshot_t *snapshot, bool previous_boot);

#endif // BWG_PREPARATION_RECEIPT_H
