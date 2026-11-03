#include <string.h>

#include "bwg_codec.h"
#include "bwg_controller_internal.h"
#include "bwg_json.h"

#define REVIEW_REPLY_BYTES 768u
#define REQUEST_NONCE_BYTES 16u
#define CAMPAIGN_ID_BYTES 16u
/** The stimulated tick must land well inside the lease, never at its expiry edge. */
#define STIMULUS_LEASE_HEADROOM_MS 5000u

static bool payload_is_empty(const bwg_ctrl_request_t *request)
{
    return request->maybe_payload != NULL && bwg_json_has_exact_fields(request->maybe_payload, NULL, 0, NULL, 0);
}

/** Reviews need fresh possession, no lease and no pending cleanup or admission. */
static bwg_ctrl_error_t require_review_idle(const bwg_controller_t *controller, uint64_t now_ms)
{
    if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (controller->has_active || controller->cleanup_required || controller->has_cleanup_reason ||
        controller->has_pending_token || controller->restoration == BWG_RESTORATION_PENDING) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    return BWG_CTRL_OK;
}

static void write_str(bwg_jw_t *writer, const char *key, const char *value)
{
    bwg_jw_key(writer, key);
    bwg_jw_cstr(writer, value);
}

static void write_stimulus_review(const bwg_controller_t *controller, bwg_jw_t *writer)
{
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "discontinuitiesDetected");
    bwg_jw_u64(writer, controller->clock_discontinuities);
    bwg_jw_key(writer, "offsetMilliseconds");
    bwg_jw_u64(writer, BWG_CLOCK_STIMULUS_OFFSET_MS);
    write_str(writer, "schema", "worker-clock-discontinuity-stimulus-review-v1");
    write_str(writer, "state", bwg_clock_stimulus_label(&controller->stimulus));
    bwg_jw_object_end(writer);
}

static void write_boot_review(const bwg_controller_t *controller, bwg_jw_t *writer)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    bwg_jw_object_begin(writer);
    write_str(writer, "resetCause", ops->maybe_boot_reset_cause != NULL ? ops->maybe_boot_reset_cause(ops->ctx) : "other");
    write_str(writer, "schema", "worker-boot-review-v1");
    bwg_jw_object_end(writer);
}

static void write_rejection_record(const bwg_rejection_record_t *record, bwg_jw_t *writer)
{
    bwg_jw_object_begin(writer);
    write_str(writer, "context", bwg_auth_context_label(record->attribution.context));
    write_str(writer, "operation", bwg_auth_operation_label(record->operation));
    bwg_jw_key(writer, "ordinal");
    bwg_jw_u64(writer, record->ordinal);
    write_str(writer, "replayGuard", bwg_auth_replay_guard_label(record->attribution.replay_guard));
    write_str(writer, "safeStop", record->has_safe_stop ? bwg_reason_name(record->safe_stop) : "none");
    write_str(writer, "signature", bwg_auth_signature_label(record->attribution.signature));
    bwg_jw_object_end(writer);
}

static bwg_ctrl_error_t write_rejection_review(const bwg_controller_t *controller, bwg_jw_t *writer)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    char fingerprint[BWG_FINGERPRINT_HEX_SIZE];
    if (ops->maybe_high_water_fingerprint == NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!ops->maybe_high_water_fingerprint(ops->ctx, fingerprint)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    const bwg_rejection_log_t *log = &controller->rejections;
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "bootRejections");
    bwg_jw_u64(writer, log->boot_rejections);
    bwg_jw_key(writer, "highWater");
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "advancedThisBoot");
    bwg_jw_bool(writer, log->high_water_advanced);
    write_str(writer, "fingerprintSha256", fingerprint);
    bwg_jw_object_end(writer);
    bwg_jw_key(writer, "last");
    if (log->has_last) {
        write_rejection_record(&log->last, writer);
    } else {
        bwg_jw_null(writer);
    }
    write_str(writer, "schema", "worker-authorization-rejection-review-v2");
    bwg_jw_object_end(writer);
    return BWG_CTRL_OK;
}

bool bwg_ctrl_is_review(bwg_str_t command)
{
    return bwg_str_equals(command, "clock_discontinuity_stimulus_review") || bwg_str_equals(command, "boot_review") ||
           bwg_str_equals(command, "authorization_rejection_review") ||
           bwg_str_equals(command, "qualification_attempt_review") || bwg_str_equals(command, "soak_allowance_review");
}

static bwg_ctrl_error_t write_budget_review(const bwg_controller_t *controller, bwg_allowance_kind_t kind,
                                            bwg_str_t campaign_id, bwg_jw_t *writer)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (ops->maybe_budget_review == NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    return ops->maybe_budget_review(ops->ctx, kind, campaign_id, writer) == BWG_EVIDENCE_WRITTEN ? BWG_CTRL_OK
                                                                                                : BWG_CTRL_SESSION_FAILED;
}

bwg_ctrl_error_t bwg_ctrl_prepare_review(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                         bwg_ctrl_outcome_t *outcome)
{
    if (!payload_is_empty(request)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    bwg_ctrl_error_t error = require_review_idle(controller, now_ms);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, REVIEW_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (bwg_str_equals(request->command, "boot_review")) {
        write_boot_review(controller, &writer);
    } else if (bwg_str_equals(request->command, "clock_discontinuity_stimulus_review")) {
        write_stimulus_review(controller, &writer);
    } else if (bwg_str_equals(request->command, "qualification_attempt_review")) {
        error = write_budget_review(controller, BWG_ALLOWANCE_ATTEMPT, (bwg_str_t) {0}, &writer);
    } else if (bwg_str_equals(request->command, "soak_allowance_review")) {
        error = write_budget_review(controller, BWG_ALLOWANCE_SOAK, (bwg_str_t) {0}, &writer);
    } else {
        error = write_rejection_review(controller, &writer);
    }
    return error != BWG_CTRL_OK ? error : bwg_ctrl_reply_close(outcome, &writer);
}

bwg_ctrl_error_t bwg_ctrl_prepare_campaign_review(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                                  uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    static const char *const REQUIRED[] = {"campaignId"};
    bwg_str_t campaign_id;
    if (request->maybe_payload == NULL || !bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 1, NULL, 0) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->maybe_payload, "campaignId"), &campaign_id)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    bwg_ctrl_error_t error = require_review_idle(controller, now_ms);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    if (!bwg_b64url_is_canonical(campaign_id.ptr, campaign_id.len, CAMPAIGN_ID_BYTES)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, REVIEW_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    error = write_budget_review(controller, BWG_ALLOWANCE_CAMPAIGN, campaign_id, &writer);
    return error != BWG_CTRL_OK ? error : bwg_ctrl_reply_close(outcome, &writer);
}

static bool stimulus_window_is_open(const bwg_controller_t *controller, uint64_t now_ms)
{
    if (!controller->has_active || controller->has_cleanup_reason || !controller->has_last_ms ||
        controller->last_ms < BWG_CLOCK_STIMULUS_OFFSET_MS) {
        return false;
    }
    const uint64_t expires_ms = controller->deadlines.expires_at_ms;
    return expires_ms > now_ms && expires_ms - now_ms >= STIMULUS_LEASE_HEADROOM_MS;
}

bwg_ctrl_error_t bwg_ctrl_prepare_stimulus(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                           bwg_ctrl_outcome_t *outcome)
{
    static const char *const REQUIRED[] = {"requestNonce"};
    bwg_str_t nonce;
    if (request->maybe_payload == NULL || !bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 1, NULL, 0) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->maybe_payload, "requestNonce"), &nonce) ||
        !bwg_b64url_is_canonical(nonce.ptr, nonce.len, REQUEST_NONCE_BYTES)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (bwg_ctrl_maybe_active_context(controller) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (!stimulus_window_is_open(controller, now_ms) || !bwg_clock_stimulus_can_prepare(&controller->stimulus)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, REVIEW_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "armedForMilliseconds");
    bwg_jw_u64(&writer, BWG_CLOCK_STIMULUS_ARMED_MS);
    bwg_jw_key(&writer, "offsetMilliseconds");
    bwg_jw_u64(&writer, BWG_CLOCK_STIMULUS_OFFSET_MS);
    bwg_jw_key(&writer, "requestNonce");
    bwg_jw_string(&writer, nonce.ptr, nonce.len);
    write_str(&writer, "schema", "worker-clock-discontinuity-stimulus-v1");
    bwg_jw_object_end(&writer);
    bwg_ctrl_error_t error = bwg_ctrl_reply_close(outcome, &writer);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    if (controller->next_token != UINT64_MAX) {
        controller->next_token++;
    }
    bwg_clock_stimulus_prepare(&controller->stimulus, controller->next_token);
    outcome->effect.kind = BWG_CTRL_EFFECT_CLOCK_STIMULUS;
    outcome->effect.generation = controller->generation;
    outcome->effect.token = controller->next_token;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_confirm_stimulus(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms)
{
    uint64_t token;
    if (!bwg_clock_stimulus_take_pending(&controller->stimulus, &token) || token != effect->token) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    bwg_ctrl_error_t error = bwg_ctrl_enforce_clock(controller, now_ms);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    if (bwg_ctrl_maybe_active_context(controller) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (!stimulus_window_is_open(controller, now_ms)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    return bwg_clock_stimulus_arm(&controller->stimulus, now_ms) ? BWG_CTRL_OK : BWG_CTRL_STALE_RESPONSE;
}
