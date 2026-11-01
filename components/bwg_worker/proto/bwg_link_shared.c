#include "bwg_link_shared.h"

// Rust's terminal window, which is also the record write deadline.
#define TERMINAL_RECEIPT_MS 2000u

void bwg_link_shared_init(bwg_link_shared_t *shared)
{
    shared->maybe_trace = NULL;
    atomic_init(&shared->current_epoch, 0);
    atomic_init(&shared->authenticated_epoch, 0);
    atomic_init(&shared->credit_epoch, 0);
    atomic_init(&shared->credit_bytes, 0);
    atomic_init(&shared->latest_epoch, 0);
    atomic_init(&shared->closing_epoch, 0);
    atomic_init(&shared->closing_bytes, 0);
    atomic_init(&shared->closing_started, 0);
}

uint32_t bwg_link_shared_current_epoch(const bwg_link_shared_t *shared)
{
    return atomic_load_explicit(&shared->current_epoch, memory_order_acquire);
}

uint32_t bwg_link_shared_authenticated_epoch(const bwg_link_shared_t *shared)
{
    return atomic_load_explicit(&shared->authenticated_epoch, memory_order_acquire);
}

void bwg_link_shared_authenticate(bwg_link_shared_t *shared, uint32_t epoch)
{
    atomic_store_explicit(&shared->authenticated_epoch, epoch, memory_order_release);
}

static bool compare_clear(_Atomic uint32_t *value, uint32_t expected)
{
    return atomic_compare_exchange_strong_explicit(value, &expected, 0, memory_order_acq_rel, memory_order_acquire);
}

void bwg_link_shared_revoke(bwg_link_shared_t *shared, uint32_t epoch)
{
    // Revoking a closed epoch again leaves its receipt: a record caught mid-flight by a Close lands here, and the
    // host's Close waits on that receipt.
    if (compare_clear(&shared->current_epoch, epoch)) {
        bwg_trace_note(shared->maybe_trace, epoch, 0, BWG_TRACE_EPOCH_REVOKED, 0, 0);
        bwg_credit_finish_terminal(shared, epoch);
        compare_clear(&shared->credit_epoch, epoch);
    }
    compare_clear(&shared->authenticated_epoch, epoch);
}

void bwg_credit_begin(bwg_link_shared_t *shared, uint32_t epoch)
{
    atomic_store_explicit(&shared->closing_epoch, 0, memory_order_release);
    atomic_store_explicit(&shared->latest_epoch, epoch, memory_order_release);
    atomic_store_explicit(&shared->credit_epoch, 0, memory_order_release);
    atomic_store_explicit(&shared->credit_bytes, 0, memory_order_release);
    atomic_store_explicit(&shared->credit_epoch, epoch, memory_order_release);
}

void bwg_credit_publish(bwg_link_shared_t *shared, uint32_t received_bytes)
{
    atomic_store_explicit(&shared->credit_bytes, received_bytes, memory_order_release);
}

bool bwg_credit_received(const bwg_link_shared_t *shared, uint32_t epoch, uint32_t *received_bytes)
{
    if (epoch == 0 || atomic_load_explicit(&shared->credit_epoch, memory_order_acquire) != epoch) {
        return false;
    }
    uint32_t bytes = atomic_load_explicit(&shared->credit_bytes, memory_order_acquire);
    if (atomic_load_explicit(&shared->credit_epoch, memory_order_acquire) != epoch) {
        return false;
    }
    *received_bytes = bytes;
    return true;
}

void bwg_credit_retain_terminal(bwg_link_shared_t *shared, uint32_t epoch, bool has_previous, uint32_t previous,
                                uint32_t received, uint64_t now_ms)
{
    if (!has_previous || epoch == 0 || previous == UINT32_MAX || previous + 1 != received ||
        atomic_load_explicit(&shared->latest_epoch, memory_order_acquire) != epoch) {
        return;
    }
    atomic_store_explicit(&shared->closing_bytes, received, memory_order_release);
    atomic_store_explicit(&shared->closing_started, (uint32_t) now_ms, memory_order_release);
    atomic_store_explicit(&shared->closing_epoch, epoch, memory_order_release);
}

bool bwg_credit_terminal(const bwg_link_shared_t *shared, uint32_t epoch, uint64_t now_ms, uint32_t *received_bytes)
{
    if (epoch == 0 || atomic_load_explicit(&shared->closing_epoch, memory_order_acquire) != epoch) {
        return false;
    }
    uint32_t started = atomic_load_explicit(&shared->closing_started, memory_order_acquire);
    uint32_t bytes = atomic_load_explicit(&shared->closing_bytes, memory_order_acquire);
    if (atomic_load_explicit(&shared->latest_epoch, memory_order_acquire) != epoch ||
        atomic_load_explicit(&shared->closing_epoch, memory_order_acquire) != epoch ||
        (uint32_t) ((uint32_t) now_ms - started) >= TERMINAL_RECEIPT_MS) {
        return false;
    }
    *received_bytes = bytes;
    return true;
}

void bwg_credit_finish_terminal(bwg_link_shared_t *shared, uint32_t epoch)
{
    compare_clear(&shared->closing_epoch, epoch);
}
