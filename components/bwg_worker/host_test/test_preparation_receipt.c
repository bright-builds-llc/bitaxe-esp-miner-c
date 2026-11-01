#include <string.h>

#include "bwg_preparation_receipt.h"
#include "bwg_test.h"
#include "test_suites.h"

#define SOURCE 0x0123456789abcdefull

static bwg_prep_receipt_t started_at(uint64_t boot, uint32_t sequence, uint32_t step)
{
    return (bwg_prep_receipt_t) {
        .source_hash = SOURCE,
        .boot_ordinal = boot,
        .generation = 1,
        .sequence = sequence,
        .uptime_ms = 5000,
        .last_completed_step = step - 1,
        .current_step = step,
        .outcome = BWG_PREP_STARTED,
        .failure = BWG_PREP_FAILURE_NONE,
    };
}

static void put_slot(bwg_prep_slot_t slots[BWG_PREP_SLOTS], size_t slot, const bwg_prep_receipt_t *receipt)
{
    ASSERT_TRUE(bwg_prep_encode(receipt, slots[slot]));
}

static void impossible_receipts_are_refused(void)
{
    // Arrange
    bwg_prep_slot_t words;
    bwg_prep_receipt_t failure_without_failed = started_at(3, 1, 4);
    failure_without_failed.failure = BWG_PREP_FAILURE_ASIC_FAILED;
    bwg_prep_receipt_t failed_without_failure = started_at(3, 1, 4);
    failed_without_failure.outcome = BWG_PREP_FAILED;
    bwg_prep_receipt_t completed_elsewhere = started_at(3, 1, 4);
    completed_elsewhere.outcome = BWG_PREP_COMPLETED;
    bwg_prep_receipt_t started_already_done = started_at(3, 1, 4);
    started_already_done.last_completed_step = 4;
    bwg_prep_receipt_t no_generation = started_at(3, 1, 4);
    no_generation.generation = 0;
    bwg_prep_receipt_t step_out_of_range = started_at(3, 1, 4);
    step_out_of_range.current_step = 10;

    // Act / Assert
    ASSERT_FALSE(bwg_prep_encode(&failure_without_failed, words));
    ASSERT_FALSE(bwg_prep_encode(&failed_without_failure, words));
    ASSERT_FALSE(bwg_prep_encode(&completed_elsewhere, words));
    ASSERT_FALSE(bwg_prep_encode(&started_already_done, words));
    ASSERT_FALSE(bwg_prep_encode(&no_generation, words));
    ASSERT_FALSE(bwg_prep_encode(&step_out_of_range, words));
}

static void the_newest_committed_receipt_is_recovered(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS];
    const bwg_prep_receipt_t older = started_at(3, 1, 4);
    const bwg_prep_receipt_t newer = started_at(3, 2, 5);
    put_slot(slots, 0, &older);
    put_slot(slots, 1, &newer);
    bwg_prep_snapshot_t snapshot;

    // Act
    bwg_prep_recover(slots, SOURCE, &snapshot);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_VALID);
    ASSERT_EQ(snapshot.slot, 1);
    ASSERT_EQ(snapshot.receipt.current_step, 5);
    ASSERT_FALSE(snapshot.interrupted);
}

static void a_write_cut_short_falls_back_and_says_so(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS];
    const bwg_prep_receipt_t committed = started_at(3, 1, 4);
    const bwg_prep_receipt_t interrupted = started_at(3, 2, 5);
    put_slot(slots, 0, &committed);
    put_slot(slots, 1, &interrupted);
    slots[1][0] = 0x42585057u;
    bwg_prep_snapshot_t snapshot;

    // Act
    bwg_prep_recover(slots, SOURCE, &snapshot);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_VALID);
    ASSERT_EQ(snapshot.slot, 0);
    ASSERT_EQ(snapshot.receipt.current_step, 4);
    ASSERT_TRUE(snapshot.interrupted);
}

static void empty_slots_are_unavailable(void)
{
    // Arrange
    const bwg_prep_slot_t slots[BWG_PREP_SLOTS] = {{0}, {0}};
    bwg_prep_snapshot_t snapshot;

    // Act
    bwg_prep_recover(slots, SOURCE, &snapshot);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_UNAVAILABLE);
    ASSERT_FALSE(snapshot.has_receipt);
}

static void a_changed_word_is_corrupt(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS] = {{0}, {0}};
    const bwg_prep_receipt_t receipt = started_at(3, 1, 4);
    put_slot(slots, 0, &receipt);
    slots[0][8] ^= 1u;
    bwg_prep_snapshot_t snapshot;

    // Act
    bwg_prep_recover(slots, SOURCE, &snapshot);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_CORRUPT);
    ASSERT_FALSE(snapshot.has_receipt);
}

static void another_firmware_keeps_its_numbers_to_itself(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS] = {{0}, {0}};
    const bwg_prep_receipt_t receipt = started_at(3, 1, 4);
    put_slot(slots, 0, &receipt);
    bwg_prep_snapshot_t snapshot;
    char line[512];

    // Act
    bwg_prep_recover(slots, SOURCE + 1, &snapshot);
    bwg_prep_format(line, sizeof(line), &snapshot, true);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_WRONG_FIRMWARE);
    ASSERT_STREQ(line, "worker_preparation_receipt schema=v1 origin=previous_boot status=wrong_firmware redacted=true");
}

static void a_sequence_with_two_contents_is_corrupt(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS];
    const bwg_prep_receipt_t left = started_at(3, 2, 4);
    const bwg_prep_receipt_t right = started_at(3, 2, 5);
    put_slot(slots, 0, &left);
    put_slot(slots, 1, &right);
    bwg_prep_snapshot_t snapshot;

    // Act
    bwg_prep_recover(slots, SOURCE, &snapshot);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_CORRUPT);
}

static void a_generation_going_back_is_corrupt(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS];
    bwg_prep_receipt_t earlier = started_at(3, 1, 4);
    earlier.generation = 2;
    bwg_prep_receipt_t later = started_at(3, 2, 4);
    later.generation = 1;
    put_slot(slots, 0, &earlier);
    put_slot(slots, 1, &later);
    bwg_prep_snapshot_t snapshot;

    // Act
    bwg_prep_recover(slots, SOURCE, &snapshot);

    // Assert
    ASSERT_EQ(snapshot.status, BWG_PREP_STATUS_CORRUPT);
}

static void a_receipt_from_another_boot_is_not_this_boots(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS] = {{0}, {0}};
    const bwg_prep_receipt_t receipt = started_at(3, 1, 4);
    put_slot(slots, 0, &receipt);
    bwg_prep_snapshot_t scoped;
    bwg_prep_snapshot_t filtered;

    // Act
    bwg_prep_recover_for_boot(slots, SOURCE, 4, &scoped);
    bwg_prep_recover(slots, SOURCE, &filtered);
    bwg_prep_snapshot_for_boot(&filtered, 4);

    // Assert
    ASSERT_EQ(scoped.status, BWG_PREP_STATUS_UNAVAILABLE);
    ASSERT_EQ(filtered.status, BWG_PREP_STATUS_UNAVAILABLE);
}

static void a_valid_line_names_every_field(void)
{
    // Arrange
    bwg_prep_slot_t slots[BWG_PREP_SLOTS] = {{0}, {0}};
    bwg_prep_receipt_t receipt = started_at(3, 7, 8);
    receipt.outcome = BWG_PREP_FAILED;
    receipt.failure = BWG_PREP_FAILURE_ASIC_FAILED;
    receipt.has_heap_free = true;
    receipt.heap_free = 1234;
    receipt.has_stack_free = true;
    receipt.stack_free = 88;
    put_slot(slots, 0, &receipt);
    bwg_prep_snapshot_t snapshot;
    bwg_prep_recover(slots, SOURCE, &snapshot);
    char line[512];

    // Act
    const bool formatted = bwg_prep_format(line, sizeof(line), &snapshot, false);

    // Assert
    ASSERT_TRUE(formatted);
    ASSERT_STREQ(line,
                 "worker_preparation_receipt schema=v1 origin=current_boot status=valid interrupted=false "
                 "source_hash=0123456789abcdef boot_ordinal=3 generation=1 sequence=7 uptime_ms=5000 "
                 "last_completed_step=7 current_step=8 outcome=failed failure=asic_failed heap_free=1234 "
                 "heap_largest=unavailable stack_free=88 redacted=true");
}

static void a_commit_cut_short_reads_as_incomplete(void)
{
    // Arrange
    volatile uint32_t slot[BWG_PREP_WORDS];
    bwg_prep_slot_t words;
    const bwg_prep_receipt_t receipt = started_at(3, 1, 4);
    ASSERT_TRUE(bwg_prep_encode(&receipt, words));
    bwg_prep_commit(words, slot);
    bwg_prep_slot_t read_back[BWG_PREP_SLOTS] = {{0}, {0}};
    bwg_prep_read_slot(slot, read_back[0]);
    bwg_prep_snapshot_t complete;
    bwg_prep_recover(read_back, SOURCE, &complete);

    // Act
    slot[0] = 0x42585057u;
    bwg_prep_read_slot(slot, read_back[0]);
    bwg_prep_snapshot_t cut;
    bwg_prep_recover(read_back, SOURCE, &cut);

    // Assert
    ASSERT_EQ(complete.status, BWG_PREP_STATUS_VALID);
    ASSERT_EQ(cut.status, BWG_PREP_STATUS_INCOMPLETE);
}

void bwg_tests_preparation_receipt(void)
{
    BWG_RUN(impossible_receipts_are_refused);
    BWG_RUN(the_newest_committed_receipt_is_recovered);
    BWG_RUN(a_write_cut_short_falls_back_and_says_so);
    BWG_RUN(empty_slots_are_unavailable);
    BWG_RUN(a_changed_word_is_corrupt);
    BWG_RUN(another_firmware_keeps_its_numbers_to_itself);
    BWG_RUN(a_sequence_with_two_contents_is_corrupt);
    BWG_RUN(a_generation_going_back_is_corrupt);
    BWG_RUN(a_receipt_from_another_boot_is_not_this_boots);
    BWG_RUN(a_valid_line_names_every_field);
    BWG_RUN(a_commit_cut_short_reads_as_incomplete);
}
