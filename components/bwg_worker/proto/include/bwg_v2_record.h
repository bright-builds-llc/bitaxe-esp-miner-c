#ifndef BWG_V2_RECORD_H
#define BWG_V2_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_json_writer.h"

#define BWG_V2_MAX_EVENTS 64u
#define BWG_V2_MAX_SHARES 16u
#define BWG_V2_MAX_FAILURES 16u
#define BWG_V2_MAX_DISPATCHES 16u
/** A BM13xx work payload names its slot as this multiple. */
#define BWG_V2_ASIC_JOB_ID_STEP 8u
#define BWG_V2_AUTHORITY_US 120000000ull
#define BWG_V2_OBSERVATION_US 125000000ull
#define BWG_V2_SHARE_LIMIT_MS 180000u
/** A share's only accepted ACK, one submit at the fixed difficulty. */
#define BWG_V2_ACK_SHARES_SUM 1024u
#define BWG_V2_EXTENSION_CHANNEL 0x8000u
/** An absent time or identifier, which the record writes as null. */
#define BWG_V2_NONE UINT64_MAX
#define BWG_V2_DIGEST_BYTES 32u
/** A 16-byte attempt identifier as base64url, with its NUL. */
#define BWG_V2_ATTEMPT_ID_SIZE 23u
#define BWG_V2_IPV4_TEXT_SIZE 16u
/** The largest status reply, which the Gate bounds at 64 KiB. */
#define BWG_V2_STATUS_BYTES 40960u

typedef enum { BWG_V2_SCOPE_CHANNEL = 0, BWG_V2_SCOPE_SHARE } bwg_v2_scope_t;

typedef enum { BWG_V2_STATE_IDLE = 0, BWG_V2_STATE_ADMITTED, BWG_V2_STATE_RUNNING, BWG_V2_STATE_TERMINAL } bwg_v2_state_t;

typedef enum {
    BWG_V2_OUTCOME_NONE = 0,
    BWG_V2_OUTCOME_ACCEPTED,
    BWG_V2_OUTCOME_REJECTED,
    BWG_V2_OUTCOME_EXPIRED,
    BWG_V2_OUTCOME_CANCELLED,
    BWG_V2_OUTCOME_INCOMPLETE,
} bwg_v2_outcome_t;

typedef enum {
    BWG_V2_STAGE_ADMITTED = 0,
    BWG_V2_STAGE_PREPARING,
    BWG_V2_STAGE_CONNECTED,
    BWG_V2_STAGE_AUTHENTICATED,
    BWG_V2_STAGE_SETUP,
    BWG_V2_STAGE_CHANNEL,
    BWG_V2_STAGE_JOB,
    BWG_V2_STAGE_TARGET,
    BWG_V2_STAGE_WORK_READY,
    BWG_V2_STAGE_SOCKET_CLOSED,
    BWG_V2_STAGE_WORKER_QUIESCENT,
    BWG_V2_STAGE_ASIC_DISPATCH,
    BWG_V2_STAGE_NONCE,
    BWG_V2_STAGE_SUBMISSION,
    BWG_V2_STAGE_ACCEPTED,
    BWG_V2_STAGE_REVOKED,
    BWG_V2_STAGE_SHUTDOWN,
    BWG_V2_STAGE_COOLED,
} bwg_v2_stage_t;

typedef enum {
    BWG_V2_FAILURE_ADMISSION = 0,
    BWG_V2_FAILURE_CLOCK,
    BWG_V2_FAILURE_ALLOCATION,
    BWG_V2_FAILURE_AUTHORITY,
    BWG_V2_FAILURE_TIMEOUT,
    BWG_V2_FAILURE_EOF,
    BWG_V2_FAILURE_EXTRA,
    BWG_V2_FAILURE_AUTHENTICATION,
    BWG_V2_FAILURE_PROTOCOL,
    BWG_V2_FAILURE_CHANNEL_MISMATCH,
    BWG_V2_FAILURE_JOB_MISMATCH,
    BWG_V2_FAILURE_INVALID_NONCE,
    BWG_V2_FAILURE_REJECTED_SHARE,
    BWG_V2_FAILURE_SAFETY,
    BWG_V2_FAILURE_CLEANUP,
    BWG_V2_FAILURE_EVIDENCE,
} bwg_v2_failure_category_t;

typedef enum {
    BWG_V2_OP_INITIATOR_CONSTRUCTION = 0,
    BWG_V2_OP_ACT_ONE_CONSTRUCTION,
    BWG_V2_OP_CONNECT,
    BWG_V2_OP_ACT_ONE_WRITE,
    BWG_V2_OP_ACT_TWO_READ,
    BWG_V2_OP_ACT_TWO_AUTHENTICATION,
    BWG_V2_OP_FRAME_ENCRYPT,
    BWG_V2_OP_FRAME_WRITE,
    BWG_V2_OP_FRAME_READ,
    BWG_V2_OP_HEADER_DECRYPT,
    BWG_V2_OP_PAYLOAD_DECRYPT,
    BWG_V2_OP_SOCKET_CLOSE,
    BWG_V2_OP_WORKER_JOIN,
    BWG_V2_OP_COUNT,
} bwg_v2_operation_t;

typedef struct {
    uint32_t local_ipv4;
    uint16_t local_port;
    uint32_t remote_ipv4;
    uint16_t remote_port;
} bwg_v2_socket_t;

/** What the device sees right now. Never journaled: it holds private addresses. */
typedef struct {
    uint64_t boot_ordinal;
    uint64_t worker_generation;
    uint64_t serial_transport_epoch;
    uint64_t observed_at_us;
    bool clock_valid;
    bool wifi_connected;
    /** Empty means no address. */
    char station_ipv4[BWG_V2_IPV4_TEXT_SIZE];
} bwg_v2_observation_t;

typedef struct {
    uint64_t sequence;
    uint64_t at_us;
    bwg_v2_stage_t kind;
    uint64_t channel_id;
    uint64_t job_id;
    uint64_t submission_sequence;
    bool has_digest;
    uint8_t digest[BWG_V2_DIGEST_BYTES];
} bwg_v2_event_t;

typedef struct {
    uint64_t count;
    uint64_t failed_count;
    uint64_t max_duration_us;
    uint64_t total_duration_us;
    uint64_t first_started_at_us;
    uint64_t last_finished_at_us;
    uint64_t in_flight_started_at_us;
} bwg_v2_timing_t;

typedef struct {
    uint64_t dispatch_sequence;
    uint8_t asic_job_id;
    uint8_t work_fields_sha256[BWG_V2_DIGEST_BYTES];
    uint64_t dispatched_at_us;
    uint64_t nonce_at_us;
    uint64_t write_started_at_us;
    uint64_t write_completed_at_us;
    uint32_t nonce;
    uint32_t version_bits;
    uint8_t asic_index;
    uint8_t core_id;
    uint8_t small_core_id;
    uint32_t channel_id;
    uint32_t job_id;
    uint32_t submission_sequence;
    uint32_t ntime;
    uint32_t version;
    uint64_t ack_at_us;
    uint32_t ack_last_sequence;
    uint32_t ack_accepted_count;
    uint64_t ack_shares_sum;
} bwg_v2_share_t;

typedef struct {
    bwg_v2_stage_t stage;
    bwg_v2_failure_category_t category;
    uint64_t at_us;
} bwg_v2_failure_t;

/** One ASIC work dispatch, held so a later nonce can name what it answered. */
typedef struct {
    bool valid;
    uint64_t sequence;
    uint64_t at_us;
    uint8_t asic_job_id;
    uint8_t digest[BWG_V2_DIGEST_BYTES];
} bwg_v2_dispatch_t;

/** A qualifying nonce on its way to a submission. */
typedef struct {
    bool valid;
    uint64_t at_us;
    const bwg_v2_dispatch_t *dispatch;
    uint32_t nonce;
    uint32_t rolled_version;
    uint32_t version_bits;
    uint8_t asic_index;
    uint8_t core_id;
    uint8_t small_core_id;
} bwg_v2_pending_nonce_t;

/**
 * The bounded facts of one attempt, reserved whole so that nothing can fail to allocate while it runs.
 * Not thread safe: bwg_v2_runtime holds the only instance behind a lock.
 */
typedef struct {
    bwg_v2_scope_t scope;
    char attempt_id[BWG_V2_ATTEMPT_ID_SIZE];
    uint64_t boot_ordinal;
    uint64_t worker_generation;
    uint64_t serial_transport_epoch;
    uint64_t pool_session_generation;
    uint64_t pool_transport_epoch;
    bool has_job_commitment;
    uint8_t job_commitment[BWG_V2_DIGEST_BYTES];
    uint64_t observed_at_us;
    uint64_t last_time_us;
    bwg_v2_state_t state;
    uint64_t admitted_at_us;
    uint64_t authority_deadline_us;
    uint64_t observation_deadline_us;
    uint64_t terminal_at_us;
    bwg_v2_outcome_t outcome;
    uint32_t event_count;
    bwg_v2_event_t events[BWG_V2_MAX_EVENTS];
    bwg_v2_timing_t timings[BWG_V2_OP_COUNT];
    uint32_t share_count;
    bwg_v2_share_t shares[BWG_V2_MAX_SHARES];
    bool has_first_failure;
    bwg_v2_failure_t first_failure;
    uint32_t secondary_count;
    bwg_v2_failure_t secondary[BWG_V2_MAX_FAILURES];
    bool socket_closed;
    bool worker_quiescent;
    bool fence_retained;
    uint64_t socket_closed_at_us;
    uint64_t worker_quiescent_at_us;
    bool has_connection;
    uint64_t connection_at_us;
    bwg_v2_socket_t socket;
    bwg_v2_dispatch_t dispatches[BWG_V2_MAX_DISPATCHES];
    uint64_t dispatch_count;
    bwg_v2_pending_nonce_t pending_nonce;
    /** Which of the channel, job, target and prev-hash frames arrived, in that order. */
    uint32_t frame_phase;
    uint8_t commitment_state[128];
    bwg_v2_stage_t last_frame_stage;
} bwg_v2_record_t;

/**
 * Starts a record at the observation's time. False when the clock isn't valid, and for a channel
 * attempt a deadline other than the observed time plus 120 s: nothing is then promised.
 */
bool bwg_v2_record_admit(bwg_v2_record_t *record, bwg_v2_scope_t scope, const char *attempt_id,
                         const bwg_v2_observation_t *observation);

/** Records the first dispatch's budget clock, which is a share attempt's only deadline. */
bool bwg_v2_record_budget_armed(bwg_v2_record_t *record, uint64_t arming_epoch_ms, uint32_t limit_ms, uint64_t now_us);

/** Binds the record to its one pool session. False once bound or finished. */
bool bwg_v2_record_bind_pool(bwg_v2_record_t *record, uint64_t generation, uint64_t epoch);

/** Keeps the first cause. Later failures stay only if they are cleanup or clock problems of their own. */
void bwg_v2_record_fail(bwg_v2_record_t *record, bwg_v2_stage_t stage, bwg_v2_failure_category_t category, uint64_t at_us);

/** Appends an event. Identifiers and `maybe_digest` are absent as BWG_V2_NONE and NULL. */
void bwg_v2_record_event(bwg_v2_record_t *record, bwg_v2_stage_t kind, uint64_t now_us, uint64_t channel_id, uint64_t job_id,
                         uint64_t submission_sequence, const uint8_t *maybe_digest);

/** Times an operation. A begin while one is in flight is an evidence failure. */
void bwg_v2_record_begin(bwg_v2_record_t *record, bwg_v2_operation_t operation, uint64_t now_us);
void bwg_v2_record_end(bwg_v2_record_t *record, bwg_v2_operation_t operation, uint64_t now_us, bool failed);

/** Keeps the one socket the attempt may use. False, with `extra`, for a second connection. */
bool bwg_v2_record_connected(bwg_v2_record_t *record, const bwg_v2_socket_t *socket, uint64_t now_us);

/**
 * Feeds one authenticated frame from the pool: header and payload exactly as they crossed the wire.
 * Records the setup, channel, job and target stages and the job commitment over the four job frames.
 * An ACK settles at most one completed write. False means the attempt must end.
 */
bool bwg_v2_record_frame(bwg_v2_record_t *record, const uint8_t header[6], const uint8_t *payload, size_t payload_len,
                         uint64_t now_us);

/** The work a dispatch sends, in header byte order. */
typedef struct {
    uint32_t version;
    uint32_t nbits;
    uint32_t ntime;
    uint32_t starting_nonce;
    const uint8_t *merkle_root;
    const uint8_t *prev_hash;
} bwg_v2_work_t;

/** The digest of the 82-byte BM13xx work payload for `slot`, which is what ties a nonce to work the fixture can check. */
void bwg_v2_work_fields_digest(const bwg_v2_work_t *work, uint16_t slot, uint8_t digest[BWG_V2_DIGEST_BYTES]);

/** Notes a dispatch to ASIC job slot `slot`. Share scope only. */
void bwg_v2_record_dispatch(bwg_v2_record_t *record, uint16_t slot, const uint8_t digest[BWG_V2_DIGEST_BYTES], uint64_t now_us);

/**
 * Notes a qualifying nonce answering job slot `slot`, whose work must still hash to `digest`. A slot since reused for
 * other work, or never dispatched, is ignored.
 */
void bwg_v2_record_nonce(bwg_v2_record_t *record, uint16_t slot, const uint8_t digest[BWG_V2_DIGEST_BYTES], uint32_t nonce,
                         uint32_t rolled_version, uint32_t version_bits, uint8_t asic_index, uint8_t core_id,
                         uint8_t small_core_id, uint64_t now_us);

/** Joins the pending nonce to its submit, before the write. False: this submit must not be sent. */
bool bwg_v2_record_submission_begin(bwg_v2_record_t *record, uint32_t channel_id, uint32_t job_id, uint32_t sequence, uint32_t nonce,
                                    uint32_t ntime, uint32_t version, uint64_t now_us);
void bwg_v2_record_submission_end(bwg_v2_record_t *record, uint32_t sequence, bool written, uint64_t now_us);

void bwg_v2_record_socket_closed(bwg_v2_record_t *record, uint64_t now_us);

/** The worker returned. With `fence_released` the attempt finishes; otherwise bwg_v2_record_release_fence does. */
void bwg_v2_record_joined(bwg_v2_record_t *record, uint64_t now_us, bool fence_released);
bool bwg_v2_record_release_fence(bwg_v2_record_t *record, uint64_t now_us);

/** Applies the deadlines at `now_us`. */
void bwg_v2_record_tick(bwg_v2_record_t *record, uint64_t now_us);

bool bwg_v2_record_has_stage(const bwg_v2_record_t *record, bwg_v2_stage_t stage);
bool bwg_v2_record_has_accepted_share(const bwg_v2_record_t *record);

/** Writes the record. `worker-v2-serial-evidence-v1`: the only part fit to persist. */
void bwg_v2_write_record(bwg_jw_t *writer, const bwg_v2_record_t *record);

/** Writes a `worker-stratum-v2-status-v1`. `maybe_record` is NULL or of a different scope for idle. */
void bwg_v2_write_status(bwg_jw_t *writer, bwg_v2_scope_t scope, const bwg_v2_observation_t *observation,
                         const bwg_v2_record_t *maybe_record);

#endif // BWG_V2_RECORD_H
