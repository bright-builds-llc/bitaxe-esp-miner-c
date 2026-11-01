#include "bwg_codec.h"
#include "bwg_controller_internal.h"
#include "bwg_json.h"
#include "bwg_trace.h"

#define EVIDENCE_REPLY_BYTES 384u
#define REQUEST_NONCE_BYTES 16u
#define MAX_SAFE_BOOT_ORDINAL 9007199254740990ull

/** A restart needs fresh possession and an idle board with nothing left to clean up. */
static bwg_ctrl_error_t require_evidence_idle(const bwg_controller_t *controller, uint64_t now_ms)
{
    if (bwg_ctrl_start_context(controller, now_ms) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (controller->has_active || controller->cleanup_required || controller->has_cleanup_reason ||
        controller->has_pending_token || controller->restoration == BWG_RESTORATION_PENDING || bwg_ctrl_attempt_busy(controller)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    return BWG_CTRL_OK;
}

/** A request that ends this boot, a restart or a panic: one per session, with its reply sent first. */
static bwg_ctrl_error_t prepare_boot_end(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                         bwg_ctrl_outcome_t *outcome, bool (*maybe_context)(void *, uint64_t *),
                                         bool has_effect, const char *schema, bwg_ctrl_effect_kind_t kind)
{
    static const char *const REQUIRED[] = {"requestNonce", "expectedBootOrdinal"};
    const bwg_controller_ops_t *ops = &controller->ops;
    bwg_str_t nonce;
    uint64_t expected = 0;
    if (maybe_context == NULL || !has_effect || request->maybe_payload == NULL ||
        !bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 2, NULL, 0) ||
        !bwg_json_get_str(bwg_json_field(request->maybe_payload, "requestNonce"), &nonce) ||
        !bwg_b64url_is_canonical(nonce.ptr, nonce.len, REQUEST_NONCE_BYTES) ||
        !bwg_json_get_u64(bwg_json_field(request->maybe_payload, "expectedBootOrdinal"), &expected) || expected == 0 ||
        expected > MAX_SAFE_BOOT_ORDINAL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return idle;
    }
    uint64_t boot_ordinal = 0;
    if (controller->restart_consumed || !maybe_context(ops->ctx, &boot_ordinal) || boot_ordinal != expected) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, EVIDENCE_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    bwg_jw_object_begin(&writer);
    bwg_jw_key(&writer, "bootOrdinal");
    bwg_jw_u64(&writer, boot_ordinal);
    bwg_jw_key(&writer, "nextBootOrdinal");
    bwg_jw_u64(&writer, boot_ordinal + 1);
    bwg_jw_key(&writer, "requestNonce");
    bwg_jw_string(&writer, nonce.ptr, nonce.len);
    bwg_jw_key(&writer, "schema");
    bwg_jw_cstr(&writer, schema);
    bwg_jw_object_end(&writer);
    const bwg_ctrl_error_t error = bwg_ctrl_reply_close(outcome, &writer);
    if (error != BWG_CTRL_OK) {
        return error;
    }
    controller->restart_consumed = true;
    outcome->effect.kind = kind;
    outcome->effect.generation = controller->generation;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_prepare_restart(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                          bwg_ctrl_outcome_t *outcome)
{
    return prepare_boot_end(controller, request, now_ms, outcome, controller->ops.maybe_restart_context,
                            controller->ops.maybe_restart != NULL, "worker-qualification-restart-v1",
                            BWG_CTRL_EFFECT_QUALIFICATION_RESTART);
}

bwg_ctrl_error_t bwg_ctrl_prepare_core_dump_self_test(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                                      uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    return prepare_boot_end(controller, request, now_ms, outcome, controller->ops.maybe_core_dump_context,
                            controller->ops.maybe_core_dump_self_test != NULL,
                            "worker-qualification-core-dump-self-test-v1", BWG_CTRL_EFFECT_CORE_DUMP_SELF_TEST);
}

bwg_ctrl_error_t bwg_ctrl_confirm_restart(bwg_controller_t *controller, uint64_t now_ms)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    uint64_t boot_ordinal = 0;
    if (!ops->maybe_restart_context(ops->ctx, &boot_ordinal)) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    ops->maybe_restart(ops->ctx);
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_confirm_core_dump_self_test(bwg_controller_t *controller, uint64_t now_ms)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    uint64_t boot_ordinal = 0;
    if (!ops->maybe_core_dump_context(ops->ctx, &boot_ordinal)) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    ops->maybe_core_dump_self_test(ops->ctx);
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_prepare_endpoint(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                           bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (ops->maybe_write_endpoint == NULL || request->maybe_payload == NULL ||
        !bwg_json_has_exact_fields(request->maybe_payload, NULL, 0, NULL, 0)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return idle;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, EVIDENCE_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!ops->maybe_write_endpoint(ops->ctx, &writer)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    return bwg_ctrl_reply_close(outcome, &writer);
}

bwg_ctrl_error_t bwg_ctrl_prepare_trace(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                        bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (ops->maybe_write_trace == NULL || request->maybe_payload == NULL ||
        !bwg_json_has_exact_fields(request->maybe_payload, NULL, 0, NULL, 0)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return idle;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_TRACE_REVIEW_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    ops->maybe_write_trace(ops->ctx, &writer);
    return bwg_ctrl_reply_close(outcome, &writer);
}

static bool parse_phase(yyjson_val *maybe_value, bwg_cadence_phase_t *phase)
{
    bwg_str_t label;
    if (!bwg_json_get_str(maybe_value, &label)) {
        return false;
    }
    if (bwg_str_equals(label, "idle")) {
        *phase = BWG_CADENCE_IDLE;
    } else if (bwg_str_equals(label, "usb")) {
        *phase = BWG_CADENCE_USB;
    } else if (bwg_str_equals(label, "mining")) {
        *phase = BWG_CADENCE_MINING;
    } else {
        return false;
    }
    return true;
}

bwg_ctrl_error_t bwg_ctrl_prepare_cadence_arm(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                              bwg_ctrl_outcome_t *outcome)
{
    static const char *const REQUIRED[] = {"phase"};
    const bwg_controller_ops_t *ops = &controller->ops;
    bwg_cadence_phase_t phase;
    if (ops->maybe_cadence_arm == NULL || request->maybe_payload == NULL ||
        !bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 1, NULL, 0) ||
        !parse_phase(bwg_json_field(request->maybe_payload, "phase"), &phase)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return idle;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, EVIDENCE_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!ops->maybe_cadence_arm(ops->ctx, phase, &writer)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    return bwg_ctrl_reply_close(outcome, &writer);
}

bwg_ctrl_error_t bwg_ctrl_prepare_cadence_review(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                                 bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (ops->maybe_write_cadence == NULL || request->maybe_payload == NULL ||
        !bwg_json_has_exact_fields(request->maybe_payload, NULL, 0, NULL, 0)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const bwg_ctrl_error_t idle = require_evidence_idle(controller, now_ms);
    if (idle != BWG_CTRL_OK) {
        return idle;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_CADENCE_REVIEW_BYTES + EVIDENCE_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    ops->maybe_write_cadence(ops->ctx, &writer);
    return bwg_ctrl_reply_close(outcome, &writer);
}
