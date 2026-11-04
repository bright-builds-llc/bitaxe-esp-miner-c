#ifndef BWG_CTRL_LIFECYCLE_H
#define BWG_CTRL_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_json.h"
#include "bwg_reason.h"

/** What the controller holds, and so what a stop must undo and what a status reports. */

/** A lease or challenge ID of at most 128 bytes, as a validated grant's are, with its NUL. */
#define BWG_CTRL_LEASE_ID_SIZE 129u

/** Both computed from one captured `now`. */
typedef struct {
    uint64_t renew_at_ms;
    uint64_t expires_at_ms;
} bwg_lease_deadlines_t;

typedef struct {
    char lease_id[BWG_CTRL_LEASE_ID_SIZE];
    char challenge_id[BWG_CTRL_LEASE_ID_SIZE];
    bwg_lease_deadlines_t deadlines;
    /** Its renewals must use the exact 60,000/20,000 window. */
    bool exact_window;
} bwg_ctrl_lease_t;

typedef enum {
    /** Nothing held: the device is at its mining baseline. */
    BWG_CTRL_LIFE_IDLE = 0,
    /** A lease is mining under the PENDING marker. */
    BWG_CTRL_LIFE_LEASE,
    /** The lease's stop failed, and every tick retries it. */
    BWG_CTRL_LIFE_LEASE_STOPPING,
    /** The fan is held at full under the PENDING marker, with no lease. */
    BWG_CTRL_LIFE_COOLING,
    /** The fan hold's stop failed, and every tick retries it. */
    BWG_CTRL_LIFE_COOLING_STOPPING,
} bwg_ctrl_life_kind_t;

typedef enum {
    /** Nothing has needed restoring since boot. */
    BWG_CTRL_IDLE_NOT_REQUIRED = 0,
    /** The last effect was stopped and its marker cleared. */
    BWG_CTRL_IDLE_RESTORED,
    /** The boot found a marker, and no status reporting the reboot has been confirmed sent in this session. */
    BWG_CTRL_IDLE_REBOOT_UNREPORTED,
    /** A status reporting the reboot was confirmed sent; the session's next request clears the marker. */
    BWG_CTRL_IDLE_REBOOT_REPORTED,
} bwg_ctrl_idle_kind_t;

/** Zeroed, it is IDLE with nothing to restore. Read it through the functions below, which know which member is live. */
typedef struct {
    bwg_ctrl_life_kind_t kind;
    union {
        /** IDLE. */
        struct {
            bwg_ctrl_idle_kind_t kind;
            /** RESTORED and both REBOOT kinds: the last stop's reason, reported once no reboot is due. */
            bwg_reason_t reason;
            /** REBOOT_REPORTED: the session whose status reported the reboot. */
            uint64_t reported_generation;
        } idle;
        /** LEASE and LEASE_STOPPING. */
        struct {
            bwg_ctrl_lease_t lease;
            /** LEASE_STOPPING. */
            bwg_reason_t stop_reason;
        } lease;
        /** COOLING_STOPPING. */
        bwg_reason_t cooling_stop_reason;
    };
} bwg_ctrl_lifecycle_t;

typedef enum {
    BWG_RESTORATION_NOT_REQUIRED = 0,
    BWG_RESTORATION_PENDING,
    BWG_RESTORATION_CONFIRMED,
} bwg_restoration_status_t;

/** What a status reports as `restoration`. */
typedef struct {
    bwg_restoration_status_t status;
    /** CONFIRMED only. */
    bwg_reason_t reason;
} bwg_ctrl_restoration_t;

/** Anything held, including a stop still being retried. */
bool bwg_ctrl_lifecycle_holds_effect(const bwg_ctrl_lifecycle_t *lifecycle);
/** Idle with no reboot to report: what an effect needs before it starts. */
bool bwg_ctrl_lifecycle_is_quiet(const bwg_ctrl_lifecycle_t *lifecycle);
/** Idle with the boot's reboot not yet acknowledged. */
bool bwg_ctrl_lifecycle_reboot_due(const bwg_ctrl_lifecycle_t *lifecycle);
/** Whether a status reporting the reboot was confirmed sent in session `generation`. */
bool bwg_ctrl_lifecycle_reboot_reported_in(const bwg_ctrl_lifecycle_t *lifecycle, uint64_t generation);
/** The held lease, including one whose stop is being retried, or NULL. */
const bwg_ctrl_lease_t *bwg_ctrl_lifecycle_maybe_lease(const bwg_ctrl_lifecycle_t *lifecycle);
/** The held lease while its stop isn't pending, or NULL. */
const bwg_ctrl_lease_t *bwg_ctrl_lifecycle_maybe_running_lease(const bwg_ctrl_lifecycle_t *lifecycle);
/** Whether the fan hold is in place with no stop pending. */
bool bwg_ctrl_lifecycle_cooling_held(const bwg_ctrl_lifecycle_t *lifecycle);
/** Whether a stop failed and must be retried, and with which reason. `maybe_reason` may be NULL. */
bool bwg_ctrl_lifecycle_stop_pending(const bwg_ctrl_lifecycle_t *lifecycle, bwg_reason_t *maybe_reason);
bwg_ctrl_restoration_t bwg_ctrl_lifecycle_restoration(const bwg_ctrl_lifecycle_t *lifecycle);

/** A boot that found a marker. The reason reported once it is acknowledged starts as `reboot`. */
void bwg_ctrl_lifecycle_find_reboot(bwg_ctrl_lifecycle_t *lifecycle);
/** Holds a lease. The caller has checked the lifecycle is quiet and set the marker. IDs longer than 128 bytes are cut. */
void bwg_ctrl_lifecycle_start_lease(bwg_ctrl_lifecycle_t *lifecycle, bwg_str_t lease_id, bwg_str_t challenge_id,
                                    bwg_lease_deadlines_t deadlines, bool exact_window);
/** Moves a held lease's deadlines; without a lease it does nothing. */
void bwg_ctrl_lifecycle_renew_lease(bwg_ctrl_lifecycle_t *lifecycle, bwg_lease_deadlines_t deadlines);
/** Holds the fan. The caller has checked the lifecycle is quiet and set the marker. */
void bwg_ctrl_lifecycle_hold_cooling(bwg_ctrl_lifecycle_t *lifecycle);
/** Marks whatever is held as stopping for `reason`, replacing an earlier reason. Idle stays idle. */
void bwg_ctrl_lifecycle_begin_stop(bwg_ctrl_lifecycle_t *lifecycle, bwg_reason_t reason);
/** The stop finished and the marker is clear. A due reboot stays due, keeping `reason` for after it. */
void bwg_ctrl_lifecycle_finish_stop(bwg_ctrl_lifecycle_t *lifecycle, bwg_reason_t reason);
/** A status reporting the reboot was confirmed sent in session `generation`. False when no reboot is due. */
bool bwg_ctrl_lifecycle_note_reboot_reported(bwg_ctrl_lifecycle_t *lifecycle, uint64_t generation);
/** The session ended, so its report no longer counts. */
void bwg_ctrl_lifecycle_forget_reboot_report(bwg_ctrl_lifecycle_t *lifecycle);
/** A reported reboot's marker was cleared: restored, with the reason kept for after it. Otherwise it does nothing. */
void bwg_ctrl_lifecycle_acknowledge_reboot(bwg_ctrl_lifecycle_t *lifecycle);

#endif // BWG_CTRL_LIFECYCLE_H
