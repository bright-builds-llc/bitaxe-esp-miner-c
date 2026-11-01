#include "bwg_link_report.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "esp_log.h"

static const char *TAG = "bwg_gate";

// The Gate's grammar stops at five digits.
#define OBSERVED_BYTES_MAX 99999u

// The first failure of a session, reported once.
static _Atomic bool s_rx_failure_seen;
static _Atomic(const char *) s_rx_failure_stage;
static _Atomic uint32_t s_rx_failure_bytes;

bool bwg_link_report_format_rx_failure(char *line, size_t capacity)
{
    const char *stage = atomic_exchange(&s_rx_failure_stage, NULL);
    if (stage == NULL) {
        return false;
    }
    snprintf(line, capacity, "usb_rx_failure schema=v1 stage=%s observed_bytes=%" PRIu32 " redacted=true", stage,
             atomic_load(&s_rx_failure_bytes));
    return true;
}

void bwg_link_report_note_rx_failure(const char *label, size_t observed_bytes)
{
    if (atomic_exchange(&s_rx_failure_seen, true)) {
        return;
    }
    ESP_LOGW(TAG, "usb_rx_failure stage=%s observed_bytes=%u", label, (unsigned)observed_bytes);
    // The Gate's grammar has one allocation stage.
    const char *stage = strcmp(label, "frame_allocation") == 0 ? "control_allocation" : label;
    atomic_store(&s_rx_failure_bytes, observed_bytes > OBSERVED_BYTES_MAX ? OBSERVED_BYTES_MAX : (uint32_t)observed_bytes);
    atomic_store(&s_rx_failure_stage, stage);
}

// The Gate's grammar stops at five digits; Rust caps these at the record limit.
#define TX_BYTES_MAX 66560u

// The writer task alone uses these.
static const char *s_tx_failure_stage;
static uint32_t s_tx_failure_elapsed_ms;
static size_t s_tx_failure_queued;
static size_t s_tx_failure_record;

static const char *tx_failure_stage(const bwg_writer_result_t *result)
{
    switch (result->failure) {
    case BWG_WRITER_FAILED_DEADLINE:
        return result->failed_flushing ? "flush_timeout" : "write_timeout";
    case BWG_WRITER_FAILED_SINK:
        return "write";
    case BWG_WRITER_FAILED_REVOKED:
        // Only a line cut short costs anything. A Close often lands while a whole record drains.
        return result->failed_queued_bytes > 0 && result->failed_queued_bytes < result->failed_record_bytes
                   ? "cancelled"
                   : NULL;
    default:
        return NULL;
    }
}

void bwg_link_report_note_tx_failure(const bwg_writer_result_t *result)
{
    const char *stage = tx_failure_stage(result);
    if (stage == NULL) {
        return;
    }
    ESP_LOGW(TAG, "usb_tx_failure stage=%s", stage);
    s_tx_failure_stage = stage;
    s_tx_failure_elapsed_ms = result->failed_elapsed_ms;
    s_tx_failure_queued = result->failed_queued_bytes;
    s_tx_failure_record = result->failed_record_bytes;
}

bool bwg_link_report_format_tx_failure(char *line, size_t capacity)
{
    if (s_tx_failure_stage == NULL) {
        return false;
    }
    snprintf(line, capacity, "usb_tx_failure schema=v1 stage=%s elapsed_ms=%" PRIu32 " queued_bytes=%u record_bytes=%u redacted=true",
             s_tx_failure_stage, s_tx_failure_elapsed_ms, (unsigned) MIN(s_tx_failure_queued, TX_BYTES_MAX),
             (unsigned) MIN(s_tx_failure_record, TX_BYTES_MAX));
    s_tx_failure_stage = NULL;
    return true;
}

void bwg_link_report_clear_failures(void)
{
    atomic_store(&s_rx_failure_seen, false);
}
