#ifndef BWG_LINK_H
#define BWG_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"
#include "bwg_frame.h"
#include "bwg_link_shared.h"

#define BWG_HEARTBEAT_TIMEOUT_MS 2800u
#define BWG_RECEIVE_WINDOW_BYTES 2048u
#define BWG_FIRMWARE_COMMIT_LEN 40u
#define BWG_APP_ELF_HEX_LEN 64u
/** Fits the signed manifest plus the other acknowledgement fields. */
#define BWG_HELLO_ACK_MAX_BYTES 1280u

/** Why the receiver closed a link. Codes are a stable marker format; never renumber them. */
typedef enum {
    BWG_LINK_FAILURE_READ = 1,
    BWG_LINK_FAILURE_FRAMING = 2,
    BWG_LINK_FAILURE_ENVELOPE = 3,
    BWG_LINK_FAILURE_SEQUENCE = 4,
    BWG_LINK_FAILURE_HEARTBEAT_PAYLOAD = 5,
    BWG_LINK_FAILURE_HEARTBEAT_TIMEOUT = 6,
    BWG_LINK_FAILURE_CONTROL_ALLOCATION = 7,
    BWG_LINK_FAILURE_CONTROL_QUEUE = 8,
    BWG_LINK_FAILURE_UNEXPECTED_KIND = 9,
    BWG_LINK_FAILURE_SESSION_REVOKED = 10,
    BWG_LINK_FAILURE_INTEGRITY = 11,
    BWG_LINK_FAILURE_RECEIVE_COUNTER = 12,
    BWG_LINK_FAILURE_FRAME_ALLOCATION = 13,
} bwg_link_failure_t;

/** The redaction-safe stage label for `usb_rx_failure`. */
const char *bwg_link_failure_label(bwg_link_failure_t failure);

/** One Hello's identifiers, NUL terminated. */
typedef struct {
    char session_id[BWG_SESSION_ID_TEXT_LEN + 1];
    char host_nonce[44];
    char device_nonce[44];
} bwg_session_binding_t;

/** Effects the receiver asks for. Each runs on the link task and must not wait on Worker effects or output. */
typedef struct {
    void *ctx;
    /** Fills `bytes` with fresh entropy. False refuses the Hello silently. */
    bool (*fill_random)(void *ctx, uint8_t *bytes, size_t len);
    /** Whether a new generation may begin at `now_ms`. False means the Hello gets no reply at all. */
    bool (*begin_generation)(void *ctx, uint64_t now_ms, uint32_t *generation);
    /** An authenticated heartbeat arrived. */
    void (*heartbeat_generation)(void *ctx, uint32_t generation, uint64_t now_ms);
    /** The link closed: work under this generation must stop. */
    void (*close_generation)(void *ctx, uint32_t generation, uint64_t now_ms);
    /** Tells the owner about a new session. False closes it. */
    bool (*enqueue_session)(void *ctx, uint32_t epoch, const bwg_session_binding_t *binding, uint32_t generation);
    /**
     * Passes one control payload with a trailing LF to the owner. On true the owner has taken `frame`
     * and later releases it with bwg_buffer_release; on false the core wipes it and closes the link.
     */
    bool (*enqueue_control)(void *ctx, uint32_t epoch, uint32_t request_sequence, bwg_buffer_t frame);
    /** Passes the acknowledgement to the writer (bwg_writer_begin_session). False closes the link. */
    bool (*send_hello_ack)(void *ctx, uint32_t epoch, const char *session_id, const uint8_t *payload, size_t len);
    /** Records a first failure for diagnostics. */
    void (*on_failure)(void *ctx, bwg_link_failure_t failure, size_t observed_bytes);
    /** A new session began, so earlier failures no longer apply. */
    void (*on_failure_cleared)(void *ctx);
} bwg_link_ops_t;

/** The receiver's state. Owned by the link task; no other task touches it. */
typedef struct {
    const bwg_allocator_t *allocator;
    bwg_link_shared_t *shared;
    bwg_link_ops_t ops;
    char firmware_commit[BWG_FIRMWARE_COMMIT_LEN + 1];
    char app_elf_hex[BWG_APP_ELF_HEX_LEN + 1];
    bwg_accumulator_t accumulator;
    uint32_t next_epoch;
    size_t observed_bytes;
    bool open;
    uint32_t epoch;
    bwg_session_binding_t binding;
    uint32_t generation;
    uint32_t sequence;
    uint64_t last_observation_ms;
    uint64_t last_heartbeat_ms;
    bool authenticated;
    bool closed;
    uint32_t received;
    /** Held here rather than on the task stack. */
    uint8_t ack_scratch[BWG_HELLO_ACK_MAX_BYTES];
} bwg_link_t;

/**
 * Prepares a receiver. `firmware_commit` must be 40 and `app_elf_hex` 64 lowercase hex characters,
 * else returns false. Nothing is allocated until the first byte arrives.
 */
bool bwg_link_init(bwg_link_t *link, const bwg_allocator_t *allocator, bwg_link_shared_t *shared,
                   const bwg_link_ops_t *ops, const char *firmware_commit, const char *app_elf_hex);

/** Wipes and frees the frame buffer, closing any open link first. */
void bwg_link_deinit(bwg_link_t *link, uint64_t now_ms);

/**
 * Consumes one transport read in order. Every byte after a Hello's LF earns credit, including
 * incomplete records. Malformed lines with no link are ignored, so boot text is harmless.
 */
void bwg_link_feed(bwg_link_t *link, const uint8_t *bytes, size_t len, uint64_t now_ms);

/**
 * Call with the same clock at every read tick. Closes a link whose epoch was revoked
 * (`session_revoked`), or whose heartbeat clock lapsed or ran backwards (`heartbeat_timeout`).
 * The owner then disconnects its controller; see bwg_controller_disconnect.
 */
void bwg_link_supervise(bwg_link_t *link, uint64_t now_ms);

/** A native read failed: records `read` if linked, then drops the link and any partial record. */
void bwg_link_read_failed(bwg_link_t *link, uint64_t now_ms);

/** The open link's epoch, or 0. */
uint32_t bwg_link_epoch(const bwg_link_t *link);

#endif // BWG_LINK_H
