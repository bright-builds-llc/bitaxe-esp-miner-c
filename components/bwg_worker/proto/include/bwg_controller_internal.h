#ifndef BWG_CONTROLLER_INTERNAL_H
#define BWG_CONTROLLER_INTERNAL_H

#include "bwg_controller.h"

/** One frame, parsed. `doc` is owned and holds any secrets the frame carried. */
typedef struct {
    yyjson_doc *doc;
    yyjson_val *root;
    bool is_possession;
    /** The frame is a structurally valid controller request, so a rejection can name its `requestId`. */
    bool correlated;
    bwg_str_t request_id;
    bwg_str_t command;
    /** NULL when the payload is absent or null. */
    yyjson_val *maybe_payload;
    /** With the newline that ends the frame. */
    size_t frame_len;
} bwg_ctrl_request_t;

/** Parses and classifies a frame without touching controller state. Sets `doc` on anything that parsed. */
bwg_ctrl_error_t bwg_ctrl_parse_frame(const bwg_controller_t *controller, const uint8_t *frame, size_t len,
                                      bwg_ctrl_request_t *request);

/** Allocates `outcome->response` and writes the success envelope up to the result value. False: no memory. */
bool bwg_ctrl_reply_open(const bwg_controller_t *controller, bwg_ctrl_outcome_t *outcome, bwg_str_t request_id,
                         size_t capacity, bwg_jw_t *writer);

/** Closes the envelope. `encoding_failed` on overflow, `invalid_frame` past the payload limit. */
bwg_ctrl_error_t bwg_ctrl_reply_close(bwg_ctrl_outcome_t *outcome, bwg_jw_t *writer);

/** Replaces any response with the closed rejection, if the request allows one and memory does. */
void bwg_ctrl_reply_reject(const bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                           bwg_ctrl_error_t error, bwg_ctrl_outcome_t *outcome);

/** Writes a whole status result with its evidence. `restoration_pending` if no state can be reported yet. */
bwg_ctrl_error_t bwg_ctrl_write_status(const bwg_controller_t *controller, bwg_jw_t *writer, uint64_t now_ms);

/** The size of a success envelope around an empty string `padding`, given its `requestPaddingBytes`. */
size_t bwg_ctrl_probe_overhead(bwg_str_t request_id, uint64_t request_padding_bytes);

/** bwg_controller_tick, shared with the frame path. */
bwg_ctrl_error_t bwg_ctrl_enforce_clock(bwg_controller_t *controller, uint64_t now_ms);
/** Clears admission first, then stops any effect and its marker. On failure the reason stays pending for a retry. */
bwg_ctrl_error_t bwg_ctrl_safe_stop(bwg_controller_t *controller, bwg_reason_t reason, uint64_t now_ms);
/** Clears the marker if a confirmed status in this session already reported the reboot. */
bwg_ctrl_error_t bwg_ctrl_acknowledge_boot(bwg_controller_t *controller);

/** The binding of an admission under 60,000 ms old, or NULL. */
const char *bwg_ctrl_maybe_start_context(const bwg_controller_t *controller, uint64_t now_ms);

/** The binding of the current admission of any age, or NULL. */
const char *bwg_ctrl_maybe_active_context(const bwg_controller_t *controller);

/** Runs the injected verifier, or the deployment-trust check, and notes a rejection in the log. */
bwg_auth_result_t bwg_ctrl_verify_start(bwg_controller_t *controller, const bwg_lease_grant_t *grant,
                                        const char *binding);
bwg_auth_result_t bwg_ctrl_verify_renew(bwg_controller_t *controller, const bwg_lease_renewal_t *renewal,
                                        const char *challenge_id, const char *binding);
/** Logs a Start or Renew refused for its context, before any signature check. */
void bwg_ctrl_note_context_rejection(bwg_controller_t *controller, bwg_auth_operation_t operation);

/** The read-only restoration reviews, which take an empty payload. */
bool bwg_ctrl_is_review(bwg_str_t command);
bwg_ctrl_error_t bwg_ctrl_prepare_review(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                         bwg_ctrl_outcome_t *outcome);
/** Answers `acceptance_budget_review`, whose payload names the campaign. */
bwg_ctrl_error_t bwg_ctrl_prepare_campaign_review(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                                  uint64_t now_ms, bwg_ctrl_outcome_t *outcome);
/** Answers `qualification_cooling`: a lease-free fan effect with its own restoration. */
bwg_ctrl_error_t bwg_ctrl_prepare_restart(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                          bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_confirm_restart(bwg_controller_t *controller, uint64_t now_ms);
bwg_ctrl_error_t bwg_ctrl_prepare_core_dump_self_test(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                                      uint64_t now_ms, bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_confirm_core_dump_self_test(bwg_controller_t *controller, uint64_t now_ms);
bwg_ctrl_error_t bwg_ctrl_prepare_endpoint(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                           bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_prepare_trace(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                        bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_prepare_cadence_arm(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                              bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_prepare_cadence_review(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                                 bwg_ctrl_outcome_t *outcome);
/** Whether `command` is `onboarding_inspect` or `onboarding_verify`. */
bool bwg_ctrl_is_onboarding(bwg_str_t command);
/** Answers an onboarding command. Read-only and open to any session, as `discover` is. */
bwg_ctrl_error_t bwg_ctrl_prepare_onboarding(bwg_controller_t *controller, const bwg_ctrl_request_t *request,
                                             bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_prepare_cooling(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                          bwg_ctrl_outcome_t *outcome);
/** Answers `clock_discontinuity_stimulus`. It arms only once the reply was truly delivered. */
bwg_ctrl_error_t bwg_ctrl_prepare_stimulus(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                           bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_confirm_stimulus(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms);

/** Answers `stratum_v2_status`, `stratum_v2_channel_start` and `stratum_v2_channel_cancel`. */
bwg_ctrl_error_t bwg_ctrl_prepare_v2(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                     bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_confirm_v2_dispatch(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms);
/** Whether a V2 attempt or a Noise diagnostic still holds the network or its fence. */
bool bwg_ctrl_attempt_busy(const bwg_controller_t *controller);
/** Whether two observations were of the same boot, session and network, with a usable clock. */
bool bwg_ctrl_same_network(const bwg_v2_observation_t *current, const bwg_v2_observation_t *previous);
/** Whether an observation is recent enough to start an attempt from what was seen at `since_us`. */
bool bwg_ctrl_observation_is_fresh(const bwg_v2_observation_t *current, uint64_t since_us);
/** Answers `noise_diagnostic_status`, `noise_diagnostic_start` and `noise_diagnostic_cancel`. */
bwg_ctrl_error_t bwg_ctrl_prepare_noise(bwg_controller_t *controller, const bwg_ctrl_request_t *request, uint64_t now_ms,
                                        bwg_ctrl_outcome_t *outcome);
bwg_ctrl_error_t bwg_ctrl_confirm_noise_dispatch(bwg_controller_t *controller, const bwg_ctrl_effect_t *effect, uint64_t now_ms);
/** The idle status a share Start must follow. Consumes it. */
bwg_ctrl_error_t bwg_ctrl_validate_v2_share_observation(bwg_controller_t *controller);

#endif // BWG_CONTROLLER_INTERNAL_H
