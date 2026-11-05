#include <sodium.h>
#include <string.h>

#include "bwg_codec.h"
#include "bwg_controller_internal.h"

#define NOISE_ATTEMPT_ID_BYTES 16u
#define NOISE_KEY_BYTES 32u
#define MAX_SAFE_INTEGER 9007199254740991ull
#define NOISE_ENVELOPE_BYTES 512u

bool bwg_ctrl_attempt_busy(const bwg_controller_t *controller)
{
    const bwg_controller_ops_t *ops = &controller->ops;
    const bool v2_busy = ops->maybe_v2 != NULL && ops->maybe_v2->busy(ops->ctx);
    return v2_busy || (ops->maybe_noise != NULL && ops->maybe_noise->busy(ops->ctx));
}

static bool attempt_id_is_valid(bwg_str_t attempt_id)
{
    return bwg_b64url_is_canonical(attempt_id.ptr, attempt_id.len, NOISE_ATTEMPT_ID_BYTES);
}

static bool schema_is(yyjson_val *payload, const char *expected)
{
    bwg_str_t schema;
    return bwg_json_get_str(bwg_json_maybe_field(payload, "schema"), &schema) && bwg_str_equals(schema, expected);
}

static bool view_of(const bwg_controller_t *controller, const bwg_noise_ops_t *noise, bwg_noise_view_t *view)
{
    memset(view, 0, sizeof(*view));
    return noise->view(controller->ops.ctx, controller->generation, view);
}

static bwg_ctrl_error_t reply_status(const bwg_controller_t *controller, const bwg_noise_ops_t *noise,
                                     const bwg_ctrl_request_t *request, const bwg_v2_observation_t *observation,
                                     bwg_ctrl_outcome_t *outcome)
{
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_NOISE_STATUS_BYTES + NOISE_ENVELOPE_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    noise->write_status(controller->ops.ctx, observation, &writer);
    return bwg_ctrl_reply_close(outcome, &writer);
}

static bool parse_query(yyjson_val *payload, bool *has_attempt_id, bwg_str_t *attempt_id)
{
    static const char *const FIELDS[] = {"schema", "attemptId"};
    if (payload == NULL || !bwg_json_has_exact_fields(payload, FIELDS, 2, NULL, 0) || !schema_is(payload, "worker-noise-diagnostic-query-v1")) {
        return false;
    }
    yyjson_val *attempt = bwg_json_maybe_field(payload, "attemptId");
    *has_attempt_id = !yyjson_is_null(attempt);
    return !*has_attempt_id || (bwg_json_get_str(attempt, attempt_id) && attempt_id_is_valid(*attempt_id));
}

static bwg_ctrl_error_t observe(bwg_controller_t *controller, const bwg_noise_ops_t *noise, const bwg_ctrl_request_t *request,
                                uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    const bool cancel = bwg_str_equals(request->command, "noise_diagnostic_cancel");
    bool has_attempt_id = false;
    bwg_str_t attempt_id = {0};
    if (!parse_query(request->maybe_payload, &has_attempt_id, &attempt_id)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    // A retained binding allows observing or cancelling an attempt, never starting one.
    if ((!controller->has_noise_generation || controller->noise_generation != controller->generation) &&
        bwg_ctrl_maybe_active_context(controller) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    bwg_noise_view_t view;
    if (!view_of(controller, noise, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!has_attempt_id && !view.has_record && !cancel) {
        if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
            return BWG_CTRL_ADMISSION_REQUIRED;
        }
        const bwg_ctrl_error_t error = reply_status(controller, noise, request, &view.observation, outcome);
        if (error != BWG_CTRL_OK) {
            return error;
        }
        controller->noise_pending_observation = view.observation;
        outcome->effect.kind = BWG_CTRL_EFFECT_NOISE_OBSERVATION;
        outcome->effect.generation = controller->generation;
        return BWG_CTRL_OK;
    }
    if (!has_attempt_id || !view.has_record || !bwg_str_equals(attempt_id, view.attempt_id)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    controller->has_noise_generation = true;
    controller->noise_generation = controller->generation;
    if (cancel && !view.terminal && !noise->cancel(controller->ops.ctx)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (cancel && !view_of(controller, noise, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    return reply_status(controller, noise, request, &view.observation, outcome);
}

static bool parse_start(yyjson_val *payload, bwg_noise_input_t *input, uint64_t *boot_ordinal, uint64_t *observed_at_us)
{
    static const char *const FIELDS[] = {"schema", "attemptId", "expectedBootOrdinal", "networkObservedAtUs", "fixtureIpv4", "fixturePort", "authorityPublicKey"};
    uint64_t port = 0;
    uint8_t octets[4];
    if (payload == NULL || !bwg_json_has_exact_fields(payload, FIELDS, 7, NULL, 0) || !schema_is(payload, "worker-noise-diagnostic-start-v2") ||
        !bwg_json_get_str(bwg_json_maybe_field(payload, "attemptId"), &input->attempt_id) || !attempt_id_is_valid(input->attempt_id) ||
        !bwg_json_get_u64(bwg_json_maybe_field(payload, "expectedBootOrdinal"), boot_ordinal) || *boot_ordinal == 0 || *boot_ordinal > MAX_SAFE_INTEGER ||
        !bwg_json_get_u64(bwg_json_maybe_field(payload, "networkObservedAtUs"), observed_at_us) || *observed_at_us > MAX_SAFE_INTEGER ||
        !bwg_json_get_str(bwg_json_maybe_field(payload, "fixtureIpv4"), &input->fixture_ipv4) || !bwg_private_ipv4_parse(input->fixture_ipv4, octets) ||
        !bwg_json_get_u64(bwg_json_maybe_field(payload, "fixturePort"), &port) || port == 0 || port > UINT16_MAX ||
        !bwg_json_get_str(bwg_json_maybe_field(payload, "authorityPublicKey"), &input->authority_public_key) ||
        !bwg_b64url_is_canonical(input->authority_public_key.ptr, input->authority_public_key.len, NOISE_KEY_BYTES)) {
        return false;
    }
    input->fixture_port = (uint16_t)port;
    return true;
}

/** The host compares this with its own hash, so what it asked for is what the record says was admitted. */
static bool hash_input(yyjson_val *payload, char text[BWG_NOISE_DIGEST_TEXT_SIZE])
{
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    bwg_jw_t writer;
    bwg_jw_init_hash(&writer, &hash);
    if (!bwg_json_write_value(&writer, payload, BWG_JSON_ORDER_SORTED) || !bwg_jw_ok(&writer)) {
        return false;
    }
    uint8_t raw[BWG_SHA256_BYTES];
    crypto_hash_sha256_final(&hash, raw);
    sodium_bin2hex(text, BWG_NOISE_DIGEST_TEXT_SIZE, raw, sizeof(raw));
    return true;
}

static bwg_ctrl_error_t admit(bwg_controller_t *controller, const bwg_noise_ops_t *noise, const bwg_ctrl_request_t *request,
                              uint64_t now_ms, bwg_ctrl_outcome_t *outcome)
{
    void *ctx = controller->ops.ctx;
    if (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    if (!bwg_ctrl_lifecycle_is_quiet(&controller->lifecycle) || bwg_ctrl_attempt_busy(controller)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    bwg_noise_input_t input;
    uint64_t boot_ordinal = 0;
    uint64_t observed_at_us = 0;
    if (!parse_start(request->maybe_payload, &input, &boot_ordinal, &observed_at_us) || !hash_input(request->maybe_payload, input.input_sha256)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->has_noise_observation) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    bwg_noise_view_t view;
    if (!view_of(controller, noise, &view)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (view.has_record || !bwg_ctrl_same_network(&view.observation, &controller->noise_observation) ||
        view.observation.boot_ordinal != boot_ordinal || controller->noise_observation.observed_at_us != observed_at_us ||
        !bwg_ctrl_observation_is_fresh(&view.observation, observed_at_us)) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    // Reserved before admitting, so that running out of memory can't leave an attempt no reply announced.
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, BWG_NOISE_STATUS_BYTES + NOISE_ENVELOPE_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!noise->admit(ctx, &input, &view.observation)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    controller->has_noise_generation = true;
    controller->noise_generation = controller->generation;
    controller->has_noise_observation = false;
    noise->write_status(ctx, &view.observation, &writer);
    const bwg_ctrl_error_t error = bwg_ctrl_reply_close(outcome, &writer);
    if (error != BWG_CTRL_OK) {
        // The reply's error is the one reported, whether or not the cancel succeeds.
        (void) noise->cancel(ctx);
        return error;
    }
    outcome->effect.kind = BWG_CTRL_EFFECT_NOISE_DISPATCH;
    outcome->effect.generation = controller->generation;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_prepare_noise(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                        bwg_ctrl_outcome_t *outcome)
{
    const bwg_noise_ops_t *maybe_noise = controller->ops.maybe_noise;
    if (maybe_noise == NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!controller->authenticated_logical_session || controller->has_pending_token) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    return bwg_str_equals(request->command, "noise_diagnostic_start") ? admit(controller, maybe_noise, request, now_ms, outcome)
                                                                      : observe(controller, maybe_noise, request, now_ms, outcome);
}

bwg_ctrl_error_t bwg_ctrl_confirm_noise_dispatch(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms)
{
    const bwg_noise_ops_t *maybe_noise = controller->ops.maybe_noise;
    // Only an admitted diagnostic asks for a dispatch, and admitting needs the Noise ops.
    if (maybe_noise == NULL) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    void *ctx = controller->ops.ctx;
    bwg_ctrl_error_t error = bwg_ctrl_enforce_clock(controller, now_ms);
    if (error == BWG_CTRL_OK &&
        (bwg_ctrl_maybe_start_context(controller, now_ms) == NULL || effect->generation != controller->generation ||
         !controller->has_noise_generation || controller->noise_generation != effect->generation || controller->has_pending_token ||
         bwg_ctrl_lifecycle_holds_effect(&controller->lifecycle) || !controller->authenticated_logical_session)) {
        error = BWG_CTRL_STALE_RESPONSE;
    }
    if (error == BWG_CTRL_OK && !maybe_noise->dispatch(ctx)) {
        error = BWG_CTRL_SESSION_FAILED;
    }
    if (error != BWG_CTRL_OK && !maybe_noise->cancel(ctx)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    return error;
}
