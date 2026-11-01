#ifndef BWG_V2_RUNTIME_H
#define BWG_V2_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "asic_job.h"
#include "bwg_controller.h"
#include "bwg_v2_record.h"

/**
 * This boot's one Stratum V2 attempt and its evidence, shared by the tasks that touch it. A failure in
 * any of them cancels the attempt and, for a share attempt, closes the lease. Without an attempt
 * every note does nothing, so upstream's own V2 mining is unaffected.
 */

/** Reserves the record. False leaves V2 attempts refused. */
bool bwg_v2_runtime_init(void);

/** Starts the attempt. False when this boot already had one: its record stays readable. */
bool bwg_v2_runtime_admit(bwg_v2_scope_t scope, const char *attempt_id, const bwg_v2_observation_t *observation);

/** Applies the deadlines, then fills what the record adds to `view->observation`. */
bool bwg_v2_runtime_view(bwg_v2_scope_t scope, bwg_v2_view_t *view);
void bwg_v2_runtime_write_status(bwg_v2_scope_t scope, const bwg_v2_observation_t *observation, bwg_jw_t *writer);

/** Whether the attempt still holds the network or its fence. */
bool bwg_v2_runtime_busy(void);
void bwg_v2_runtime_cancel(void);

/** The owner's tick: deadlines, the session a channel depends on, and what a stop has done so far. */
void bwg_v2_runtime_poll(uint64_t generation, bool session_alive);

/** A share attempt whose lease ended before it reached the pool. */
void bwg_v2_runtime_never_started(void);

/** Binds a channel attempt to its session. */
void bwg_v2_runtime_bind_channel(uint64_t generation);

/** Whether a share attempt is open, which is what upstream's V2 client asks before noting anything. */
bool bwg_v2_share_live(void);

/** Before connecting. False: this attempt already used its one connection, or has ended. */
bool bwg_v2_note_preparing(void);
void bwg_v2_note_begin(bwg_v2_operation_t operation);
void bwg_v2_note_end(bwg_v2_operation_t operation, bool failed);
/** Takes the socket's addresses. False refuses it: not private, or a second connection. */
bool bwg_v2_note_connected(int socket_fd);
void bwg_v2_note_authenticated(void);
/** Between waits on the pool. False means leave. */
bool bwg_v2_keep_going(void);
/** One authenticated frame. False means leave. */
bool bwg_v2_note_frame(const uint8_t header[6], const uint8_t *payload, int payload_len);
void bwg_v2_note_dispatch(uint16_t slot, const asic_job_t *job);
void bwg_v2_note_nonce(const asic_job_t *job, uint16_t slot, uint32_t nonce, uint32_t rolled_version, uint8_t asic_index,
                       uint8_t core_id, uint8_t small_core_id);
/** Before writing a submit. False: it must not be sent. */
bool bwg_v2_note_submission_begin(uint32_t channel_id, uint32_t job_id, uint32_t sequence, uint32_t nonce, uint32_t ntime,
                                  uint32_t version);
void bwg_v2_note_submission_end(uint32_t sequence, bool written);
void bwg_v2_note_socket_closed(void);
/** Upstream's V2 client returned for a share attempt. */
void bwg_v2_note_worker_returned(void);
/** The channel task is done: nothing of the attempt is left running. */
void bwg_v2_note_channel_returned(void);

#endif // BWG_V2_RUNTIME_H
