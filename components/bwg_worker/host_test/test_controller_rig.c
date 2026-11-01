#include "test_controller_rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "test_jws.h"

#define COMMIT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ELF "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define FIXTURE_NONCE "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE"

static void note(rig_device_t *device, const char *event)
{
    size_t used = strlen(device->events);
    snprintf(device->events + used, sizeof(device->events) - used, "%s%s", used > 0 ? "," : "", event);
}

static bool op_start(void *ctx, const bwg_lease_grant_t *grant, bwg_lease_deadlines_t deadlines)
{
    (void) grant;
    rig_device_t *device = ctx;
    note(device, "start");
    device->start_deadlines = deadlines;
    return !device->fail_start;
}

static bool op_renew(void *ctx, const bwg_lease_renewal_t *renewal, bwg_lease_deadlines_t deadlines)
{
    (void) renewal;
    rig_device_t *device = ctx;
    note(device, "renew");
    device->renew_deadlines = deadlines;
    return !device->fail_renew;
}

static bool op_safe_stop(void *ctx, bwg_reason_t reason)
{
    rig_device_t *device = ctx;
    note(device, bwg_reason_name(reason));
    if (device->safe_stop_failures > 0) {
        device->safe_stop_failures--;
        return false;
    }
    return true;
}

static bool op_load(void *ctx, const char *kid, bool *present, uint64_t *value)
{
    (void) kid;
    rig_device_t *device = ctx;
    if (device->high_water_corrupt) {
        return false;
    }
    *present = device->high_water_present;
    *value = device->high_water;
    return true;
}

static bwg_store_result_t op_store(void *ctx, const char *kid, bool has_expected, uint64_t expected, uint64_t next)
{
    (void) kid;
    rig_device_t *device = ctx;
    if (has_expected != device->high_water_present || (has_expected && expected != device->high_water)) {
        return BWG_STORE_STALE;
    }
    device->high_water_present = true;
    device->high_water = next;
    return BWG_STORE_COMMITTED;
}

static bool op_effect_get(void *ctx, bwg_effect_state_t *state)
{
    rig_device_t *device = ctx;
    *state = device->effect;
    return !device->effect_unreadable;
}

static bool op_effect_set(void *ctx, bwg_effect_state_t state)
{
    rig_device_t *device = ctx;
    device->effect_writes++;
    note(device, state == BWG_EFFECT_CLEAR ? "clear_pending" : "mark_pending");
    int *failures = state == BWG_EFFECT_CLEAR ? &device->clear_failures : &device->mark_failures;
    if (*failures > 0) {
        (*failures)--;
        return false;
    }
    device->effect = state;
    return true;
}

static bwg_evidence_t op_budget_review(void *ctx, bwg_allowance_kind_t kind, bwg_str_t campaign_id, bwg_jw_t *writer)
{
    rig_device_t *device = ctx;
    if (!device->has_budget) {
        return BWG_EVIDENCE_ABSENT;
    }
    device->last_review_kind = kind;
    device->last_review_matched = bwg_str_equals(campaign_id, "AAAAAAAAAAAAAAAAAAAAAA");
    if (device->budget_review_fails) {
        return BWG_EVIDENCE_SESSION_FAILED;
    }
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "kind");
    bwg_jw_u64(writer, kind);
    bwg_jw_object_end(writer);
    return BWG_EVIDENCE_WRITTEN;
}

static bool op_restart_context(void *ctx, uint64_t *boot_ordinal)
{
    rig_device_t *device = ctx;
    *boot_ordinal = device->boot_ordinal;
    return device->restart_allowed;
}

static void op_restart(void *ctx)
{
    rig_device_t *device = ctx;
    device->restarts++;
}

static bool op_core_dump_context(void *ctx, uint64_t *boot_ordinal)
{
    rig_device_t *device = ctx;
    *boot_ordinal = device->boot_ordinal;
    return device->restart_allowed && device->outputs_off;
}

static void op_core_dump_self_test(void *ctx)
{
    rig_device_t *device = ctx;
    device->core_dump_self_tests++;
}

static bool op_cadence_arm(void *ctx, bwg_cadence_phase_t phase, bwg_jw_t *writer)
{
    rig_device_t *device = ctx;
    return bwg_cadence_arm(&device->cadence, phase, 5000, 1, 1, writer);
}

static void op_write_cadence(void *ctx, bwg_jw_t *writer)
{
    rig_device_t *device = ctx;
    bwg_cadence_write_review(&device->cadence, writer);
}

static void op_note_max_probe(void *ctx)
{
    rig_device_t *device = ctx;
    device->max_probes++;
}

static bool op_write_endpoint(void *ctx, bwg_jw_t *writer)
{
    rig_device_t *device = ctx;
    if (!device->has_endpoint) {
        return false;
    }
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-telemetry-endpoint-v1");
    bwg_jw_object_end(writer);
    return true;
}

static bool op_cooling_prove(void *ctx, uint32_t *fan_rpm)
{
    rig_device_t *device = ctx;
    note(device, "cooling_prove");
    *fan_rpm = 1200;
    return !device->fail_cooling_prove;
}

static bool op_cooling_restore(void *ctx)
{
    rig_device_t *device = ctx;
    note(device, "cooling_restore");
    return !device->fail_cooling_restore;
}

static bool op_noise_view(void *ctx, uint64_t generation, bwg_noise_view_t *view)
{
    (void) generation;
    rig_device_t *device = ctx;
    *view = device->noise_view;
    return device->has_noise;
}

static void op_noise_write_status(void *ctx, const bwg_v2_observation_t *observation, bwg_jw_t *writer)
{
    rig_device_t *device = ctx;
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-noise-diagnostic-status-v2");
    bwg_jw_key(writer, "observedAtUs");
    bwg_jw_u64(writer, observation->observed_at_us);
    bwg_jw_key(writer, "hasRecord");
    bwg_jw_bool(writer, device->noise_view.has_record);
    bwg_jw_object_end(writer);
}

static bool op_noise_busy(void *ctx)
{
    rig_device_t *device = ctx;
    return device->noise_busy;
}

static bool op_noise_admit(void *ctx, const bwg_noise_input_t *input, const bwg_v2_observation_t *observation)
{
    (void) observation;
    rig_device_t *device = ctx;
    note(device, "noise_admit");
    if (device->fail_noise_admit) {
        return false;
    }
    device->noise_view.has_record = true;
    snprintf(device->noise_view.attempt_id, sizeof(device->noise_view.attempt_id), "%.*s", (int) input->attempt_id.len, input->attempt_id.ptr);
    memcpy(device->noise_input_sha256, input->input_sha256, sizeof(device->noise_input_sha256));
    return true;
}

static bool op_noise_dispatch(void *ctx)
{
    rig_device_t *device = ctx;
    note(device, "noise_dispatch");
    return !device->fail_noise_dispatch;
}

static bool op_noise_cancel(void *ctx)
{
    note(ctx, "noise_cancel");
    return true;
}

static bool op_v2_view(void *ctx, bwg_v2_scope_t scope, uint64_t generation, bwg_v2_view_t *view)
{
    (void) scope;
    (void) generation;
    rig_device_t *device = ctx;
    *view = device->v2_view;
    return device->has_v2;
}

static void op_v2_write_status(void *ctx, bwg_v2_scope_t scope, const bwg_v2_observation_t *observation, bwg_jw_t *writer)
{
    (void) scope;
    rig_device_t *device = ctx;
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-stratum-v2-status-v1");
    bwg_jw_key(writer, "observedAtUs");
    bwg_jw_u64(writer, observation->observed_at_us);
    bwg_jw_key(writer, "hasRecord");
    bwg_jw_bool(writer, device->v2_view.has_record);
    bwg_jw_object_end(writer);
}

static bool op_v2_busy(void *ctx)
{
    const rig_device_t *device = ctx;
    return device->v2_busy;
}

static bool op_v2_admit(void *ctx, const bwg_v2_channel_input_t *input, uint64_t generation)
{
    (void) generation;
    rig_device_t *device = ctx;
    note(device, "v2_admit");
    if (device->fail_v2_admit) {
        return false;
    }
    device->v2_view.has_record = true;
    device->v2_view.state = BWG_V2_STATE_ADMITTED;
    snprintf(device->v2_view.attempt_id, sizeof(device->v2_view.attempt_id), "%.*s", (int) input->attempt_id.len, input->attempt_id.ptr);
    return true;
}

static bool op_v2_dispatch(void *ctx)
{
    rig_device_t *device = ctx;
    note(device, "v2_dispatch");
    return !device->fail_v2_dispatch;
}

static bool op_v2_cancel(void *ctx)
{
    note(ctx, "v2_cancel");
    return true;
}

static bwg_evidence_t op_qualification(void *ctx, bwg_jw_t *writer)
{
    rig_device_t *device = ctx;
    if (device->qualification_fill_bytes > 0) {
        bwg_jw_string_repeat(writer, 'q', device->qualification_fill_bytes);
        return BWG_EVIDENCE_WRITTEN;
    }
    if (device->maybe_qualification_json != NULL) {
        bwg_jw_raw_value(writer, (const uint8_t *) device->maybe_qualification_json,
                         strlen(device->maybe_qualification_json));
    }
    return device->qualification_result;
}

static bwg_evidence_t op_preservation(void *ctx, bwg_jw_t *writer, const char *device_identity_sha256)
{
    rig_device_t *device = ctx;
    if (device->preservation_result != BWG_EVIDENCE_WRITTEN) {
        return device->preservation_result;
    }
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "device_identity_sha256");
    bwg_jw_cstr(writer, device_identity_sha256);
    bwg_jw_object_end(writer);
    return BWG_EVIDENCE_WRITTEN;
}

static bool op_fingerprint(void *ctx, char fingerprint[BWG_FINGERPRINT_HEX_SIZE])
{
    const rig_device_t *device = ctx;
    if (device->fingerprint_fails) {
        return false;
    }
    memcpy(fingerprint, RIG_FINGERPRINT, BWG_FINGERPRINT_HEX_SIZE);
    return true;
}

static const char *op_boot_reset_cause(void *ctx)
{
    const rig_device_t *device = ctx;
    return device->maybe_boot_reset_cause != NULL ? device->maybe_boot_reset_cause : "power_on";
}

static bwg_auth_result_t stub_verify_start(void *ctx, const bwg_lease_grant_t *grant, const char *binding)
{
    (void) binding;
    note(ctx, "verify_start");
    return bwg_str_equals(grant->authorization, RIG_FIXTURE_AUTH) ? BWG_AUTH_ACCEPTED : BWG_AUTH_INVALID;
}

static bwg_auth_result_t stub_verify_renew(void *ctx, const bwg_lease_renewal_t *renewal, const char *challenge_id,
                                           const char *binding)
{
    (void) challenge_id;
    (void) binding;
    note(ctx, "verify_renew");
    return bwg_str_equals(renewal->authorization, RIG_FIXTURE_RENEW_AUTH) ? BWG_AUTH_ACCEPTED : BWG_AUTH_INVALID;
}

static bool open_controller(rig_t *rig)
{
    size_t capability_len = 0;
    size_t trust_len = 0;
    char *capability = bwg_test_read_fixture("deployment-trust/signed-capability.json", &capability_len);
    char *trust = bwg_test_read_fixture("deployment-trust/trust.json", &trust_len);
    bwg_lease_verifier_t verifier = {
        .ctx = &rig->device,
        .verify_start = stub_verify_start,
        .verify_renew = stub_verify_renew,
    };
    bwg_controller_config_t config = {
        .capability_json = (const uint8_t *) capability,
        .capability_len = capability_len,
        .manifest_json = (const uint8_t *) BWG_SERIAL_MANIFEST_JSON,
        .manifest_len = strlen(BWG_SERIAL_MANIFEST_JSON),
        .firmware_commit = COMMIT,
        .app_elf_hex = ELF,
        .maybe_trust_json = (const uint8_t *) (rig->custom_trust ? rig->maybe_trust_text : trust),
        .trust_len = rig->custom_trust && rig->maybe_trust_text != NULL ? strlen(rig->maybe_trust_text) : trust_len,
        .seed = BWG_TEST_RFC8032_SEED,
        .maybe_verifier = rig->real_trust ? NULL : &verifier,
    };
    bwg_controller_ops_t ops = {
        .ctx = &rig->device,
        .start = op_start,
        .renew = op_renew,
        .safe_stop = op_safe_stop,
        .high_water_load = op_load,
        .high_water_compare_and_store = op_store,
        .effect_get = op_effect_get,
        .effect_set = op_effect_set,
        .maybe_append_qualification = op_qualification,
        .maybe_append_preservation = op_preservation,
        .maybe_high_water_fingerprint = op_fingerprint,
        .maybe_boot_reset_cause = op_boot_reset_cause,
        .maybe_budget_review = op_budget_review,
        .maybe_cooling_prove = op_cooling_prove,
        .maybe_cooling_restore = op_cooling_restore,
        .maybe_restart_context = op_restart_context,
        .maybe_restart = op_restart,
        .maybe_core_dump_context = op_core_dump_context,
        .maybe_core_dump_self_test = op_core_dump_self_test,
        .maybe_write_endpoint = op_write_endpoint,
        .maybe_cadence_arm = op_cadence_arm,
        .maybe_write_cadence = op_write_cadence,
        .maybe_note_max_probe = op_note_max_probe,
        .maybe_v2_view = op_v2_view,
        .maybe_v2_write_status = op_v2_write_status,
        .maybe_v2_busy = op_v2_busy,
        .maybe_v2_admit = op_v2_admit,
        .maybe_v2_dispatch = op_v2_dispatch,
        .maybe_v2_cancel = op_v2_cancel,
        .maybe_noise_view = op_noise_view,
        .maybe_noise_write_status = op_noise_write_status,
        .maybe_noise_busy = op_noise_busy,
        .maybe_noise_admit = op_noise_admit,
        .maybe_noise_dispatch = op_noise_dispatch,
        .maybe_noise_cancel = op_noise_cancel,
    };
    bool ok = bwg_controller_init(&rig->controller, &bwg_test_allocator, &config, &ops);
    bwg_test_free_fixture(capability);
    bwg_test_free_fixture(trust);
    return ok;
}

static bool open_rig(rig_t *rig, bool real_trust)
{
    memset(rig, 0, sizeof(*rig));
    rig->real_trust = real_trust;
    rig->device.qualification_result = BWG_EVIDENCE_ABSENT;
    rig->device.preservation_result = BWG_EVIDENCE_ABSENT;
    return open_controller(rig);
}

bool rig_open(rig_t *rig)
{
    return open_rig(rig, false);
}

bool rig_open_real_trust(rig_t *rig)
{
    return open_rig(rig, true);
}

bool rig_open_with_lease(rig_t *rig, uint64_t now_ms)
{
    return rig_open(rig) && rig_admit(rig, now_ms) &&
           rig_send(rig, "start_lease", RIG_START_PAYLOAD, now_ms) == BWG_CTRL_OK;
}

bool rig_open_with_trust_text(rig_t *rig, const char *maybe_trust)
{
    memset(rig, 0, sizeof(*rig));
    rig->real_trust = true;
    rig->custom_trust = true;
    rig->maybe_trust_text = maybe_trust;
    return open_controller(rig);
}

static void drop_outcome(rig_t *rig)
{
    if (rig->outcome_held) {
        bwg_controller_outcome_release(&rig->controller, &rig->outcome);
        rig->outcome_held = false;
    }
}

bool rig_reboot(rig_t *rig)
{
    drop_outcome(rig);
    bwg_controller_deinit(&rig->controller);
    rig->next_nonce = 0;
    return open_controller(rig);
}

void rig_close(rig_t *rig)
{
    drop_outcome(rig);
    bwg_controller_deinit(&rig->controller);
}

bwg_session_binding_t rig_fixture_session(void)
{
    bwg_session_binding_t binding;
    strcpy(binding.session_id, "AgICAgICAgICAgICAgICAg");
    strcpy(binding.host_nonce, "AwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwM");
    strcpy(binding.device_nonce, "BAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ");
    return binding;
}

bwg_ctrl_error_t rig_send_raw(rig_t *rig, const void *frame, size_t len, uint64_t now_ms)
{
    drop_outcome(rig);
    bwg_ctrl_error_t error = bwg_controller_frame(&rig->controller, frame, len, now_ms, &rig->outcome);
    rig->outcome_held = true;
    return error;
}

bwg_ctrl_error_t rig_send_json(rig_t *rig, const char *json, uint64_t now_ms)
{
    size_t len = strlen(json);
    char *frame = malloc(len + 1);
    memcpy(frame, json, len);
    frame[len] = '\n';
    bwg_ctrl_error_t error = rig_send_raw(rig, frame, len + 1, now_ms);
    free(frame);
    return error;
}

bwg_ctrl_error_t rig_send(rig_t *rig, const char *command, const char *maybe_payload, uint64_t now_ms)
{
    size_t capacity = 256 + strlen(command) + (maybe_payload != NULL ? strlen(maybe_payload) : 0);
    char *json = malloc(capacity);
    int written = snprintf(json, capacity,
                           "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"serial_test\","
                           "\"command\":\"%s\"",
                           command);
    if (maybe_payload != NULL) {
        written += snprintf(json + written, capacity - (size_t) written, ",\"payload\":%s", maybe_payload);
    }
    snprintf(json + written, capacity - (size_t) written, "}");
    bwg_ctrl_error_t error = rig_send_json(rig, json, now_ms);
    free(json);
    return error;
}

bwg_ctrl_error_t rig_send_possession(rig_t *rig, int nonce, uint64_t now_ms)
{
    char nonce_text[BWG_DIGEST_TEXT_SIZE];
    strcpy(nonce_text, FIXTURE_NONCE);
    if (nonce != 0) {
        uint8_t bytes[32] = {(uint8_t) (nonce >> 8), (uint8_t) nonce, 0xee};
        bwg_b64url_encode(bytes, sizeof(bytes), nonce_text, sizeof(nonce_text), NULL);
    }
    bwg_session_binding_t session = rig_fixture_session();
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"profile\":\"bwg-worker-possession/0.2\",\"requestId\":\"pos_initial_01\",\"command\":\"prove_possession\","
             "\"payload\":{\"possessionNonce\":\"%s\",\"challengeBindingSha256\":"
             "\"X4zZvQZS4kl8eriGLhoxvxVwcFz5Tx40nnKDBbgLW6k\",\"controllerCapabilitySha256\":\"%s\",\"sessionId\":\"%s\","
             "\"hostNonce\":\"%s\",\"deviceNonce\":\"%s\",\"serialManifestSha256\":\"%s\","
             "\"purpose\":\"initial_admission\"}}",
             nonce_text, bwg_controller_capability_digest(&rig->controller), session.session_id, session.host_nonce,
             session.device_nonce, BWG_SERIAL_MANIFEST_SHA256);
    return rig_send_json(rig, json, now_ms);
}

bwg_ctrl_error_t rig_confirm_at(rig_t *rig, uint64_t now_ms)
{
    return bwg_controller_confirm_sent(&rig->controller, &rig->outcome.effect, now_ms);
}

bwg_ctrl_error_t rig_confirm(rig_t *rig)
{
    return rig_confirm_at(rig, 0);
}

bool rig_reprove(rig_t *rig, uint64_t now_ms)
{
    return rig_send_possession(rig, rig->next_nonce++, now_ms) == BWG_CTRL_OK && rig_confirm(rig) == BWG_CTRL_OK;
}

bool rig_admit(rig_t *rig, uint64_t now_ms)
{
    bwg_session_binding_t session = rig_fixture_session();
    return bwg_controller_begin_session(&rig->controller, &session) == BWG_CTRL_OK && rig_reprove(rig, now_ms);
}

const char *rig_reply_text(const rig_t *rig)
{
    static char text[BWG_MAX_CONTROL_PAYLOAD_BYTES + 1];
    size_t len = rig->outcome.has_reply ? rig->outcome.response.len : 0;
    if (len > 0) {
        memcpy(text, rig->outcome.response.data, len);
    }
    text[len] = '\0';
    return text;
}

static yyjson_doc *reply_doc(const rig_t *rig)
{
    if (!rig->outcome.has_reply) {
        return NULL;
    }
    return yyjson_read((const char *) rig->outcome.response.data, rig->outcome.response.len, 0);
}

bool rig_reply_str_is(const rig_t *rig, const char *pointer, const char *expected)
{
    yyjson_doc *doc = reply_doc(rig);
    const char *actual = doc != NULL ? yyjson_get_str(yyjson_doc_ptr_get(doc, pointer)) : NULL;
    bool same = actual != NULL && strcmp(actual, expected) == 0;
    yyjson_doc_free(doc);
    return same;
}

bool rig_reply_u64_is(const rig_t *rig, const char *pointer, uint64_t expected)
{
    yyjson_doc *doc = reply_doc(rig);
    yyjson_val *value = doc != NULL ? yyjson_doc_ptr_get(doc, pointer) : NULL;
    bool same = yyjson_is_uint(value) && yyjson_get_uint(value) == expected;
    yyjson_doc_free(doc);
    return same;
}

bool rig_reply_has(const rig_t *rig, const char *pointer)
{
    yyjson_doc *doc = reply_doc(rig);
    bool found = doc != NULL && yyjson_doc_ptr_get(doc, pointer) != NULL;
    yyjson_doc_free(doc);
    return found;
}

bool rig_reply_is_rejection(const rig_t *rig, bwg_ctrl_error_t error)
{
    return rig->outcome.revoke_after_send && rig_reply_str_is(rig, "/requestId", "serial_test") &&
           rig_reply_str_is(rig, "/error/code", "command_rejected") &&
           rig_reply_str_is(rig, "/error/message", bwg_ctrl_error_category(error));
}
