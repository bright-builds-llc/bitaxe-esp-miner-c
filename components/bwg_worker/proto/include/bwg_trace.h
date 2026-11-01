#ifndef BWG_TRACE_H
#define BWG_TRACE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "bwg_json_writer.h"

#define BWG_TRACE_CAPACITY 64u
/** The largest review a full trace can write. */
#define BWG_TRACE_REVIEW_BYTES 20480u

typedef enum {
    BWG_TRACE_HELLO = 0,
    BWG_TRACE_VALIDATED,
    BWG_TRACE_VALIDATION_REJECTED,
    BWG_TRACE_ENQUEUE_STARTED,
    BWG_TRACE_ENQUEUED,
    BWG_TRACE_ENQUEUE_REJECTED,
    BWG_TRACE_DISPATCH_STARTED,
    BWG_TRACE_DISPATCH_REJECTED,
    BWG_TRACE_REPLY_CREATED,
    BWG_TRACE_REPLY_REJECTED,
    BWG_TRACE_WRITER_ACCEPTED,
    BWG_TRACE_WRITER_REJECTED,
    BWG_TRACE_WRITER_STARTED,
    BWG_TRACE_WRITER_QUEUED,
    BWG_TRACE_WRITER_COMPLETED,
    BWG_TRACE_WRITER_ABANDONED,
    BWG_TRACE_EPOCH_REVOKED,
} bwg_trace_stage_t;

typedef struct {
    uint64_t at_ms;
    uint32_t ordinal;
    uint32_t request_sequence;
    uint32_t wire_bytes;
    uint32_t queued_bytes;
    uint8_t stage;
} bwg_trace_event_t;

typedef struct {
    uint32_t epoch;
    uint32_t next_ordinal;
    bwg_trace_event_t events[BWG_TRACE_CAPACITY];
} bwg_trace_window_t;

/**
 * Metadata about the serial path: stages, sizes and times, never a payload. The link, the writer and the owner
 * all note events, so a note that finds it busy is counted as dropped rather than waited for.
 */
typedef struct {
    atomic_flag busy;
    _Atomic uint32_t dropped;
    uint64_t (*now_ms)(void);
    bool has_previous;
    bwg_trace_window_t current;
    bwg_trace_window_t previous;
} bwg_trace_t;

void bwg_trace_init(bwg_trace_t *trace, uint64_t (*now_ms)(void));

/** Does nothing without a trace. An event for an epoch older than the current one is dropped. */
void bwg_trace_note(bwg_trace_t *maybe_trace, uint32_t epoch, uint32_t request_sequence, bwg_trace_stage_t stage,
                    uint32_t wire_bytes, uint32_t queued_bytes);

/** Writes the worker-serial-trace-v1 review. A note in progress makes it the snapshot-unavailable form. */
void bwg_trace_write(bwg_trace_t *trace, bwg_jw_t *writer);

#endif // BWG_TRACE_H
