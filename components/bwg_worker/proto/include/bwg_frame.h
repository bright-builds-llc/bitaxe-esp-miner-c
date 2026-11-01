#ifndef BWG_FRAME_H
#define BWG_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_alloc.h"

#define BWG_SERIAL_PROFILE "bwg-worker-serial/0.2"
#define BWG_MAX_WIRE_FRAME_BYTES 66560u
#define BWG_MAX_CONTROL_PAYLOAD_BYTES 65536u
#define BWG_MAX_HELLO_FRAME_BYTES 2048u
#define BWG_SESSION_ID_BYTES 16u
#define BWG_SESSION_ID_TEXT_LEN 22u
#define BWG_NONCE_BYTES 32u
/** Largest envelope around a payload, with room to spare. */
#define BWG_ENVELOPE_OVERHEAD_BYTES 256u

/** The signed transport manifest. Its digest is part of the signed capability; never change it. */
#define BWG_SERIAL_MANIFEST_JSON                                                                  \
    "{\"profile\":\"bwg-worker-serial/0.2\","                                                     \
    "\"qualificationAttemptProfile\":\"worker-qualification-attempt-v1\","                        \
    "\"poolDifficultyHintProfile\":\"worker-stratum-difficulty-hint-v1\","                        \
    "\"transport\":\"esp32s3_usb_serial_jtag\",\"baudRate\":115200,\"framing\":\"utf8_ndjson\","  \
    "\"maximumControlPayloadBytes\":65536,\"maximumWireFrameBytes\":66560,"                       \
    "\"heartbeatIntervalMilliseconds\":1000,\"heartbeatTimeoutMilliseconds\":2800,"               \
    "\"foregroundOnly\":true,\"hostToDeviceReceiveWindowBytes\":2048,"                            \
    "\"maximumHostWriteChunkBytes\":1024,\"recordWriteTimeoutMilliseconds\":2000,"                \
    "\"payloadIntegrity\":\"sha256_exact_utf8_json\"}"
#define BWG_SERIAL_MANIFEST_SHA256 "pDwsfWH5X8bHMTqyB0SgAxTKA0_KpzBqdSLxMpKVNhU"

typedef enum {
    BWG_KIND_SESSION = 0,
    BWG_KIND_CONTROL,
    BWG_KIND_HEARTBEAT,
    BWG_KIND_DIAGNOSTIC,
} bwg_kind_t;

typedef enum {
    BWG_FRAME_OK = 0,
    BWG_FRAME_INVALID,
    BWG_FRAME_OVERSIZED,
    BWG_FRAME_INTEGRITY,
} bwg_frame_status_t;

/** A validated record borrowing the line it was parsed from. */
typedef struct {
    bwg_kind_t kind;
    /** NULL when the wire value is null. */
    const char *maybe_session_id;
    uint32_t sequence;
    /** The exact transmitted bytes of the payload value. */
    const uint8_t *payload;
    size_t payload_len;
} bwg_record_t;

/**
 * Parses one complete line including its LF, in the protocol's order: size, LF, CR or inner LF,
 * strict envelope, control payload bound, then integrity. Envelope strings holding any escape are
 * invalid. Nothing past the size check reads beyond `len`.
 */
bwg_frame_status_t bwg_record_parse(const uint8_t *line, size_t len, bwg_record_t *record);

/**
 * Writes the record with its LF, then parses the result back, so no unreadable record leaves.
 * `maybe_session_id` must be NULL or 22 characters. `payload` must be one JSON object with no CR
 * or LF. Needs `len + BWG_ENVELOPE_OVERHEAD_BYTES` of capacity, else BWG_FRAME_OVERSIZED.
 */
bwg_frame_status_t bwg_record_encode(bwg_kind_t kind, const char *maybe_session_id, uint32_t sequence,
                                     const uint8_t *payload, size_t len, uint8_t *out, size_t capacity,
                                     size_t *out_len);

typedef enum {
    /** No complete line yet. */
    BWG_ACC_NONE = 0,
    /** `*line` is one complete line, valid until bwg_accumulator_clear, which the caller must call. */
    BWG_ACC_LINE,
    /** The line outgrew the bound. Bytes are discarded through its LF. */
    BWG_ACC_OVERSIZED,
    /** The frame buffer could not be allocated. Bytes are discarded through the next LF. */
    BWG_ACC_UNAVAILABLE,
} bwg_acc_result_t;

/** Splits a byte stream into lines. One buffer of BWG_MAX_WIRE_FRAME_BYTES, allocated at first use. */
typedef struct {
    const bwg_allocator_t *allocator;
    uint8_t *maybe_bytes;
    size_t len;
    bool discarding;
} bwg_accumulator_t;

/** Starts empty. Nothing is allocated until the first byte. */
void bwg_accumulator_init(bwg_accumulator_t *accumulator, const bwg_allocator_t *allocator);

/** Takes one byte. See bwg_acc_result_t. `*line` and `*line_len` are set only for BWG_ACC_LINE. */
bwg_acc_result_t bwg_accumulator_push(bwg_accumulator_t *accumulator, uint8_t byte, const uint8_t **line,
                                      size_t *line_len);

/** Wipes partial input and ends any discarding. The buffer stays allocated for the next record. */
void bwg_accumulator_clear(bwg_accumulator_t *accumulator);

/** Wipes and frees the buffer. */
void bwg_accumulator_deinit(bwg_accumulator_t *accumulator);

#endif // BWG_FRAME_H
