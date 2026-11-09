#include "bwg_controller_internal.h"
#include "bwg_json.h"
#include "bwg_onboarding.h"

#define ONBOARDING_REPLY_BYTES 768u

bool bwg_ctrl_is_onboarding(bwg_str_t command)
{
    return bwg_str_equals(command, "onboarding_inspect") || bwg_str_equals(command, "onboarding_verify");
}

static bool running_version(const bwg_controller_t *controller, char out[BWG_ONBOARDING_VERSION_MAX])
{
    yyjson_doc *doc = NULL;
    if (bwg_json_parse(controller->allocator, controller->capability.data, controller->capability.len, &doc) !=
        BWG_JSON_OK) {
        return false;
    }
    const bool ok = bwg_onboarding_capability_version(yyjson_doc_get_root(doc), out);
    yyjson_doc_free(doc);
    return ok;
}

static bwg_ctrl_error_t inspect(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint8_t *scratch,
                                bwg_ctrl_outcome_t *outcome)
{
    const bwg_onboarding_ops_t *ops = controller->ops.maybe_onboarding;
    void *ctx = controller->ops.ctx;
    if (request->maybe_payload != NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    bwg_partition_facts_t facts;
    if (!ops->partition_facts(ctx, &facts)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    const bwg_settings_reader_t reader = {.ctx = ctx, .read = ops->read_setting};
    const bool readable = bwg_settings_readable(&reader, scratch);
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, ONBOARDING_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    bwg_onboarding_write_inspection(&writer, &facts, readable);
    return bwg_ctrl_reply_close(outcome, &writer);
}

static bwg_ctrl_error_t verify(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint8_t *scratch,
                               bwg_ctrl_outcome_t *outcome)
{
    const bwg_onboarding_ops_t *ops = controller->ops.maybe_onboarding;
    void *ctx = controller->ops.ctx;
    char version[BWG_ONBOARDING_VERSION_MAX];
    bwg_onboarding_verification_t verification = {.firmware_version = version};
    if (!bwg_onboarding_parse_verify(request->maybe_payload, verification.nonce)) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (!running_version(controller, version)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    if (!ops->partition_facts(ctx, &verification.facts)) {
        return BWG_CTRL_PERSISTENCE_FAILED;
    }
    const bwg_settings_reader_t reader = {.ctx = ctx, .read = ops->read_setting};
    for (int i = 0; i < BWG_SETTINGS_CATEGORY_COUNT; i++) {
        // Unreadable settings have no HMAC to give, so the proof is refused rather than weakened.
        if (!bwg_settings_hmac((bwg_settings_category_t) i, &reader, verification.nonce, scratch, verification.hmacs[i])) {
            return BWG_CTRL_PERSISTENCE_FAILED;
        }
    }
    bwg_jw_t writer;
    if (!bwg_ctrl_reply_open(controller, outcome, request->request_id, ONBOARDING_REPLY_BYTES, &writer)) {
        return BWG_CTRL_SESSION_FAILED;
    }
    bwg_onboarding_write_verification(&writer, &verification);
    return bwg_ctrl_reply_close(outcome, &writer);
}

bwg_ctrl_error_t bwg_ctrl_prepare_onboarding(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                             bwg_ctrl_outcome_t *outcome)
{
    const bwg_onboarding_ops_t *maybe_ops = controller->ops.maybe_onboarding;
    if (maybe_ops == NULL || maybe_ops->partition_facts == NULL || maybe_ops->read_setting == NULL) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    // Values such as the Wi-Fi passphrase pass through the scratch buffer, so it is wiped on free.
    uint8_t *scratch = bwg_alloc(controller->allocator, BWG_SETTINGS_VALUE_MAX_BYTES);
    if (scratch == NULL) {
        return BWG_CTRL_SESSION_FAILED;
    }
    const bwg_ctrl_error_t error = bwg_str_equals(request->command, "onboarding_inspect")
                                       ? inspect(controller, request, scratch, outcome)
                                       : verify(controller, request, scratch, outcome);
    bwg_free_secret(controller->allocator, scratch, BWG_SETTINGS_VALUE_MAX_BYTES);
    return error;
}
