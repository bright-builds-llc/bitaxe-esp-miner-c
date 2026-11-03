#include "bwg_controller.h"

#include <sodium.h>
#include <string.h>

#include "bwg_codec.h"
#include "bwg_controller_internal.h"
#include "bwg_json.h"

const char *bwg_ctrl_error_category(bwg_ctrl_error_t error)
{
    switch (error) {
    case BWG_CTRL_OK:
        return NULL;
    case BWG_CTRL_INVALID_FRAME:
        return "invalid_frame";
    case BWG_CTRL_INVALID_REQUEST:
        return "invalid_request";
    case BWG_CTRL_ADMISSION_REQUIRED:
        return "admission_required";
    case BWG_CTRL_INVALID_PROOF:
        return "invalid_proof";
    case BWG_CTRL_AUTHENTICATION_FAILED:
        return "authentication_failed";
    case BWG_CTRL_INVALID_TRANSITION:
        return "invalid_transition";
    case BWG_CTRL_PERSISTENCE_FAILED:
        return "persistence_failed";
    case BWG_CTRL_MONOTONIC_RESET:
        return "monotonic_reset";
    case BWG_CTRL_SESSION_FAILED:
        return "session_failed";
    case BWG_CTRL_RESTORATION_PENDING:
        return "restoration_pending";
    case BWG_CTRL_STALE_RESPONSE:
        return "stale_response";
    case BWG_CTRL_ENCODING_FAILED:
        return "encoding_failed";
    }
    return "session_failed";
}

/** Hashes the document in canonical form and, if asked, keeps a minified copy in document order. */
static bool absorb_document(const bwg_allocator_t *allocator, const uint8_t *text, size_t len,
                            char digest[BWG_DIGEST_TEXT_SIZE], bwg_buffer_t *maybe_minified)
{
    yyjson_doc *doc = NULL;
    if (text == NULL || bwg_json_parse_strict(allocator, text, len, &doc) != BWG_JSON_OK) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = yyjson_is_obj(root);
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    bwg_jw_t writer;
    bwg_jw_init_hash(&writer, &hash);
    ok = ok && bwg_json_write_value(&writer, root, BWG_JSON_ORDER_SORTED) && bwg_jw_ok(&writer);
    uint8_t raw[BWG_SHA256_BYTES];
    crypto_hash_sha256_final(&hash, raw);
    ok = ok && bwg_b64url_encode(raw, sizeof(raw), digest, BWG_DIGEST_TEXT_SIZE, NULL);
    if (ok && maybe_minified != NULL) {
        ok = bwg_buffer_alloc(allocator, maybe_minified, len);
        if (ok) {
            bwg_jw_init_buffer(&writer, maybe_minified->data, maybe_minified->capacity);
            ok = bwg_json_write_value(&writer, root, BWG_JSON_ORDER_DOCUMENT) && bwg_jw_ok(&writer);
            maybe_minified->len = writer.len;
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

static bool copy_hex(char *out, const char *maybe_text, size_t expected)
{
    if (maybe_text == NULL || strlen(maybe_text) != expected || !bwg_hex_is_lower(maybe_text, expected, expected)) {
        return false;
    }
    memcpy(out, maybe_text, expected + 1);
    return true;
}

static bool absorb_boot_marker(bwg_controller_t *controller)
{
    bwg_effect_state_t state = BWG_EFFECT_CLEAR;
    if (!controller->ops.effect_get(controller->ops.ctx, &state)) {
        return false;
    }
    if (state == BWG_EFFECT_CLEAR) {
        return true;
    }
    if (state != BWG_EFFECT_PENDING && state != BWG_EFFECT_REBOOT_CONFIRMED) {
        return false;
    }
    if (state == BWG_EFFECT_PENDING && !controller->ops.effect_set(controller->ops.ctx, BWG_EFFECT_REBOOT_CONFIRMED)) {
        return false;
    }
    controller->boot_clear_required = true;
    controller->restoration = BWG_RESTORATION_CONFIRMED;
    controller->restoration_reason = BWG_REASON_REBOOT;
    return true;
}

static void release_owned(bwg_controller_t *controller)
{
    bwg_buffer_release(controller->allocator, &controller->capability);
    bwg_free_secret(controller->allocator, controller->seen_nonces,
                    BWG_CONTROLLER_MAX_SEEN_NONCES * BWG_POSSESSION_NONCE_DIGEST_BYTES);
    controller->seen_nonces = NULL;
    bwg_identity_wipe(&controller->identity);
}

bool bwg_controller_init(bwg_controller_t *controller, const bwg_allocator_t *allocator,
                         const bwg_controller_config_t *config, const bwg_controller_ops_t *ops)
{
    memset(controller, 0, sizeof(*controller));
    controller->allocator = allocator;
    controller->ops = *ops;
    if (config->seed == NULL || !copy_hex(controller->firmware_commit, config->firmware_commit, BWG_FIRMWARE_COMMIT_LEN) ||
        !copy_hex(controller->app_elf_hex, config->app_elf_hex, BWG_APP_ELF_HEX_LEN)) {
        return false;
    }
    if (config->maybe_verifier != NULL) {
        controller->has_verifier = true;
        controller->verifier = *config->maybe_verifier;
    }
    controller->trust_valid =
        config->maybe_trust_json != NULL &&
        bwg_trust_parse(allocator, config->maybe_trust_json, config->trust_len, &controller->trust);
    bwg_identity_from_seed(&controller->identity, config->seed);
    controller->seen_nonces = bwg_alloc(allocator, BWG_CONTROLLER_MAX_SEEN_NONCES * BWG_POSSESSION_NONCE_DIGEST_BYTES);
    bool ok = controller->seen_nonces != NULL &&
              absorb_document(allocator, config->manifest_json, config->manifest_len, controller->manifest_digest, NULL) &&
              absorb_document(allocator, config->capability_json, config->capability_len, controller->capability_digest,
                              &controller->capability) &&
              absorb_boot_marker(controller);
    if (!ok) {
        release_owned(controller);
    }
    return ok;
}

void bwg_controller_deinit(bwg_controller_t *controller)
{
    release_owned(controller);
}

static void invalidate_session(bwg_controller_t *controller)
{
    controller->authenticated_logical_session = false;
    controller->has_binding = false;
    if (controller->generation != UINT64_MAX) {
        controller->generation++;
    }
    controller->has_admission = false;
    controller->has_pending_token = false;
    controller->restart_consumed = false;
    controller->has_boot_report_generation = false;
    controller->has_v2_observation = false;
    controller->has_noise_observation = false;
    controller->seen_count = 0;
    bwg_clock_stimulus_clear_pending(&controller->stimulus);
}

bwg_ctrl_error_t bwg_controller_begin_session(bwg_controller_t *controller, const bwg_session_binding_t *binding)
{
    if (controller->has_active || controller->cleanup_required) {
        return BWG_CTRL_INVALID_TRANSITION;
    }
    invalidate_session(controller);
    controller->binding = *binding;
    controller->has_binding = true;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_safe_stop(bwg_controller_t *controller, bwg_reason_t reason, uint64_t now_ms)
{
    controller->has_admission = false;
    controller->has_pending_token = false;
    bwg_clock_stimulus_cancel(&controller->stimulus);
    controller->has_cleanup_reason = true;
    controller->cleanup_reason = reason;
    controller->restoration = BWG_RESTORATION_PENDING;
    if (controller->has_active || controller->cleanup_required) {
        if (!controller->ops.safe_stop(controller->ops.ctx, reason)) {
            return BWG_CTRL_SESSION_FAILED;
        }
        if (!controller->ops.effect_set(controller->ops.ctx, BWG_EFFECT_CLEAR)) {
            return BWG_CTRL_PERSISTENCE_FAILED;
        }
    }
    controller->has_active = false;
    controller->lease_exact_window = false;
    controller->cleanup_required = false;
    controller->has_cleanup_reason = false;
    controller->restoration = BWG_RESTORATION_CONFIRMED;
    controller->restoration_reason = reason;
    controller->has_last_ms = true;
    controller->last_ms = now_ms;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_enforce_clock(bwg_controller_t *controller, uint64_t now_ms)
{
    if (controller->has_cleanup_reason) {
        bwg_ctrl_error_t error = bwg_ctrl_safe_stop(controller, controller->cleanup_reason, now_ms);
        if (error != BWG_CTRL_OK) {
            return error;
        }
    }
    if (controller->has_last_ms && now_ms < controller->last_ms) {
        if (controller->clock_discontinuities != UINT32_MAX) {
            controller->clock_discontinuities++;
        }
        bwg_ctrl_error_t error = bwg_ctrl_safe_stop(controller, BWG_REASON_MONOTONIC_RESET, now_ms);
        return error != BWG_CTRL_OK ? error : BWG_CTRL_MONOTONIC_RESET;
    }
    controller->has_last_ms = true;
    controller->last_ms = now_ms;
    if (controller->has_active && now_ms >= controller->deadlines.expires_at_ms) {
        return bwg_ctrl_safe_stop(controller, BWG_REASON_LEASE_EXPIRED, now_ms);
    }
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_ctrl_acknowledge_boot(bwg_controller_t *controller)
{
    if (!controller->has_boot_report_generation || controller->boot_report_generation != controller->generation) {
        return BWG_CTRL_OK;
    }
    if (!controller->ops.effect_set(controller->ops.ctx, BWG_EFFECT_CLEAR)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    controller->boot_clear_required = false;
    controller->has_boot_report_generation = false;
    return BWG_CTRL_OK;
}

bwg_ctrl_error_t bwg_controller_tick(bwg_controller_t *controller, uint64_t now_ms)
{
    // Only this path may see the armed stimulus: frames and confirmations read the real clock.
    const uint64_t now = bwg_clock_stimulus_sample(&controller->stimulus, now_ms, controller->has_last_ms, controller->last_ms);
    return bwg_ctrl_enforce_clock(controller, now);
}

static bwg_ctrl_error_t stop_and_invalidate(bwg_controller_t *controller, bwg_reason_t reason, uint64_t now_ms)
{
    bwg_ctrl_error_t error = bwg_ctrl_safe_stop(controller, reason, now_ms);
    invalidate_session(controller);
    return error;
}

bwg_ctrl_error_t bwg_controller_disconnect(bwg_controller_t *controller, uint64_t now_ms)
{
    return stop_and_invalidate(controller, BWG_REASON_CONNECTIVITY_LOST, now_ms);
}

bwg_ctrl_error_t bwg_controller_reboot(bwg_controller_t *controller, uint64_t now_ms)
{
    return stop_and_invalidate(controller, BWG_REASON_REBOOT, now_ms);
}

bwg_ctrl_error_t bwg_controller_control_failed(bwg_controller_t *controller, uint64_t now_ms)
{
    return stop_and_invalidate(controller, BWG_REASON_CONTROL_FAILED, now_ms);
}

bwg_ctrl_error_t bwg_controller_confirm_sent(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms)
{
    if (effect->kind == BWG_CTRL_EFFECT_NONE) {
        return BWG_CTRL_OK;
    }
    if (effect->kind == BWG_CTRL_EFFECT_V2_DISPATCH) {
        return bwg_ctrl_confirm_v2_dispatch(controller, effect, now_ms);
    }
    if (effect->kind == BWG_CTRL_EFFECT_NOISE_DISPATCH) {
        return bwg_ctrl_confirm_noise_dispatch(controller, effect, now_ms);
    }
    if (effect->generation != controller->generation) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    if (effect->kind == BWG_CTRL_EFFECT_CLOCK_STIMULUS) {
        return bwg_ctrl_confirm_stimulus(controller, effect, now_ms);
    }
    if (effect->kind == BWG_CTRL_EFFECT_QUALIFICATION_RESTART) {
        return bwg_ctrl_confirm_restart(controller, now_ms);
    }
    if (effect->kind == BWG_CTRL_EFFECT_CORE_DUMP_SELF_TEST) {
        return bwg_ctrl_confirm_core_dump_self_test(controller, now_ms);
    }
    if (effect->kind == BWG_CTRL_EFFECT_V2_OBSERVATION) {
        controller->has_v2_observation = true;
        controller->v2_observation_scope = controller->v2_pending_scope;
        controller->v2_observation = controller->v2_pending_observation;
        return BWG_CTRL_OK;
    }
    if (effect->kind == BWG_CTRL_EFFECT_NOISE_OBSERVATION) {
        controller->has_noise_observation = true;
        controller->noise_observation = controller->noise_pending_observation;
        return BWG_CTRL_OK;
    }
    if (effect->kind == BWG_CTRL_EFFECT_BOOT_REPORTED) {
        if (!controller->boot_clear_required) {
            return BWG_CTRL_STALE_RESPONSE;
        }
        controller->has_boot_report_generation = true;
        controller->boot_report_generation = effect->generation;
        return BWG_CTRL_OK;
    }
    if (!controller->has_pending_token || controller->pending_token != effect->token) {
        return BWG_CTRL_STALE_RESPONSE;
    }
    controller->has_pending_token = false;
    controller->authenticated_logical_session = true;
    controller->has_admission = true;
    controller->admission_generation = effect->generation;
    controller->admission_established_at_ms = effect->established_at_ms;
    memcpy(controller->admission_binding, effect->binding, BWG_DIGEST_TEXT_SIZE);
    return BWG_CTRL_OK;
}

bool bwg_controller_is_admitted(const bwg_controller_t *controller)
{
    return controller->has_admission;
}

bool bwg_controller_has_active_lease(const bwg_controller_t *controller)
{
    return controller->has_active;
}

const char *bwg_controller_capability_digest(const bwg_controller_t *controller)
{
    return controller->capability_digest;
}

const char *bwg_ctrl_maybe_active_context(const bwg_controller_t *controller)
{
    if (!controller->has_admission || controller->admission_generation != controller->generation) {
        return NULL;
    }
    return controller->admission_binding;
}

const char *bwg_ctrl_maybe_start_context(const bwg_controller_t *controller, uint64_t now_ms)
{
    const char *maybe_binding = bwg_ctrl_maybe_active_context(controller);
    if (maybe_binding == NULL) {
        return NULL;
    }
    // A clock below the admission time was already caught by the clock check, so this saturates to fresh.
    uint64_t age = now_ms > controller->admission_established_at_ms ? now_ms - controller->admission_established_at_ms : 0;
    return age < BWG_CONTROLLER_ADMISSION_MAX_AGE_MS ? maybe_binding : NULL;
}

static bwg_sequence_store_t store_of(const bwg_controller_t *controller)
{
    return (bwg_sequence_store_t) {
        .ctx = controller->ops.ctx,
        .load = controller->ops.high_water_load,
        .compare_and_store = controller->ops.high_water_compare_and_store,
    };
}

static void note_verification(bwg_controller_t *controller, bwg_auth_operation_t operation, bwg_auth_result_t result,
                              const bwg_auth_attribution_t *attribution)
{
    if (result == BWG_AUTH_ACCEPTED) {
        bwg_rejection_log_mark_advanced(&controller->rejections);
    } else {
        bwg_rejection_log_record(&controller->rejections, operation, attribution);
    }
}

void bwg_ctrl_note_context_rejection(bwg_controller_t *controller, bwg_auth_operation_t operation)
{
    const bool current = controller->has_admission && controller->admission_generation == controller->generation;
    const bwg_auth_attribution_t attribution = {.context = current ? BWG_AUTH_CONTEXT_EXPIRED : BWG_AUTH_CONTEXT_ABSENT};
    bwg_rejection_log_record(&controller->rejections, operation, &attribution);
}

bwg_auth_result_t bwg_ctrl_verify_start(bwg_controller_t *controller, const bwg_lease_grant_t *grant, const char *binding)
{
    bwg_auth_attribution_t attribution = {0};
    bwg_auth_result_t result = BWG_AUTH_INVALID;
    if (controller->has_verifier) {
        result = controller->verifier.verify_start(controller->verifier.ctx, grant, binding);
    } else if (controller->trust_valid) {
        bwg_sequence_store_t store = store_of(controller);
        result = bwg_lease_auth_verify_start(controller->allocator, &controller->trust, &store, grant, binding, &attribution);
    }
    note_verification(controller, BWG_AUTH_OPERATION_START, result, &attribution);
    return result;
}

bwg_auth_result_t bwg_ctrl_verify_renew(bwg_controller_t *controller, const bwg_lease_renewal_t *renewal, const char *binding)
{
    bwg_auth_attribution_t attribution = {0};
    bwg_auth_result_t result = BWG_AUTH_INVALID;
    if (controller->has_verifier) {
        result = controller->verifier.verify_renew(controller->verifier.ctx, renewal, controller->challenge_id, binding);
    } else if (controller->trust_valid) {
        bwg_sequence_store_t store = store_of(controller);
        result = bwg_lease_auth_verify_renew(controller->allocator, &controller->trust, &store, renewal,
                                             controller->challenge_id, binding, &attribution);
    }
    note_verification(controller, BWG_AUTH_OPERATION_RENEW, result, &attribution);
    return result;
}
