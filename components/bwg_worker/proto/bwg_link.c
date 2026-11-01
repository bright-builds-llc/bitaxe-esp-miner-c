#include "bwg_link.h"

#include <sodium.h>

#include <string.h>

#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_json_writer.h"
#include "bwg_reason.h"

const char *bwg_link_failure_label(bwg_link_failure_t failure)
{
    switch (failure) {
    case BWG_LINK_FAILURE_READ: return "read";
    case BWG_LINK_FAILURE_FRAMING: return "framing";
    case BWG_LINK_FAILURE_ENVELOPE: return "envelope";
    case BWG_LINK_FAILURE_SEQUENCE: return "sequence";
    case BWG_LINK_FAILURE_HEARTBEAT_PAYLOAD: return "heartbeat_payload";
    case BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT: return "heartbeat_timeout";
    case BWG_LINK_FAILURE_CONTROL_ALLOCATION: return "control_allocation";
    case BWG_LINK_FAILURE_CONTROL_QUEUE: return "control_queue";
    case BWG_LINK_FAILURE_UNEXPECTED_KIND: return "unexpected_kind";
    case BWG_LINK_FAILURE_SESSION_REVOKED: return "session_revoked";
    case BWG_LINK_FAILURE_INTEGRITY: return "integrity";
    case BWG_LINK_FAILURE_RECEIVE_COUNTER: return "receive_counter";
    case BWG_LINK_FAILURE_FRAME_ALLOCATION: return "frame_allocation";
    }
    return "";
}

static void close_link(bwg_link_t *link, uint64_t now_ms)
{
    if (!link->open) {
        return;
    }
    link->open = false;
    link->ops.close_generation(link->ops.ctx, link->generation, now_ms);
    bwg_link_shared_revoke(link->shared, link->epoch);
}

static void fail_and_close(bwg_link_t *link, bwg_link_failure_t failure, size_t observed, uint64_t now_ms)
{
    link->ops.on_failure(link->ops.ctx, failure, observed);
    close_link(link, now_ms);
}

static void reset_input(bwg_link_t *link)
{
    bwg_accumulator_clear(&link->accumulator);
    link->observed_bytes = 0;
}

static bool liveness_poll(bwg_link_t *link, uint64_t now_ms)
{
    if (link->closed || now_ms < link->last_observation_ms ||
        now_ms - link->last_heartbeat_ms >= BWG_HEARTBEAT_TIMEOUT_MS) {
        link->closed = true;
        link->authenticated = false;
        return false;
    }
    link->last_observation_ms = now_ms;
    return true;
}

static bool liveness_heartbeat(bwg_link_t *link, uint64_t now_ms)
{
    if (!liveness_poll(link, now_ms) || !link->authenticated) {
        return false;
    }
    link->last_heartbeat_ms = now_ms;
    return true;
}

static bool parse_hello_payload(bwg_link_t *link, const uint8_t *payload, size_t len, char host_nonce[44])
{
    static const char *const fields[] = {"op", "hostNonce"};
    yyjson_doc *doc;
    if (bwg_json_parse_strict(link->allocator, payload, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bwg_str_t op;
    bwg_str_t nonce;
    bool ok = bwg_json_has_exact_fields(root, fields, 2, NULL, 0) &&
              bwg_json_get_str(bwg_json_field(root, "op"), &op) && bwg_str_equals(op, "hello") &&
              bwg_json_get_str(bwg_json_field(root, "hostNonce"), &nonce) &&
              bwg_b64url_is_canonical(nonce.ptr, nonce.len, BWG_NONCE_BYTES);
    if (ok) {
        memcpy(host_nonce, nonce.ptr, 43);
        host_nonce[43] = '\0';
    }
    yyjson_doc_free(doc);
    return ok;
}

static bool is_clean_close(bwg_link_t *link, const bwg_record_t *record)
{
    static const char *const fields[] = {"op", "reason"};
    if (record->kind != BWG_KIND_SESSION) {
        return false;
    }
    yyjson_doc *doc;
    if (bwg_json_parse_strict(link->allocator, record->payload, record->payload_len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bwg_str_t op;
    bwg_str_t reason_text;
    bwg_reason_t reason;
    bool ok = bwg_json_has_exact_fields(root, fields, 2, NULL, 0) &&
              bwg_json_get_str(bwg_json_field(root, "op"), &op) && bwg_str_equals(op, "close") &&
              bwg_json_get_str(bwg_json_field(root, "reason"), &reason_text) &&
              bwg_reason_parse(reason_text.ptr, reason_text.len, &reason);
    yyjson_doc_free(doc);
    return ok;
}

static bool make_binding(bwg_link_t *link, const char host_nonce[44], bwg_session_binding_t *binding)
{
    uint8_t session_bytes[BWG_SESSION_ID_BYTES];
    uint8_t device_bytes[BWG_NONCE_BYTES];
    bool ok = link->ops.fill_random(link->ops.ctx, session_bytes, sizeof(session_bytes)) &&
              link->ops.fill_random(link->ops.ctx, device_bytes, sizeof(device_bytes)) &&
              bwg_b64url_encode(session_bytes, sizeof(session_bytes), binding->session_id,
                                sizeof(binding->session_id), NULL) &&
              bwg_b64url_encode(device_bytes, sizeof(device_bytes), binding->device_nonce,
                                sizeof(binding->device_nonce), NULL);
    sodium_memzero(session_bytes, sizeof(session_bytes));
    sodium_memzero(device_bytes, sizeof(device_bytes));
    memcpy(binding->host_nonce, host_nonce, 44);
    return ok;
}

static bool build_ack(bwg_link_t *link, const bwg_session_binding_t *binding, size_t *len)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, link->ack_scratch, sizeof(link->ack_scratch));
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "op");
    bwg_jw_cstr(&writer, "hello_ack");
    bwg_jw_key(&writer, "hostNonce");
    bwg_jw_cstr(&writer, binding->host_nonce);
    bwg_jw_key(&writer, "deviceNonce");
    bwg_jw_cstr(&writer, binding->device_nonce);
    bwg_jw_key(&writer, "serialManifest");
    bwg_jw_raw_value(&writer, (const uint8_t *) BWG_SERIAL_MANIFEST_JSON, sizeof(BWG_SERIAL_MANIFEST_JSON) - 1);
    bwg_jw_key(&writer, "firmwareSourceCommit");
    bwg_jw_cstr(&writer, link->firmware_commit);
    bwg_jw_key(&writer, "appElfSha256");
    bwg_jw_cstr(&writer, link->app_elf_hex);
    bwg_jw_key(&writer, "receiveWindowBytes");
    bwg_jw_u64(&writer, BWG_RECEIVE_WINDOW_BYTES);
    bwg_jw_key(&writer, "receivedBytes");
    bwg_jw_u64(&writer, 0);
    bwg_jw_object_end(&writer);
    *len = writer.len;
    return bwg_jw_ok(&writer);
}

static void handle_hello(bwg_link_t *link, const bwg_record_t *record, uint64_t now_ms)
{
    char host_nonce[44];
    if (!parse_hello_payload(link, record->payload, record->payload_len, host_nonce)) {
        return;
    }
    if (link->next_epoch == UINT32_MAX) {
        return;
    }
    link->next_epoch++;
    bwg_session_binding_t binding;
    if (!make_binding(link, host_nonce, &binding)) {
        return;
    }
    uint32_t generation;
    if (!link->ops.begin_generation(link->ops.ctx, now_ms, &generation)) {
        return;
    }
    uint32_t epoch = link->next_epoch;
    link->ops.on_failure_cleared(link->ops.ctx);
    bwg_credit_begin(link->shared, epoch);
    atomic_store_explicit(&link->shared->current_epoch, epoch, memory_order_release);
    bwg_trace_note(link->shared->maybe_trace, epoch, 0, BWG_TRACE_HELLO, (uint32_t)record->payload_len, 0);
    size_t ack_len = 0;
    if (!link->ops.enqueue_session(link->ops.ctx, epoch, &binding, generation) || !build_ack(link, &binding, &ack_len) ||
        !link->ops.send_hello_ack(link->ops.ctx, epoch, binding.session_id, link->ack_scratch, ack_len)) {
        link->ops.close_generation(link->ops.ctx, generation, now_ms);
        bwg_link_shared_revoke(link->shared, epoch);
        return;
    }
    link->open = true;
    link->epoch = epoch;
    link->binding = binding;
    link->generation = generation;
    link->sequence = 0;
    link->last_observation_ms = now_ms;
    link->last_heartbeat_ms = now_ms;
    link->authenticated = false;
    link->closed = false;
    link->received = 0;
}

static void handle_heartbeat(bwg_link_t *link, const bwg_record_t *record, size_t frame_bytes, uint64_t now_ms)
{
    size_t significant = 0;
    bool only_braces = true;
    for (size_t i = 0; i < record->payload_len; i++) {
        uint8_t c = record->payload[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            continue;
        }
        only_braces = only_braces && significant < 2 && c == "{}"[significant];
        significant++;
    }
    if (!only_braces || significant != 2) {
        fail_and_close(link, BWG_LINK_FAILURE_HEARTBEAT_PAYLOAD, frame_bytes, now_ms);
        return;
    }
    // A host can receive the proof just before the owner confirms it sent. Those heartbeats never refresh.
    if (bwg_link_shared_authenticated_epoch(link->shared) != link->epoch) {
        return;
    }
    link->authenticated = !link->closed;
    if (!liveness_heartbeat(link, now_ms)) {
        fail_and_close(link, BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT, frame_bytes, now_ms);
        return;
    }
    link->ops.heartbeat_generation(link->ops.ctx, link->generation, now_ms);
}

static void handle_control(bwg_link_t *link, const bwg_record_t *record, size_t frame_bytes, uint64_t now_ms)
{
    bwg_trace_t *maybe_trace = link->shared->maybe_trace;
    const uint32_t wire = (uint32_t)frame_bytes;
    bwg_trace_note(maybe_trace, link->epoch, record->sequence, BWG_TRACE_VALIDATED, wire, 0);
    bwg_buffer_t frame;
    if (!bwg_buffer_alloc(link->allocator, &frame, record->payload_len + 1)) {
        fail_and_close(link, BWG_LINK_FAILURE_CONTROL_ALLOCATION, frame_bytes, now_ms);
        return;
    }
    memcpy(frame.data, record->payload, record->payload_len);
    frame.data[record->payload_len] = '\n';
    frame.len = record->payload_len + 1;
    bwg_trace_note(maybe_trace, link->epoch, record->sequence, BWG_TRACE_ENQUEUE_STARTED, wire, 0);
    if (!link->ops.enqueue_control(link->ops.ctx, link->epoch, record->sequence, frame)) {
        bwg_trace_note(maybe_trace, link->epoch, record->sequence, BWG_TRACE_ENQUEUE_REJECTED, wire, 0);
        bwg_buffer_release(link->allocator, &frame);
        fail_and_close(link, BWG_LINK_FAILURE_CONTROL_QUEUE, frame_bytes, now_ms);
        return;
    }
    bwg_trace_note(maybe_trace, link->epoch, record->sequence, BWG_TRACE_ENQUEUED, wire, 0);
}

static void handle_close(bwg_link_t *link, uint64_t now_ms)
{
    uint32_t previous = 0;
    uint32_t epoch = link->epoch;
    uint32_t received = link->received;
    bool has_previous = bwg_credit_received(link->shared, epoch, &previous);
    close_link(link, now_ms);
    bwg_credit_retain_terminal(link->shared, epoch, has_previous, previous, received, now_ms);
}

static void handle_record(bwg_link_t *link, const bwg_record_t *record, size_t line_len, size_t frame_bytes,
                          uint64_t now_ms)
{
    if (record->kind == BWG_KIND_SESSION && record->maybe_session_id == NULL && record->sequence == 0) {
        close_link(link, now_ms);
        if (line_len <= BWG_MAX_HELLO_FRAME_BYTES) {
            handle_hello(link, record, now_ms);
        }
        return;
    }
    if (!link->open) {
        return;
    }
    if (record->maybe_session_id == NULL ||
        memcmp(record->maybe_session_id, link->binding.session_id, BWG_SESSION_ID_TEXT_LEN) != 0 ||
        record->sequence <= link->sequence || record->sequence == UINT32_MAX) {
        fail_and_close(link, BWG_LINK_FAILURE_SEQUENCE, frame_bytes, now_ms);
        return;
    }
    link->sequence = record->sequence;
    bool clean_close = is_clean_close(link, record);
    if (!clean_close) {
        bwg_credit_publish(link->shared, link->received);
    }
    switch (record->kind) {
    case BWG_KIND_HEARTBEAT:
        handle_heartbeat(link, record, frame_bytes, now_ms);
        return;
    case BWG_KIND_CONTROL:
        handle_control(link, record, frame_bytes, now_ms);
        return;
    case BWG_KIND_SESSION:
        if (clean_close) {
            handle_close(link, now_ms);
            return;
        }
        break;
    case BWG_KIND_DIAGNOSTIC:
        break;
    }
    fail_and_close(link, BWG_LINK_FAILURE_UNEXPECTED_KIND, frame_bytes, now_ms);
}

static void handle_line(bwg_link_t *link, const uint8_t *line, size_t line_len, size_t frame_bytes, uint64_t now_ms)
{
    bwg_record_t record;
    bwg_frame_status_t status = bwg_record_parse(line, line_len, &record);
    if (status == BWG_FRAME_OK) {
        handle_record(link, &record, line_len, frame_bytes, now_ms);
        return;
    }
    // Boot text is permissible before Hello; corrupt established input closes.
    if (link->open) {
        fail_and_close(link, status == BWG_FRAME_INTEGRITY ? BWG_LINK_FAILURE_INTEGRITY : BWG_LINK_FAILURE_ENVELOPE,
                       frame_bytes, now_ms);
    }
}

static void receive_byte(bwg_link_t *link, uint8_t byte, uint64_t now_ms)
{
    if (link->open) {
        if (link->received == UINT32_MAX) {
            fail_and_close(link, BWG_LINK_FAILURE_RECEIVE_COUNTER, link->observed_bytes, now_ms);
            reset_input(link);
            return;
        }
        link->received++;
        // The LF is credited after classification, so a validated Close has one strictly newer receipt.
        if (byte != '\n') {
            bwg_credit_publish(link->shared, link->received);
        }
    }
    if (link->observed_bytes < BWG_MAX_WIRE_FRAME_BYTES) {
        link->observed_bytes++;
    }
    const uint8_t *line = NULL;
    size_t line_len = 0;
    bwg_acc_result_t result = bwg_accumulator_push(&link->accumulator, byte, &line, &line_len);
    if (result == BWG_ACC_NONE) {
        return;
    }
    size_t frame_bytes = link->observed_bytes;
    link->observed_bytes = 0;
    if (result == BWG_ACC_LINE) {
        handle_line(link, line, line_len, frame_bytes, now_ms);
        bwg_accumulator_clear(&link->accumulator);
        return;
    }
    if (link->open) {
        link->ops.on_failure(link->ops.ctx,
                             result == BWG_ACC_UNAVAILABLE ? BWG_LINK_FAILURE_FRAME_ALLOCATION
                                                           : BWG_LINK_FAILURE_FRAMING,
                             frame_bytes);
    }
    close_link(link, now_ms);
}

bool bwg_link_init(bwg_link_t *link, const bwg_allocator_t *allocator, bwg_link_shared_t *shared,
                   const bwg_link_ops_t *ops, const char *firmware_commit, const char *app_elf_hex)
{
    if (!bwg_hex_is_lower(firmware_commit, strlen(firmware_commit), BWG_FIRMWARE_COMMIT_LEN) ||
        !bwg_hex_is_lower(app_elf_hex, strlen(app_elf_hex), BWG_APP_ELF_HEX_LEN)) {
        return false;
    }
    memset(link, 0, sizeof(*link));
    link->allocator = allocator;
    link->shared = shared;
    link->ops = *ops;
    memcpy(link->firmware_commit, firmware_commit, BWG_FIRMWARE_COMMIT_LEN + 1);
    memcpy(link->app_elf_hex, app_elf_hex, BWG_APP_ELF_HEX_LEN + 1);
    bwg_accumulator_init(&link->accumulator, allocator);
    return true;
}

void bwg_link_deinit(bwg_link_t *link, uint64_t now_ms)
{
    close_link(link, now_ms);
    bwg_accumulator_clear(&link->accumulator);
    bwg_accumulator_deinit(&link->accumulator);
}

void bwg_link_feed(bwg_link_t *link, const uint8_t *bytes, size_t len, uint64_t now_ms)
{
    for (size_t i = 0; i < len; i++) {
        receive_byte(link, bytes[i], now_ms);
    }
}

void bwg_link_supervise(bwg_link_t *link, uint64_t now_ms)
{
    if (!link->open) {
        return;
    }
    bwg_link_failure_t failure;
    if (bwg_link_shared_current_epoch(link->shared) != link->epoch) {
        failure = BWG_LINK_FAILURE_SESSION_REVOKED;
    } else if (!liveness_poll(link, now_ms)) {
        failure = BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT;
    } else {
        return;
    }
    fail_and_close(link, failure, link->observed_bytes, now_ms);
    reset_input(link);
}

void bwg_link_read_failed(bwg_link_t *link, uint64_t now_ms)
{
    if (link->open) {
        link->ops.on_failure(link->ops.ctx, BWG_LINK_FAILURE_READ, link->observed_bytes);
    }
    close_link(link, now_ms);
    reset_input(link);
}

uint32_t bwg_link_epoch(const bwg_link_t *link)
{
    return link->open ? link->epoch : 0;
}
