#include "bwg_review.h"

void bwg_rejection_log_record(bwg_rejection_log_t *log, bwg_auth_operation_t operation,
                              const bwg_auth_attribution_t *attribution)
{
    if (log->boot_rejections != UINT32_MAX) {
        log->boot_rejections++;
    }
    log->has_last = true;
    log->last = (bwg_rejection_record_t) {
        .ordinal = log->boot_rejections,
        .operation = operation,
        .attribution = *attribution,
    };
    log->last_awaits_safe_stop = true;
}

void bwg_rejection_log_attribute_stop(bwg_rejection_log_t *log, bwg_reason_t reason)
{
    if (!log->last_awaits_safe_stop) {
        return;
    }
    log->last_awaits_safe_stop = false;
    log->last.has_safe_stop = true;
    log->last.safe_stop = reason;
}

void bwg_rejection_log_mark_advanced(bwg_rejection_log_t *log)
{
    log->last_awaits_safe_stop = false;
    log->high_water_advanced = true;
}

const char *bwg_auth_operation_label(bwg_auth_operation_t operation)
{
    return operation == BWG_AUTH_OPERATION_START ? "start" : "renew";
}

const char *bwg_auth_signature_label(bwg_auth_signature_t signature)
{
    switch (signature) {
    case BWG_AUTH_SIGNATURE_VALID:
        return "valid";
    case BWG_AUTH_SIGNATURE_INVALID:
        return "invalid";
    case BWG_AUTH_SIGNATURE_NOT_EVALUATED:
        break;
    }
    return "not_evaluated";
}

const char *bwg_auth_context_label(bwg_auth_context_t context)
{
    switch (context) {
    case BWG_AUTH_CONTEXT_CURRENT:
        return "current";
    case BWG_AUTH_CONTEXT_EXPIRED:
        return "expired";
    case BWG_AUTH_CONTEXT_ABSENT:
        return "absent";
    case BWG_AUTH_CONTEXT_MISMATCH:
        break;
    }
    return "mismatch";
}

const char *bwg_auth_replay_guard_label(bwg_auth_replay_guard_t replay_guard)
{
    switch (replay_guard) {
    case BWG_AUTH_REPLAY_FRESH:
        return "fresh";
    case BWG_AUTH_REPLAY_AT_OR_BELOW:
        return "at_or_below_durable_high_water";
    case BWG_AUTH_REPLAY_UNAVAILABLE:
        return "unavailable";
    case BWG_AUTH_REPLAY_NOT_EVALUATED:
        break;
    }
    return "not_evaluated";
}

uint64_t bwg_clock_stimulus_sample(bwg_clock_stimulus_t *stimulus, uint64_t real_now_ms, bool has_last, uint64_t last_ms)
{
    if (stimulus->state != BWG_CLOCK_STIMULUS_ARMED) {
        return real_now_ms;
    }
    if (real_now_ms >= stimulus->expires_ms) {
        stimulus->state = BWG_CLOCK_STIMULUS_EXPIRED;
        return real_now_ms;
    }
    stimulus->state = BWG_CLOCK_STIMULUS_CONSUMED;
    const uint64_t base = has_last && last_ms < real_now_ms ? last_ms : real_now_ms;
    return base > BWG_CLOCK_STIMULUS_OFFSET_MS ? base - BWG_CLOCK_STIMULUS_OFFSET_MS : 0;
}

bool bwg_clock_stimulus_can_prepare(const bwg_clock_stimulus_t *stimulus)
{
    return stimulus->state == BWG_CLOCK_STIMULUS_IDLE && !stimulus->has_pending_token;
}

void bwg_clock_stimulus_prepare(bwg_clock_stimulus_t *stimulus, uint64_t token)
{
    stimulus->has_pending_token = true;
    stimulus->pending_token = token;
}

bool bwg_clock_stimulus_take_pending(bwg_clock_stimulus_t *stimulus, uint64_t *token)
{
    if (!stimulus->has_pending_token) {
        return false;
    }
    stimulus->has_pending_token = false;
    *token = stimulus->pending_token;
    return true;
}

void bwg_clock_stimulus_clear_pending(bwg_clock_stimulus_t *stimulus)
{
    stimulus->has_pending_token = false;
}

bool bwg_clock_stimulus_arm(bwg_clock_stimulus_t *stimulus, uint64_t real_now_ms)
{
    if (stimulus->state != BWG_CLOCK_STIMULUS_IDLE) {
        return false;
    }
    stimulus->state = BWG_CLOCK_STIMULUS_ARMED;
    stimulus->expires_ms = real_now_ms + BWG_CLOCK_STIMULUS_ARMED_MS;
    return true;
}

void bwg_clock_stimulus_cancel(bwg_clock_stimulus_t *stimulus)
{
    stimulus->has_pending_token = false;
    if (stimulus->state == BWG_CLOCK_STIMULUS_ARMED) {
        stimulus->state = BWG_CLOCK_STIMULUS_EXPIRED;
    }
}

const char *bwg_clock_stimulus_label(const bwg_clock_stimulus_t *stimulus)
{
    switch (stimulus->state) {
    case BWG_CLOCK_STIMULUS_ARMED:
        return "armed";
    case BWG_CLOCK_STIMULUS_CONSUMED:
        return "consumed";
    case BWG_CLOCK_STIMULUS_EXPIRED:
        return "expired";
    case BWG_CLOCK_STIMULUS_IDLE:
        break;
    }
    return "idle";
}
