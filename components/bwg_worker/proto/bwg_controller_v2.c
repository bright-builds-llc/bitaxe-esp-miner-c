#include <string.h>

#include "bwg_codec.h"
#include "bwg_controller_internal.h"

#define V2_ATTEMPT_ID_BYTES 16u
#define V2_OBSERVATION_MAX_AGE_US 5000000ull
#define MAX_SAFE_INTEGER 9007199254740991ull
#define V2_ENVELOPE_BYTES 512u

static bool attempt_id_is_valid(bwg_str_t attempt_id)
{
    return bwg_b64url_is_canonical(attempt_id.ptr, attempt_id.len, V2_ATTEMPT_ID_BYTES);
}

static bool parse_scope(yyjson_val *maybe_value, bwg_v2_scope_t *scope)
{
    bwg_str_t text;
    if (!bwg_json_get_str(maybe_value, &text)) {
        return false;
    }
    if (bwg_str_equals(text, "channel")) {
        *scope = BWG_V2_SCOPE_CHANNEL;
    } else if (bwg_str_equals(text, "share")) {
        *scope = BWG_V2_SCOPE_SHARE;
    } else {
        return false;
    }
    return true;
}

static bool schema_is(yyjson_val *payload, const char *expected)
{
    bwg_str_t schema;
    return bwg_json_get_str(bwg_json_maybe_field(payload, "schema"), &schema) && bwg_str_equals(schema, expected);
}

typedef struct {
    bwg_v2_scope_t scope;
    bool has_attempt_id;
    bwg_str_t attempt_id;
    bool cancel;
} v2_query_t;

static bool parse_query(const bwg_ctrl_request_t *request, v2_query_t *query)
{
    static const char *const QUERY[] = {"schema", "scope", "attemptId"};
    static const char *const CANCEL[] = {"schema", "attemptId"};
    yyjson_val *payload = request->maybe_payload;
    query->cancel = bwg_str_equals(request->command, "stratum_v2_channel_cancel");
    if (payload == NULL) {
        return false;
    }
    if (query->cancel) {
        query->scope = BWG_V2_SCOPE_CHANNEL;
        query->has_attempt_id = true;
        return bwg_json_has_exact_fields(payload, CANCEL, 2, NULL, 0) && schema_is(payload, "worker-stratum-v2-channel-cancel-v1") &&
               bwg_json_get_str(bwg_json_maybe_field(payload, "attemptId"), &query->attempt_id) && attempt_id_is_valid(query->attempt_id);
    }
    if (!bwg_json_has_exact_fields(payload, QUERY, 3, NULL, 0) || !schema_is(payload, "worker-stratum-v2-query-v1") ||
        !parse_scope(bwg_json_maybe_field(payload, "scope"), &query->scope)) {
        return false;
    }
    yyjson_val *attempt = bwg_json_maybe_field(payload, "attemptId");
    query->has_attempt_id = !yyjson_is_null(attempt);
    return !query->has_attempt_id || (bwg_json_get_str(attempt, &query->attempt_id) && attempt_id_is_valid(query->attempt_id));
}

static bool view_of(const bwg_controller_t *controller, bwg_v2_scope_t scope, bwg_v2_view_t *view)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    memset(view, 0, sizeof(*view));
    return ops->maybe_v2_view(ops->ctx, scope, controller->generation, view);
}

static bwg_ctrl_error_t reply_status(const bwg_controller_t *controller, const bwg_ctrl_request_t *request, bwg_v2_scope_t scope,
                                     const bwg_v2_observation_t *observation, bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_V2_STATUS_BYTES + V2_ENVELOPE_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    ops->maybe_v2_write_status(ops->ctx, scope, observation, &writer);
    return bwg_ctrl_reply_close(outcome, &writer);
}

static bwg_ctrl_error_t observe(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    v2_query_t query;
    if (!parse_query(request, &query)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    // A retained binding allows observing or cancelling an attempt, never starting one.
    if ((!controller->has_v2_generation || controller->v2_generation != controller->generation) &&
        bwg_ctrl_maybe_active_context(controller) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    bwg_v2_view_t view;
    if (!view_of(controller, query.scope, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!query.has_attempt_id && !view.has_record && !query.cancel && view.state == BWG_V2_STATE_IDLE) {
        if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
            return BWG_CTRL_ADMISSION_REQUIRED;
        }
        const bwg_ctrl_error_t error = reply_status(controller, request, query.scope, &view.observation, outcome);
        if (error != BWG_CTRL_OK) {
            return error;
        }
        controller->v2_pending_scope = query.scope;
        controller->v2_pending_observation = view.observation;
        outcome->effect.kind = BWG_CTRL_EFFECT_V2_OBSERVATION;
        outcome->effect.generation = controller->generation;
        return BWG_CTRL_OK;
    }
    if (!query.has_attempt_id || !view.has_record || !bwg_str_equals(query.attempt_id, view.attempt_id)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    controller->has_v2_generation = true;
    controller->v2_generation = controller->generation;
    if (query.cancel && view.state != BWG_V2_STATE_TERMINAL && !ops->maybe_v2_cancel(ops->ctx)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (query.cancel && !view_of(controller, query.scope, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    return reply_status(controller, request, query.scope, &view.observation, outcome);
}

bool bwg_ctrl_same_network(const bwg_v2_observation_t *current, const bwg_v2_observation_t *previous)
{
    return current->clock_valid && previous->clock_valid && current->wifi_connected && current->station_ipv4[0] != '\0' &&
           strcmp(current->station_ipv4, previous->station_ipv4) == 0 && current->boot_ordinal == previous->boot_ordinal &&
           current->worker_generation == previous->worker_generation &&
           current->serial_transport_epoch == previous->serial_transport_epoch;
}

bool bwg_ctrl_observation_is_fresh(const bwg_v2_observation_t *current, uint64_t since_us)
{
    return current->observed_at_us >= since_us && current->observed_at_us - since_us <= V2_OBSERVATION_MAX_AGE_US;
}

static bool parse_channel_start(yyjson_val *payload, bwg_v2_channel_input_t *input, uint64_t *boot_ordinal, uint64_t *observed_at_us)
{
    static const char *const REQUIRED[] = {"schema", "attemptId", "expectedBootOrdinal", "networkObservedAtUs", "stratum"};
    return payload != NULL && bwg_json_has_exact_fields(payload, REQUIRED, 5, NULL, 0) &&
           schema_is(payload, "worker-stratum-v2-channel-start-v1") &&
           bwg_json_get_str(bwg_json_maybe_field(payload, "attemptId"), &input->attempt_id) && attempt_id_is_valid(input->attempt_id) &&
           bwg_json_get_u64(bwg_json_maybe_field(payload, "expectedBootOrdinal"), boot_ordinal) && *boot_ordinal > 0 &&
           *boot_ordinal <= MAX_SAFE_INTEGER && bwg_json_get_u64(bwg_json_maybe_field(payload, "networkObservedAtUs"), observed_at_us) &&
           *observed_at_us <= MAX_SAFE_INTEGER &&
           bwg_stratum_v2_parse(bwg_json_maybe_field(payload, "stratum"), &input->endpoint, &input->authority_public_key, &input->user_identity) &&
           bwg_stratum_v2_is_valid(input->endpoint, input->authority_public_key, input->user_identity);
}

static bwg_ctrl_error_t admit(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (!bwg_ctrl_lifecycle_is_quiet(&controller->lifecycle) || bwg_ctrl_attempt_busy(controller)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_v2_channel_input_t input;
    uint64_t boot_ordinal = 0;
    uint64_t observed_at_us = 0;
    if (!parse_channel_start(request->maybe_payload, &input, &boot_ordinal, &observed_at_us)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->has_v2_observation) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    bwg_v2_view_t view;
    if (!view_of(controller, BWG_V2_SCOPE_CHANNEL, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (controller->v2_observation_scope != BWG_V2_SCOPE_CHANNEL || !bwg_ctrl_same_network(&view.observation, &controller->v2_observation) ||
        view.observation.boot_ordinal != boot_ordinal || controller->v2_observation.observed_at_us != observed_at_us ||
        !bwg_ctrl_observation_is_fresh(&view.observation, observed_at_us)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    // Reserved before admitting, so that running out of memory can't leave an attempt no reply announced.
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_V2_STATUS_BYTES + V2_ENVELOPE_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!ops->maybe_v2_admit(ops->ctx, &input, controller->generation)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    controller->has_v2_generation = true;
    controller->v2_generation = controller->generation;
    controller->has_v2_observation = false;
    ops->maybe_v2_write_status(ops->ctx, BWG_V2_SCOPE_CHANNEL, &view.observation, &writer);
    const bwg_ctrl_error_t error = bwg_ctrl_reply_close(outcome, &writer);
    if (error != BWG_CTRL_OK) {
        ops->maybe_v2_cancel(ops->ctx);
        return error;
    }
    outcome->effect.kind = BWG_CTRL_EFFECT_V2_DISPATCH;
    outcome->effect.generation = controller->generation;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_prepare_v2(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                     bwg_ctrl_outcome_t *outcome)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    if (ops->maybe_v2_view == NULL || ops->maybe_v2_write_status == NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->authenticated_logical_session || controller->has_pending_token) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    return bwg_str_equals(request->command, "stratum_v2_channel_start") ? admit(controller, request, now_ms, outcome)
                                                                        : observe(controller, request, now_ms, outcome);
}

bwg_ctrl_error_t bwg_ctrl_validate_v2_share_observation(bwg_controller_t *controller)
{
    if (!controller->has_v2_observation) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    bwg_v2_view_t view;
    if (controller->ops.maybe_v2_view == NULL || !view_of(controller, BWG_V2_SCOPE_SHARE, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (controller->v2_observation_scope != BWG_V2_SCOPE_SHARE || view.state != BWG_V2_STATE_IDLE ||
        !bwg_ctrl_same_network(&view.observation, &controller->v2_observation) ||
        !bwg_ctrl_observation_is_fresh(&view.observation, controller->v2_observation.observed_at_us)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    controller->has_v2_observation = false;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_confirm_v2_dispatch(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    bwg_ctrl_error_t error = bwg_ctrl_enforce_clock(controller, now_ms);
    if (error == BWG_CTRL_OK &&
        (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL || effect->generation != controller->generation ||
         !controller->has_v2_generation || controller->v2_generation != effect->generation || controller->has_pending_token ||
         bwg_ctrl_lifecycle_holds_effect(&controller->lifecycle) || !controller->authenticated_logical_session)) {
        error = BWG_CTRL_STALE_RESPONSE;
    }
    if (error == BWG_CTRL_OK && !ops->maybe_v2_dispatch(ops->ctx)) {
        error = BWG_CTRL_SESSION_FAILED;
    }
    if (error != BWG_CTRL_OK && !ops->maybe_v2_cancel(ops->ctx)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    return error;
}
