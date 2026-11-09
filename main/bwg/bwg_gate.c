#include "bwg_noise_gate.h"
#include "bwg_v2_runtime.h"
#include "bwg_gate.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bwg_controller.h"
#include "bwg_unsafe_detail.h"
#include "bwg_evidence.h"
#include "bwg_gate_budget.h"
#include "bwg_gate_ops.h"
#include "bwg_link_report.h"
#include "bwg_link.h"
#include "bwg_link_presence.h"
#include "bwg_mark_valid.h"
#include "bwg_mining.h"
#include "bwg_owner_watchdog.h"
#include "bwg_provision.h"
#include "bwg_revocation.h"
#include "bwg_store.h"
#include "bwg_writer.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sodium.h"

#define USB_RX_BUFFER_BYTES 4096
#define USB_TX_BUFFER_BYTES 2048
#define LINK_READ_BYTES 512
#define LINK_READ_TICKS pdMS_TO_TICKS(5)
#define WRITER_IDLE_TICKS pdMS_TO_TICKS(5)
#define OWNER_TICK_TICKS pdMS_TO_TICKS(20)
#define OWNER_QUEUE_LENGTH 4
#define WRITER_QUEUE_LENGTH 6
#define DIAGNOSTIC_INTERVAL_MS 1000
#define ANNOUNCEMENT_INTERVAL_MS 150

// These tasks stay in internal RAM, since they hold secrets and the owner writes to flash.
#define OWNER_STACK_BYTES 24576
#define LINK_STACK_BYTES 8192
#define WRITER_STACK_BYTES 6144
// The link and the writer outrank every mining task but the job task, which blocks often, so
// heartbeats and credit keep moving while the owner task is busy with a slow Start or stop.
#define LINK_TASK_PRIORITY 17
#define WRITER_TASK_PRIORITY 17
#define OWNER_TASK_PRIORITY 11

static const char *TAG = "bwg_gate";

typedef enum {
    OWNER_MESSAGE_SESSION,
    OWNER_MESSAGE_FRAME,
} owner_message_kind_t;

typedef struct {
    owner_message_kind_t kind;
    uint32_t epoch;
    uint32_t sequence;
    bwg_session_binding_t binding;
    bwg_buffer_t frame;
} owner_message_t;

typedef enum {
    WRITER_MESSAGE_ACK,
    WRITER_MESSAGE_CONTROL,
} writer_message_kind_t;

typedef struct {
    writer_message_kind_t kind;
    uint32_t epoch;
    uint32_t token;
    uint32_t sequence;
    char session_id[BWG_SESSION_ID_TEXT_LEN + 1];
    // An acknowledgement owns this copy. A control reply borrows it from the owner task, which waits.
    const uint8_t *bytes;
    size_t len;
} writer_message_t;

typedef struct {
    uint32_t token;
    bool sent;
} reply_finished_t;

// Atomics in s_shared need internal RAM, and the rest holds the identity and short-lived secrets.
static bwg_link_shared_t s_shared;
static bwg_trace_t s_trace;
static uint32_t s_owner_sequence;
static bwg_link_t s_link;
static bwg_writer_t s_writer;
static bwg_controller_t s_controller;

static GlobalState *s_global_state;
static QueueHandle_t s_owner_queue;
static QueueHandle_t s_writer_queue;
static QueueHandle_t s_reply_queue;

// Link task only.
static bwg_link_presence_t s_presence;
static uint32_t s_generation;
static uint8_t s_read_buffer[LINK_READ_BYTES];

// Owner task only.
static uint32_t s_owner_epoch;
static uint32_t s_next_token;

uint32_t bwg_gate_owner_epoch(void)
{
    return s_owner_epoch;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void *gate_alloc(void *ctx, size_t size)
{
    return heap_caps_malloc(size, esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_8BIT);
}

static void gate_free(void *ctx, void *ptr)
{
    heap_caps_free(ptr);
}

static const bwg_allocator_t s_allocator = {
    .alloc = gate_alloc,
    .free = gate_free,
};

static bool link_fill_random(void *ctx, uint8_t *bytes, size_t len)
{
    esp_fill_random(bytes, len);
    return true;
}

// A new generation opens only from a stopped device: anything else starts the stop if it can and
// gets silence, so the Gate's next Connect finds the chip reset and cooled. A board already paused
// has nothing to stop, so it takes this Hello.
static bool link_begin_generation(void *ctx, uint64_t now, uint32_t *generation)
{
    if (bwg_revocation_snapshot().state == BWG_AUTHORITY_LOCAL) {
        bwg_revocation_revoke(BWG_STOP_REASON_LOCAL_HANDOVER);
        if (bwg_mining_local_is_stopped()) {
            bwg_revocation_note_stopped();
        }
    }
    const bool admitted = bwg_revocation_snapshot().state == BWG_AUTHORITY_STOPPED;
    bwg_link_presence_note_hello(&s_presence, now, admitted);
    if (!admitted) {
        return false;
    }
    *generation = ++s_generation;
    return true;
}

static void link_heartbeat(void *ctx, uint32_t generation, uint64_t now)
{
    bwg_link_presence_note_activity(&s_presence, now);
    bwg_revocation_feed_heartbeat_at((uint32_t)now);
}

static void link_close(void *ctx, uint32_t generation, uint64_t now)
{
    // The link's poll and the revocation timer share one deadline, and either can fire first, so a lost
    // heartbeat is named the same whichever saw it.
    const bool heartbeat_lost = now - s_presence.last_activity_ms >= BWG_HEARTBEAT_TIMEOUT_MS;
    bwg_revocation_revoke_lease(heartbeat_lost ? BWG_STOP_REASON_HEARTBEAT_TIMEOUT : BWG_STOP_REASON_LINK_CLOSED);
}

static bool link_enqueue_session(void *ctx, uint32_t epoch, const bwg_session_binding_t *binding, uint32_t generation)
{
    owner_message_t message = {.kind = OWNER_MESSAGE_SESSION, .epoch = epoch, .binding = *binding};
    return xQueueSend(s_owner_queue, &message, 0) == pdTRUE;
}

static bool link_enqueue_control(void *ctx, uint32_t epoch, uint32_t request_sequence, bwg_buffer_t frame)
{
    owner_message_t message = {.kind = OWNER_MESSAGE_FRAME, .epoch = epoch, .sequence = request_sequence, .frame = frame};
    return xQueueSend(s_owner_queue, &message, 0) == pdTRUE;
}

static bool link_send_ack(void *ctx, uint32_t epoch, const char *session_id, const uint8_t *payload, size_t len)
{
    uint8_t *copy = bwg_alloc(&s_allocator, len);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, payload, len);
    writer_message_t message = {.kind = WRITER_MESSAGE_ACK, .epoch = epoch, .bytes = copy, .len = len};
    strlcpy(message.session_id, session_id, sizeof(message.session_id));
    if (xQueueSend(s_writer_queue, &message, 0) != pdTRUE) {
        bwg_free_secret(&s_allocator, copy, len);
        return false;
    }
    return true;
}

static void link_failure(void *ctx, bwg_link_failure_t failure, size_t observed_bytes)
{
    bwg_link_report_note_rx_failure(bwg_link_failure_label(failure), observed_bytes);
}

static void link_failure_cleared(void *ctx)
{
    bwg_link_report_clear_failures();
}

static const bwg_link_ops_t s_link_ops = {
    .fill_random = link_fill_random,
    .begin_generation = link_begin_generation,
    .heartbeat_generation = link_heartbeat,
    .close_generation = link_close,
    .enqueue_session = link_enqueue_session,
    .enqueue_control = link_enqueue_control,
    .send_hello_ack = link_send_ack,
    .on_failure = link_failure,
    .on_failure_cleared = link_failure_cleared,
};

// The Gate leaving is the only way back to local mining: a stopped device with no link and a quiet
// presence clock returns to upstream's own behaviour, which still honours an operator's pause.
static void link_release_when_quiet(uint64_t now)
{
    if (bwg_link_epoch(&s_link) != 0 || bwg_link_presence_holds_local(&s_presence, now) ||
        bwg_mining_lease_pool_active() || bwg_revocation_snapshot().state != BWG_AUTHORITY_STOPPED) {
        return;
    }
    if (bwg_mining_release_to_local()) {
        ESP_LOGI(TAG, "Gate quiet, returning to local mining");
    }
}

static void link_task(void *arg)
{
    for (;;) {
        const int read = usb_serial_jtag_read_bytes(s_read_buffer, sizeof(s_read_buffer), LINK_READ_TICKS);
        const uint64_t now = now_ms();
        if (read > 0) {
            bwg_link_feed(&s_link, s_read_buffer, (size_t)read, now);
        } else if (read < 0) {
            bwg_link_read_failed(&s_link, now);
        }
        bwg_link_supervise(&s_link, now);
        link_release_when_quiet(now);
    }
}

static int32_t sink_write(void *ctx, const uint8_t *bytes, size_t len)
{
    return usb_serial_jtag_write_bytes(bytes, len, 0);
}

static bool sink_flushed(void *ctx)
{
    return usb_serial_jtag_wait_tx_done(0) == ESP_OK;
}

static const bwg_sink_t s_sink = {
    .write = sink_write,
    .flushed = sink_flushed,
};

static void report_reply_finished(uint32_t token, bool sent)
{
    const reply_finished_t finished = {.token = token, .sent = sent};
    xQueueSend(s_reply_queue, &finished, portMAX_DELAY);
}

static void writer_take_message(const writer_message_t *message)
{
    if (message->kind == WRITER_MESSAGE_ACK) {
        if (!bwg_writer_begin_session(&s_writer, message->epoch, message->session_id, message->bytes, message->len)) {
            bwg_link_shared_revoke(&s_shared, message->epoch);
        }
        bwg_free_secret(&s_allocator, (void *)message->bytes, message->len);
        return;
    }
    s_writer.next_trace_sequence = message->sequence;
    if (!bwg_writer_offer_control(&s_writer, message->epoch, message->bytes, message->len, message->token)) {
        report_reply_finished(message->token, false);
    }
}

// Before any session the board announces itself for a host watching it restart. Only after the last
// line left the driver: with no host reading, a line would sit there, and a Hello's acknowledgement behind it.
static void writer_offer_announcement(uint64_t now, uint64_t *last_ms)
{
    if (now - *last_ms < ANNOUNCEMENT_INTERVAL_MS || !sink_flushed(NULL)) {
        return;
    }
    *last_ms = now;
    char line[BWG_DIAGNOSTIC_MAX_BYTES];
    if (bwg_evidence_format_announcement(line, sizeof(line), now)) {
        bwg_writer_offer_diagnostic(&s_writer, line);
    }
}

static void writer_offer_diagnostic(uint64_t now, uint64_t *last_ms)
{
    if (bwg_link_shared_current_epoch(&s_shared) == 0) {
        writer_offer_announcement(now, last_ms);
        return;
    }
    if (now - *last_ms < DIAGNOSTIC_INTERVAL_MS) {
        return;
    }
    *last_ms = now;
    char line[BWG_DIAGNOSTIC_MAX_BYTES];
    if (!bwg_link_report_format_rx_failure(line, sizeof(line)) && !bwg_link_report_format_tx_failure(line, sizeof(line)) &&
        !bwg_evidence_format_receipt(line, sizeof(line), now)) {
        // Once a revocation detail exists, it alternates with the admission line, as in Rust.
        static bool s_detail_turn;
        s_detail_turn = !s_detail_turn;
        if (!s_detail_turn || !bwg_unsafe_detail_format(line, sizeof(line))) {
            bwg_gate_ops_format_admission(line, sizeof(line));
        }
    }
    // A line the writer can't take now is dropped, since the next one is a second away.
    bwg_writer_offer_diagnostic(&s_writer, line);
}

static void writer_task(void *arg)
{
    uint64_t last_diagnostic_ms = 0;
    for (;;) {
        const uint64_t now = now_ms();
        writer_offer_diagnostic(now, &last_diagnostic_ms);

        bwg_writer_result_t result;
        do {
            result = bwg_writer_next(&s_writer, &s_sink, now_ms());
            // An announcement nobody read isn't a link failure.
            if (result.failure != BWG_WRITER_OK && bwg_link_shared_current_epoch(&s_shared) != 0) {
                bwg_link_report_note_tx_failure(&result);
            }
            if (result.control_finished) {
                report_reply_finished(result.control_token, result.control_sent);
            }
        } while (result.status == BWG_WRITER_WROTE);

        const TickType_t wait = result.status == BWG_WRITER_BLOCKED ? 1 : WRITER_IDLE_TICKS;
        writer_message_t message;
        if (xQueueReceive(s_writer_queue, &message, wait) == pdTRUE) {
            writer_take_message(&message);
        }
    }
}

static bool owner_send_reply(uint32_t epoch, const bwg_ctrl_outcome_t *outcome)
{
    const uint32_t token = ++s_next_token;
    const writer_message_t message = {
        .kind = WRITER_MESSAGE_CONTROL,
        .epoch = epoch,
        .token = token,
        .sequence = s_owner_sequence,
        .bytes = outcome->response.data,
        .len = outcome->response.len,
    };
    if (xQueueSend(s_writer_queue, &message, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    // The writer holds the reply until it reports back, and it always reports: a stale epoch is
    // refused when selected and a record in flight has a 2,000 ms limit.
    for (;;) {
        reply_finished_t finished;
        if (xQueueReceive(s_reply_queue, &finished, portMAX_DELAY) == pdTRUE && finished.token == token) {
            return finished.sent;
        }
    }
}

static void owner_handle_frame(owner_message_t *message)
{
    bwg_trace_note(&s_trace, message->epoch, message->sequence, BWG_TRACE_DISPATCH_STARTED, (uint32_t)message->frame.len, 0);
    s_owner_sequence = message->sequence;
    bwg_ctrl_outcome_t outcome = {0};
    bwg_controller_frame(&s_controller, message->frame.data, message->frame.len, now_ms(), &outcome);
    // The frame may hold the grant's pool credentials, so it goes as soon as it's been read.
    bwg_buffer_release(&s_allocator, &message->frame);

    const uint32_t epoch = message->epoch;
    if (outcome.error != BWG_CTRL_OK) {
        bwg_trace_note(&s_trace, epoch, message->sequence, BWG_TRACE_DISPATCH_REJECTED, 0, 0);
    }
    bwg_trace_note(&s_trace, epoch, message->sequence, outcome.has_reply ? BWG_TRACE_REPLY_CREATED : BWG_TRACE_REPLY_REJECTED,
                   outcome.has_reply ? (uint32_t)outcome.response.len : 0, 0);
    if (outcome.has_reply && owner_send_reply(epoch, &outcome)) {
        if (bwg_controller_confirm_sent(&s_controller, &outcome.effect, now_ms()) != BWG_CTRL_OK) {
            bwg_link_shared_revoke(&s_shared, epoch);
        } else if (bwg_controller_is_admitted(&s_controller) && bwg_link_shared_current_epoch(&s_shared) == epoch) {
            bwg_link_shared_authenticate(&s_shared, epoch);
        }
    }
    if (outcome.revoke_after_send) {
        bwg_link_shared_revoke(&s_shared, epoch);
    }
    bwg_controller_outcome_release(&s_controller, &outcome);
}

static void owner_disconnect_if_stale(void)
{
    if (s_owner_epoch == 0 || bwg_link_shared_current_epoch(&s_shared) == s_owner_epoch) {
        return;
    }
    s_owner_epoch = 0;
    if (bwg_controller_disconnect(&s_controller, now_ms()) != BWG_CTRL_OK) {
        ESP_LOGW(TAG, "Stop after the link closed is still pending");
    }
}

static void owner_handle_session(const owner_message_t *message)
{
    if (s_owner_epoch != 0 && s_owner_epoch != message->epoch) {
        s_owner_epoch = 0;
        if (bwg_controller_disconnect(&s_controller, now_ms()) != BWG_CTRL_OK) {
            ESP_LOGW(TAG, "Stop for the replaced session is still pending");
        }
    }
    if (bwg_link_shared_current_epoch(&s_shared) != message->epoch) {
        return;
    }
    if (bwg_controller_begin_session(&s_controller, &message->binding) != BWG_CTRL_OK) {
        bwg_link_shared_revoke(&s_shared, message->epoch);
        return;
    }
    s_owner_epoch = message->epoch;
}

// A local stop during a lease (overheat, an operator's pause) closes authority behind the controller's
// back, and it must not go on reporting that lease as running. Deadlines and the link report themselves.
static bool lease_closed_locally(void)
{
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    return snapshot.state != BWG_AUTHORITY_LEASED &&
           (snapshot.reason == BWG_STOP_REASON_UNSAFE_OBSERVATION || snapshot.reason == BWG_STOP_REASON_LOCAL_STOP ||
            snapshot.reason == BWG_STOP_REASON_CONTROL_FAILED);
}

static void owner_supervise(void)
{
    if (bwg_controller_has_active_lease(&s_controller) && lease_closed_locally()) {
        // This ends the session too. Closing it first means the host gets a refusal now, not a timeout
        // after the stop, which this task waits out.
        bwg_link_shared_revoke(&s_shared, s_owner_epoch);
        bwg_controller_control_failed(&s_controller, now_ms());
    }
    // A clock error leaves the session open, so the host's next status reads the reason the lease ended.
    const bwg_ctrl_error_t error = bwg_controller_tick(&s_controller, now_ms());
    if (error != BWG_CTRL_OK) {
        ESP_LOGW(TAG, "Controller tick error=%d", (int)error);
    }
    bwg_gate_ops_finish_stopped_lease();
    bwg_v2_runtime_poll(s_controller.generation, s_controller.authenticated_logical_session);
    bwg_noise_gate_poll(s_controller.generation, s_controller.authenticated_logical_session);
    bwg_gate_ops_note_owner_stack(uxTaskGetStackHighWaterMark(NULL));
}

static void owner_run(void)
{
    bwg_owner_watchdog_start();
    for (;;) {
        owner_message_t message;
        const bool received = xQueueReceive(s_owner_queue, &message, OWNER_TICK_TICKS) == pdTRUE;
        bwg_owner_watchdog_feed();
        if (received) {
            if (message.kind == OWNER_MESSAGE_SESSION) {
                owner_handle_session(&message);
            } else if (message.epoch == s_owner_epoch && bwg_link_shared_current_epoch(&s_shared) == message.epoch) {
                owner_handle_frame(&message);
            } else {
                bwg_buffer_release(&s_allocator, &message.frame);
            }
        }
        owner_disconnect_if_stale();
        owner_supervise();
    }
}

static bool controller_start(const bwg_provision_t *provision, const char *commit, const char *elf_hex)
{
    if (!bwg_gate_budget_start(&s_allocator, s_global_state)) {
        return false;
    }
    uint8_t seed[BWG_DEVICE_SEED_BYTES];
    if (!bwg_store_load_or_create_seed(seed)) {
        ESP_LOGE(TAG, "Device identity is unavailable");
        return false;
    }
    const bwg_controller_config_t config = {
        .capability_json = provision->capability_json,
        .capability_len = provision->capability_len,
        .manifest_json = (const uint8_t *)BWG_SERIAL_MANIFEST_JSON,
        .manifest_len = strlen(BWG_SERIAL_MANIFEST_JSON),
        .firmware_commit = commit,
        .app_elf_hex = elf_hex,
        .maybe_trust_json = provision->trust_json,
        .trust_len = provision->trust_len,
        .seed = seed,
    };
    bwg_controller_ops_t ops;
    bwg_gate_ops_fill(&ops, &s_allocator, s_global_state);
    const bool ok = bwg_controller_init(&s_controller, &s_allocator, &config, &ops);
    sodium_memzero(seed, sizeof(seed));
    return ok;
}

static bool create_tasks(void)
{
    return xTaskCreate(writer_task, "bwg writer", WRITER_STACK_BYTES, NULL, WRITER_TASK_PRIORITY, NULL) == pdPASS &&
           xTaskCreate(link_task, "bwg link", LINK_STACK_BYTES, NULL, LINK_TASK_PRIORITY, NULL) == pdPASS;
}

static void owner_task(void *arg)
{
    bwg_provision_t provision;
    bwg_provision_for_board(s_global_state->DEVICE_CONFIG.board_version, &provision);
    const char *commit = bwg_provision_firmware_commit();
    char elf_hex[BWG_APP_ELF_HEX_LEN + 1];
    // The description holds all 32 bytes, unlike esp_app_get_elf_sha256's configured prefix.
    sodium_bin2hex(elf_hex, sizeof(elf_hex), esp_app_get_description()->app_elf_sha256,
                   sizeof(esp_app_get_description()->app_elf_sha256));

    usb_serial_jtag_driver_config_t usb = {
        .rx_buffer_size = USB_RX_BUFFER_BYTES,
        .tx_buffer_size = USB_TX_BUFFER_BYTES,
    };
    const bool ready = sodium_init() >= 0 && usb_serial_jtag_driver_install(&usb) == ESP_OK &&
                       controller_start(&provision, commit, elf_hex);
    bwg_evidence_set_identity(elf_hex);
    if (ready && bwg_link_init(&s_link, &s_allocator, &s_shared, &s_link_ops, commit, elf_hex) && create_tasks()) {
        ESP_LOGI(TAG, "Gate link ready");
        bwg_mark_valid_note_link_ready();
        owner_run();
    }
    ESP_LOGE(TAG, "Gate link failed to start");
    vTaskDelete(NULL);
}

void bwg_gate_start(GlobalState *GLOBAL_STATE)
{
    bwg_provision_t provision;
    if (!bwg_provision_for_board(GLOBAL_STATE->DEVICE_CONFIG.board_version, &provision)) {
        ESP_LOGI(TAG, "No Gate provisioning for this image and board");
        bwg_mark_valid_note_no_link();
        return;
    }
    s_global_state = GLOBAL_STATE;
    bwg_evidence_start(bwg_provision_firmware_commit(), GLOBAL_STATE);
    bwg_mining_init(GLOBAL_STATE);
    bwg_link_shared_init(&s_shared);
    bwg_trace_init(&s_trace, now_ms);
    s_shared.maybe_trace = &s_trace;
    bwg_gate_ops_set_trace(&s_trace);
    bwg_writer_init(&s_writer, &s_allocator, &s_shared);
    s_owner_queue = xQueueCreate(OWNER_QUEUE_LENGTH, sizeof(owner_message_t));
    s_writer_queue = xQueueCreate(WRITER_QUEUE_LENGTH, sizeof(writer_message_t));
    s_reply_queue = xQueueCreate(1, sizeof(reply_finished_t));
    if (s_owner_queue == NULL || s_writer_queue == NULL || s_reply_queue == NULL ||
        xTaskCreate(owner_task, "bwg owner", OWNER_STACK_BYTES, NULL, OWNER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Gate link failed to start");
    }
}
