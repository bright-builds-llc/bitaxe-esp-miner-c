#ifndef BWG_CONTROLLER_H
#define BWG_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"
#include "bwg_cadence.h"
#include "bwg_ctrl_lifecycle.h"
#include "bwg_json_writer.h"
#include "bwg_lease.h"
#include "bwg_lease_auth.h"
#include "bwg_link.h"
#include "bwg_possession.h"
#include "bwg_reason.h"
#include "bwg_review.h"
#include "bwg_trust.h"
#include "bwg_noise_record.h"
#include "bwg_onboarding.h"
#include "bwg_v2_record.h"

#define BWG_CONTROLLER_ADMISSION_MAX_AGE_MS 60000u
#define BWG_CONTROLLER_MAX_SEEN_NONCES 256u
/** Room for a status with its `qualification` and `preservation` objects. Past this is `encoding_failed`. */
#define BWG_CONTROLLER_STATUS_MAX_BYTES 16384u
/** A SHA-256 as lowercase hex, with its NUL. */
#define BWG_FINGERPRINT_HEX_SIZE 65u

/** The closed rejection categories. BWG_CTRL_OK is the only value that is not one. */
typedef enum {
    BWG_CTRL_OK = 0,
    BWG_CTRL_INVALID_FRAME,
    BWG_CTRL_INVALID_REQUEST,
    BWG_CTRL_ADMISSION_REQUIRED,
    BWG_CTRL_INVALID_PROOF,
    BWG_CTRL_AUTHENTICATION_FAILED,
    BWG_CTRL_INVALID_TRANSITION,
    BWG_CTRL_PERSISTENCE_FAILED,
    BWG_CTRL_MONOTONIC_RESET,
    BWG_CTRL_SESSION_FAILED,
    BWG_CTRL_RESTORATION_PENDING,
    BWG_CTRL_STALE_RESPONSE,
    BWG_CTRL_ENCODING_FAILED,
} bwg_ctrl_error_t;

/** The protocol text of a category. BWG_CTRL_OK returns NULL: it is never sent. */
const char *bwg_ctrl_error_category(bwg_ctrl_error_t error);

/** The durable marker of a possibly running effect. Stored values are the protocol's; CLEAR is an absent key. */
typedef enum {
    BWG_EFFECT_CLEAR = 0,
    BWG_EFFECT_PENDING = 1,
    BWG_EFFECT_REBOOT_CONFIRMED = 2,
} bwg_effect_state_t;

typedef enum {
    /** Nothing to report: the controller omits the key and discards anything written. */
    BWG_EVIDENCE_ABSENT = 0,
    /** Exactly one JSON value was written. */
    BWG_EVIDENCE_WRITTEN,
    BWG_EVIDENCE_SESSION_FAILED,
    BWG_EVIDENCE_PERSISTENCE_FAILED,
} bwg_evidence_t;

/** What the device reports of a V2 scope, read as one snapshot. */
typedef struct {
    bwg_v2_observation_t observation;
    bwg_v2_state_t state;
    bool has_record;
    char attempt_id[BWG_V2_ATTEMPT_ID_SIZE];
} bwg_v2_view_t;

/** A validated channel start. Borrows from its document, valid only during the call. */
typedef struct {
    bwg_str_t attempt_id;
    bwg_str_t endpoint;
    bwg_str_t authority_public_key;
    bwg_str_t user_identity;
} bwg_v2_channel_input_t;

/** What the device reports of its Noise diagnostic, read as one snapshot. */
typedef struct {
    bwg_v2_observation_t observation;
    bool has_record;
    bool terminal;
    char attempt_id[BWG_V2_ATTEMPT_ID_SIZE];
} bwg_noise_view_t;

/** A validated diagnostic start. Borrows from its document, valid only during the call. */
typedef struct {
    bwg_str_t attempt_id;
    bwg_str_t fixture_ipv4;
    uint16_t fixture_port;
    bwg_str_t authority_public_key;
    char input_sha256[BWG_NOISE_DIGEST_TEXT_SIZE];
} bwg_noise_input_t;

/** A device's Stratum V2 attempts. Each call gets the controller ops' `ctx`. */
typedef struct {
    /** Reads a V2 scope for this session. False when it can't. */
    bool (*view)(void *ctx, bwg_v2_scope_t scope, uint64_t generation, bwg_v2_view_t *view);
    /** Writes a V2 status of this observation, which is what a later command must name. */
    void (*write_status)(void *ctx, bwg_v2_scope_t scope, const bwg_v2_observation_t *observation, bwg_jw_t *writer);
    /** Whether a V2 attempt still holds the network or its fence. */
    bool (*busy)(void *ctx);
    /** Admits a channel attempt without starting it. */
    bool (*admit)(void *ctx, const bwg_v2_channel_input_t *input, uint64_t generation);
    /** Starts the admitted channel attempt, once its reply was sent. */
    bool (*dispatch)(void *ctx);
    /** Cancels a channel attempt. */
    bool (*cancel)(void *ctx);
} bwg_v2_ops_t;

/** A device's Noise diagnostic. Each call gets the controller ops' `ctx`. */
typedef struct {
    /** Reads the diagnostic for this session. False when it can't. */
    bool (*view)(void *ctx, uint64_t generation, bwg_noise_view_t *view);
    /** Writes a diagnostic status of this observation, which is what a later Start must name. */
    void (*write_status)(void *ctx, const bwg_v2_observation_t *observation, bwg_jw_t *writer);
    /** Whether a diagnostic still holds the network. */
    bool (*busy)(void *ctx);
    /** Admits a diagnostic without starting it. */
    bool (*admit)(void *ctx, const bwg_noise_input_t *input, const bwg_v2_observation_t *observation);
    /** Starts the admitted diagnostic, once its reply was sent. */
    bool (*dispatch)(void *ctx);
    /** Cancels a diagnostic. */
    bool (*cancel)(void *ctx);
} bwg_noise_ops_t;

/** Everything the controller asks of the device. Every call runs on the owner task, and may block it. */
typedef struct {
    void *ctx;
    /**
     * Starts mining. `grant` is valid only during the call and holds the pool secrets: copy what is
     * needed and wipe the copies later. False stops the lease with `control_failed`.
     */
    bool (*start)(void *ctx, const bwg_lease_grant_t *grant, bwg_lease_deadlines_t deadlines);
    /** Extends the running lease. False stops it with `control_failed`. */
    bool (*renew)(void *ctx, const bwg_lease_renewal_t *renewal, bwg_lease_deadlines_t deadlines);
    /** Blocks until the stop is finished. False leaves the cleanup pending and the next call retries it. */
    bool (*safe_stop)(void *ctx, bwg_reason_t reason);
    /** Same contract as bwg_sequence_store_t.load. */
    bool (*high_water_load)(void *ctx, const char *kid, bool *present, uint64_t *value);
    /** Same contract as bwg_sequence_store_t.compare_and_store. */
    bwg_store_result_t (*high_water_compare_and_store)(void *ctx, const char *kid, bool has_expected,
                                                       uint64_t expected, uint64_t next);
    /** Reads the marker. False means it is unreadable or holds an unknown value. */
    bool (*effect_get)(void *ctx, bwg_effect_state_t *state);
    /** Stores the marker and verifies it by reading back. */
    bool (*effect_set)(void *ctx, bwg_effect_state_t state);
    /** Optional. Writes the `qualification` value for a status-bearing result. */
    bwg_evidence_t (*maybe_append_qualification)(void *ctx, bwg_jw_t *writer);
    /** Optional. Writes the `preservation` value, called only after possession in this logical session. */
    bwg_evidence_t (*maybe_append_preservation)(void *ctx, bwg_jw_t *writer, const char *device_identity_sha256);
    /** Optional. The same high-water digest `preservation` reports. Without it the rejection review is refused. */
    bool (*maybe_high_water_fingerprint)(void *ctx, char fingerprint[BWG_FINGERPRINT_HEX_SIZE]);
    /** Optional. This boot's closed reset cause. Without it the boot review says `other`. */
    const char *(*maybe_boot_reset_cause)(void *ctx);
    /**
     * Optional. Writes a read-only budget review for `kind`; `campaign_id` applies to a campaign. WRITTEN, or
     * SESSION_FAILED for an unreadable ledger. Without it the reviews are refused.
     */
    bwg_evidence_t (*maybe_budget_review)(void *ctx, bwg_allowance_kind_t kind, bwg_str_t campaign_id, bwg_jw_t *writer);
    /** Optional. Holds the fan at full and proves it turning, with no ASIC effect. Without it the command is refused. */
    bool (*maybe_cooling_prove)(void *ctx, uint32_t *fan_rpm);
    /** Optional. Returns the fan to its baseline once the board is cool enough. */
    bool (*maybe_cooling_restore)(void *ctx);
    /** Optional. Whether an idle board may restart now, and the boot being left. Without it the command is refused. */
    bool (*maybe_restart_context)(void *ctx, uint64_t *boot_ordinal);
    /** Optional. Restarts the board, after a restart's reply has been sent. */
    void (*maybe_restart)(void *ctx);
    /** Optional. As the restart's context, for a board that is also proven off and has a store ready. */
    bool (*maybe_core_dump_context)(void *ctx, uint64_t *boot_ordinal);
    /** Optional. Panics the board, after the self-test's reply has been sent. */
    void (*maybe_core_dump_self_test)(void *ctx);
    /** Optional. Writes where the board's own HTTP status can be read; false when it has no usable address. */
    bool (*maybe_write_endpoint)(void *ctx, bwg_jw_t *writer);
    /** Optional. Writes the serial path's trace, which never holds a payload. */
    void (*maybe_write_trace)(void *ctx, bwg_jw_t *writer);
    /** Optional. Arms a phase of the telemetry cadence recorder and writes its reply; false when it can't be armed. */
    bool (*maybe_cadence_arm)(void *ctx, bwg_cadence_phase_t phase, bwg_jw_t *writer);
    /** Optional. Writes the telemetry cadence recorder's review. */
    void (*maybe_write_cadence)(void *ctx, bwg_jw_t *writer);
    /** Optional. A transport probe of the largest size is about to be answered. */
    void (*maybe_note_max_probe)(void *ctx);
    /** Optional, and whole when present. Without it the V2 commands are refused. */
    const bwg_v2_ops_t *maybe_v2;
    /** Optional, and whole when present. Without it the Noise diagnostic commands are refused. */
    const bwg_noise_ops_t *maybe_noise;
    /** Optional, and whole when present. Without it the onboarding commands are refused. */
    const bwg_onboarding_ops_t *maybe_onboarding;
} bwg_controller_ops_t;

/** Replaces the deployment-trust check, for fixtures that carry no real signature. */
typedef struct {
    void *ctx;
    bwg_auth_result_t (*verify_start)(void *ctx, const bwg_lease_grant_t *grant, const char *binding);
    bwg_auth_result_t (*verify_renew)(void *ctx, const bwg_lease_renewal_t *renewal, const char *challenge_id,
                                      const char *binding);
} bwg_lease_verifier_t;

typedef struct {
    /** The signed capability document. Sent minified with its key order kept; hashed in canonical form. */
    const uint8_t *capability_json;
    size_t capability_len;
    /** The serial manifest, hashed in canonical form. */
    const uint8_t *manifest_json;
    size_t manifest_len;
    /** 40 lowercase hex characters. */
    const char *firmware_commit;
    /** 64 lowercase hex characters. */
    const char *app_elf_hex;
    /** The deployment trust document. Absent or invalid disables leases: Start answers `authentication_failed`. */
    const uint8_t *maybe_trust_json;
    size_t trust_len;
    /** The device seed. The caller wipes its copy after bwg_controller_init. */
    const uint8_t *seed;
    /** NULL verifies against the trust document and the high-water store. Tests only otherwise. */
    const bwg_lease_verifier_t *maybe_verifier;
} bwg_controller_config_t;

/** What the owner holds until a reply's fate is known, then passes to bwg_controller_confirm_sent. */
typedef enum {
    BWG_CTRL_EFFECT_NONE = 0,
    BWG_CTRL_EFFECT_ADMIT,
    BWG_CTRL_EFFECT_BOOT_REPORTED,
    BWG_CTRL_EFFECT_CLOCK_STIMULUS,
    BWG_CTRL_EFFECT_QUALIFICATION_RESTART,
    BWG_CTRL_EFFECT_CORE_DUMP_SELF_TEST,
    BWG_CTRL_EFFECT_V2_OBSERVATION,
    BWG_CTRL_EFFECT_V2_DISPATCH,
    BWG_CTRL_EFFECT_NOISE_OBSERVATION,
    BWG_CTRL_EFFECT_NOISE_DISPATCH,
} bwg_ctrl_effect_kind_t;

typedef struct {
    bwg_ctrl_effect_kind_t kind;
    uint64_t generation;
    uint64_t token;
    uint64_t established_at_ms;
    char binding[BWG_DIGEST_TEXT_SIZE];
} bwg_ctrl_effect_t;

/** One frame's result. Keep it until the writer finishes with `response`, then release it. */
typedef struct {
    bwg_ctrl_error_t error;
    /** `response` holds one JSON document without LF, for bwg_writer_offer_control. */
    bool has_reply;
    /** Revoke the epoch once the reply, if any, was sent. Set on every error, so a retry needs a new Hello. */
    bool revoke_after_send;
    bwg_buffer_t response;
    /** Apply through bwg_controller_confirm_sent only after the writer reports the reply sent. */
    bwg_ctrl_effect_t effect;
} bwg_ctrl_outcome_t;

/**
 * The Controller 0.4 state machine. Touched by the owner task only. It keeps no pool credential or
 * authorization once a call returns, and holds about 1.5 KiB inline, so keep it off a task stack.
 */
typedef struct {
    const bwg_allocator_t *allocator;
    bwg_controller_ops_t ops;
    bool has_verifier;
    bwg_lease_verifier_t verifier;
    bool trust_valid;
    bwg_lease_trust_t trust;
    bwg_identity_t identity;
    bwg_buffer_t capability;
    char capability_digest[BWG_DIGEST_TEXT_SIZE];
    char manifest_digest[BWG_DIGEST_TEXT_SIZE];
    char firmware_commit[BWG_FIRMWARE_COMMIT_LEN + 1];
    char app_elf_hex[BWG_APP_ELF_HEX_LEN + 1];

    bool has_binding;
    bwg_session_binding_t binding;
    uint64_t generation;

    bool has_admission;
    uint64_t admission_generation;
    uint64_t admission_established_at_ms;
    char admission_binding[BWG_DIGEST_TEXT_SIZE];
    bool authenticated_logical_session;
    bool has_pending_token;
    uint64_t pending_token;
    bool restart_consumed;
    uint64_t next_token;
    uint8_t *seen_nonces;
    size_t seen_count;

    bwg_ctrl_lifecycle_t lifecycle;
    bool has_last_ms;
    uint64_t last_ms;

    bwg_rejection_log_t rejections;
    bwg_clock_stimulus_t stimulus;
    /** Decreasing-clock detections since boot, real or stimulated. */
    uint32_t clock_discontinuities;

    /** An idle V2 status reported in this session, which a Start must follow. */
    bool has_v2_observation;
    bwg_v2_scope_t v2_observation_scope;
    bwg_v2_observation_t v2_observation;
    /** The same, until its reply is confirmed sent. */
    bwg_v2_scope_t v2_pending_scope;
    bwg_v2_observation_t v2_pending_observation;
    /** The session whose admission a retained attempt was bound to: it may read that attempt without a fresh one. */
    bool has_v2_generation;
    uint64_t v2_generation;

    /** The same three for the Noise diagnostic. */
    bool has_noise_observation;
    bwg_v2_observation_t noise_observation;
    bwg_v2_observation_t noise_pending_observation;
    bool has_noise_generation;
    uint64_t noise_generation;
} bwg_controller_t;

/**
 * Prepares the controller. Call it only after the device has confirmed its mining baseline: a
 * stored PENDING marker moves to REBOOT_CONFIRMED here, and any marker makes the first baseline
 * report `reboot`. False means bad configuration, no memory or a marker failure; nothing remains
 * allocated, and the device must not take part in the Gate.
 */
bool bwg_controller_init(bwg_controller_t *controller, const bwg_allocator_t *allocator,
                         const bwg_controller_config_t *config, const bwg_controller_ops_t *ops);

/** Wipes the identity and frees everything. It stops nothing, so stop any lease first. */
void bwg_controller_deinit(bwg_controller_t *controller);

/**
 * Installs a new Hello's identifiers, discarding admission and seen nonces. `invalid_transition`
 * while a lease is active or its cleanup is unfinished; the caller then revokes the link.
 */
bwg_ctrl_error_t bwg_controller_begin_session(bwg_controller_t *controller, const bwg_session_binding_t *binding);

/**
 * Handles one control frame, with its trailing LF, at the single clock reading `now_ms`. Always
 * fills `outcome`, whose error is also returned. A rejection is sent only for a structurally valid
 * controller request, so a malformed or possession frame gets none. Needs about 3 KiB of stack
 * inside libsodium on the Start and Renew paths; other big buffers come from the allocator.
 */
bwg_ctrl_error_t bwg_controller_frame(bwg_controller_t *controller, const uint8_t *frame, size_t len, uint64_t now_ms,
                                      bwg_ctrl_outcome_t *outcome);

/** Wipes and frees the response. An empty outcome is a no-op. */
void bwg_controller_outcome_release(bwg_controller_t *controller, bwg_ctrl_outcome_t *outcome);

/**
 * Applies a reply's effect, called only once that reply was truly sent: this, not the proof, is
 * what admits. `stale_response` means the session moved on, and the caller revokes the link.
 */
bwg_ctrl_error_t bwg_controller_confirm_sent(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms);

/** Whether possession in this logical session was confirmed sent and not since invalidated. */
bool bwg_controller_is_admitted(const bwg_controller_t *controller);

/** Whether a lease is active, including one whose stop is failing and still being retried. */
bool bwg_controller_has_active_lease(const bwg_controller_t *controller);

/** The canonical capability digest that a possession request must name. */
const char *bwg_controller_capability_digest(const bwg_controller_t *controller);

/**
 * Supervises the clock, every 20 ms on the owner task: retries an unfinished stop, then stops with
 * `monotonic_reset` if the clock went backwards (and returns that error) or `lease_expired` at the
 * deadline. Any other error is a stop that failed, and the next tick retries it.
 */
bwg_ctrl_error_t bwg_controller_tick(bwg_controller_t *controller, uint64_t now_ms);

/** The link closed: stops with `connectivity_lost` and discards the logical session even if the stop failed. */
bwg_ctrl_error_t bwg_controller_disconnect(bwg_controller_t *controller, uint64_t now_ms);

/** The device is about to reboot: as disconnect, with reason `reboot`. */
bwg_ctrl_error_t bwg_controller_reboot(bwg_controller_t *controller, uint64_t now_ms);

/** The owner saw an unrecoverable control failure: as disconnect, with reason `control_failed`. */
bwg_ctrl_error_t bwg_controller_control_failed(bwg_controller_t *controller, uint64_t now_ms);

#endif // BWG_CONTROLLER_H
