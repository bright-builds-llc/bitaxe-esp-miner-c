#include "bwg_v2_gate.h"

#include <string.h>

#include "bwg_codec.h"
#include "bwg_evidence.h"
#include "bwg_gate.h"
#include "bwg_mining.h"
#include "bwg_v2_runtime.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "stratum_socket.h"
#include "sv2_protocol.h"

#define CHANNEL_STACK_BYTES 16384
#define CHANNEL_TASK_PRIORITY 16
#define CONNECT_TIMEOUT_MS 5000
#define HOST_BYTES 16u
#define USER_BYTES (BWG_STRATUM_V2_MAX_USER_BYTES + 1u)
#define CHANNEL_REQUEST_ID 1u
/** What the record's bound leaves room for, and any real chip's rate: nothing depends on its accuracy. */
#define NOMINAL_HASHRATE 400e9f

static const char *TAG = "bwg_v2";

static GlobalState *s_global_state;
static uint64_t s_generation;

// Set before the channel task starts, and read only by it.
static char s_host[HOST_BYTES];
static uint16_t s_port;
static uint8_t s_authority_key[BWG_STRATUM_V2_AUTHORITY_KEY_BYTES];
static char s_user[USER_BYTES];

static bwg_v2_observation_t observe(uint64_t generation)
{
    const SystemModule *system = &s_global_state->SYSTEM_MODULE;
    bwg_v2_observation_t observation = {
        .boot_ordinal = bwg_evidence_boot_ordinal(),
        .worker_generation = generation,
        .serial_transport_epoch = bwg_gate_owner_epoch(),
        .observed_at_us = (uint64_t)esp_timer_get_time(),
        .clock_valid = true,
        .wifi_connected = system->is_connected,
    };
    if (system->is_connected) {
        strlcpy(observation.station_ipv4, system->ip_addr_str, sizeof(observation.station_ipv4));
    }
    return observation;
}

static bool op_view(void *ctx, bwg_v2_scope_t scope, uint64_t generation, bwg_v2_view_t *view)
{
    s_generation = generation;
    view->observation = observe(generation);
    return bwg_v2_runtime_view(scope, view);
}

static void op_write_status(void *ctx, bwg_v2_scope_t scope, const bwg_v2_observation_t *observation, bwg_jw_t *writer)
{
    bwg_v2_runtime_write_status(scope, observation, writer);
}

static bool op_busy(void *ctx)
{
    return bwg_v2_runtime_busy();
}

static void copy_counted(char *out, size_t capacity, bwg_str_t text)
{
    const size_t len = text.len < capacity - 1 ? text.len : capacity - 1;
    memcpy(out, text.ptr, len);
    out[len] = '\0';
}

static bool op_admit(void *ctx, const bwg_v2_channel_input_t *input, uint64_t generation)
{
    bwg_str_t host;
    uint16_t port;
    size_t key_len = 0;
    // A channel shares the chip's network and clock with nothing: local mining would muddy both.
    const char *activity = bwg_mining_activity(s_global_state);
    if (strcmp(activity, "paused") != 0) {
        ESP_LOGW(TAG, "Channel refused while %s", activity);
        return false;
    }
    if (!bwg_stratum_endpoint_parse(input->endpoint, &host, &port) || host.len >= HOST_BYTES ||
        !bwg_b64url_decode(input->authority_public_key.ptr, input->authority_public_key.len, s_authority_key,
                           sizeof(s_authority_key), &key_len) ||
        key_len != sizeof(s_authority_key)) {
        ESP_LOGW(TAG, "Channel refused its input");
        return false;
    }
    char attempt_id[BWG_V2_ATTEMPT_ID_SIZE];
    copy_counted(attempt_id, sizeof(attempt_id), input->attempt_id);
    const bwg_v2_observation_t observation = observe(generation);
    if (!bwg_v2_runtime_admit(BWG_V2_SCOPE_CHANNEL, attempt_id, &observation)) {
        return false;
    }
    bwg_v2_runtime_bind_channel(generation);
    copy_counted(s_host, sizeof(s_host), host);
    s_port = port;
    copy_counted(s_user, sizeof(s_user), input->user_identity);
    return true;
}

static bool send_frame(sv2_noise_ctx_t *noise, esp_transport_handle_t transport, const uint8_t *frame, int frame_len)
{
    return frame_len >= 0 && sv2_noise_send(noise, transport, frame, frame_len) == 0;
}

/** Everything a channel proves: a private socket, an authenticated pool, and one job's four frames. */
static void channel_exchange(esp_transport_handle_t transport, sv2_noise_ctx_t *noise, uint8_t *frame_buf, uint8_t *recv_buf)
{
    uint8_t header[SV2_FRAME_HEADER_SIZE];
    int payload_len;
    const uint32_t flags = sv2_setup_flags_for_channel(SV2_CHANNEL_STANDARD);
    if (sv2_noise_handshake(noise, transport, s_authority_key) != 0) {
        return;
    }
    bwg_v2_note_authenticated();
    const int setup = sv2_build_setup_connection(frame_buf, SV2_MAX_FRAME_SIZE, s_host, s_port, "bitaxe", "", "", "", flags);
    if (!send_frame(noise, transport, frame_buf, setup) ||
        sv2_noise_recv(noise, transport, header, recv_buf, SV2_MAX_FRAME_SIZE, &payload_len) != 0) {
        return;
    }
    const int open = sv2_build_open_standard_mining_channel(frame_buf, SV2_MAX_FRAME_SIZE, CHANNEL_REQUEST_ID, s_user, NOMINAL_HASHRATE);
    if (!send_frame(noise, transport, frame_buf, open)) {
        return;
    }
    // The record ends the wait: it holds the commitment once the fourth frame came, and refuses anything out of order.
    while (bwg_v2_keep_going()) {
        if (sv2_noise_recv(noise, transport, header, recv_buf, SV2_MAX_FRAME_SIZE, &payload_len) != 0) {
            return;
        }
        sv2_frame_header_t parsed;
        sv2_parse_frame_header(header, &parsed);
        if (parsed.msg_type == SV2_MSG_SET_NEW_PREV_HASH) {
            return;
        }
    }
}

static void channel_run(void)
{
    uint8_t *frame_buf = heap_caps_malloc(SV2_MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    uint8_t *recv_buf = heap_caps_malloc(SV2_MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    esp_transport_handle_t transport = esp_transport_tcp_init();
    bwg_v2_note_begin(BWG_V2_OP_INITIATOR_CONSTRUCTION);
    sv2_noise_ctx_t *noise = sv2_noise_create();
    const bool prepared = frame_buf != NULL && recv_buf != NULL && transport != NULL && noise != NULL;
    bwg_v2_note_end(BWG_V2_OP_INITIATOR_CONSTRUCTION, !prepared);
    if (prepared) {
        sv2_noise_set_observer(noise, bwg_v2_noise_observer());
        bwg_v2_note_begin(BWG_V2_OP_CONNECT);
        const bool connected = esp_transport_connect(transport, s_host, s_port, CONNECT_TIMEOUT_MS) == ESP_OK;
        bwg_v2_note_end(BWG_V2_OP_CONNECT, !connected);
        if (connected && bwg_v2_note_connected(esp_transport_get_socket(transport))) {
            stratum_socket_set_options(transport);
            channel_exchange(transport, noise, frame_buf, recv_buf);
        }
    }
    bwg_v2_note_begin(BWG_V2_OP_SOCKET_CLOSE);
    if (transport != NULL) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
    }
    bwg_v2_note_end(BWG_V2_OP_SOCKET_CLOSE, false);
    bwg_v2_note_socket_closed();
    if (noise != NULL) {
        sv2_noise_destroy(noise);
    }
    heap_caps_free(frame_buf);
    heap_caps_free(recv_buf);
}

static void channel_task(void *arg)
{
    if (bwg_v2_note_preparing()) {
        bwg_mining_quiet_pool_logs(true);
        channel_run();
        bwg_mining_quiet_pool_logs(false);
    } else {
        ESP_LOGW(TAG, "Channel attempt ended before it started");
    }
    bwg_v2_note_channel_returned();
    // One attempt per boot: this stack has no further use, and a stack in PSRAM can't free itself here.
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static bool op_dispatch(void *ctx)
{
    return xTaskCreateWithCaps(channel_task, "bwg v2 channel", CHANNEL_STACK_BYTES, NULL, CHANNEL_TASK_PRIORITY, NULL,
                               MALLOC_CAP_SPIRAM) == pdPASS;
}

static bool op_cancel(void *ctx)
{
    bwg_v2_runtime_cancel();
    return true;
}

void bwg_v2_gate_fill_ops(bwg_controller_ops_t *ops, GlobalState *GLOBAL_STATE)
{
    s_global_state = GLOBAL_STATE;
    if (!bwg_v2_runtime_init()) {
        ESP_LOGE(TAG, "No memory for the V2 record: V2 attempts stay refused");
        return;
    }
    ops->maybe_v2_view = op_view;
    ops->maybe_v2_write_status = op_write_status;
    ops->maybe_v2_busy = op_busy;
    ops->maybe_v2_admit = op_admit;
    ops->maybe_v2_dispatch = op_dispatch;
    ops->maybe_v2_cancel = op_cancel;
}

bool bwg_v2_gate_seed_share(bwg_str_t attempt_id)
{
    char id[BWG_V2_ATTEMPT_ID_SIZE];
    if (attempt_id.len != BWG_V2_ATTEMPT_ID_SIZE - 1) {
        return false;
    }
    copy_counted(id, sizeof(id), attempt_id);
    const bwg_v2_observation_t observation = observe(s_generation);
    return bwg_v2_runtime_admit(BWG_V2_SCOPE_SHARE, id, &observation);
}

static bwg_v2_operation_t operation_of(sv2_noise_op_t op)
{
    switch (op) {
    case SV2_NOISE_OP_ACT_ONE_CONSTRUCTION:
        return BWG_V2_OP_ACT_ONE_CONSTRUCTION;
    case SV2_NOISE_OP_ACT_ONE_WRITE:
        return BWG_V2_OP_ACT_ONE_WRITE;
    case SV2_NOISE_OP_ACT_TWO_READ:
        return BWG_V2_OP_ACT_TWO_READ;
    case SV2_NOISE_OP_ACT_TWO_AUTHENTICATION:
        return BWG_V2_OP_ACT_TWO_AUTHENTICATION;
    case SV2_NOISE_OP_FRAME_ENCRYPT:
        return BWG_V2_OP_FRAME_ENCRYPT;
    case SV2_NOISE_OP_FRAME_WRITE:
        return BWG_V2_OP_FRAME_WRITE;
    case SV2_NOISE_OP_FRAME_READ:
        return BWG_V2_OP_FRAME_READ;
    case SV2_NOISE_OP_HEADER_DECRYPT:
        return BWG_V2_OP_HEADER_DECRYPT;
    default:
        return BWG_V2_OP_PAYLOAD_DECRYPT;
    }
}

static void observer_begin(sv2_noise_op_t op)
{
    bwg_v2_note_begin(operation_of(op));
}

static void observer_end(sv2_noise_op_t op, bool failed)
{
    bwg_v2_note_end(operation_of(op), failed);
}

const sv2_noise_observer_t *bwg_v2_noise_observer(void)
{
    static const sv2_noise_observer_t observer = {
        .begin = observer_begin,
        .end = observer_end,
        .keep_going = bwg_v2_keep_going,
        .frame = bwg_v2_note_frame,
    };
    return &observer;
}
