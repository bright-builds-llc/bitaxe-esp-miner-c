#ifndef BWG_NOISE_RECORD_H
#define BWG_NOISE_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_json_writer.h"
#include "bwg_v2_record.h"

/**
 * What one Noise diagnostic did: a private connection, an authenticated handshake and one reserved
 * proof frame. It carries no pool work. The record is what the host judges, so it keeps the first
 * failure, and only a joined worker may close it. It holds no lock and reads no clock.
 */

/** No value yet, written as JSON null. */
#define BWG_NOISE_NONE UINT64_MAX
#define BWG_NOISE_DIGEST_TEXT_SIZE 65u
#define BWG_NOISE_STAGE_COUNT 8u
/** Upper bound of a status document, so the reply can be reserved before anything is admitted. */
#define BWG_NOISE_STATUS_BYTES 4096u
#define BWG_NOISE_AUTHORITY_US 120000000ull
#define BWG_NOISE_TAIL_US 5000000ull

/** The longest each stage may take. */
#define BWG_NOISE_PREPARE_LIMIT_US 60000000ull
#define BWG_NOISE_CONNECT_LIMIT_US 5000000ull
#define BWG_NOISE_WRITE_LIMIT_US 2000000ull
#define BWG_NOISE_READ_LIMIT_US 10000000ull

typedef enum {
    BWG_NOISE_STATE_IDLE = 0,
    BWG_NOISE_STATE_ADMITTED,
    BWG_NOISE_STATE_RUNNING,
    BWG_NOISE_STATE_CANCELLING,
    BWG_NOISE_STATE_TERMINAL,
} bwg_noise_state_t;

typedef enum {
    BWG_NOISE_STAGE_NOISE_PREPARED = 0,
    BWG_NOISE_STAGE_TCP_CONNECTED,
    BWG_NOISE_STAGE_ACT_ONE_WRITTEN,
    BWG_NOISE_STAGE_ACT_TWO_RECEIVED,
    BWG_NOISE_STAGE_AUTHORITY_VERIFIED,
    BWG_NOISE_STAGE_PROOF_WRITTEN,
    BWG_NOISE_STAGE_SOCKET_CLOSED,
    BWG_NOISE_STAGE_WORKER_QUIESCENT,
    /** Where a failure can land besides a stage. */
    BWG_NOISE_AT_ADMISSION,
    BWG_NOISE_AT_FIXTURE_READY,
    BWG_NOISE_AT_CANDIDATE_INVENTORY,
    BWG_NOISE_AT_PROOF_RECEIVED,
    BWG_NOISE_AT_CLEANUP,
    BWG_NOISE_AT_EVIDENCE,
} bwg_noise_stage_t;

typedef enum {
    /** No failure: a failure slot holds this until one comes. */
    BWG_NOISE_CATEGORY_NONE = 0,
    BWG_NOISE_CATEGORY_PREPARATION,
    BWG_NOISE_CATEGORY_CONNECT,
    BWG_NOISE_CATEGORY_WRITE,
    BWG_NOISE_CATEGORY_READ,
    BWG_NOISE_CATEGORY_AUTHENTICATION,
    BWG_NOISE_CATEGORY_PROOF,
    BWG_NOISE_CATEGORY_AUTHORITY_LOST,
    BWG_NOISE_CATEGORY_CLOCK_INVALID,
    BWG_NOISE_CATEGORY_CLEANUP,
    BWG_NOISE_CATEGORY_EVIDENCE_INCOMPLETE,
} bwg_noise_category_t;

typedef enum {
    BWG_NOISE_DETAIL_TIMEOUT = 0,
    BWG_NOISE_DETAIL_EOF,
    BWG_NOISE_DETAIL_EXTRA,
    BWG_NOISE_DETAIL_MALFORMED,
    BWG_NOISE_DETAIL_IO,
    BWG_NOISE_DETAIL_WRONG_AUTHORITY,
    BWG_NOISE_DETAIL_CERTIFICATE_TIME,
    BWG_NOISE_DETAIL_SESSION_REPLACED,
    BWG_NOISE_DETAIL_CANCEL_REQUESTED,
    BWG_NOISE_DETAIL_CLOCK_DISCONTINUITY,
    BWG_NOISE_DETAIL_RESOURCE_UNRELEASED,
    BWG_NOISE_DETAIL_MISSING,
    BWG_NOISE_DETAIL_DUPLICATE,
    BWG_NOISE_DETAIL_RNG,
    BWG_NOISE_DETAIL_ALLOCATION,
} bwg_noise_detail_t;

typedef enum {
    /** Not yet terminal. */
    BWG_NOISE_OUTCOME_NONE = 0,
    BWG_NOISE_OUTCOME_ACCEPTED,
    BWG_NOISE_OUTCOME_REJECTED,
    BWG_NOISE_OUTCOME_CANCELLED,
    BWG_NOISE_OUTCOME_EXPIRED,
    BWG_NOISE_OUTCOME_INCOMPLETE,
} bwg_noise_outcome_t;

/** Whether cleanup finished within the attempt's tail, known once the worker returns or cleanup gives up. */
typedef enum {
    BWG_NOISE_DEADLINE_UNKNOWN = 0,
    BWG_NOISE_DEADLINE_MET,
    BWG_NOISE_DEADLINE_MISSED,
} bwg_noise_deadline_t;

typedef enum {
    BWG_NOISE_SOCKET_NOT_CREATED = 0,
    BWG_NOISE_SOCKET_OPEN,
    BWG_NOISE_SOCKET_CLOSED,
} bwg_noise_socket_t;

typedef struct {
    bwg_noise_stage_t stage;
    bwg_noise_category_t category;
    bwg_noise_detail_t detail;
    uint64_t at_us;
} bwg_noise_failure_t;

typedef struct {
    bwg_noise_stage_t stage;
    uint64_t at_us;
    uint64_t duration_us;
    /** Zero when the stage moves no bytes. */
    uint16_t bytes;
} bwg_noise_stage_row_t;

typedef struct {
    bwg_noise_state_t state;
    char attempt_id[BWG_V2_ATTEMPT_ID_SIZE];
    char input_sha256[BWG_NOISE_DIGEST_TEXT_SIZE];
    uint64_t boot_ordinal;
    uint64_t worker_generation;
    uint64_t transport_epoch;
    uint64_t admitted_at_us;
    uint64_t authority_deadline_us;
    uint64_t local_socket_port;
    uint32_t stage_count;
    bwg_noise_stage_row_t stages[BWG_NOISE_STAGE_COUNT];
    /** Its category is BWG_NOISE_CATEGORY_NONE until the first failure. */
    bwg_noise_failure_t first_failure;
    /** BWG_NOISE_OUTCOME_NONE until the record is terminal. */
    bwg_noise_outcome_t outcome;
    uint64_t decided_at_us;
    bwg_noise_socket_t socket;
    bool worker_quiescent;
    bool volatile_inputs_disposed;
    uint64_t cleanup_started_at_us;
    uint64_t cleanup_deadline_at_us;
    uint64_t released_at_us;
    bwg_noise_deadline_t deadline_met;
    /** Its category is BWG_NOISE_CATEGORY_NONE unless cleanup itself failed. */
    bwg_noise_failure_t resource_failure;
    uint64_t last_us;
    bool clock_valid;
} bwg_noise_record_t;

/** Begins the boot's one attempt, already holding its worker. False when the deadline can't be represented. */
bool bwg_noise_record_admit(bwg_noise_record_t *record, const char *attempt_id, const char *input_sha256,
                            const bwg_v2_observation_t *observation);
/** Whether the attempt may keep touching the network. */
bool bwg_noise_record_permitted(const bwg_noise_record_t *record);
/** Admitted to running. False when the attempt has already failed. */
bool bwg_noise_record_dispatch(bwg_noise_record_t *record, uint64_t now_us);
/** Keeps the first failure, and starts cleanup at `at_us`, which may be BWG_NOISE_NONE. */
void bwg_noise_record_fail(bwg_noise_record_t *record, bwg_noise_stage_t stage, bwg_noise_category_t category,
                           bwg_noise_detail_t detail, uint64_t at_us);
void bwg_noise_record_cleanup(bwg_noise_record_t *record, uint64_t now_us);
/** Applies the deadlines and the clock. A terminal record no longer changes through it. */
void bwg_noise_record_tick(bwg_noise_record_t *record, uint64_t now_us);
/** Adds a stage in its fixed order. False, and failed, when it can't be added. */
bool bwg_noise_record_stage(bwg_noise_record_t *record, bwg_noise_stage_t stage, uint64_t now_us, uint64_t duration_us, uint16_t bytes);
void bwg_noise_record_socket_opened(bwg_noise_record_t *record, uint16_t port);
void bwg_noise_record_socket_closed(bwg_noise_record_t *record, uint64_t now_us, uint64_t duration_us);
/** The worker has returned: everything it held is gone, so the record can reach its outcome. */
void bwg_noise_record_joined(bwg_noise_record_t *record, uint64_t now_us);

/** Writes `worker-noise-diagnostic-status-v2`. NULL is an idle device. */
void bwg_noise_write_status(bwg_jw_t *writer, const bwg_v2_observation_t *observation, const bwg_noise_record_t *maybe_record);

#endif // BWG_NOISE_RECORD_H
