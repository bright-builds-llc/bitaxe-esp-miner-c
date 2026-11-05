#include "bwg_v2_runtime.h"

#include <lwip/sockets.h>
#include <sodium.h>
#include <stdatomic.h>
#include <string.h>

#include "bwg_revocation.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define POOL_GENERATION 1u
#define POOL_EPOCH 1u
/** How long an attempt may wait on the pool before it has work. */
#define WAITING_LIMIT_US 10000000ull

static SemaphoreHandle_t s_lock;
static bwg_v2_record_t *s_record;
// Readable without the lock: the note paths ask this on every job and nonce.
static atomic_bool s_live;
static atomic_bool s_cancelled;
static bool s_admitted;
static uint64_t s_channel_generation;
static uint64_t s_waiting_since_us;
static bool s_worker_returned;

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void take(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

/** Lets go of the record, then has any failure end the attempt: the revocation path isn't called under this lock. */
static void give(void)
{
    const bool failed = bwg_v2_record_failed(s_record);
    const bool timeout = failed && s_record->first_failure.category == BWG_V2_FAILURE_TIMEOUT;
    const bool share = s_record->scope == BWG_V2_SCOPE_SHARE;
    if (s_record->state == BWG_V2_STATE_TERMINAL) {
        atomic_store(&s_live, false);
    }
    xSemaphoreGive(s_lock);
    if (!failed) {
        return;
    }
    atomic_store(&s_cancelled, true);
    if (share) {
        bwg_revocation_revoke_lease(timeout ? BWG_STOP_REASON_LEASE_EXPIRED : BWG_STOP_REASON_CONTROL_FAILED);
    }
}

bool bwg_v2_runtime_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_record = heap_caps_calloc(1, sizeof(*s_record), MALLOC_CAP_SPIRAM);
    return s_lock != NULL && s_record != NULL;
}

bool bwg_v2_runtime_admit(bwg_v2_scope_t scope, const char *attempt_id, const bwg_v2_observation_t *observation)
{
    if (s_record == NULL) {
        return false;
    }
    take();
    bool admitted = !s_admitted && bwg_v2_record_admit(s_record, scope, attempt_id, observation);
    if (admitted) {
        s_admitted = true;
        s_worker_returned = false;
        atomic_store(&s_cancelled, false);
        atomic_store(&s_live, true);
    }
    xSemaphoreGive(s_lock);
    return admitted;
}

void bwg_v2_runtime_bind_channel(uint64_t generation)
{
    s_channel_generation = generation;
}

bool bwg_v2_runtime_view(bwg_v2_scope_t scope, bwg_v2_view_t *view)
{
    if (s_record == NULL) {
        return false;
    }
    take();
    if (s_admitted && s_record->scope == scope) {
        bwg_v2_record_tick(s_record, now_us());
        view->has_record = true;
        view->state = s_record->state;
        strlcpy(view->attempt_id, s_record->attempt_id, sizeof(view->attempt_id));
    }
    if (s_admitted) {
        give();
    } else {
        xSemaphoreGive(s_lock);
    }
    return true;
}

void bwg_v2_runtime_write_status(bwg_v2_scope_t scope, const bwg_v2_observation_t *observation, bwg_jw_t *writer)
{
    take();
    bwg_v2_write_status(writer, scope, observation, s_admitted ? s_record : NULL);
    xSemaphoreGive(s_lock);
}

bool bwg_v2_runtime_busy(void)
{
    return atomic_load(&s_live);
}

void bwg_v2_runtime_cancel(void)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    take();
    bwg_v2_record_fail(s_record, BWG_V2_STAGE_REVOKED, BWG_V2_FAILURE_AUTHORITY, now_us());
    give();
}

bool bwg_v2_share_live(void)
{
    return atomic_load(&s_live) && s_record->scope == BWG_V2_SCOPE_SHARE;
}

/** The lease's ending as events: facts of when they were seen, not when they happened. */
static void note_safety_facts(const bwg_revocation_snapshot_t *snapshot)
{
    const struct {
        bool seen;
        bwg_v2_stage_t stage;
    } facts[] = {
        {snapshot->lease_closed_ms != 0, BWG_V2_STAGE_REVOKED},
        {snapshot->lease_stop_started_ms != 0, BWG_V2_STAGE_SHUTDOWN},
        {snapshot->lease_closed_ms != 0 && snapshot->state != BWG_AUTHORITY_STOPPING, BWG_V2_STAGE_COOLED},
    };
    for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); i++) {
        if (facts[i].seen && !bwg_v2_record_has_stage(s_record, facts[i].stage)) {
            bwg_v2_record_event(s_record, facts[i].stage, now_us(), BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
        }
    }
}

static void join_never_started(void)
{
    if (!bwg_v2_record_failed(s_record)) {
        bwg_v2_record_fail(s_record, BWG_V2_STAGE_PREPARING, BWG_V2_FAILURE_ADMISSION, now_us());
    }
    bwg_v2_record_socket_closed(s_record, now_us());
    bwg_v2_record_begin(s_record, BWG_V2_OP_WORKER_JOIN, now_us());
    bwg_v2_record_end(s_record, BWG_V2_OP_WORKER_JOIN, now_us(), false);
    bwg_v2_record_joined(s_record, now_us(), false);
    s_worker_returned = true;
}

static void poll_share(void)
{
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    note_safety_facts(&snapshot);
    const bool restored = snapshot.lease_closed_ms != 0 && snapshot.state != BWG_AUTHORITY_STOPPING;
    if (!restored) {
        return;
    }
    if (!s_worker_returned && s_record->pool_session_generation == BWG_V2_NONE) {
        join_never_started();
    }
    if (s_worker_returned) {
        bwg_v2_record_release_fence(s_record, now_us());
    }
}

void bwg_v2_runtime_poll(uint64_t generation, bool session_alive)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    take();
    bwg_v2_record_tick(s_record, now_us());
    if (s_record->scope == BWG_V2_SCOPE_SHARE) {
        poll_share();
    } else if (!session_alive || generation != s_channel_generation) {
        bwg_v2_record_fail(s_record, BWG_V2_STAGE_REVOKED, BWG_V2_FAILURE_AUTHORITY, now_us());
    }
    give();
}

void bwg_v2_runtime_never_started(void)
{
    if (!bwg_v2_share_live()) {
        return;
    }
    take();
    if (s_record->pool_session_generation == BWG_V2_NONE && !s_worker_returned) {
        join_never_started();
        bwg_v2_record_release_fence(s_record, now_us());
    }
    give();
}

bool bwg_v2_note_preparing(void)
{
    if (!atomic_load(&s_live)) {
        return false;
    }
    take();
    const bool bound = !bwg_v2_record_failed(s_record) && bwg_v2_record_bind_pool(s_record, POOL_GENERATION, POOL_EPOCH);
    if (bound) {
        bwg_v2_record_event(s_record, BWG_V2_STAGE_PREPARING, now_us(), BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
        s_waiting_since_us = now_us();
    }
    give();
    return bound;
}

void bwg_v2_note_begin(bwg_v2_operation_t operation)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    take();
    bwg_v2_record_begin(s_record, operation, now_us());
    give();
}

static bwg_v2_failure_category_t failure_of(bwg_v2_operation_t operation)
{
    switch (operation) {
    case BWG_V2_OP_CONNECT:
        return BWG_V2_FAILURE_TIMEOUT;
    case BWG_V2_OP_ACT_TWO_READ:
    case BWG_V2_OP_FRAME_READ:
    case BWG_V2_OP_FRAME_WRITE:
    case BWG_V2_OP_ACT_ONE_WRITE:
        return BWG_V2_FAILURE_EOF;
    case BWG_V2_OP_ACT_TWO_AUTHENTICATION:
        return BWG_V2_FAILURE_AUTHENTICATION;
    default:
        return BWG_V2_FAILURE_PROTOCOL;
    }
}

void bwg_v2_note_end(bwg_v2_operation_t operation, bool failed)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    // Leaving because authority closed is not the pool's failure: bwg_v2_note_worker_returned decides what it was.
    const bool leaving = atomic_load(&s_cancelled) || bwg_revocation_stop_wanted();
    take();
    bwg_v2_record_end(s_record, operation, now_us(), failed);
    if (failed && !leaving) {
        bwg_v2_record_fail(s_record, BWG_V2_STAGE_WORKER_QUIESCENT, failure_of(operation), now_us());
    }
    give();
}

static bool address_is_private(uint32_t host_order)
{
    return (host_order >> 24) == 10 || (host_order >> 20) == 0xac1 || (host_order >> 16) == 0xc0a8;
}

bool bwg_v2_note_connected(int socket_fd)
{
    if (!atomic_load(&s_live)) {
        return false;
    }
    struct sockaddr_in local = {0};
    struct sockaddr_in remote = {0};
    socklen_t local_len = sizeof(local);
    socklen_t remote_len = sizeof(remote);
    const bool read = getsockname(socket_fd, (struct sockaddr *)&local, &local_len) == 0 &&
                      getpeername(socket_fd, (struct sockaddr *)&remote, &remote_len) == 0 &&
                      local.sin_family == AF_INET && remote.sin_family == AF_INET;
    const bwg_v2_socket_t socket = {
        .local_ipv4 = ntohl(local.sin_addr.s_addr),
        .local_port = ntohs(local.sin_port),
        .remote_ipv4 = ntohl(remote.sin_addr.s_addr),
        .remote_port = ntohs(remote.sin_port),
    };
    take();
    bool connected = false;
    if (read && address_is_private(socket.local_ipv4) && address_is_private(socket.remote_ipv4)) {
        connected = bwg_v2_record_connected(s_record, &socket, now_us());
    } else {
        bwg_v2_record_fail(s_record, BWG_V2_STAGE_CONNECTED, BWG_V2_FAILURE_PROTOCOL, now_us());
    }
    give();
    return connected;
}

void bwg_v2_note_authenticated(void)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    take();
    bwg_v2_record_event(s_record, BWG_V2_STAGE_AUTHENTICATED, now_us(), BWG_V2_NONE, BWG_V2_NONE, BWG_V2_NONE, NULL);
    s_waiting_since_us = now_us();
    give();
}

bool bwg_v2_keep_going(void)
{
    if (!atomic_load(&s_live) || atomic_load(&s_cancelled)) {
        return false;
    }
    if (s_record->scope == BWG_V2_SCOPE_SHARE && bwg_revocation_stop_wanted()) {
        return false;
    }
    take();
    bwg_v2_record_tick(s_record, now_us());
    if (!bwg_v2_record_committed(s_record) && now_us() - s_waiting_since_us >= WAITING_LIMIT_US) {
        bwg_v2_record_fail(s_record, BWG_V2_STAGE_WORKER_QUIESCENT, BWG_V2_FAILURE_TIMEOUT, now_us());
    }
    const bool going = !bwg_v2_record_failed(s_record);
    give();
    return going;
}

bool bwg_v2_note_frame(const uint8_t header[6], const uint8_t *payload, int payload_len)
{
    if (!atomic_load(&s_live)) {
        return false;
    }
    take();
    const bool accepted = bwg_v2_record_frame(s_record, header, payload, (size_t)payload_len, now_us());
    s_waiting_since_us = now_us();
    give();
    return accepted;
}

static void hash_work(const asic_job_t *job, uint16_t slot, uint8_t digest[BWG_V2_DIGEST_BYTES])
{
    const bwg_v2_work_t work = {
        .version = job->version,
        .nbits = job->nbits,
        .ntime = job->ntime,
        .starting_nonce = job->starting_nonce,
        .merkle_root = job->merkle_root,
        .prev_hash = job->prev_hash,
    };
    bwg_v2_work_fields_digest(&work, slot, digest);
}

void bwg_v2_note_dispatch(uint16_t slot, const asic_job_t *job)
{
    if (job == NULL || !bwg_v2_share_live()) {
        return;
    }
    uint8_t digest[BWG_V2_DIGEST_BYTES];
    hash_work(job, slot, digest);
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    take();
    if (s_record->authority_deadline_us == BWG_V2_NONE) {
        bwg_v2_record_budget_armed(s_record, snapshot.first_work_ms, snapshot.active_limit_ms, now_us());
    }
    bwg_v2_record_dispatch(s_record, slot, digest, now_us());
    give();
}

void bwg_v2_note_nonce(const asic_job_t *job, uint16_t slot, uint32_t nonce, uint32_t rolled_version, uint8_t asic_index,
                       uint8_t core_id, uint8_t small_core_id)
{
    if (!bwg_v2_share_live()) {
        return;
    }
    uint8_t digest[BWG_V2_DIGEST_BYTES];
    hash_work(job, slot, digest);
    take();
    bwg_v2_record_nonce(s_record, slot, digest, nonce, rolled_version, rolled_version ^ job->version, asic_index, core_id,
                        small_core_id, now_us());
    give();
}

bool bwg_v2_note_submission_begin(uint32_t channel_id, uint32_t job_id, uint32_t sequence, uint32_t nonce, uint32_t ntime,
                                  uint32_t version)
{
    if (!bwg_v2_share_live()) {
        return true;
    }
    take();
    const bool allowed = bwg_v2_record_submission_begin(s_record, channel_id, job_id, sequence, nonce, ntime, version, now_us());
    give();
    return allowed;
}

void bwg_v2_note_submission_end(uint32_t sequence, bool written)
{
    if (!bwg_v2_share_live()) {
        return;
    }
    take();
    bwg_v2_record_submission_end(s_record, sequence, written, now_us());
    give();
}

void bwg_v2_note_socket_closed(void)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    take();
    if (!s_record->socket_closed) {
        bwg_v2_record_socket_closed(s_record, now_us());
    }
    give();
}

static bool heartbeat_ended_an_accepted_share(const bwg_revocation_snapshot_t *snapshot)
{
    return snapshot->reason == BWG_STOP_REASON_HEARTBEAT_TIMEOUT && bwg_v2_record_has_accepted_share(s_record);
}

void bwg_v2_note_worker_returned(void)
{
    if (!bwg_v2_share_live()) {
        return;
    }
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    const bool stopping = bwg_revocation_stop_wanted() || snapshot.lease_closed_ms != 0;
    take();
    if (s_worker_returned) {
        give();
        return;
    }
    if (!bwg_v2_record_failed(s_record) && !(stopping && heartbeat_ended_an_accepted_share(&snapshot))) {
        bwg_v2_record_fail(s_record, BWG_V2_STAGE_WORKER_QUIESCENT, stopping ? BWG_V2_FAILURE_AUTHORITY : BWG_V2_FAILURE_PROTOCOL,
                           now_us());
    }
    if (!s_record->socket_closed) {
        bwg_v2_record_socket_closed(s_record, now_us());
    }
    bwg_v2_record_begin(s_record, BWG_V2_OP_WORKER_JOIN, now_us());
    bwg_v2_record_end(s_record, BWG_V2_OP_WORKER_JOIN, now_us(), false);
    bwg_v2_record_joined(s_record, now_us(), false);
    s_worker_returned = true;
    give();
}

void bwg_v2_note_channel_returned(void)
{
    if (!atomic_load(&s_live)) {
        return;
    }
    take();
    if (!s_record->socket_closed) {
        bwg_v2_record_socket_closed(s_record, now_us());
    }
    bwg_v2_record_begin(s_record, BWG_V2_OP_WORKER_JOIN, now_us());
    bwg_v2_record_end(s_record, BWG_V2_OP_WORKER_JOIN, now_us(), false);
    bwg_v2_record_joined(s_record, now_us(), true);
    s_worker_returned = true;
    give();
}
