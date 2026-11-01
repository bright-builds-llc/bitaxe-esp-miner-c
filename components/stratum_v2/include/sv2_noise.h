#ifndef SV2_NOISE_H
#define SV2_NOISE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_transport.h"

typedef struct sv2_noise_ctx sv2_noise_ctx_t;

typedef enum {
    SV2_NOISE_OP_ACT_ONE_CONSTRUCTION,
    SV2_NOISE_OP_ACT_ONE_WRITE,
    SV2_NOISE_OP_ACT_TWO_READ,
    SV2_NOISE_OP_ACT_TWO_AUTHENTICATION,
    SV2_NOISE_OP_FRAME_ENCRYPT,
    SV2_NOISE_OP_FRAME_WRITE,
    SV2_NOISE_OP_FRAME_READ,
    SV2_NOISE_OP_HEADER_DECRYPT,
    SV2_NOISE_OP_PAYLOAD_DECRYPT,
} sv2_noise_op_t;

// Watches one connection, for a caller that has to account for it.
typedef struct {
    void (*begin)(sv2_noise_op_t op);
    void (*end)(sv2_noise_op_t op, bool failed);
    // Asked between short waits on the pool. False gives up on the read in progress.
    bool (*keep_going)(void);
    // Called with each authenticated frame. False fails the receive.
    bool (*frame)(const uint8_t header[6], const uint8_t *payload, int payload_len);
} sv2_noise_observer_t;

// Sets the observer for this context. NULL, the default, observes nothing.
void sv2_noise_set_observer(sv2_noise_ctx_t *ctx, const sv2_noise_observer_t *observer);

typedef enum {
    SV2_NOISE_FAILURE_OTHER,
    SV2_NOISE_FAILURE_TIMEOUT,
    SV2_NOISE_FAILURE_EOF,
    SV2_NOISE_FAILURE_IO,
    SV2_NOISE_FAILURE_CERTIFICATE_TIME,
    SV2_NOISE_FAILURE_WRONG_AUTHORITY,
} sv2_noise_failure_t;

// Why the last failed read or certificate check failed, for a caller that has to report it.
sv2_noise_failure_t sv2_noise_last_failure(const sv2_noise_ctx_t *ctx);

// For unit tests of the frame paths: a real handshake needs a real responder. Keys stay unset, so frames fail to decrypt.
void sv2_noise_test_skip_handshake(sv2_noise_ctx_t *ctx);

// Create a new Noise context (allocates secp256k1 context internally).
sv2_noise_ctx_t *sv2_noise_create(void);

// Destroy a Noise context and free all resources.
void sv2_noise_destroy(sv2_noise_ctx_t *ctx);

// Perform Noise_NX handshake as initiator over the given transport.
// authority_pubkey is an optional 32-byte x-only public key for Schnorr
// signature verification of the responder's certificate. Pass NULL to skip.
// Returns 0 on success, -1 on error.
int sv2_noise_handshake(sv2_noise_ctx_t *ctx, esp_transport_handle_t transport,
                        const uint8_t *authority_pubkey);

// Whether a certificate's validity window holds at `now` (Unix seconds), with
// ten seconds of leeway each way. A clock that was never set never holds.
bool sv2_noise_certificate_time_valid(uint32_t valid_from, uint32_t not_valid_after, uint32_t now);

// Send an SV2 frame (header + payload) encrypted via Noise.
// frame points to the complete plaintext frame (header + payload).
// Returns 0 on success, -1 on error.
int sv2_noise_send(sv2_noise_ctx_t *ctx, esp_transport_handle_t transport,
                   const uint8_t *frame, int frame_len);

// Receive and decrypt an SV2 frame via Noise.
// hdr_out receives the 6-byte decrypted frame header.
// payload_out receives the decrypted payload (up to max_payload_len bytes).
// payload_len_out receives the actual payload length.
// Returns 0 on success, -1 on error.
int sv2_noise_recv(sv2_noise_ctx_t *ctx, esp_transport_handle_t transport,
                   uint8_t hdr_out[6], uint8_t *payload_out,
                   int max_payload_len, int *payload_len_out);

#endif /* SV2_NOISE_H */
