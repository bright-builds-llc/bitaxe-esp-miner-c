#include <stdio.h>
#include <string.h>

#include "bwg_cadence.h"
#include "bwg_test.h"
#include "test_suites.h"
#include "yyjson.h"

#define SECOND_US 1000000ull
#define STEP_US 600000ull
#define LOOP_START_US 1000ull
#define ARM_US 2000ull
#define FIRST_STEP_US (LOOP_START_US + STEP_US)
#define SECOND_STEP_US (LOOP_START_US + 2 * STEP_US)

static char s_text[BWG_CADENCE_REVIEW_BYTES];

static void iterate_from(bwg_cadence_t *cadence, uint64_t started_at_us, uint32_t cpu, uint32_t priority)
{
    const bwg_cadence_iteration_t iteration = {
        .started_at_us = started_at_us,
        .finished_at_us = started_at_us + 1000,
        .subscribers = 1,
        .cpu = cpu,
        .priority = priority,
    };
    bwg_cadence_note_iteration(cadence, &iteration);
}

static bool arm_at(bwg_cadence_t *cadence, bwg_cadence_phase_t phase, uint64_t at_us, uint32_t generation)
{
    char reply[BWG_CADENCE_ARM_REPLY_BYTES];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, (uint8_t *)reply, sizeof(reply));
    return bwg_cadence_arm(cadence, phase, at_us, generation, 1, &writer);
}

/** Runs the loop from `from_us`, as the board's contract wants it, until past `until_us`. */
static uint64_t run_contract_loop(bwg_cadence_t *cadence, uint64_t from_us, uint64_t until_us)
{
    uint64_t at = from_us;
    for (; at <= until_us; at += STEP_US) {
        iterate_from(cadence, at, 0, 5);
    }
    return at;
}

static yyjson_doc *read_review(bwg_cadence_t *cadence)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, (uint8_t *)s_text, sizeof(s_text));
    bwg_cadence_write_review(cadence, &writer);
    return bwg_jw_ok(&writer) ? yyjson_read(s_text, writer.len, 0) : NULL;
}

static yyjson_val *phase_of(yyjson_doc *doc, bwg_cadence_phase_t phase)
{
    return yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "phases"), phase);
}

static uint64_t field(yyjson_val *object, const char *key)
{
    return yyjson_get_uint(yyjson_obj_get(object, key));
}

static bool flag(yyjson_val *object, const char *key)
{
    return yyjson_get_bool(yyjson_obj_get(object, key));
}

static void arming_needs_the_loop_to_have_run(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};

    // Act
    const bool armed = arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0);

    // Assert
    ASSERT_FALSE(armed);
}

static void the_arm_reply_names_the_phase_and_time(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    char reply[BWG_CADENCE_ARM_REPLY_BYTES];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, (uint8_t *)reply, sizeof(reply));

    // Act
    const bool armed = bwg_cadence_arm(&cadence, BWG_CADENCE_IDLE, ARM_US, 0, 1, &writer);

    // Assert
    ASSERT_TRUE(armed);
    yyjson_doc *doc = yyjson_read(reply, writer.len, 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(root, "schema")), "worker-telemetry-cadence-arm-v1");
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(root, "phase")), "idle");
    ASSERT_EQ(field(root, "armedAtUs"), ARM_US);
    yyjson_doc_free(doc);
}

static void a_phase_arms_only_once(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));

    // Act
    const bool again = arm_at(&cadence, BWG_CADENCE_IDLE, 2 * ARM_US, 0);

    // Assert
    ASSERT_FALSE(again);
}

static void a_later_phase_waits_for_the_one_before(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));

    // Act
    const bool armed = arm_at(&cadence, BWG_CADENCE_USB, 2 * SECOND_US, 0);

    // Assert
    ASSERT_FALSE(armed);
}

static void a_capture_ends_after_sixty_seconds_and_passes_on_contract(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));

    // Act
    run_contract_loop(&cadence, FIRST_STEP_US, 62 * SECOND_US);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *idle = phase_of(doc, BWG_CADENCE_IDLE);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(idle, "state")), "complete");
    ASSERT_TRUE(flag(idle, "passed"));
    yyjson_doc_free(doc);
}

static void buckets_add_up_to_the_interval_count(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));

    // Act
    uint64_t at = LOOP_START_US;
    const uint64_t steps[] = {500000, 500001, 750000, 750001, 1500000, 1500001};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        at += steps[i];
        iterate_from(&cadence, at, 0, 5);
    }
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *idle = phase_of(doc, BWG_CADENCE_IDLE);
    yyjson_val *buckets = yyjson_obj_get(idle, "intervalBuckets");
    ASSERT_EQ(yyjson_get_uint(yyjson_arr_get(buckets, 0)), 1u);
    ASSERT_EQ(yyjson_get_uint(yyjson_arr_get(buckets, 1)), 2u);
    ASSERT_EQ(yyjson_get_uint(yyjson_arr_get(buckets, 2)), 2u);
    ASSERT_EQ(yyjson_get_uint(yyjson_arr_get(buckets, 3)), 1u);
    ASSERT_EQ(field(idle, "intervalCount"), 6u);
    yyjson_doc_free(doc);
}

static void another_cores_and_prioritys_loop_never_passes(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, 1000, 1, 2);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));

    // Act
    for (uint64_t at = FIRST_STEP_US; at <= 62 * SECOND_US; at += STEP_US) {
        iterate_from(&cadence, at, 1, 2);
    }
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *idle = phase_of(doc, BWG_CADENCE_IDLE);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(idle, "state")), "complete");
    ASSERT_FALSE(flag(idle, "passed"));
    ASSERT_EQ(field(idle, "cpuMismatchCount"), field(idle, "intervalCount"));
    ASSERT_EQ(field(idle, "priorityMismatchCount"), field(idle, "intervalCount"));
    yyjson_doc_free(doc);
}

static void a_lost_subscriber_fails_the_phase(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));
    const bwg_cadence_iteration_t alone = {.started_at_us = FIRST_STEP_US, .finished_at_us = FIRST_STEP_US + 1, .cpu = 0, .priority = 5};

    // Act
    bwg_cadence_note_iteration(&cadence, &alone);
    run_contract_loop(&cadence, SECOND_STEP_US, 62 * SECOND_US);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *idle = phase_of(doc, BWG_CADENCE_IDLE);
    ASSERT_EQ(field(idle, "subscriberMismatchCount"), 1u);
    ASSERT_FALSE(flag(idle, "passed"));
    yyjson_doc_free(doc);
}

static void a_long_execution_fails_the_phase(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));
    const bwg_cadence_iteration_t slow = {
        .started_at_us = FIRST_STEP_US, .finished_at_us = FIRST_STEP_US + 500001, .subscribers = 1, .cpu = 0, .priority = 5};

    // Act
    bwg_cadence_note_iteration(&cadence, &slow);
    run_contract_loop(&cadence, SECOND_STEP_US, 62 * SECOND_US);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_FALSE(flag(phase_of(doc, BWG_CADENCE_IDLE), "passed"));
    yyjson_doc_free(doc);
}

static void a_clock_running_backwards_is_counted(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, 5 * SECOND_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, 6 * SECOND_US, 0));

    // Act
    iterate_from(&cadence, SECOND_US, 0, 5);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_EQ(field(phase_of(doc, BWG_CADENCE_IDLE), "clockFailures"), 1u);
    yyjson_doc_free(doc);
}

static uint64_t complete_idle(bwg_cadence_t *cadence)
{
    iterate_from(cadence, LOOP_START_US, 0, 5);
    arm_at(cadence, BWG_CADENCE_IDLE, ARM_US, 0);
    return run_contract_loop(cadence, FIRST_STEP_US, 62 * SECOND_US);
}

static void the_usb_phase_needs_twelve_probes_to_pass(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    const uint64_t at = complete_idle(&cadence);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_USB, at, 0));

    // Act
    for (uint32_t i = 0; i < 11; i++) {
        bwg_cadence_note_max_probe(&cadence, at + (i + 1) * SECOND_US);
    }
    run_contract_loop(&cadence, at, at + 61 * SECOND_US);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *usb = phase_of(doc, BWG_CADENCE_USB);
    ASSERT_EQ(field(usb, "maxProbeCount"), 11u);
    ASSERT_FALSE(flag(usb, "passed"));
    yyjson_doc_free(doc);
}

static void twelve_probes_pass_the_usb_phase(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    const uint64_t at = complete_idle(&cadence);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_USB, at, 0));

    // Act
    for (uint32_t i = 0; i < 12; i++) {
        bwg_cadence_note_max_probe(&cadence, at + (i + 1) * SECOND_US);
    }
    run_contract_loop(&cadence, at, at + 61 * SECOND_US);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_TRUE(flag(phase_of(doc, BWG_CADENCE_USB), "passed"));
    yyjson_doc_free(doc);
}

static void a_probe_outside_the_capture_is_not_counted(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    const uint64_t at = complete_idle(&cadence);

    // Act
    bwg_cadence_note_max_probe(&cadence, at);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_EQ(field(phase_of(doc, BWG_CADENCE_USB), "maxProbeCount"), 0u);
    yyjson_doc_free(doc);
}

static uint64_t complete_idle_and_usb(bwg_cadence_t *cadence)
{
    const uint64_t at = complete_idle(cadence);
    arm_at(cadence, BWG_CADENCE_USB, at, 0);
    return run_contract_loop(cadence, at, at + 61 * SECOND_US);
}

static void mining_waits_for_its_generations_first_work(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    const uint64_t at = complete_idle_and_usb(&cadence);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_MINING, at, 3));

    // Act
    uint32_t generation = 0;
    const bool waiting = bwg_cadence_awaits_dispatch(&cadence, &generation);
    bwg_cadence_note_dispatch(&cadence, 2, at + 1, 1);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_TRUE(waiting);
    ASSERT_EQ(generation, 3u);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(phase_of(doc, BWG_CADENCE_MINING), "state")), "armed");
    yyjson_doc_free(doc);
}

static void mining_captures_from_the_first_work(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    const uint64_t at = complete_idle_and_usb(&cadence);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_MINING, at, 3));

    // Act
    bwg_cadence_note_dispatch(&cadence, 3, at + SECOND_US, 1);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *mining = phase_of(doc, BWG_CADENCE_MINING);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(mining, "state")), "capturing");
    ASSERT_EQ(field(mining, "startedAtUs"), at + SECOND_US);
    ASSERT_EQ(field(mining, "generation"), 3u);
    yyjson_doc_free(doc);
}

static void mining_needs_a_generation(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    const uint64_t at = complete_idle_and_usb(&cadence);

    // Act
    const bool armed = arm_at(&cadence, BWG_CADENCE_MINING, at, 0);

    // Assert
    ASSERT_FALSE(armed);
}

static void publications_and_sends_count_only_during_a_capture(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    bwg_cadence_note_publication(&cadence, BWG_CADENCE_PROJECTED);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));

    // Act
    bwg_cadence_note_publication(&cadence, BWG_CADENCE_UNCHANGED);
    bwg_cadence_note_publication(&cadence, BWG_CADENCE_SERIALIZATION_FAILED);
    bwg_cadence_note_send(&cadence, true);
    bwg_cadence_note_send(&cadence, false);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    yyjson_val *idle = phase_of(doc, BWG_CADENCE_IDLE);
    ASSERT_EQ(field(idle, "projectionCount"), 0u);
    ASSERT_EQ(field(idle, "unchangedCount"), 1u);
    ASSERT_EQ(field(idle, "serializationFailures"), 1u);
    ASSERT_EQ(field(idle, "sendsQueued"), 2u);
    ASSERT_EQ(field(idle, "sendsCompleted"), 2u);
    ASSERT_EQ(field(idle, "sendFailures"), 1u);
    yyjson_doc_free(doc);
}

static void a_busy_recorder_loses_the_note_while_capturing(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));
    atomic_flag_test_and_set(&cadence.busy);

    // Act
    bwg_cadence_note_send(&cadence, true);
    atomic_flag_clear(&cadence.busy);
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_EQ(field(yyjson_doc_get_root(doc), "droppedObservations"), 1u);
    yyjson_doc_free(doc);
}

static void a_busy_recorder_reviews_as_unavailable(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    iterate_from(&cadence, LOOP_START_US, 0, 5);
    ASSERT_TRUE(arm_at(&cadence, BWG_CADENCE_IDLE, ARM_US, 0));
    atomic_flag_test_and_set(&cadence.busy);

    // Act
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_FALSE(flag(yyjson_doc_get_root(doc), "snapshotAvailable"));
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(phase_of(doc, BWG_CADENCE_IDLE), "state")), "empty");
    ASSERT_TRUE(atomic_flag_test_and_set(&cadence.busy));
    yyjson_doc_free(doc);
}

static void a_full_review_fits_its_reply(void)
{
    // Arrange
    bwg_cadence_t cadence = {0};
    memset(cadence.phases, 0, sizeof(cadence.phases));
    for (uint32_t i = 0; i < BWG_CADENCE_PHASES; i++) {
        bwg_cadence_summary_t *summary = &cadence.phases[i];
        memset(summary, 0xff, sizeof(*summary));
        summary->state = BWG_CADENCE_CAPTURING;
        summary->overflow = true;
        summary->armed_at_us = 9007199254740991ull;
        summary->started_at_us = 9007199254740991ull;
        summary->ended_at_us = 9007199254740991ull;
        summary->first_max_probe_at_us = 9007199254740991ull;
        summary->last_max_probe_at_us = 9007199254740991ull;
        summary->maximum_interval_us = 9007199254740991ull;
        summary->maximum_execution_us = 9007199254740991ull;
    }

    // Act
    yyjson_doc *doc = read_review(&cadence);

    // Assert
    ASSERT_TRUE(doc != NULL);
    yyjson_doc_free(doc);
}

void bwg_tests_cadence(void)
{
    BWG_RUN(arming_needs_the_loop_to_have_run);
    BWG_RUN(the_arm_reply_names_the_phase_and_time);
    BWG_RUN(a_phase_arms_only_once);
    BWG_RUN(a_later_phase_waits_for_the_one_before);
    BWG_RUN(a_capture_ends_after_sixty_seconds_and_passes_on_contract);
    BWG_RUN(buckets_add_up_to_the_interval_count);
    BWG_RUN(another_cores_and_prioritys_loop_never_passes);
    BWG_RUN(a_lost_subscriber_fails_the_phase);
    BWG_RUN(a_long_execution_fails_the_phase);
    BWG_RUN(a_clock_running_backwards_is_counted);
    BWG_RUN(the_usb_phase_needs_twelve_probes_to_pass);
    BWG_RUN(twelve_probes_pass_the_usb_phase);
    BWG_RUN(a_probe_outside_the_capture_is_not_counted);
    BWG_RUN(mining_waits_for_its_generations_first_work);
    BWG_RUN(mining_captures_from_the_first_work);
    BWG_RUN(mining_needs_a_generation);
    BWG_RUN(publications_and_sends_count_only_during_a_capture);
    BWG_RUN(a_busy_recorder_loses_the_note_while_capturing);
    BWG_RUN(a_busy_recorder_reviews_as_unavailable);
    BWG_RUN(a_full_review_fits_its_reply);
}
