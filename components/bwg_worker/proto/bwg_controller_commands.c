#include <string.h>

#include "bwg_controller_internal.h"
#include "bwg_frame.h"
#include "bwg_json.h"

/** Room around the capability for the envelope: its fixed keys plus the longest request ID. */
#define DISCOVER_ENVELOPE_BYTES 256u

static bool seen_nonce(const bwg_controller_t *controller, const uint8_t digest[BWG_POSSESSION_NONCE_DIGEST_BYTES])
{
    for (size_t i = 0; i < controller->seen_count; i++) {
        const uint8_t *entry = controller->seen_nonces + i * BWG_POSSESSION_NONCE_DIGEST_BYTES;
        if (memcmp(entry, digest, BWG_POSSESSION_NONCE_DIGEST_BYTES) == 0) {
            return true;
        }
    }
    return false;
}

static bwg_ctrl_error_t prepare_possession(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                           uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    bwg_possession_request_t possession;
    if (!bwg_possession_parse_request(request->root, &possession)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->has_binding) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    uint8_t digest[BWG_POSSESSION_NONCE_DIGEST_BYTES];
    bwg_possession_nonce_digest(&possession, digest);
    if (!bwg_possession_matches(&possession, controller->capability_digest, controller->manifest_digest,
                                &controller->binding) ||
        seen_nonce(controller, digest) || controller->seen_count >= BWG_CONTROLLER_MAX_SEEN_NONCES ||
        controller->has_pending_token || bwg_ctrl_lifecycle_maybe_lease(&controller->lifecycle) != NULL) {
        return BWG_CTRL_INVALID_PROOF;
    }
    bwg_ctrl_error_t error = bwg_ctrl_acknowledge_boot(controller);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    memcpy(controller->seen_nonces + controller->seen_count * BWG_POSSESSION_NONCE_DIGEST_BYTES, digest, sizeof(digest));
    controller->seen_count++;
    if (!bwg_possession_prove(controller->allocator, &controller->identity, &possession, controller->firmware_commit,
                              controller->app_elf_hex, &outcome->response, outcome->effect.binding)) {
        return BWG_CTRL_ENCODING_FAILED;
    }
    if (controller->next_token != UINT64_MAX) {
        controller->next_token++;
    }
    controller->has_pending_token = true;
    controller->pending_token = controller->next_token;
    outcome->has_reply = true;
    outcome->effect.kind = BWG_CTRL_EFFECT_ADMIT;
    outcome->effect.generation = controller->generation;
    outcome->effect.token = controller->next_token;
    outcome->effect.established_at_ms = now_ms;
    return BWG_CTRL_OK;
}

static bool deadlines_from_window(uint64_t now_ms, uint64_t duration_ms, uint64_t renew_after_ms,
                                  bwg_lease_deadlines_t *deadlines)
{
    if (duration_ms > UINT64_MAX - now_ms || renew_after_ms > UINT64_MAX - now_ms) {
        return false;
    }
    deadlines->renew_at_ms = now_ms + renew_after_ms;
    deadlines->expires_at_ms = now_ms + duration_ms;
    return true;
}

/** Past this point a status is the only thing left, and the buffer exists, so it cannot run out of memory. */
static bwg_ctrl_error_t finish_status(bwg_controller_t *controller, bwg_ctrl_outcome_t *outcome, bwg_jw_t *writer,
                                      uint64_t now_ms)
{
    bwg_ctrl_error_t error = bwg_ctrl_write_status(controller, writer, now_ms);
    return error != BWG_CTRL_OK ? error : bwg_ctrl_reply_close(outcome, writer);
}

static bwg_ctrl_error_t stop_then(bwg_controller_t *controller, uint64_t now_ms, bwg_ctrl_error_t error)
{
    bwg_ctrl_error_t stop_error = bwg_ctrl_safe_stop(controller, BWG_REASON_CONTROL_FAILED, now_ms);
    return stop_error != BWG_CTRL_OK ? stop_error : error;
}

static bwg_ctrl_error_t prepare_start(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                      bwg_ctrl_outcome_t *outcome)
{
    bwg_lease_grant_t grant;
    if (request->maybe_payload == NULL || !bwg_lease_grant_parse(request->maybe_payload, &grant)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const char *maybe_binding = bwg_ctrl_maybe_start_context(controller, now_ms);
    if (maybe_binding == NULL) {
        bwg_ctrl_note_context_rejection(controller, BWG_AUTH_OPERATION_START);
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (!bwg_ctrl_lifecycle_is_quiet(&controller->lifecycle) || bwg_ctrl_attempt_busy(controller)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    if (!bwg_lease_grant_validate(&grant)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (grant.stratum_kind == BWG_STRATUM_V2) {
        const bwg_ctrl_error_t observed = bwg_ctrl_validate_v2_share_observation(controller);
        if (observed != BWG_CTRL_OK) {
            return observed;
        }
    }
    // Allocated before verifying, so that running out of memory cannot burn an authorization's sequence.
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_CONTROLLER_STATUS_MAX_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (bwg_ctrl_verify_start(controller, &grant, maybe_binding) != BWG_AUTH_ACCEPTED) {
        return BWG_CTRL_AUTHENTICATION_FAILED;
    }
    bwg_lease_deadlines_t deadlines;
    if (!deadlines_from_window(now_ms, grant.duration_ms, grant.renew_after_ms, &deadlines)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->ops.effect_set(controller->ops.ctx, BWG_EFFECT_PENDING)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    const bool exact_window = grant.allowance.kind == BWG_ALLOWANCE_SOAK || grant.stratum_kind == BWG_STRATUM_V2;
    bwg_ctrl_lifecycle_start_lease(&controller->lifecycle, grant.lease_id, grant.challenge_id, deadlines, exact_window);
    if (grant.stratum_kind == BWG_STRATUM_V2) {
        controller->has_v2_generation = true;
        controller->v2_generation = controller->generation;
    }
    if (!controller->ops.start(controller->ops.ctx, &grant, deadlines)) {
        return stop_then(controller, now_ms, BWG_CTRL_SESSION_FAILED);
    }
    return finish_status(controller, outcome, &writer, now_ms);
}

static bwg_ctrl_error_t prepare_renew(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                      bwg_ctrl_outcome_t *outcome)
{
    bwg_lease_renewal_t renewal;
    if (request->maybe_payload == NULL || !bwg_lease_renewal_parse(request->maybe_payload, &renewal)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const char *maybe_binding = bwg_ctrl_maybe_active_context(controller);
    if (maybe_binding == NULL) {
        bwg_ctrl_note_context_rejection(controller, BWG_AUTH_OPERATION_RENEW);
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    const bwg_ctrl_lease_t *maybe_lease = bwg_ctrl_lifecycle_maybe_lease(&controller->lifecycle);
    if (!bwg_lease_renewal_validate(&renewal) ||
        (maybe_lease != NULL && maybe_lease->exact_window &&
         (renewal.duration_ms != BWG_SOAK_DURATION_MS || renewal.renew_after_ms != BWG_SOAK_RENEW_AFTER_MS))) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (maybe_lease == NULL) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_CONTROLLER_STATUS_MAX_BYTES, &writer)) {
        return stop_then(controller, now_ms, BWG_CTRL_SESSION_FAILED);
    }
    if (!bwg_str_equals(renewal.lease_id, maybe_lease->lease_id)) {
        return stop_then(controller, now_ms, BWG_CTRL_AUTHENTICATION_FAILED);
    }
    if (bwg_ctrl_verify_renew(controller, &renewal, maybe_lease->challenge_id, maybe_binding) != BWG_AUTH_ACCEPTED) {
        // Attributed first, so the record doesn't depend on how the stop goes.
        bwg_rejection_log_attribute_stop(&controller->rejections, BWG_REASON_CONTROL_FAILED);
        return stop_then(controller, now_ms, BWG_CTRL_AUTHENTICATION_FAILED);
    }
    bwg_lease_deadlines_t deadlines;
    if (!deadlines_from_window(now_ms, renewal.duration_ms, renewal.renew_after_ms, &deadlines)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->ops.renew(controller->ops.ctx, &renewal, deadlines)) {
        return stop_then(controller, now_ms, BWG_CTRL_SESSION_FAILED);
    }
    bwg_ctrl_lifecycle_renew_lease(&controller->lifecycle, deadlines);
    return finish_status(controller, outcome, &writer, now_ms);
}

static bwg_ctrl_error_t reply_with_status(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                          uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_CONTROLLER_STATUS_MAX_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    return finish_status(controller, outcome, &writer, now_ms);
}

static bwg_ctrl_error_t stop_and_report(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                        bwg_reason_t reason, uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    bwg_ctrl_error_t error = bwg_ctrl_safe_stop(controller, reason, now_ms);
    return error != BWG_CTRL_OK ? error : reply_with_status(controller, request, now_ms, outcome);
}

static bwg_ctrl_error_t prepare_restore(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                        uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    static const char *const REQUIRED[] = {"reason"};
    bwg_str_t text;
    bwg_reason_t reason;
    if (!bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 1, NULL, 0) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->maybe_payload, "reason"), &text) ||
        !bwg_reason_parse(text.ptr, text.len, &reason)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    return stop_and_report(controller, request, reason, now_ms, outcome);
}

static bwg_ctrl_error_t prepare_status(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                       uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    if (bwg_ctrl_attempt_busy(controller) && bwg_ctrl_lifecycle_maybe_running_lease(&controller->lifecycle) == NULL) {
        return BWG_CTRL_RESTORATION_PENDING;
    }
    bwg_ctrl_error_t error = reply_with_status(controller, request, now_ms, outcome);
    if (error == BWG_CTRL_OK && bwg_ctrl_lifecycle_reboot_due(&controller->lifecycle)) {
        outcome->effect.kind = BWG_CTRL_EFFECT_BOOT_REPORTED;
        outcome->effect.generation = controller->generation;
    }
    return error;
}

static bwg_ctrl_error_t prepare_discover(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                         bwg_ctrl_outcome_t *outcome)
{
    bwg_jw_t writer;
    size_t capacity = controller->capability.len + DISCOVER_ENVELOPE_BYTES;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, capacity, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    bwg_jw_raw_value(&writer, controller->capability.data, controller->capability.len);
    return bwg_ctrl_reply_close(outcome, &writer);
}

static bool padding_is_fixed_pattern(bwg_str_t padding)
{
    for (size_t i = 0; i < padding.len; i++) {
        if (padding.ptr[i] != 'x') {
            return false;
        }
    }
    return true;
}

static bwg_ctrl_error_t prepare_probe(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                      bwg_ctrl_outcome_t *outcome)
{
    static const char *const REQUIRED[] = {"padding", "responsePaddingBytes"};
    bwg_str_t padding;
    uint64_t wanted;
    if (!bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 2, NULL, 0) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->maybe_payload, "padding"), &padding) ||
        !bwg_json_get_u64(bwg_json_maybe_field(request->maybe_payload, "responsePaddingBytes"), &wanted)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (bwg_ctrl_lifecycle_holds_effect(&controller->lifecycle)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    if (!padding_is_fixed_pattern(padding)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    size_t overhead = bwg_ctrl_probe_overhead(request->request_id, padding.len);
    if (wanted < padding.len || overhead > BWG_MAX_CONTROL_PAYLOAD_BYTES ||
        wanted > BWG_MAX_CONTROL_PAYLOAD_BYTES - overhead) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, overhead + (size_t) wanted, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "padding");
    bwg_jw_string_repeat(&writer, 'x', (size_t) wanted);
    bwg_jw_key(&writer, "requestPaddingBytes");
    bwg_jw_u64(&writer, padding.len);
    bwg_jw_object_end(&writer);
    const bwg_ctrl_error_t error = bwg_ctrl_reply_close(outcome, &writer);
    if (error == BWG_CTRL_OK && controller->ops.maybe_note_max_probe != NULL &&
        request->frame_len == BWG_MAX_CONTROL_PAYLOAD_BYTES + 1 && outcome->response.len == BWG_MAX_CONTROL_PAYLOAD_BYTES) {
        controller->ops.maybe_note_max_probe(controller->ops.ctx);
    }
    return error;
}

static bwg_ctrl_error_t route_no_payload(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                         uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    if (request->maybe_payload != NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (bwg_str_equals(request->command, "discover")) {
        return prepare_discover(controller, request, outcome);
    }
    if (bwg_str_equals(request->command, "status")) {
        return prepare_status(controller, request, now_ms, outcome);
    }
    bwg_reason_t reason = bwg_str_equals(request->command, "pause") ? BWG_REASON_PAUSED : BWG_REASON_CANCELLED;
    return stop_and_report(controller, request, reason, now_ms, outcome);
}

static bwg_ctrl_error_t prepare_controller(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                           uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    bwg_ctrl_error_t error = bwg_ctrl_acknowledge_boot(controller);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    bwg_str_t command = request->command;
    if (bwg_str_equals(command, "start_lease")) {
        return prepare_start(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "renew_lease")) {
        return prepare_renew(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "transport_probe")) {
        return request->maybe_payload == NULL ? BWG_CTRL_INVALID_REQUEST
                                              : prepare_probe(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "restore")) {
        return request->maybe_payload == NULL ? BWG_CTRL_INVALID_REQUEST
                                              : prepare_restore(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "clock_discontinuity_stimulus")) {
        if (bwg_ctrl_attempt_busy(controller)) {
            return BWG_CTRL_INVALID_TRANSITION;
        }
        return bwg_ctrl_prepare_stimulus(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "acceptance_budget_review")) {
        return bwg_ctrl_prepare_campaign_review(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "serial_trace_review")) {
        return bwg_ctrl_prepare_trace(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "qualification_restart")) {
        return bwg_ctrl_prepare_restart(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "qualification_core_dump_self_test")) {
        return bwg_ctrl_prepare_core_dump_self_test(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "telemetry_cadence_endpoint")) {
        return bwg_ctrl_prepare_endpoint(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "telemetry_cadence_arm")) {
        return bwg_ctrl_prepare_cadence_arm(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "telemetry_cadence_review")) {
        return bwg_ctrl_prepare_cadence_review(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "stratum_v2_channel_start") || bwg_str_equals(command, "stratum_v2_channel_cancel")) {
        return bwg_ctrl_prepare_v2(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "noise_diagnostic_start") || bwg_str_equals(command, "noise_diagnostic_cancel")) {
        return bwg_ctrl_prepare_noise(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "qualification_cooling")) {
        return bwg_ctrl_prepare_cooling(controller, request, now_ms, outcome);
    }
    if (bwg_ctrl_is_review(command)) {
        return bwg_ctrl_prepare_review(controller, request, now_ms, outcome);
    }
    if (bwg_str_equals(command, "discover") || bwg_str_equals(command, "status") || bwg_str_equals(command, "pause") ||
        bwg_str_equals(command, "cancel")) {
        return route_no_payload(controller, request, now_ms, outcome);
    }
    return BWG_CTRL_INVALID_REQUEST;
}

bwg_ctrl_error_t bwg_controller_frame(bwg_controller_t *controller, const uint8_t *frame, size_t len, uint64_t now_ms,
                                      bwg_ctrl_outcome_t *outcome)
{
    memset(outcome, 0, sizeof(*outcome));
    bwg_ctrl_request_t request;
    bwg_ctrl_error_t parse_error = bwg_ctrl_parse_frame(controller, frame, len, &request);
    // A retained attempt's status has no authority to give, so it stays readable while a stop is pending.
    const bool v2_status = parse_error == BWG_CTRL_OK && !request.is_possession && bwg_str_equals(request.command, "stratum_v2_status");
    const bool noise_status = parse_error == BWG_CTRL_OK && !request.is_possession && bwg_str_equals(request.command, "noise_diagnostic_status");
    // The clock runs even for a frame that failed to parse, and a clock failure is the one reported.
    bwg_ctrl_error_t error = v2_status || noise_status ? BWG_CTRL_OK : bwg_ctrl_enforce_clock(controller, now_ms);
    if (error == BWG_CTRL_OK) {
        error = parse_error;
    }
    if (error == BWG_CTRL_OK && v2_status) {
        error = bwg_ctrl_prepare_v2(controller, &request, now_ms, outcome);
    } else if (error == BWG_CTRL_OK && noise_status) {
        error = bwg_ctrl_prepare_noise(controller, &request, now_ms, outcome);
    } else if (error == BWG_CTRL_OK) {
        error = request.is_possession ? prepare_possession(controller, &request, now_ms, outcome)
                                      : prepare_controller(controller, &request, now_ms, outcome);
    }
    if (error != BWG_CTRL_OK) {
        bwg_ctrl_reply_reject(controller, &request, error, outcome);
        outcome->revoke_after_send = true;
    }
    outcome->error = error;
    if (request.doc != NULL) {
        yyjson_doc_free(request.doc);
    }
    return error;
}

void bwg_controller_outcome_release(bwg_controller_t *controller, bwg_ctrl_outcome_t *outcome)
{
    bwg_buffer_release(controller->allocator, &outcome->response);
    outcome->has_reply = false;
}
