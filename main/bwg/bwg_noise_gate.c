#include "bwg_noise_gate.h"

#include <errno.h>
#include <lwip/sockets.h>
#include <stdatomic.h>
#include <string.h>

#include "bwg_codec.h"
#include "bwg_evidence.h"
#include "bwg_gate.h"
#include "bwg_mining.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sv2_noise.h"

#define WORKER_STACK_BYTES 16384
#define WORKER_TASK_PRIORITY 16
#define CONNECT_TIMEOUT_MS 5000
#define HOST_BYTES 16u
#define AUTHORITY_KEY_BYTES 32u
#define ACT_ONE_BYTES 64u
#define ACT_TWO_BYTES 234u
#define PROOF_WIRE_BYTES 22u

static const char *TAG = "bwg_noise";

static GlobalState *s_global_state;
static SemaphoreHandle_t s_lock;
static bwg_noise_record_t *s_record;
static bool s_admitted;
static bool s_dispatched;
static uint64_t s_generation;
// Readable without the lock: the worker asks between short waits on the fixture.
static atomic_bool s_busy;
static atomic_bool s_permitted;

// Set before the worker starts, and read only by it.
static char s_host[HOST_BYTES];
static uint16_t s_port;
static uint8_t s_authority_key[AUTHORITY_KEY_BYTES];
static uint64_t s_open_op_started_us;
// What Noise says about a failed operation, read by the observer.
static sv2_noise_ctx_t *s_noise;

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void take(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void give(void)
{
    atomic_store(&s_permitted, bwg_noise_record_permitted(s_record));
    atomic_store(&s_busy, s_admitted && !s_record->worker_quiescent);
    xSemaphoreGive(s_lock);
}

static void fail(bwg_noise_stage_t stage, bwg_noise_category_t category, bwg_noise_detail_t detail)
{
    take();
    bwg_noise_record_fail(s_record, stage, category, detail, now_us());
    give();
}

/** Adds a finished stage, unless it took too long or the attempt has already ended. */
static bool complete(bwg_noise_stage_t stage, bwg_noise_category_t category, uint64_t started_us, uint64_t limit_us, uint16_t bytes)
{
    const uint64_t at_us = now_us();
    take();
    bwg_noise_record_tick(s_record, at_us);
    bool added;
    if (at_us - started_us > limit_us) {
        bwg_noise_record_fail(s_record, stage, category, BWG_NOISE_DETAIL_TIMEOUT, at_us);
        added = false;
    } else {
        added = bwg_noise_record_stage(s_record, stage, at_us, at_us - started_us, bytes);
    }
    added = added && bwg_noise_record_permitted(s_record);
    give();
    return added;
}

static bwg_noise_detail_t detail_of_failure(void)
{
    switch (sv2_noise_last_failure(s_noise)) {
    case SV2_NOISE_FAILURE_TIMEOUT:
        return BWG_NOISE_DETAIL_TIMEOUT;
    case SV2_NOISE_FAILURE_EOF:
        return BWG_NOISE_DETAIL_EOF;
    case SV2_NOISE_FAILURE_IO:
        return BWG_NOISE_DETAIL_IO;
    case SV2_NOISE_FAILURE_CERTIFICATE_TIME:
        return BWG_NOISE_DETAIL_CERTIFICATE_TIME;
    case SV2_NOISE_FAILURE_WRONG_AUTHORITY:
        return BWG_NOISE_DETAIL_WRONG_AUTHORITY;
    default:
        return BWG_NOISE_DETAIL_MALFORMED;
    }
}

static void observer_begin(sv2_noise_op_t op)
{
    s_open_op_started_us = now_us();
}

static void observer_end(sv2_noise_op_t op, bool failed)
{
    switch (op) {
    case SV2_NOISE_OP_ACT_ONE_CONSTRUCTION:
        if (failed) {
            fail(BWG_NOISE_STAGE_ACT_ONE_WRITTEN, BWG_NOISE_CATEGORY_PREPARATION, BWG_NOISE_DETAIL_RNG);
        }
        break;
    case SV2_NOISE_OP_ACT_ONE_WRITE:
        if (failed) {
            fail(BWG_NOISE_STAGE_ACT_ONE_WRITTEN, BWG_NOISE_CATEGORY_WRITE, BWG_NOISE_DETAIL_IO);
        } else {
            complete(BWG_NOISE_STAGE_ACT_ONE_WRITTEN, BWG_NOISE_CATEGORY_WRITE, s_open_op_started_us, BWG_NOISE_WRITE_LIMIT_US, ACT_ONE_BYTES);
        }
        break;
    case SV2_NOISE_OP_ACT_TWO_READ:
        if (failed) {
            fail(BWG_NOISE_STAGE_ACT_TWO_RECEIVED, BWG_NOISE_CATEGORY_READ, detail_of_failure());
        } else {
            complete(BWG_NOISE_STAGE_ACT_TWO_RECEIVED, BWG_NOISE_CATEGORY_READ, s_open_op_started_us, BWG_NOISE_READ_LIMIT_US, ACT_TWO_BYTES);
        }
        break;
    case SV2_NOISE_OP_ACT_TWO_AUTHENTICATION:
        if (failed) {
            fail(BWG_NOISE_STAGE_AUTHORITY_VERIFIED, BWG_NOISE_CATEGORY_AUTHENTICATION, detail_of_failure());
        } else {
            complete(BWG_NOISE_STAGE_AUTHORITY_VERIFIED, BWG_NOISE_CATEGORY_AUTHENTICATION, s_open_op_started_us, BWG_NOISE_AUTHORITY_US, 0);
        }
        break;
    case SV2_NOISE_OP_FRAME_ENCRYPT:
        if (failed) {
            fail(BWG_NOISE_STAGE_PROOF_WRITTEN, BWG_NOISE_CATEGORY_PROOF, BWG_NOISE_DETAIL_IO);
        }
        break;
    case SV2_NOISE_OP_FRAME_WRITE:
        if (failed) {
            fail(BWG_NOISE_STAGE_PROOF_WRITTEN, BWG_NOISE_CATEGORY_PROOF, BWG_NOISE_DETAIL_IO);
        } else {
            complete(BWG_NOISE_STAGE_PROOF_WRITTEN, BWG_NOISE_CATEGORY_WRITE, s_open_op_started_us, BWG_NOISE_WRITE_LIMIT_US, PROOF_WIRE_BYTES);
        }
        break;
    default:
        break;
    }
}

static bool observer_keep_going(void)
{
    if (atomic_load(&s_permitted)) {
        take();
        bwg_noise_record_tick(s_record, now_us());
        give();
    }
    return atomic_load(&s_permitted);
}

static bool observer_frame(const uint8_t header[6], const uint8_t *payload, int payload_len)
{
    return false;
}

static const sv2_noise_observer_t OBSERVER = {
    .begin = observer_begin,
    .end = observer_end,
    .keep_going = observer_keep_going,
    .frame = observer_frame,
};

/** Bytes already waiting are a peer doing more than the exchange allows. This says nothing of bytes yet to come. */
static bool no_buffered_bytes(int fd)
{
    uint8_t byte;
    const int count = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (count < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        return true;
    }
    fail(BWG_NOISE_STAGE_ACT_TWO_RECEIVED, BWG_NOISE_CATEGORY_READ,
         count > 0 ? BWG_NOISE_DETAIL_EXTRA : count == 0 ? BWG_NOISE_DETAIL_EOF : BWG_NOISE_DETAIL_IO);
    return false;
}

/** A frame with no payload on an extension and message type no pool assigns: the fixture requires exactly this. */
static void send_proof(esp_transport_handle_t transport)
{
    static const uint8_t PROOF[] = {0xff, 0xff, 0xff, 0x00, 0x00, 0x00};
    if (sv2_noise_send(s_noise, transport, PROOF, sizeof(PROOF)) != 0) {
        fail(BWG_NOISE_STAGE_PROOF_WRITTEN, BWG_NOISE_CATEGORY_PROOF, BWG_NOISE_DETAIL_IO);
    }
}

static bool connect_fixture(esp_transport_handle_t transport)
{
    const uint64_t started_us = now_us();
    if (esp_transport_connect(transport, s_host, s_port, CONNECT_TIMEOUT_MS) != ESP_OK) {
        fail(BWG_NOISE_STAGE_TCP_CONNECTED, BWG_NOISE_CATEGORY_CONNECT, BWG_NOISE_DETAIL_IO);
        return false;
    }
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    if (getsockname(esp_transport_get_socket(transport), (struct sockaddr *)&local, &length) != 0) {
        fail(BWG_NOISE_STAGE_TCP_CONNECTED, BWG_NOISE_CATEGORY_CONNECT, BWG_NOISE_DETAIL_IO);
        return false;
    }
    take();
    bwg_noise_record_socket_opened(s_record, ntohs(local.sin_port));
    give();
    return complete(BWG_NOISE_STAGE_TCP_CONNECTED, BWG_NOISE_CATEGORY_CONNECT, started_us, BWG_NOISE_CONNECT_LIMIT_US, 0);
}

static void exchange(esp_transport_handle_t transport)
{
    if (!connect_fixture(transport)) {
        return;
    }
    const int fd = esp_transport_get_socket(transport);
    if (sv2_noise_handshake(s_noise, transport, s_authority_key) != 0) {
        // The observer named the operation that failed, and the first failure stays: this covers one that had none.
        fail(BWG_NOISE_STAGE_AUTHORITY_VERIFIED, BWG_NOISE_CATEGORY_AUTHENTICATION, BWG_NOISE_DETAIL_MALFORMED);
        return;
    }
    if (atomic_load(&s_permitted) && no_buffered_bytes(fd)) {
        send_proof(transport);
    }
}

static void note_closed(uint64_t started_us)
{
    const uint64_t at_us = now_us();
    take();
    if (s_record->socket == BWG_NOISE_SOCKET_OPEN) {
        bwg_noise_record_socket_closed(s_record, at_us, at_us - started_us);
    }
    give();
}

static void note_cleanup(void)
{
    take();
    bwg_noise_record_cleanup(s_record, now_us());
    give();
}

static void worker_run(void)
{
    const uint64_t started_us = now_us();
    s_noise = sv2_noise_create();
    esp_transport_handle_t transport = esp_transport_tcp_init();
    if (s_noise == NULL || transport == NULL) {
        fail(BWG_NOISE_STAGE_NOISE_PREPARED, BWG_NOISE_CATEGORY_PREPARATION, BWG_NOISE_DETAIL_ALLOCATION);
    } else if (complete(BWG_NOISE_STAGE_NOISE_PREPARED, BWG_NOISE_CATEGORY_PREPARATION, started_us, BWG_NOISE_PREPARE_LIMIT_US, 0)) {
        sv2_noise_set_observer(s_noise, &OBSERVER);
        exchange(transport);
    }
    note_cleanup();
    const uint64_t closing_us = now_us();
    if (transport != NULL) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
    }
    note_closed(closing_us);
    if (s_noise != NULL) {
        sv2_noise_destroy(s_noise);
        s_noise = NULL;
    }
}

static void note_joined(void)
{
    memset(s_authority_key, 0, sizeof(s_authority_key));
    take();
    bwg_noise_record_joined(s_record, now_us());
    give();
}

static void worker_task(void *arg)
{
    take();
    const bool running = bwg_noise_record_dispatch(s_record, now_us());
    give();
    if (running) {
        bwg_mining_quiet_pool_logs(true);
        worker_run();
        bwg_mining_quiet_pool_logs(false);
    }
    note_joined();
    // One attempt per boot: this stack has no further use, and a stack in PSRAM can't free itself here.
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static bwg_v2_observation_t observe(uint64_t generation)
{
    const SystemModule *system = &s_global_state->SYSTEM_MODULE;
    bwg_v2_observation_t observation = {
        .boot_ordinal = bwg_evidence_boot_ordinal(),
        .worker_generation = generation,
        .serial_transport_epoch = bwg_gate_owner_epoch(),
        .observed_at_us = now_us(),
        .clock_valid = true,
        .wifi_connected = system->is_connected,
    };
    if (system->is_connected) {
        strlcpy(observation.station_ipv4, system->ip_addr_str, sizeof(observation.station_ipv4));
    }
    return observation;
}

static bool op_view(void *ctx, uint64_t generation, bwg_noise_view_t *view)
{
    view->observation = observe(generation);
    take();
    if (s_admitted) {
        bwg_noise_record_tick(s_record, now_us());
        view->has_record = true;
        view->terminal = s_record->has_terminal;
        strlcpy(view->attempt_id, s_record->attempt_id, sizeof(view->attempt_id));
    }
    give();
    return true;
}

static void op_write_status(void *ctx, const bwg_v2_observation_t *observation, bwg_jw_t *writer)
{
    take();
    bwg_noise_write_status(writer, observation, s_admitted ? s_record : NULL);
    xSemaphoreGive(s_lock);
}

static bool op_busy(void *ctx)
{
    return atomic_load(&s_busy);
}

static bool op_admit(void *ctx, const bwg_noise_input_t *input, const bwg_v2_observation_t *observation)
{
    // A diagnostic shares the chip's network and clock with nothing: local mining would muddy both.
    const char *activity = bwg_mining_activity(s_global_state);
    if (strcmp(activity, "paused") != 0) {
        ESP_LOGW(TAG, "Diagnostic refused while %s", activity);
        return false;
    }
    size_t key_len = 0;
    if (input->fixture_ipv4.len >= HOST_BYTES ||
        !bwg_b64url_decode(input->authority_public_key.ptr, input->authority_public_key.len, s_authority_key, sizeof(s_authority_key), &key_len) ||
        key_len != sizeof(s_authority_key)) {
        ESP_LOGW(TAG, "Diagnostic refused its input");
        return false;
    }
    char attempt_id[BWG_V2_ATTEMPT_ID_SIZE];
    if (input->attempt_id.len >= sizeof(attempt_id)) {
        return false;
    }
    memcpy(attempt_id, input->attempt_id.ptr, input->attempt_id.len);
    attempt_id[input->attempt_id.len] = '\0';
    take();
    const bool admitted = !s_admitted && bwg_noise_record_admit(s_record, attempt_id, input->input_sha256, observation);
    if (admitted) {
        s_admitted = true;
        s_generation = observation->worker_generation;
    }
    give();
    if (admitted) {
        memcpy(s_host, input->fixture_ipv4.ptr, input->fixture_ipv4.len);
        s_host[input->fixture_ipv4.len] = '\0';
        s_port = input->fixture_port;
    }
    return admitted;
}

static bool op_dispatch(void *ctx)
{
    if (xTaskCreateWithCaps(worker_task, "bwg noise", WORKER_STACK_BYTES, NULL, WORKER_TASK_PRIORITY, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return false;
    }
    s_dispatched = true;
    return true;
}

/** Ends the attempt for this cause. One that never got its worker has nothing to wait for. */
static void latch(bwg_noise_detail_t detail)
{
    fail(BWG_NOISE_AT_CLEANUP, BWG_NOISE_CATEGORY_AUTHORITY_LOST, detail);
    if (!s_dispatched) {
        note_joined();
    }
}

static bool op_cancel(void *ctx)
{
    if (atomic_load(&s_busy)) {
        latch(BWG_NOISE_DETAIL_CANCEL_REQUESTED);
    }
    return true;
}

void bwg_noise_gate_poll(uint64_t generation, bool session_alive)
{
    if (!atomic_load(&s_busy)) {
        return;
    }
    if (!session_alive || generation != s_generation) {
        latch(BWG_NOISE_DETAIL_SESSION_REPLACED);
        return;
    }
    take();
    bwg_noise_record_tick(s_record, now_us());
    give();
}

void bwg_noise_gate_fill_ops(bwg_controller_ops_t *ops, GlobalState *GLOBAL_STATE)
{
    s_global_state = GLOBAL_STATE;
    s_lock = xSemaphoreCreateMutex();
    s_record = heap_caps_calloc(1, sizeof(*s_record), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_record == NULL) {
        ESP_LOGE(TAG, "No memory for the diagnostic record: its commands stay refused");
        return;
    }
    ops->maybe_noise_view = op_view;
    ops->maybe_noise_write_status = op_write_status;
    ops->maybe_noise_busy = op_busy;
    ops->maybe_noise_admit = op_admit;
    ops->maybe_noise_dispatch = op_dispatch;
    ops->maybe_noise_cancel = op_cancel;
}
