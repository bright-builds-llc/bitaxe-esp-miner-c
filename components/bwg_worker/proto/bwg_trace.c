#include "bwg_trace.h"

#include <string.h>

#define MAX_WIRE_BYTES 66560u

static const char *const STAGE_LABELS[] = {
    [BWG_TRACE_HELLO] = "hello",
    [BWG_TRACE_VALIDATED] = "validated",
    [BWG_TRACE_VALIDATION_REJECTED] = "validation_rejected",
    [BWG_TRACE_ENQUEUE_STARTED] = "enqueue_started",
    [BWG_TRACE_ENQUEUED] = "enqueued",
    [BWG_TRACE_ENQUEUE_REJECTED] = "enqueue_rejected",
    [BWG_TRACE_DISPATCH_STARTED] = "dispatch_started",
    [BWG_TRACE_DISPATCH_REJECTED] = "dispatch_rejected",
    [BWG_TRACE_REPLY_CREATED] = "reply_created",
    [BWG_TRACE_REPLY_REJECTED] = "reply_rejected",
    [BWG_TRACE_WRITER_ACCEPTED] = "writer_accepted",
    [BWG_TRACE_WRITER_REJECTED] = "writer_rejected",
    [BWG_TRACE_WRITER_STARTED] = "writer_started",
    [BWG_TRACE_WRITER_QUEUED] = "writer_queued",
    [BWG_TRACE_WRITER_COMPLETED] = "writer_completed",
    [BWG_TRACE_WRITER_ABANDONED] = "writer_abandoned",
    [BWG_TRACE_EPOCH_REVOKED] = "epoch_revoked",
};

void bwg_trace_init(bwg_trace_t *trace, uint64_t (*now_ms)(void))
{
    memset(&trace->current, 0, sizeof(trace->current));
    memset(&trace->previous, 0, sizeof(trace->previous));
    atomic_flag_clear(&trace->busy);
    atomic_init(&trace->dropped, 0);
    trace->now_ms = now_ms;
    trace->has_previous = false;
}

static void note_dropped(bwg_trace_t *trace)
{
    // Saturates, so the count never wraps back to looking clean.
    uint32_t seen = atomic_load_explicit(&trace->dropped, memory_order_relaxed);
    while (seen != UINT32_MAX &&
           !atomic_compare_exchange_weak_explicit(&trace->dropped, &seen, seen + 1, memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void begin_window(bwg_trace_t *trace, uint32_t epoch)
{
    if (trace->current.epoch != 0) {
        trace->previous = trace->current;
        trace->has_previous = true;
    }
    trace->current.epoch = epoch;
    trace->current.next_ordinal = 1;
}

void bwg_trace_note(bwg_trace_t *maybe_trace, uint32_t epoch, uint32_t request_sequence, bwg_trace_stage_t stage,
                    uint32_t wire_bytes, uint32_t queued_bytes)
{
    if (maybe_trace == NULL) {
        return;
    }
    bwg_trace_t *trace = maybe_trace;
    if (epoch == 0 || wire_bytes > MAX_WIRE_BYTES || queued_bytes > wire_bytes ||
        atomic_flag_test_and_set_explicit(&trace->busy, memory_order_acquire)) {
        note_dropped(trace);
        return;
    }
    if (epoch < trace->current.epoch) {
        note_dropped(trace);
    } else {
        if (epoch != trace->current.epoch) {
            begin_window(trace, epoch);
        }
        bwg_trace_window_t *window = &trace->current;
        bwg_trace_event_t *event = &window->events[(window->next_ordinal - 1) % BWG_TRACE_CAPACITY];
        event->ordinal = window->next_ordinal++;
        event->request_sequence = request_sequence;
        event->stage = (uint8_t)stage;
        event->at_ms = trace->now_ms();
        event->wire_bytes = wire_bytes;
        event->queued_bytes = queued_bytes;
    }
    atomic_flag_clear_explicit(&trace->busy, memory_order_release);
}

static void write_u64(bwg_jw_t *writer, const char *key, uint64_t value)
{
    bwg_jw_key(writer, key);
    bwg_jw_u64(writer, value);
}

static void write_event(const bwg_trace_event_t *event, uint32_t epoch, bwg_jw_t *writer)
{
    bwg_jw_object_begin(writer);
    write_u64(writer, "atMs", event->at_ms);
    write_u64(writer, "epoch", epoch);
    write_u64(writer, "ordinal", event->ordinal);
    write_u64(writer, "queuedBytes", event->queued_bytes);
    write_u64(writer, "requestSequence", event->request_sequence);
    bwg_jw_key(writer, "stage");
    bwg_jw_cstr(writer, STAGE_LABELS[event->stage]);
    write_u64(writer, "wireBytes", event->wire_bytes);
    bwg_jw_object_end(writer);
}

static void write_window(const bwg_trace_window_t *window, bwg_jw_t *writer)
{
    const uint32_t written = window->next_ordinal > 0 ? window->next_ordinal - 1 : 0;
    const uint32_t overwritten = written > BWG_TRACE_CAPACITY ? written - BWG_TRACE_CAPACITY : 0;
    bwg_jw_object_begin(writer);
    write_u64(writer, "epoch", window->epoch);
    bwg_jw_key(writer, "events");
    bwg_jw_array_begin(writer);
    for (uint32_t ordinal = overwritten + 1; ordinal <= written; ordinal++) {
        write_event(&window->events[(ordinal - 1) % BWG_TRACE_CAPACITY], window->epoch, writer);
    }
    bwg_jw_array_end(writer);
    write_u64(writer, "firstEventOrdinal", written > 0 ? overwritten + 1 : 0);
    write_u64(writer, "nextEventOrdinal", window->next_ordinal);
    write_u64(writer, "overwrittenEvents", overwritten);
    bwg_jw_object_end(writer);
}

void bwg_trace_write(bwg_trace_t *trace, bwg_jw_t *writer)
{
    const bool held = !atomic_flag_test_and_set_explicit(&trace->busy, memory_order_acquire);
    const bool available = held && trace->current.epoch != 0;
    static const bwg_trace_window_t EMPTY = {0};
    bwg_jw_object_begin(writer);
    write_u64(writer, "capacity", BWG_TRACE_CAPACITY);
    bwg_jw_key(writer, "current");
    write_window(available ? &trace->current : &EMPTY, writer);
    write_u64(writer, "droppedEvents", atomic_load_explicit(&trace->dropped, memory_order_relaxed));
    bwg_jw_key(writer, "previous");
    if (available && trace->has_previous) {
        write_window(&trace->previous, writer);
    } else {
        bwg_jw_null(writer);
    }
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-serial-trace-v1");
    bwg_jw_key(writer, "snapshotAvailable");
    bwg_jw_bool(writer, available);
    bwg_jw_object_end(writer);
    if (held) {
        atomic_flag_clear_explicit(&trace->busy, memory_order_release);
    }
}
