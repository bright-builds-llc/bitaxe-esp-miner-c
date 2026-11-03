#include <string.h>

#include "bwg_controller_internal.h"
#include "bwg_frame.h"
#include "bwg_json.h"

#define REJECTION_CAPACITY 384u

static bool frame_is_well_formed(const uint8_t *frame, size_t len)
{
    if (len < 2 || len > BWG_MAX_CONTROL_PAYLOAD_BYTES + 1 || frame[len - 1] != '\n') {
        return false;
    }
    return memchr(frame, '\n', len - 1) == NULL && memchr(frame, '\r', len - 1) == NULL;
}

static bool request_id_is_valid(bwg_str_t id)
{
    static const char PREFIX[] = "serial_";
    size_t prefix_len = sizeof(PREFIX) - 1;
    if (id.len < prefix_len || id.len > 128 || memcmp(id.ptr, PREFIX, prefix_len) != 0) {
        return false;
    }
    for (size_t i = 0; i < id.len; i++) {
        char ch = id.ptr[i];
        bool allowed = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                       ch == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool parse_controller_envelope(bwg_ctrl_request_t *request)
{
    static const char *const REQUIRED[] = {"protocolVersion", "requestId", "command"};
    static const char *const OPTIONAL[] = {"payload"};
    if (!bwg_json_has_exact_fields(request->root, REQUIRED, 3, OPTIONAL, 1)) {
        return false;
    }
    bwg_str_t version;
    if (!bwg_json_get_str(bwg_json_maybe_field(request->root, "protocolVersion"), &version) ||
        !bwg_str_equals(version, BWG_CONTROLLER_PROTOCOL) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->root, "requestId"), &request->request_id) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->root, "command"), &request->command)) {
        return false;
    }
    if (!request_id_is_valid(request->request_id)) {
        return false;
    }
    yyjson_val *maybe_payload = bwg_json_maybe_field(request->root, "payload");
    request->maybe_payload = yyjson_is_null(maybe_payload) ? NULL : maybe_payload;
    return true;
}

bwg_ctrl_error_t bwg_ctrl_parse_frame(const bwg_controller_t *controller, const uint8_t *frame, size_t len,
                                      bwg_ctrl_request_t *request)
{
    memset(request, 0, sizeof(*request));
    request->frame_len = len;
    if (!frame_is_well_formed(frame, len)) {
        return BWG_CTRL_INVALID_FRAME;
    }
    bwg_json_status_t status = bwg_json_parse(controller->allocator, frame, len - 1, &request->doc);
    if (status == BWG_JSON_NO_MEMORY) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (status != BWG_JSON_OK) {
        return BWG_CTRL_INVALID_FRAME;
    }
    request->root = yyjson_doc_get_root(request->doc);
    if (!yyjson_is_obj(request->root) || !bwg_json_object_is_unique(request->root)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    yyjson_val *maybe_profile = bwg_json_maybe_field(request->root, "profile");
    if (maybe_profile != NULL) {
        request->is_possession = yyjson_is_str(maybe_profile);
        return request->is_possession && bwg_json_doc_is_unique(request->doc) ? BWG_CTRL_OK : BWG_CTRL_INVALID_REQUEST;
    }
    if (!parse_controller_envelope(request)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    request->correlated = true;
    return bwg_json_doc_is_unique(request->doc) ? BWG_CTRL_OK : BWG_CTRL_INVALID_REQUEST;
}

static void write_envelope_head(bwg_jw_t *writer, bwg_str_t request_id)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "protocolVersion");
    bwg_jw_cstr(writer, BWG_CONTROLLER_PROTOCOL);
    bwg_jw_key(writer, "requestId");
    bwg_jw_string(writer, request_id.ptr, request_id.len);
}

bool bwg_ctrl_reply_open(const bwg_controller_t *controller, bwg_ctrl_outcome_t *outcome, bwg_str_t request_id,
                         size_t capacity, bwg_jw_t *writer)
{
    if (!bwg_buffer_alloc(controller->allocator, &outcome->response, capacity)) {
        return false;
    }
    bwg_jw_init_buffer(writer, outcome->response.data, outcome->response.capacity);
    write_envelope_head(writer, request_id);
    bwg_jw_key(writer, "ok");
    bwg_jw_bool(writer, true);
    bwg_jw_key(writer, "result");
    return true;
}

bwg_ctrl_error_t bwg_ctrl_reply_close(bwg_ctrl_outcome_t *outcome, bwg_jw_t *writer)
{
    bwg_jw_object_end(writer);
    if (!bwg_jw_ok(writer)) {
        return BWG_CTRL_ENCODING_FAILED;
    }
    if (writer->len > BWG_MAX_CONTROL_PAYLOAD_BYTES) {
        return BWG_CTRL_INVALID_FRAME;
    }
    outcome->response.len = writer->len;
    outcome->has_reply = true;
    return BWG_CTRL_OK;
}

void bwg_ctrl_reply_reject(const bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                           bwg_ctrl_error_t error, bwg_ctrl_outcome_t *outcome)
{
    bwg_buffer_release(controller->allocator, &outcome->response);
    outcome->has_reply = false;
    memset(&outcome->effect, 0, sizeof(outcome->effect));
    if (!request->correlated || !bwg_buffer_alloc(controller->allocator, &outcome->response, REJECTION_CAPACITY)) {
        return;
    }
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, outcome->response.data, outcome->response.capacity);
    write_envelope_head(&writer, request->request_id);
    bwg_jw_key(&writer, "ok");
    bwg_jw_bool(&writer, false);
    bwg_jw_key(&writer, "error");
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "code");
    bwg_jw_cstr(&writer, "command_rejected");
    bwg_jw_key(&writer, "message");
    bwg_jw_cstr(&writer, bwg_ctrl_error_category(error));
    bwg_jw_object_end(&writer);
    bwg_jw_object_end(&writer);
    if (!bwg_jw_ok(&writer)) {
        bwg_buffer_release(controller->allocator, &outcome->response);
        return;
    }
    outcome->response.len = writer.len;
    outcome->has_reply = true;
}

size_t bwg_ctrl_probe_overhead(bwg_str_t request_id, uint64_t request_padding_bytes)
{
    // The request ID is at most 128 characters that need no escaping, so this always fits.
    uint8_t scratch[REJECTION_CAPACITY];
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, scratch, sizeof(scratch));
    write_envelope_head(&writer, request_id);
    bwg_jw_key(&writer, "ok");
    bwg_jw_bool(&writer, true);
    bwg_jw_key(&writer, "result");
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "padding");
    bwg_jw_cstr(&writer, "");
    bwg_jw_key(&writer, "requestPaddingBytes");
    bwg_jw_u64(&writer, request_padding_bytes);
    bwg_jw_object_end(&writer);
    bwg_jw_object_end(&writer);
    return writer.len;
}

static void write_restoration(bwg_jw_t *writer, const char *status, const char *maybe_reason)
{
    bwg_jw_key(writer, "restoration");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "status");
    bwg_jw_cstr(writer, status);
    if (maybe_reason != NULL) {
        bwg_jw_key(writer, "reason");
        bwg_jw_cstr(writer, maybe_reason);
    }
    bwg_jw_object_end(writer);
}

static void write_lease(const bwg_controller_t *controller, bwg_jw_t *writer)
{
    bwg_jw_key(writer, "lease");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "leaseId");
    bwg_jw_cstr(writer, controller->lease_id);
    bwg_jw_key(writer, "challengeId");
    bwg_jw_cstr(writer, controller->challenge_id);
    bwg_jw_key(writer, "renewAtMonotonicMilliseconds");
    bwg_jw_u64(writer, controller->deadlines.renew_at_ms);
    bwg_jw_key(writer, "expiresAtMonotonicMilliseconds");
    bwg_jw_u64(writer, controller->deadlines.expires_at_ms);
    bwg_jw_object_end(writer);
}

static bwg_ctrl_error_t evidence_error(bwg_evidence_t evidence, bwg_jw_t *writer, const bwg_jw_t *saved)
{
    switch (evidence) {
    case BWG_EVIDENCE_WRITTEN:
        return BWG_CTRL_OK;
    case BWG_EVIDENCE_ABSENT:
        *writer = *saved;
        return BWG_CTRL_OK;
    case BWG_EVIDENCE_PERSISTENCE_FAILED:
        return BWG_CTRL_PERSISTENCE_FAILED;
    case BWG_EVIDENCE_SESSION_FAILED:
        return BWG_CTRL_SESSION_FAILED;
    }
    return BWG_CTRL_SESSION_FAILED;
}

static bwg_ctrl_error_t append_evidence(const bwg_controller_t *controller, bwg_jw_t *writer)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (ops->maybe_append_qualification != NULL) {
        bwg_jw_t saved = *writer;
        bwg_jw_key(writer, "qualification");
        bwg_ctrl_error_t error = evidence_error(ops->maybe_append_qualification(ops->ctx, writer), writer, &saved);
        if (error != BWG_CTRL_OK) {
            return error;
        }
    }
    if (ops->maybe_append_preservation == NULL || !controller->authenticated_logical_session) {
        return BWG_CTRL_OK;
    }
    bwg_jw_t saved = *writer;
    bwg_jw_key(writer, "preservation");
    bwg_evidence_t evidence = ops->maybe_append_preservation(ops->ctx, writer, controller->identity.fingerprint_hex);
    return evidence_error(evidence, writer, &saved);
}

bwg_ctrl_error_t bwg_ctrl_write_status(const bwg_controller_t *controller, bwg_jw_t *writer, uint64_t now_ms)
{
    if (!controller->has_active && !controller->boot_clear_required &&
        controller->restoration == BWG_RESTORATION_PENDING) {
        return BWG_CTRL_RESTORATION_PENDING;
    }
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "protocolVersion");
    bwg_jw_cstr(writer, BWG_CONTROLLER_PROTOCOL);
    bwg_jw_key(writer, "state");
    bwg_jw_cstr(writer, controller->has_active ? "mining" : "baseline");
    bwg_jw_key(writer, "monotonicMilliseconds");
    bwg_jw_u64(writer, now_ms);
    if (controller->has_active) {
        write_lease(controller, writer);
        write_restoration(writer, "pending", NULL);
    } else if (controller->boot_clear_required) {
        write_restoration(writer, "confirmed", bwg_reason_name(BWG_REASON_REBOOT));
    } else if (controller->restoration == BWG_RESTORATION_NOT_REQUIRED) {
        write_restoration(writer, "not_required", NULL);
    } else {
        write_restoration(writer, "confirmed", bwg_reason_name(controller->restoration_reason));
    }
    bwg_ctrl_error_t error = append_evidence(controller, writer);
    bwg_jw_object_end(writer);
    return error;
}
