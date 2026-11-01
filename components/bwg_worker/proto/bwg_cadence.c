#include "bwg_cadence.h"

#include <stddef.h>
#include <string.h>

#define CAPTURE_DURATION_US 60000000ull
#define BUCKET_LIMIT_0_US 500000ull
#define BUCKET_LIMIT_1_US 750000ull
#define BUCKET_LIMIT_2_US 1500000ull
#define MAXIMUM_INTERVAL_US BUCKET_LIMIT_2_US
#define MAXIMUM_EXECUTION_US 500000ull
#define MINIMUM_INTERVALS 60u
#define USB_MAX_PROBES 12u
#define IN_TIME_PERCENT 95u
// The Gate's contract names the core and priority of the Rust image's telemetry loop.
#define CONTRACT_CPU 0u
#define CONTRACT_PRIORITY 5u

static const char *const PHASE_LABELS[BWG_CADENCE_PHASES] = {"idle", "usb", "mining"};
static const char *const STATE_LABELS[] = {
    [BWG_CADENCE_EMPTY] = "empty",
    [BWG_CADENCE_ARMED] = "armed",
    [BWG_CADENCE_CAPTURING] = "capturing",
    [BWG_CADENCE_COMPLETE] = "complete",
};

static bool acquire(bwg_cadence_t *cadence)
{
    if (!atomic_flag_test_and_set_explicit(&cadence->busy, memory_order_acquire)) {
        return true;
    }
    if (atomic_load_explicit(&cadence->enabled, memory_order_relaxed)) {
        atomic_store_explicit(&cadence->dropped, 1, memory_order_relaxed);
    }
    return false;
}

static void release(bwg_cadence_t *cadence)
{
    atomic_flag_clear_explicit(&cadence->busy, memory_order_release);
}

static void increment(uint32_t *counter, bool *overflow)
{
    if (*counter == UINT32_MAX) {
        *overflow = true;
        return;
    }
    (*counter)++;
}

static bwg_cadence_summary_t *maybe_capturing(bwg_cadence_t *cadence)
{
    for (uint32_t i = 0; i < BWG_CADENCE_PHASES; i++) {
        if (cadence->phases[i].state == BWG_CADENCE_CAPTURING) {
            return &cadence->phases[i];
        }
    }
    return NULL;
}

static void write_arm_reply(bwg_cadence_phase_t phase, const bwg_cadence_summary_t *summary, bwg_jw_t *writer)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "armedAtUs");
    bwg_jw_u64(writer, summary->armed_at_us);
    bwg_jw_key(writer, "generation");
    bwg_jw_u64(writer, summary->generation);
    bwg_jw_key(writer, "phase");
    bwg_jw_cstr(writer, PHASE_LABELS[phase]);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-telemetry-cadence-arm-v1");
    bwg_jw_object_end(writer);
}

static bool may_arm(const bwg_cadence_t *cadence, bwg_cadence_phase_t phase, uint64_t at_us, uint32_t generation)
{
    if (at_us == 0 || (phase == BWG_CADENCE_MINING && generation == 0) ||
        cadence->phases[phase].state != BWG_CADENCE_EMPTY || !cadence->has_previous ||
        cadence->previous.started_at_us > at_us) {
        return false;
    }
    for (uint32_t i = 0; i < (uint32_t)phase; i++) {
        if (cadence->phases[i].state != BWG_CADENCE_COMPLETE) {
            return false;
        }
    }
    return true;
}

bool bwg_cadence_arm(bwg_cadence_t *cadence, bwg_cadence_phase_t phase, uint64_t at_us, uint32_t generation,
                     uint32_t subscribers, bwg_jw_t *writer)
{
    if (!acquire(cadence)) {
        return false;
    }
    const bool allowed = may_arm(cadence, phase, at_us, generation);
    if (allowed) {
        bwg_cadence_summary_t *summary = &cadence->phases[phase];
        summary->armed_at_us = at_us;
        summary->generation = phase == BWG_CADENCE_MINING ? generation : 0;
        if (phase == BWG_CADENCE_MINING) {
            summary->state = BWG_CADENCE_ARMED;
        } else {
            summary->started_at_us = at_us;
            summary->state = BWG_CADENCE_CAPTURING;
        }
        if (subscribers != 1) {
            summary->subscriber_mismatch_count = 1;
        }
        atomic_store_explicit(&cadence->enabled, true, memory_order_relaxed);
        write_arm_reply(phase, summary, writer);
    }
    release(cadence);
    return allowed;
}

bool bwg_cadence_awaits_dispatch(bwg_cadence_t *cadence, uint32_t *generation)
{
    if (!acquire(cadence)) {
        return false;
    }
    const bwg_cadence_summary_t *mining = &cadence->phases[BWG_CADENCE_MINING];
    const bool waiting = mining->state == BWG_CADENCE_ARMED;
    *generation = mining->generation;
    release(cadence);
    return waiting;
}

void bwg_cadence_note_dispatch(bwg_cadence_t *cadence, uint32_t generation, uint64_t at_us, uint32_t subscribers)
{
    if (!acquire(cadence)) {
        return;
    }
    bwg_cadence_summary_t *mining = &cadence->phases[BWG_CADENCE_MINING];
    if (mining->state == BWG_CADENCE_ARMED && mining->generation == generation) {
        if (at_us < mining->armed_at_us) {
            increment(&mining->clock_failures, &mining->overflow);
        } else {
            mining->started_at_us = at_us;
            mining->state = BWG_CADENCE_CAPTURING;
            if (subscribers != 1) {
                increment(&mining->subscriber_mismatch_count, &mining->overflow);
            }
        }
    }
    release(cadence);
}

static uint32_t bucket_for(uint64_t interval_us)
{
    if (interval_us <= BUCKET_LIMIT_0_US) {
        return 0;
    }
    if (interval_us <= BUCKET_LIMIT_1_US) {
        return 1;
    }
    return interval_us <= BUCKET_LIMIT_2_US ? 2 : 3;
}

static bool iteration_is_ordered(const bwg_cadence_iteration_t *previous, const bwg_cadence_iteration_t *iteration)
{
    return previous->started_at_us < iteration->started_at_us && previous->started_at_us <= previous->finished_at_us &&
           previous->finished_at_us <= iteration->started_at_us && iteration->started_at_us <= iteration->finished_at_us;
}

static void record_iteration(bwg_cadence_summary_t *summary, const bwg_cadence_iteration_t *previous,
                             const bwg_cadence_iteration_t *iteration)
{
    if (iteration->subscribers != 1) {
        increment(&summary->subscriber_mismatch_count, &summary->overflow);
    }
    if (iteration->cpu != CONTRACT_CPU) {
        increment(&summary->cpu_mismatch_count, &summary->overflow);
    }
    if (iteration->priority != CONTRACT_PRIORITY) {
        increment(&summary->priority_mismatch_count, &summary->overflow);
    }
    if (!iteration_is_ordered(previous, iteration)) {
        increment(&summary->clock_failures, &summary->overflow);
        return;
    }
    const uint64_t interval = iteration->started_at_us - previous->started_at_us;
    const uint64_t execution = iteration->finished_at_us - iteration->started_at_us;
    increment(&summary->interval_count, &summary->overflow);
    increment(&summary->buckets[bucket_for(interval)], &summary->overflow);
    if (interval > summary->maximum_interval_us) {
        summary->maximum_interval_us = interval;
    }
    if (execution > summary->maximum_execution_us) {
        summary->maximum_execution_us = execution;
    }
    if (iteration->started_at_us >= summary->started_at_us &&
        iteration->started_at_us - summary->started_at_us >= CAPTURE_DURATION_US) {
        summary->ended_at_us = iteration->finished_at_us;
        summary->state = BWG_CADENCE_COMPLETE;
    }
}

void bwg_cadence_note_iteration(bwg_cadence_t *cadence, const bwg_cadence_iteration_t *iteration)
{
    if (!acquire(cadence)) {
        return;
    }
    bwg_cadence_summary_t *maybe_summary = maybe_capturing(cadence);
    if (maybe_summary != NULL) {
        if (cadence->has_previous) {
            record_iteration(maybe_summary, &cadence->previous, iteration);
        } else {
            increment(&maybe_summary->clock_failures, &maybe_summary->overflow);
        }
    }
    cadence->previous = *iteration;
    cadence->has_previous = true;
    release(cadence);
}

void bwg_cadence_note_publication(bwg_cadence_t *cadence, bwg_cadence_publication_t publication)
{
    if (!acquire(cadence)) {
        return;
    }
    bwg_cadence_summary_t *maybe_summary = maybe_capturing(cadence);
    if (maybe_summary != NULL) {
        uint32_t *const counters[] = {
            [BWG_CADENCE_PROJECTED] = &maybe_summary->projection_count,
            [BWG_CADENCE_UNCHANGED] = &maybe_summary->unchanged_count,
            [BWG_CADENCE_NO_SUBSCRIBERS] = &maybe_summary->no_subscriber_count,
            [BWG_CADENCE_PROJECTION_FAILED] = &maybe_summary->projection_failures,
            [BWG_CADENCE_SERIALIZATION_FAILED] = &maybe_summary->serialization_failures,
        };
        increment(counters[publication], &maybe_summary->overflow);
    }
    release(cadence);
}

void bwg_cadence_note_send(bwg_cadence_t *cadence, bool succeeded)
{
    if (!acquire(cadence)) {
        return;
    }
    bwg_cadence_summary_t *maybe_summary = maybe_capturing(cadence);
    if (maybe_summary != NULL) {
        increment(&maybe_summary->sends, &maybe_summary->overflow);
        if (!succeeded) {
            increment(&maybe_summary->send_failures, &maybe_summary->overflow);
        }
    }
    release(cadence);
}

void bwg_cadence_note_max_probe(bwg_cadence_t *cadence, uint64_t at_us)
{
    if (!acquire(cadence)) {
        return;
    }
    bwg_cadence_summary_t *usb = &cadence->phases[BWG_CADENCE_USB];
    if (usb->state == BWG_CADENCE_CAPTURING && at_us >= usb->started_at_us &&
        at_us - usb->started_at_us < CAPTURE_DURATION_US) {
        if (at_us < usb->last_max_probe_at_us) {
            increment(&usb->clock_failures, &usb->overflow);
        } else {
            if (usb->max_probe_count == 0) {
                usb->first_max_probe_at_us = at_us;
            }
            usb->last_max_probe_at_us = at_us;
            increment(&usb->max_probe_count, &usb->overflow);
        }
    }
    release(cadence);
}

static bool summary_passed(const bwg_cadence_summary_t *summary, bwg_cadence_phase_t phase, uint32_t dropped)
{
    const uint64_t in_time = (uint64_t)summary->buckets[0] + summary->buckets[1];
    return summary->state == BWG_CADENCE_COMPLETE && summary->interval_count >= MINIMUM_INTERVALS &&
           (phase != BWG_CADENCE_USB || summary->max_probe_count == USB_MAX_PROBES) &&
           in_time * 100 >= (uint64_t)summary->interval_count * IN_TIME_PERCENT &&
           summary->maximum_interval_us <= MAXIMUM_INTERVAL_US && summary->maximum_execution_us <= MAXIMUM_EXECUTION_US &&
           summary->cpu_mismatch_count == 0 && summary->priority_mismatch_count == 0 &&
           summary->subscriber_mismatch_count == 0 && summary->projection_failures == 0 &&
           summary->serialization_failures == 0 && summary->send_failures == 0 && summary->clock_failures == 0 &&
           !summary->overflow && dropped == 0;
}

static void write_u64(bwg_jw_t *writer, const char *key, uint64_t value)
{
    bwg_jw_key(writer, key);
    bwg_jw_u64(writer, value);
}

static void write_summary(const bwg_cadence_summary_t *summary, bwg_cadence_phase_t phase, uint32_t dropped, bwg_jw_t *writer)
{
    bwg_jw_object_begin(writer);
    write_u64(writer, "armedAtUs", summary->armed_at_us);
    write_u64(writer, "clockFailures", summary->clock_failures);
    write_u64(writer, "cpuMismatchCount", summary->cpu_mismatch_count);
    write_u64(writer, "endedAtUs", summary->ended_at_us);
    write_u64(writer, "firstMaxProbeAtUs", summary->first_max_probe_at_us);
    write_u64(writer, "generation", summary->generation);
    write_u64(writer, "intervalCount", summary->interval_count);
    bwg_jw_key(writer, "intervalBuckets");
    bwg_jw_array_begin(writer);
    for (uint32_t i = 0; i < BWG_CADENCE_BUCKETS; i++) {
        bwg_jw_u64(writer, summary->buckets[i]);
    }
    bwg_jw_array_end(writer);
    write_u64(writer, "lastMaxProbeAtUs", summary->last_max_probe_at_us);
    write_u64(writer, "maxProbeCount", summary->max_probe_count);
    write_u64(writer, "maximumExecutionUs", summary->maximum_execution_us);
    write_u64(writer, "maximumIntervalUs", summary->maximum_interval_us);
    // This loop has one stage, with logs on their own task and nothing to prune.
    write_u64(writer, "maximumLiveUs", summary->maximum_execution_us);
    write_u64(writer, "maximumLogsUs", 0);
    write_u64(writer, "maximumPruneUs", 0);
    write_u64(writer, "noSubscriberCount", summary->no_subscriber_count);
    bwg_jw_key(writer, "overflow");
    bwg_jw_bool(writer, summary->overflow);
    bwg_jw_key(writer, "passed");
    bwg_jw_bool(writer, summary_passed(summary, phase, dropped));
    write_u64(writer, "pendingSends", 0);
    bwg_jw_key(writer, "phase");
    bwg_jw_cstr(writer, PHASE_LABELS[phase]);
    write_u64(writer, "priorityMismatchCount", summary->priority_mismatch_count);
    write_u64(writer, "projectionCount", summary->projection_count);
    write_u64(writer, "projectionFailures", summary->projection_failures);
    write_u64(writer, "queueFailures", 0);
    write_u64(writer, "sendFailures", summary->send_failures);
    write_u64(writer, "sendsCompleted", summary->sends);
    write_u64(writer, "sendsQueued", summary->sends);
    write_u64(writer, "serializationFailures", summary->serialization_failures);
    write_u64(writer, "startedAtUs", summary->started_at_us);
    bwg_jw_key(writer, "state");
    bwg_jw_cstr(writer, STATE_LABELS[summary->state]);
    write_u64(writer, "subscriberMismatchCount", summary->subscriber_mismatch_count);
    write_u64(writer, "unchangedCount", summary->unchanged_count);
    bwg_jw_object_end(writer);
}

void bwg_cadence_write_review(bwg_cadence_t *cadence, bwg_jw_t *writer)
{
    static const bwg_cadence_summary_t EMPTY = {0};
    const bool held = !atomic_flag_test_and_set_explicit(&cadence->busy, memory_order_acquire);
    const uint32_t dropped = atomic_load_explicit(&cadence->dropped, memory_order_relaxed);
    bwg_jw_object_begin(writer);
    write_u64(writer, "droppedObservations", dropped);
    bwg_jw_key(writer, "phases");
    bwg_jw_array_begin(writer);
    for (uint32_t i = 0; i < BWG_CADENCE_PHASES; i++) {
        write_summary(held ? &cadence->phases[i] : &EMPTY, (bwg_cadence_phase_t)i, dropped, writer);
    }
    bwg_jw_array_end(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-telemetry-cadence-v1");
    bwg_jw_key(writer, "snapshotAvailable");
    bwg_jw_bool(writer, held);
    write_u64(writer, "storageBytes", sizeof(*cadence));
    bwg_jw_object_end(writer);
    if (held) {
        release(cadence);
    }
}
