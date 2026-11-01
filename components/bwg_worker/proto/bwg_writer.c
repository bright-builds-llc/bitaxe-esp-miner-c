#include "bwg_writer.h"

#include <sodium.h>

#include <string.h>

#include "bwg_json_writer.h"

static uint32_t current_epoch(const bwg_writer_t *writer)
{
    return bwg_link_shared_current_epoch(writer->shared);
}

static bool permits(const bwg_writer_t *writer, bwg_admission_t admission, uint32_t epoch, uint64_t now_ms)
{
    uint32_t current = current_epoch(writer);
    switch (admission) {
    case BWG_ADMISSION_ACTIVE:
        return epoch != 0 && current == epoch;
    case BWG_ADMISSION_TERMINAL: {
        uint32_t bytes;
        return current == 0 && !writer->partial_output && bwg_credit_terminal(writer->shared, epoch, now_ms, &bytes);
    }
    case BWG_ADMISSION_PLAIN:
        return current == 0;
    }
    return false;
}

static void trace_control(const bwg_writer_t *writer, bwg_trace_stage_t stage)
{
    bwg_trace_note(writer->shared->maybe_trace, writer->record_epoch, writer->control_trace_sequence, stage,
                   (uint32_t)writer->record_len, (uint32_t)writer->record_written);
}

static void release_record(bwg_writer_t *writer)
{
    if (writer->record_owned.data != NULL) {
        bwg_buffer_release(writer->allocator, &writer->record_owned);
    } else if (writer->record_len > 0) {
        sodium_memzero(writer->record_inline, writer->record_len);
    }
    writer->record_active = false;
    writer->record_is_control = false;
    writer->record_is_credit = false;
    writer->record_flushing = false;
    writer->record_bytes = NULL;
    writer->record_len = 0;
    writer->record_written = 0;
}

static void finish_control(bwg_writer_t *writer, bool sent, bwg_writer_result_t *result)
{
    result->control_finished = true;
    result->control_sent = sent;
    result->control_token = writer->control_token;
    writer->control_pending = false;
    writer->control_heartbeat_sent = false;
    writer->control_payload = NULL;
    if (!sent) {
        bwg_link_shared_revoke(writer->shared, writer->control_epoch);
    }
}

/** Ends the record in flight unfinished. An interrupted line is terminated by the next Hello, never completed. */
static void abort_record(bwg_writer_t *writer, bwg_writer_failure_t failure, uint64_t now_ms,
                         bwg_writer_result_t *result)
{
    result->failed_flushing = writer->record_flushing;
    result->failed_queued_bytes = writer->record_written;
    result->failed_record_bytes = writer->record_len;
    result->failed_elapsed_ms = now_ms > writer->record_started_ms ? now_ms - writer->record_started_ms : 0;
    // A record fully queued keeps its delimiter, whatever interrupts the wait for it to drain.
    if (writer->record_written > 0 && writer->record_written < writer->record_len) {
        writer->partial_output = true;
    }
    if (writer->record_admission == BWG_ADMISSION_TERMINAL) {
        bwg_credit_finish_terminal(writer->shared, writer->record_epoch);
    }
    if (writer->record_admission != BWG_ADMISSION_PLAIN) {
        bwg_link_shared_revoke(writer->shared, writer->record_epoch);
    }
    if (writer->record_is_control) {
        trace_control(writer, BWG_TRACE_WRITER_ABANDONED);
        finish_control(writer, false, result);
    }
    release_record(writer);
    result->status = BWG_WRITER_WROTE;
    result->failure = failure;
}

static void complete_record(bwg_writer_t *writer, bwg_writer_result_t *result, uint64_t now_ms)
{
    if (writer->record_admission == BWG_ADMISSION_TERMINAL) {
        bwg_credit_finish_terminal(writer->shared, writer->record_epoch);
    }
    if (writer->record_is_credit) {
        writer->credited = writer->record_credit;
    }
    if (writer->record_is_control) {
        trace_control(writer, BWG_TRACE_WRITER_COMPLETED);
        finish_control(writer, permits(writer, BWG_ADMISSION_ACTIVE, writer->record_epoch, now_ms), result);
    }
    release_record(writer);
    result->status = BWG_WRITER_WROTE;
}

static void pump(bwg_writer_t *writer, const bwg_sink_t *sink, uint64_t now_ms, bwg_writer_result_t *result)
{
    if (!permits(writer, writer->record_admission, writer->record_epoch, now_ms)) {
        abort_record(writer, BWG_WRITER_FAILED_REVOKED, now_ms, result);
        return;
    }
    if (now_ms >= writer->record_started_ms && now_ms - writer->record_started_ms >= BWG_RECORD_WRITE_TIMEOUT_MS) {
        abort_record(writer, BWG_WRITER_FAILED_DEADLINE, now_ms, result);
        return;
    }
    bool moved = false;
    while (writer->record_written < writer->record_len) {
        size_t remaining = writer->record_len - writer->record_written;
        size_t chunk = remaining < BWG_WRITE_CHUNK_BYTES ? remaining : BWG_WRITE_CHUNK_BYTES;
        int32_t accepted = sink->write(sink->ctx, writer->record_bytes + writer->record_written, chunk);
        if (accepted < 0 || (size_t) accepted > chunk) {
            abort_record(writer, BWG_WRITER_FAILED_SINK, now_ms, result);
            return;
        }
        if (accepted == 0) {
            result->status = moved ? BWG_WRITER_WROTE : BWG_WRITER_BLOCKED;
            return;
        }
        writer->record_written += (size_t) accepted;
        moved = true;
        if (!permits(writer, writer->record_admission, writer->record_epoch, now_ms)) {
            abort_record(writer, BWG_WRITER_FAILED_REVOKED, now_ms, result);
            return;
        }
    }
    if (!writer->record_flushing && writer->record_is_control) {
        trace_control(writer, BWG_TRACE_WRITER_QUEUED);
    }
    writer->record_flushing = true;
    if (!sink->flushed(sink->ctx)) {
        result->status = moved ? BWG_WRITER_WROTE : BWG_WRITER_BLOCKED;
        return;
    }
    complete_record(writer, result, now_ms);
}

static void start_record(bwg_writer_t *writer, bwg_admission_t admission, uint32_t epoch, const uint8_t *bytes,
                         size_t len, uint64_t now_ms)
{
    writer->record_active = true;
    writer->record_admission = admission;
    writer->record_epoch = epoch;
    writer->record_bytes = bytes;
    writer->record_len = len;
    writer->record_written = 0;
    writer->record_flushing = false;
    writer->record_started_ms = now_ms;
}

/** Fails the selected output, revokes its epoch and ends any final credit, so it is never retried. */
static void fail_selection(bwg_writer_t *writer, uint32_t epoch, bwg_writer_failure_t failure,
                           bwg_writer_result_t *result)
{
    bwg_link_shared_revoke(writer->shared, epoch);
    bwg_credit_finish_terminal(writer->shared, epoch);
    result->status = BWG_WRITER_WROTE;
    result->failure = failure;
}

/** Frames `payload` for the writer's session and starts it, with an optional LF ending a broken line. */
static bool start_framed(bwg_writer_t *writer, bwg_admission_t admission, bwg_kind_t kind, uint32_t sequence,
                         const uint8_t *payload, size_t len, bool resynchronize, uint64_t now_ms,
                         bwg_writer_result_t *result)
{
    size_t lead = resynchronize ? 1 : 0;
    size_t capacity = lead + len + BWG_ENVELOPE_OVERHEAD_BYTES;
    uint8_t *out = writer->record_inline;
    if (capacity > sizeof(writer->record_inline)) {
        if (!bwg_buffer_alloc(writer->allocator, &writer->record_owned, capacity)) {
            fail_selection(writer, writer->epoch, BWG_WRITER_FAILED_ALLOCATION, result);
            return false;
        }
        out = writer->record_owned.data;
    }
    if (resynchronize) {
        out[0] = '\n';
    }
    size_t record_len = 0;
    if (bwg_record_encode(kind, writer->session_id, sequence, payload, len, out + lead, capacity - lead, &record_len) !=
        BWG_FRAME_OK) {
        sodium_memzero(out, capacity);
        bwg_buffer_release(writer->allocator, &writer->record_owned);
        fail_selection(writer, writer->epoch, BWG_WRITER_FAILED_ENCODING, result);
        return false;
    }
    start_record(writer, admission, writer->epoch, out, lead + record_len, now_ms);
    return true;
}

/** Takes the next output sequence, revoking the epoch on exhaustion. */
static bool next_sequence(bwg_writer_t *writer, uint32_t *sequence, bwg_writer_result_t *result)
{
    if (writer->sequence == UINT32_MAX) {
        fail_selection(writer, writer->epoch, BWG_WRITER_FAILED_SEQUENCE_EXHAUSTED, result);
        return false;
    }
    writer->sequence++;
    *sequence = writer->sequence;
    return true;
}

static bool select_hello(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    if (!writer->hello_pending) {
        return false;
    }
    writer->hello_pending = false;
    if (current_epoch(writer) != writer->hello_epoch) {
        sodium_memzero(writer->hello_payload, writer->hello_len);
        return false;
    }
    writer->epoch = writer->hello_epoch;
    memcpy(writer->session_id, writer->hello_session_id, sizeof(writer->session_id));
    writer->sequence = 0;
    writer->credited = 0;
    writer->last_heartbeat_ms = now_ms;
    bool resynchronize = writer->partial_output;
    bool started = start_framed(writer, BWG_ADMISSION_ACTIVE, BWG_KIND_SESSION, 0, writer->hello_payload,
                                writer->hello_len, resynchronize, now_ms, result);
    sodium_memzero(writer->hello_payload, writer->hello_len);
    if (started) {
        writer->partial_output = false;
    }
    return true;
}

static bool start_heartbeat(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    uint32_t sequence;
    writer->last_heartbeat_ms = now_ms;
    if (next_sequence(writer, &sequence, result)) {
        start_framed(writer, BWG_ADMISSION_ACTIVE, BWG_KIND_HEARTBEAT, sequence, (const uint8_t *) "{}", 2, false,
                     now_ms, result);
    }
    return true;
}

static bool select_heartbeat(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    if (writer->epoch == 0 || current_epoch(writer) != writer->epoch ||
        (now_ms > writer->last_heartbeat_ms ? now_ms - writer->last_heartbeat_ms : 0) <
            BWG_DEVICE_HEARTBEAT_INTERVAL_MS) {
        return false;
    }
    return start_heartbeat(writer, now_ms, result);
}

static bool select_credit(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    bool terminal = current_epoch(writer) != writer->epoch;
    bwg_admission_t admission = terminal ? BWG_ADMISSION_TERMINAL : BWG_ADMISSION_ACTIVE;
    uint32_t received = 0;
    bool known = terminal ? bwg_credit_terminal(writer->shared, writer->epoch, now_ms, &received)
                          : bwg_credit_received(writer->shared, writer->epoch, &received);
    if (!known) {
        return false;
    }
    if (received <= writer->credited || !permits(writer, admission, writer->epoch, now_ms)) {
        if (terminal) {
            bwg_credit_finish_terminal(writer->shared, writer->epoch);
        }
        return false;
    }
    uint32_t sequence;
    if (!next_sequence(writer, &sequence, result)) {
        return true;
    }
    uint8_t payload[64];
    bwg_jw_t json;
    bwg_jw_init_buffer(&json, payload, sizeof(payload));
    bwg_jw_object_begin(&json);
    bwg_jw_key(&json, "op");
    bwg_jw_cstr(&json, "receive_credit");
    bwg_jw_key(&json, "receivedBytes");
    bwg_jw_u64(&json, received);
    bwg_jw_object_end(&json);
    if (!bwg_jw_ok(&json)) {
        fail_selection(writer, writer->epoch, BWG_WRITER_FAILED_ENCODING, result);
        return true;
    }
    if (start_framed(writer, admission, BWG_KIND_SESSION, sequence, payload, json.len, false, now_ms, result)) {
        writer->record_is_credit = true;
        writer->record_credit = received;
    }
    return true;
}

static bool select_control(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    if (!writer->control_pending) {
        return false;
    }
    if (writer->control_epoch != writer->epoch || current_epoch(writer) != writer->epoch) {
        finish_control(writer, false, result);
        result->status = BWG_WRITER_WROTE;
        return true;
    }
    if (writer->control_len > BWG_LONG_RECORD_HEARTBEAT_BYTES && !writer->control_heartbeat_sent) {
        // An indivisible long record gets a fresh peer deadline before it starts.
        writer->control_heartbeat_sent = true;
        return start_heartbeat(writer, now_ms, result);
    }
    uint32_t sequence;
    if (!next_sequence(writer, &sequence, result)) {
        finish_control(writer, false, result);
        return true;
    }
    if (start_framed(writer, BWG_ADMISSION_ACTIVE, BWG_KIND_CONTROL, sequence, writer->control_payload,
                     writer->control_len, false, now_ms, result)) {
        writer->record_is_control = true;
        trace_control(writer, BWG_TRACE_WRITER_STARTED);
    } else {
        finish_control(writer, false, result);
    }
    return true;
}

static bool select_framed_diagnostic(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    // Worst-case escaping of every byte, plus the wrapping object.
    size_t capacity = writer->diagnostic_len * 6 + 16;
    bwg_buffer_t payload;
    if (!bwg_buffer_alloc(writer->allocator, &payload, capacity)) {
        fail_selection(writer, writer->epoch, BWG_WRITER_FAILED_ALLOCATION, result);
        return true;
    }
    bwg_jw_t json;
    bwg_jw_init_buffer(&json, payload.data, payload.capacity);
    bwg_jw_object_begin(&json);
    bwg_jw_key(&json, "line");
    bwg_jw_string(&json, writer->diagnostic_line, writer->diagnostic_len);
    bwg_jw_object_end(&json);
    uint32_t sequence;
    if (bwg_jw_ok(&json) && next_sequence(writer, &sequence, result)) {
        start_framed(writer, BWG_ADMISSION_ACTIVE, BWG_KIND_DIAGNOSTIC, sequence, payload.data, json.len, false, now_ms,
                     result);
    }
    bwg_buffer_release(writer->allocator, &payload);
    return true;
}

static bool select_plain_diagnostic(bwg_writer_t *writer, uint64_t now_ms)
{
    size_t lead = writer->partial_output ? 1 : 0;
    uint8_t *out = writer->record_inline;
    if (lead == 1) {
        out[0] = '\n';
    }
    memcpy(out + lead, writer->diagnostic_line, writer->diagnostic_len);
    out[lead + writer->diagnostic_len] = '\n';
    writer->partial_output = false;
    start_record(writer, BWG_ADMISSION_PLAIN, 0, out, lead + writer->diagnostic_len + 1, now_ms);
    return true;
}

static bool select_diagnostic(bwg_writer_t *writer, uint64_t now_ms, bwg_writer_result_t *result)
{
    if (!writer->diagnostic_pending) {
        return false;
    }
    writer->diagnostic_pending = false;
    uint32_t current = current_epoch(writer);
    if (writer->epoch != 0 && current == writer->epoch) {
        return select_framed_diagnostic(writer, now_ms, result);
    }
    return current == 0 && select_plain_diagnostic(writer, now_ms);
}

void bwg_writer_init(bwg_writer_t *writer, const bwg_allocator_t *allocator, bwg_link_shared_t *shared)
{
    memset(writer, 0, sizeof(*writer));
    writer->allocator = allocator;
    writer->shared = shared;
}

void bwg_writer_deinit(bwg_writer_t *writer)
{
    release_record(writer);
    sodium_memzero(writer->hello_payload, sizeof(writer->hello_payload));
}

bool bwg_writer_begin_session(bwg_writer_t *writer, uint32_t epoch, const char *session_id, const uint8_t *ack,
                              size_t len)
{
    if (len > BWG_HELLO_ACK_MAX_BYTES || strlen(session_id) != BWG_SESSION_ID_TEXT_LEN) {
        return false;
    }
    memcpy(writer->hello_payload, ack, len);
    memcpy(writer->hello_session_id, session_id, BWG_SESSION_ID_TEXT_LEN + 1);
    writer->hello_len = len;
    writer->hello_epoch = epoch;
    writer->hello_pending = true;
    return true;
}

bool bwg_writer_offer_control(bwg_writer_t *writer, uint32_t epoch, const uint8_t *payload, size_t len, uint32_t token)
{
    const bwg_trace_stage_t stage = writer->control_pending ? BWG_TRACE_WRITER_REJECTED : BWG_TRACE_WRITER_ACCEPTED;
    bwg_trace_note(writer->shared->maybe_trace, epoch, writer->next_trace_sequence, stage, (uint32_t)len, 0);
    if (writer->control_pending) {
        return false;
    }
    writer->control_trace_sequence = writer->next_trace_sequence;
    writer->control_pending = true;
    writer->control_heartbeat_sent = false;
    writer->control_epoch = epoch;
    writer->control_payload = payload;
    writer->control_len = len;
    writer->control_token = token;
    return true;
}

bool bwg_writer_offer_diagnostic(bwg_writer_t *writer, const char *line)
{
    size_t len = strlen(line);
    if (writer->diagnostic_pending || len > BWG_DIAGNOSTIC_MAX_BYTES) {
        return false;
    }
    memcpy(writer->diagnostic_line, line, len);
    writer->diagnostic_len = len;
    writer->diagnostic_pending = true;
    return true;
}

bwg_writer_result_t bwg_writer_next(bwg_writer_t *writer, const bwg_sink_t *sink, uint64_t now_ms)
{
    bwg_writer_result_t result = {.status = BWG_WRITER_IDLE, .failure = BWG_WRITER_OK};
    if (!writer->record_active) {
        bool selected = select_hello(writer, now_ms, &result) || select_heartbeat(writer, now_ms, &result) ||
                        select_credit(writer, now_ms, &result) || select_control(writer, now_ms, &result) ||
                        select_diagnostic(writer, now_ms, &result);
        if (!selected || !writer->record_active) {
            return result;
        }
    }
    pump(writer, sink, now_ms, &result);
    return result;
}
