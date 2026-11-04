#include "bwg_ctrl_lifecycle.h"

#include <string.h>

static bool is_reboot(bwg_ctrl_idle_kind_t kind)
{
    return kind == BWG_CTRL_IDLE_REBOOT_UNREPORTED || kind == BWG_CTRL_IDLE_REBOOT_REPORTED;
}

static void copy_id(char out[BWG_CTRL_LEASE_ID_SIZE], bwg_str_t id)
{
    const size_t len = id.len < BWG_CTRL_LEASE_ID_SIZE ? id.len : BWG_CTRL_LEASE_ID_SIZE - 1;
    memcpy(out, id.ptr, len);
    out[len] = '\0';
}

bool bwg_ctrl_lifecycle_holds_effect(const bwg_ctrl_lifecycle_t *lifecycle)
{
    return lifecycle->kind != BWG_CTRL_LIFE_IDLE;
}

bool bwg_ctrl_lifecycle_is_quiet(const bwg_ctrl_lifecycle_t *lifecycle)
{
    return lifecycle->kind == BWG_CTRL_LIFE_IDLE && !is_reboot(lifecycle->idle.kind);
}

bool bwg_ctrl_lifecycle_reboot_due(const bwg_ctrl_lifecycle_t *lifecycle)
{
    return lifecycle->kind == BWG_CTRL_LIFE_IDLE && is_reboot(lifecycle->idle.kind);
}

bool bwg_ctrl_lifecycle_reboot_reported_in(const bwg_ctrl_lifecycle_t *lifecycle, uint64_t generation)
{
    return lifecycle->kind == BWG_CTRL_LIFE_IDLE && lifecycle->idle.kind == BWG_CTRL_IDLE_REBOOT_REPORTED &&
           lifecycle->idle.reported_generation == generation;
}

const bwg_ctrl_lease_t *bwg_ctrl_lifecycle_maybe_lease(const bwg_ctrl_lifecycle_t *lifecycle)
{
    const bool leased = lifecycle->kind == BWG_CTRL_LIFE_LEASE || lifecycle->kind == BWG_CTRL_LIFE_LEASE_STOPPING;
    return leased ? &lifecycle->lease.lease : NULL;
}

const bwg_ctrl_lease_t *bwg_ctrl_lifecycle_maybe_running_lease(const bwg_ctrl_lifecycle_t *lifecycle)
{
    return lifecycle->kind == BWG_CTRL_LIFE_LEASE ? &lifecycle->lease.lease : NULL;
}

bool bwg_ctrl_lifecycle_cooling_held(const bwg_ctrl_lifecycle_t *lifecycle)
{
    return lifecycle->kind == BWG_CTRL_LIFE_COOLING;
}

bool bwg_ctrl_lifecycle_stop_pending(const bwg_ctrl_lifecycle_t *lifecycle, bwg_reason_t *maybe_reason)
{
    bwg_reason_t reason;
    switch (lifecycle->kind) {
    case BWG_CTRL_LIFE_LEASE_STOPPING:
        reason = lifecycle->lease.stop_reason;
        break;
    case BWG_CTRL_LIFE_COOLING_STOPPING:
        reason = lifecycle->cooling_stop_reason;
        break;
    case BWG_CTRL_LIFE_IDLE:
    case BWG_CTRL_LIFE_LEASE:
    case BWG_CTRL_LIFE_COOLING:
    default:
        return false;
    }
    if (maybe_reason != NULL) {
        *maybe_reason = reason;
    }
    return true;
}

bwg_ctrl_restoration_t bwg_ctrl_lifecycle_restoration(const bwg_ctrl_lifecycle_t *lifecycle)
{
    if (lifecycle->kind != BWG_CTRL_LIFE_IDLE) {
        return (bwg_ctrl_restoration_t){.status = BWG_RESTORATION_PENDING};
    }
    switch (lifecycle->idle.kind) {
    case BWG_CTRL_IDLE_NOT_REQUIRED:
        return (bwg_ctrl_restoration_t){.status = BWG_RESTORATION_NOT_REQUIRED};
    case BWG_CTRL_IDLE_REBOOT_UNREPORTED:
    case BWG_CTRL_IDLE_REBOOT_REPORTED:
        return (bwg_ctrl_restoration_t){.status = BWG_RESTORATION_CONFIRMED, .reason = BWG_REASON_REBOOT};
    case BWG_CTRL_IDLE_RESTORED:
    default:
        return (bwg_ctrl_restoration_t){.status = BWG_RESTORATION_CONFIRMED, .reason = lifecycle->idle.reason};
    }
}

static void become_idle(bwg_ctrl_lifecycle_t *lifecycle, bwg_ctrl_idle_kind_t kind, bwg_reason_t reason)
{
    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->kind = BWG_CTRL_LIFE_IDLE;
    lifecycle->idle.kind = kind;
    lifecycle->idle.reason = reason;
}

void bwg_ctrl_lifecycle_find_reboot(bwg_ctrl_lifecycle_t *lifecycle)
{
    become_idle(lifecycle, BWG_CTRL_IDLE_REBOOT_UNREPORTED, BWG_REASON_REBOOT);
}

void bwg_ctrl_lifecycle_start_lease(bwg_ctrl_lifecycle_t *lifecycle, bwg_str_t lease_id, bwg_str_t challenge_id,
                                    bwg_lease_deadlines_t deadlines, bool exact_window)
{
    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->kind = BWG_CTRL_LIFE_LEASE;
    bwg_ctrl_lease_t *lease = &lifecycle->lease.lease;
    copy_id(lease->lease_id, lease_id);
    copy_id(lease->challenge_id, challenge_id);
    lease->deadlines = deadlines;
    lease->exact_window = exact_window;
}

void bwg_ctrl_lifecycle_renew_lease(bwg_ctrl_lifecycle_t *lifecycle, bwg_lease_deadlines_t deadlines)
{
    if (bwg_ctrl_lifecycle_maybe_lease(lifecycle) != NULL) {
        lifecycle->lease.lease.deadlines = deadlines;
    }
}

void bwg_ctrl_lifecycle_hold_cooling(bwg_ctrl_lifecycle_t *lifecycle)
{
    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->kind = BWG_CTRL_LIFE_COOLING;
}

void bwg_ctrl_lifecycle_begin_stop(bwg_ctrl_lifecycle_t *lifecycle, bwg_reason_t reason)
{
    switch (lifecycle->kind) {
    case BWG_CTRL_LIFE_LEASE:
    case BWG_CTRL_LIFE_LEASE_STOPPING:
        lifecycle->kind = BWG_CTRL_LIFE_LEASE_STOPPING;
        lifecycle->lease.stop_reason = reason;
        break;
    case BWG_CTRL_LIFE_COOLING:
    case BWG_CTRL_LIFE_COOLING_STOPPING:
        lifecycle->kind = BWG_CTRL_LIFE_COOLING_STOPPING;
        lifecycle->cooling_stop_reason = reason;
        break;
    case BWG_CTRL_LIFE_IDLE:
    default:
        break;
    }
}

void bwg_ctrl_lifecycle_finish_stop(bwg_ctrl_lifecycle_t *lifecycle, bwg_reason_t reason)
{
    if (bwg_ctrl_lifecycle_reboot_due(lifecycle)) {
        lifecycle->idle.reason = reason;
        return;
    }
    become_idle(lifecycle, BWG_CTRL_IDLE_RESTORED, reason);
}

bool bwg_ctrl_lifecycle_note_reboot_reported(bwg_ctrl_lifecycle_t *lifecycle, uint64_t generation)
{
    if (!bwg_ctrl_lifecycle_reboot_due(lifecycle)) {
        return false;
    }
    lifecycle->idle.kind = BWG_CTRL_IDLE_REBOOT_REPORTED;
    lifecycle->idle.reported_generation = generation;
    return true;
}

void bwg_ctrl_lifecycle_forget_reboot_report(bwg_ctrl_lifecycle_t *lifecycle)
{
    if (lifecycle->kind == BWG_CTRL_LIFE_IDLE && lifecycle->idle.kind == BWG_CTRL_IDLE_REBOOT_REPORTED) {
        lifecycle->idle.kind = BWG_CTRL_IDLE_REBOOT_UNREPORTED;
        lifecycle->idle.reported_generation = 0;
    }
}

void bwg_ctrl_lifecycle_acknowledge_reboot(bwg_ctrl_lifecycle_t *lifecycle)
{
    if (lifecycle->kind == BWG_CTRL_LIFE_IDLE && lifecycle->idle.kind == BWG_CTRL_IDLE_REBOOT_REPORTED) {
        become_idle(lifecycle, BWG_CTRL_IDLE_RESTORED, lifecycle->idle.reason);
    }
}
