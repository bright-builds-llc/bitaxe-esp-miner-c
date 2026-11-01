#ifndef BWG_EVIDENCE_LINES_H
#define BWG_EVIDENCE_LINES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Survives every reset except power loss, in memory that nothing initializes. */
typedef struct {
    uint32_t magic;
    uint32_t ordinal;
    uint32_t check;
} bwg_boot_record_t;

/**
 * Counts this boot. A cold boot or a record that doesn't check out starts again from 1, since either way
 * nothing proves what came before it.
 */
uint32_t bwg_boot_record_advance(bwg_boot_record_t *record, bool cold_boot);

/** The line a host watching a reset waits for. Each returns false when `out` is too small. */
bool bwg_line_reboot(char *out, size_t size, uint32_t boot_ordinal, const char *reset_reason, uint64_t uptime_ms);
bool bwg_line_identity(char *out, size_t size, const char *firmware_commit, const char *app_elf_sha256);
bool bwg_line_runtime_ready(char *out, size_t size, uint64_t uptime_ms);
bool bwg_line_allocation_failure(char *out, size_t size, uint32_t requested_bytes, uint32_t capabilities);

/** Why a generation was revoked as `unsafe_observation`. Diagnostic only: it carries no authority. */
typedef struct {
    uint32_t generation;
    const char *trigger;
    // "none" with no failing fact to name, and then there's no value either.
    const char *fact;
    bool has_value;
    int32_t value_milli;
    uint32_t since_safe_ms;
    uint32_t closed_ms;
} bwg_unsafe_detail_t;

bool bwg_line_unsafe_detail(char *out, size_t size, const bwg_unsafe_detail_t *detail);

#endif // BWG_EVIDENCE_LINES_H
