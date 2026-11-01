#ifndef BWG_WRITER_H
#define BWG_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"
#include "bwg_frame.h"
#include "bwg_link.h"
#include "bwg_link_shared.h"

#define BWG_DEVICE_HEARTBEAT_INTERVAL_MS 1000u
#define BWG_LONG_RECORD_HEARTBEAT_BYTES 4096u
#define BWG_RECORD_WRITE_TIMEOUT_MS 2000u
#define BWG_WRITE_CHUNK_BYTES 512u
#define BWG_DIAGNOSTIC_MAX_BYTES 1024u

/** The sole transport output. Both calls must return without waiting. */
typedef struct {
    void *ctx;
    /** Queues up to `len` bytes. Returns how many were accepted, 0 when full, negative on failure. */
    int32_t (*write)(void *ctx, const uint8_t *bytes, size_t len);
    /** Whether everything accepted so far has left the device. */
    bool (*flushed)(void *ctx);
} bwg_sink_t;

typedef enum {
    /** Nothing to do until a new offer or the next tick. */
    BWG_WRITER_IDLE = 0,
    /** Output moved or a decision was made. Call again straight away. */
    BWG_WRITER_WROTE,
    /** A record is in flight and the sink is full or flushing. Call again on the next tick. */
    BWG_WRITER_BLOCKED,
} bwg_writer_status_t;

typedef enum {
    BWG_WRITER_OK = 0,
    /** The record missed its 2,000 ms deadline. The epoch was revoked. */
    BWG_WRITER_FAILED_DEADLINE,
    /** The sink reported failure. The epoch was revoked. */
    BWG_WRITER_FAILED_SINK,
    /** The record's epoch lost admission mid-record, which aborted it. */
    BWG_WRITER_FAILED_REVOKED,
    BWG_WRITER_FAILED_SEQUENCE_EXHAUSTED,
    BWG_WRITER_FAILED_ALLOCATION,
    BWG_WRITER_FAILED_ENCODING,
} bwg_writer_failure_t;

/** What one bwg_writer_next call did. */
typedef struct {
    bwg_writer_status_t status;
    bwg_writer_failure_t failure;
    /** The offered control reply ended. `control_token` identifies it; `control_sent` is the send confirmation. */
    bool control_finished;
    bool control_sent;
    uint32_t control_token;
    /** Where a failed record stood, for its report. Set only with `failure` from a record in flight. */
    bool failed_flushing;
    size_t failed_queued_bytes;
    size_t failed_record_bytes;
    uint32_t failed_elapsed_ms;
} bwg_writer_result_t;

typedef enum {
    BWG_ADMISSION_ACTIVE = 0,
    BWG_ADMISSION_TERMINAL,
    BWG_ADMISSION_PLAIN,
} bwg_admission_t;

/**
 * One output owner: heartbeat, then credit, then control replies and diagnostics, with sequences
 * assigned when a record is selected. Touched by one task only; other tasks reach it through a
 * queue the shell owns. Holds its small records inline (about 4 KiB), so keep it off a task stack.
 */
typedef struct {
    const bwg_allocator_t *allocator;
    bwg_link_shared_t *shared;
    uint32_t epoch;
    char session_id[BWG_SESSION_ID_TEXT_LEN + 1];
    uint32_t sequence;
    uint32_t credited;
    uint64_t last_heartbeat_ms;
    bool partial_output;

    bool hello_pending;
    uint32_t hello_epoch;
    char hello_session_id[BWG_SESSION_ID_TEXT_LEN + 1];
    size_t hello_len;
    uint8_t hello_payload[BWG_HELLO_ACK_MAX_BYTES];

    bool control_pending;
    /** The request a reply answers, for the trace. The owner sets the next one before offering. */
    uint32_t next_trace_sequence;
    uint32_t control_trace_sequence;
    bool control_heartbeat_sent;
    uint32_t control_epoch;
    uint32_t control_token;
    const uint8_t *control_payload;
    size_t control_len;

    bool diagnostic_pending;
    size_t diagnostic_len;
    char diagnostic_line[BWG_DIAGNOSTIC_MAX_BYTES];

    bool record_active;
    bool record_is_control;
    bool record_is_credit;
    bool record_flushing;
    bwg_admission_t record_admission;
    uint32_t record_epoch;
    uint32_t record_credit;
    uint64_t record_started_ms;
    size_t record_len;
    size_t record_written;
    const uint8_t *record_bytes;
    bwg_buffer_t record_owned;
    uint8_t record_inline[BWG_HELLO_ACK_MAX_BYTES + BWG_ENVELOPE_OVERHEAD_BYTES + 1];
} bwg_writer_t;

/** Starts with no session open. Nothing is allocated until a record needs it. */
void bwg_writer_init(bwg_writer_t *writer, const bwg_allocator_t *allocator, bwg_link_shared_t *shared);

/** Wipes and frees anything in flight. */
void bwg_writer_deinit(bwg_writer_t *writer);

/**
 * Queues a Hello acknowledgement (copied) for `epoch`, replacing any older unwritten one. False if
 * `len` exceeds BWG_HELLO_ACK_MAX_BYTES. A stale one is dropped if the epoch is no longer current.
 */
bool bwg_writer_begin_session(bwg_writer_t *writer, uint32_t epoch, const char *session_id, const uint8_t *ack,
                              size_t len);

/**
 * Offers one control reply. `payload` stays borrowed until the result reporting `control_finished`
 * for `token`, and must be one JSON object without LF. False if a reply is already waiting.
 */
bool bwg_writer_offer_control(bwg_writer_t *writer, uint32_t epoch, const uint8_t *payload, size_t len, uint32_t token);

/** Offers one diagnostic line (copied). False if it is too long or one is already waiting; the caller drops it. */
bool bwg_writer_offer_diagnostic(bwg_writer_t *writer, const char *line);

/**
 * Moves output forward without waiting: continues the record in flight, else selects the next one
 * by priority. A record that fails or runs past 2,000 ms is aborted, never spliced, and revokes its
 * epoch. Heartbeats are also due from here, so call at least every tick.
 */
bwg_writer_result_t bwg_writer_next(bwg_writer_t *writer, const bwg_sink_t *sink, uint64_t now_ms);

#endif // BWG_WRITER_H
