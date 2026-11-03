#include "bwg_controller_internal.h"
#include "bwg_json.h"

#define COOLING_REPLY_BYTES 512u
#define FULL_FAN_PERCENT 100u
#define BASELINE_FAN_PERCENT 30u

static void write_no_budget_effects(bwg_jw_t *writer)
{
    bwg_jw_key(writer, "asic_effects");
    bwg_jw_bool(writer, false);
    bwg_jw_key(writer, "budget_reserved");
    bwg_jw_bool(writer, false);
}

static bwg_ctrl_error_t prove_fan(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                  bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (controller->cleanup_required || controller->boot_clear_required) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, COOLING_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!ops->effect_set(ops->ctx, BWG_EFFECT_PENDING)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    controller->cleanup_required = true;
    controller->restoration = BWG_RESTORATION_PENDING;
    uint32_t fan_rpm = 0;
    if (!ops->maybe_cooling_prove(ops->ctx, &fan_rpm)) {
        const bwg_ctrl_error_t stop_error = bwg_ctrl_safe_stop(controller, BWG_REASON_CONTROL_FAILED, now_ms);
        return stop_error != BWG_CTRL_OK ? stop_error : BWG_CTRL_SESSION_FAILED;
    }
    bwg_jw_object_begin(&writer);
    write_no_budget_effects(&writer);
    bwg_jw_key(&writer, "fan_duty_percent");
    bwg_jw_u64(&writer, FULL_FAN_PERCENT);
    bwg_jw_key(&writer, "fan_rpm");
    bwg_jw_u64(&writer, fan_rpm);
    bwg_jw_key(&writer, "post_command_fan_proven");
    bwg_jw_bool(&writer, true);
    bwg_jw_key(&writer, "schema");
    bwg_jw_cstr(&writer, "worker-cooling-proof-v1");
    bwg_jw_object_end(&writer);
    return bwg_ctrl_reply_close(outcome, &writer);
}

static bwg_ctrl_error_t restore_baseline(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                         bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (bwg_ctrl_maybe_active_context(controller) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (!controller->cleanup_required) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, COOLING_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!ops->maybe_cooling_restore(ops->ctx)) {
        const bwg_ctrl_error_t stop_error = bwg_ctrl_safe_stop(controller, BWG_REASON_CONTROL_FAILED, now_ms);
        return stop_error != BWG_CTRL_OK ? stop_error : BWG_CTRL_SESSION_FAILED;
    }
    // Whatever comes next starts from a fresh proof.
    controller->has_admission = false;
    controller->has_pending_token = false;
    controller->has_cleanup_reason = true;
    controller->cleanup_reason = BWG_REASON_CANCELLED;
    if (!ops->effect_set(ops->ctx, BWG_EFFECT_CLEAR)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    controller->cleanup_required = false;
    controller->has_cleanup_reason = false;
    controller->restoration = BWG_RESTORATION_CONFIRMED;
    controller->restoration_reason = BWG_REASON_CANCELLED;
    controller->has_last_ms = true;
    controller->last_ms = now_ms;
    bwg_jw_object_begin(&writer);
    write_no_budget_effects(&writer);
    bwg_jw_key(&writer, "cooling_proven");
    bwg_jw_bool(&writer, true);
    bwg_jw_key(&writer, "fan_duty_percent");
    bwg_jw_u64(&writer, BASELINE_FAN_PERCENT);
    bwg_jw_key(&writer, "schema");
    bwg_jw_cstr(&writer, "worker-cooling-baseline-v1");
    bwg_jw_object_end(&writer);
    return bwg_ctrl_reply_close(outcome, &writer);
}

bwg_ctrl_error_t bwg_ctrl_prepare_cooling(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                          bwg_ctrl_outcome_t *outcome)
{
    static const char *const REQUIRED[] = {"action"};
    bwg_str_t action;
    if (request->maybe_payload == NULL || !bwg_json_has_exact_fields(request->maybe_payload, REQUIRED, 1, NULL, 0) ||
        !bwg_json_get_str(bwg_json_maybe_field(request->maybe_payload, "action"), &action) ||
        controller->ops.maybe_cooling_prove == NULL || controller->ops.maybe_cooling_restore == NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    const bool prove = bwg_str_equals(action, "prove_fan");
    if (!prove && !bwg_str_equals(action, "restore_baseline")) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (controller->has_active || controller->has_cleanup_reason) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    return prove ? prove_fan(controller, request, now_ms, outcome) : restore_baseline(controller, request, now_ms, outcome);
}
