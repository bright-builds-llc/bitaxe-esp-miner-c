#ifndef TEST_CONTROLLER_RIG_H
#define TEST_CONTROLLER_RIG_H

#include "bwg_controller.h"

#define RIG_FIXTURE_AUTH "fixture-authentication-not-a-production-secret"
#define RIG_FIXTURE_RENEW_AUTH "fixture-renewal-authentication"
#define RIG_PASSWORD "fixture-session-password"
#define RIG_BINDING "jEc1ro90lL2fX0ZozUbY0XpmxrC46gEkEUKKTyGj0Zs"

#define RIG_ENVELOPE_HEAD "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"requestId\":\"serial_test\","

#define RIG_START_PAYLOAD_WITH(duration, renew_after, authorization)                                                   \
    "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"lease_fixture_01\",\"challengeId\":"             \
    "\"challenge_00000000000000000000000000000001\",\"authorization\":\"" authorization                                \
    "\",\"durationMilliseconds\":" duration ",\"renewAfterMilliseconds\":" renew_after                                 \
    ",\"stratum\":{\"endpoint\":\"stratum+tcp://127.0.0.1:3333/\",\"username\":\"fixture-session-user\","              \
    "\"password\":\"" RIG_PASSWORD "\"}}"
#define RIG_START_PAYLOAD RIG_START_PAYLOAD_WITH("60000", "20000", RIG_FIXTURE_AUTH)

#define RIG_RENEW_PAYLOAD_WITH(lease, duration, renew_after, authorization)                                            \
    "{\"protocolVersion\":\"bwg-worker-controller/0.4\",\"leaseId\":\"" lease "\",\"authorization\":\"" authorization  \
    "\",\"durationMilliseconds\":" duration ",\"renewAfterMilliseconds\":" renew_after "}"
#define RIG_RENEW_PAYLOAD RIG_RENEW_PAYLOAD_WITH("lease_fixture_01", "60000", "20000", RIG_FIXTURE_RENEW_AUTH)

/** Everything the device would do, recorded and scriptable. It outlives a controller, as flash outlives a boot. */
typedef struct {
    char events[512];
    bwg_effect_state_t effect;
    bool effect_unreadable;
    int effect_writes;
    int mark_failures;
    int clear_failures;
    bool fail_start;
    bool fail_renew;
    int safe_stop_failures;
    bwg_lease_deadlines_t start_deadlines;
    bwg_lease_deadlines_t renew_deadlines;

    bool high_water_corrupt;
    bool high_water_present;
    uint64_t high_water;

    bwg_evidence_t qualification_result;
    const char *maybe_qualification_json;
    bwg_evidence_t preservation_result;
    const char *maybe_preservation_json;
    size_t qualification_fill_bytes;

    bool fingerprint_fails;
    const char *maybe_boot_reset_cause;

    bool has_budget;
    uint64_t boot_ordinal;
    bool restart_allowed;
    int restarts;
    bool outputs_off;
    int core_dump_self_tests;
    bool has_endpoint;
    bwg_cadence_t cadence;
    int max_probes;
    bool budget_review_fails;
    bwg_allowance_kind_t last_review_kind;
    bool last_review_matched;
    bool fail_cooling_prove;
    bool fail_cooling_restore;

    bool has_v2;
    bwg_v2_view_t v2_view;
    bool v2_busy;
    bool fail_v2_admit;
    bool fail_v2_dispatch;
    bool has_noise;
    bwg_noise_view_t noise_view;
    bool noise_busy;
    bool fail_noise_admit;
    bool fail_noise_dispatch;
    char noise_input_sha256[BWG_NOISE_DIGEST_TEXT_SIZE];
} rig_device_t;

typedef struct {
    rig_device_t device;
    bwg_controller_t controller;
    bwg_ctrl_outcome_t outcome;
    bool outcome_held;
    bool real_trust;
    bool custom_trust;
    const char *maybe_trust_text;
    int next_nonce;
} rig_t;

/** Opens a controller on the fixture capability with a stub verifier that accepts the fixture authorizations. */
bool rig_open(rig_t *rig);
/** The same on the real deployment trust and high-water store. */
bool rig_open_real_trust(rig_t *rig);
/** Opens with a stub verifier, admits and starts the fixture lease at `now_ms`. */
bool rig_open_with_lease(rig_t *rig, uint64_t now_ms);
/** Opens with this trust document on the real verifier. NULL opens with none, which disables leases. */
bool rig_open_with_trust_text(rig_t *rig, const char *maybe_trust);
/** Replaces the controller as a reboot would, keeping the device's durable state. */
bool rig_reboot(rig_t *rig);
void rig_close(rig_t *rig);

/** The Hello identifiers of the possession fixture. */
bwg_session_binding_t rig_fixture_session(void);

/** Sends raw frame bytes. The outcome stays held in the rig until the next send or close. */
bwg_ctrl_error_t rig_send_raw(rig_t *rig, const void *frame, size_t len, uint64_t now_ms);
/** Sends one JSON text with an LF added. */
bwg_ctrl_error_t rig_send_json(rig_t *rig, const char *json, uint64_t now_ms);
/** Sends a controller command with request ID `serial_test`. `maybe_payload` is JSON text. */
bwg_ctrl_error_t rig_send(rig_t *rig, const char *command, const char *maybe_payload, uint64_t now_ms);
/** Sends a possession request for this fixture session. Nonce 0 is the fixture's own, so it yields RIG_BINDING. */
bwg_ctrl_error_t rig_send_possession(rig_t *rig, int nonce, uint64_t now_ms);
/** Confirms the held outcome's effect as sent. */
bwg_ctrl_error_t rig_confirm(rig_t *rig);
bwg_ctrl_error_t rig_confirm_at(rig_t *rig, uint64_t now_ms);
/** Begins a fixture session, proves possession with the next nonce and confirms it. */
bool rig_admit(rig_t *rig, uint64_t now_ms);
/** Proves possession again in the current session and confirms it. */
bool rig_reprove(rig_t *rig, uint64_t now_ms);

/** The held reply as a NUL-terminated copy, or an empty string. Valid until the next call. */
const char *rig_reply_text(const rig_t *rig);
/** Whether the string at a JSON pointer in the held reply equals `expected`. */
bool rig_reply_str_is(const rig_t *rig, const char *pointer, const char *expected);
/** Whether the integer at a JSON pointer in the held reply equals `expected`. */
bool rig_reply_u64_is(const rig_t *rig, const char *pointer, uint64_t expected);
/** Whether a JSON pointer resolves in the held reply. */
bool rig_reply_has(const rig_t *rig, const char *pointer);
/** A `fingerprintSha256` from a rig device. */
#define RIG_FINGERPRINT "2222222222222222222222222222222222222222222222222222222222222222"

/** Whether the held reply is the closed rejection with this category, correlated to `serial_test`. */
bool rig_reply_is_rejection(const rig_t *rig, bwg_ctrl_error_t error);

#endif // TEST_CONTROLLER_RIG_H
