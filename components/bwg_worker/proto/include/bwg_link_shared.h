#ifndef BWG_LINK_SHARED_H
#define BWG_LINK_SHARED_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "bwg_trace.h"

/**
 * The lock-free state between the link task (the only publisher of credit), the writer task and
 * the owner task. Credit never waits on a Worker effect or on output. The shell owns one instance.
 */
typedef struct {
    /** Set once before any task starts, then only read. */
    bwg_trace_t *maybe_trace;
    _Atomic uint32_t current_epoch;
    _Atomic uint32_t authenticated_epoch;
    _Atomic uint32_t credit_epoch;
    _Atomic uint32_t credit_bytes;
    _Atomic uint32_t latest_epoch;
    _Atomic uint32_t closing_epoch;
    _Atomic uint32_t closing_bytes;
    _Atomic uint32_t closing_started;
} bwg_link_shared_t;

/** Starts with no epoch, no authentication and no credit. Call before any task uses the instance. */
void bwg_link_shared_init(bwg_link_shared_t *shared);

/** The epoch allowed to emit output, or 0. */
uint32_t bwg_link_shared_current_epoch(const bwg_link_shared_t *shared);

/** The epoch whose possession was confirmed sent, or 0. */
uint32_t bwg_link_shared_authenticated_epoch(const bwg_link_shared_t *shared);

/**
 * Lets heartbeats count for `epoch`. The owner calls this after bwg_controller_confirm_sent, and
 * only while bwg_controller_is_admitted holds and `epoch` is still current.
 */
void bwg_link_shared_authenticate(bwg_link_shared_t *shared, uint32_t epoch);

/**
 * Revokes `epoch` if it is current: stops its output, credit and heartbeats, and drops any closing receipt.
 * A later epoch is untouched, and so is the receipt of an epoch already closed.
 */
void bwg_link_shared_revoke(bwg_link_shared_t *shared, uint32_t epoch);

/** Link task only: starts a fresh epoch's credit at zero, discarding any closing receipt. */
void bwg_credit_begin(bwg_link_shared_t *shared, uint32_t epoch);

/** Link task only: publishes bytes drained so far. Coalescing is the writer's business. */
void bwg_credit_publish(bwg_link_shared_t *shared, uint32_t received_bytes);

/** The latest count for `epoch`. False once the epoch was closed or replaced. */
bool bwg_credit_received(const bwg_link_shared_t *shared, uint32_t epoch, uint32_t *received_bytes);

/**
 * Link task only, after authority was revoked for a validated Close: retains the one final receipt
 * `received`, stamped `now_ms`, if it is exactly one newer than `previous` and no newer Hello began.
 */
void bwg_credit_retain_terminal(bwg_link_shared_t *shared, uint32_t epoch, bool has_previous, uint32_t previous,
                                uint32_t received, uint64_t now_ms);

/** The closing epoch's retained receipt. False after 2,000 ms, after a new Hello, or once finished. */
bool bwg_credit_terminal(const bwg_link_shared_t *shared, uint32_t epoch, uint64_t now_ms, uint32_t *received_bytes);

/** Consumes the closing receipt. No retry survives this. */
void bwg_credit_finish_terminal(bwg_link_shared_t *shared, uint32_t epoch);

#endif // BWG_LINK_SHARED_H
