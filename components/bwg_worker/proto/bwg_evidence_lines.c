#include "bwg_evidence_lines.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define BOOT_RECORD_MAGIC 0x42574731u

static uint32_t record_check(uint32_t ordinal)
{
    return ~ordinal ^ BOOT_RECORD_MAGIC;
}

uint32_t bwg_boot_record_advance(bwg_boot_record_t *record, bool cold_boot)
{
    const bool intact = record->magic == BOOT_RECORD_MAGIC && record->check == record_check(record->ordinal);
    const uint32_t next = cold_boot || !intact || record->ordinal == UINT32_MAX ? 1 : record->ordinal + 1;
    record->magic = BOOT_RECORD_MAGIC;
    record->ordinal = next;
    record->check = record_check(next);
    return next;
}

static bool fits(int written, size_t size)
{
    return written > 0 && (size_t)written < size;
}

bool bwg_line_reboot(char *out, size_t size, uint32_t boot_ordinal, const char *reset_reason, uint64_t uptime_ms)
{
    return fits(snprintf(out, size, "usb_reboot_discriminator schema=v1 boot_ordinal=%" PRIu32 " reset_reason=%s uptime_ms=%" PRIu64 " redacted=true",
                         boot_ordinal, reset_reason, uptime_ms), size);
}

bool bwg_line_identity(char *out, size_t size, const char *firmware_commit, const char *app_elf_sha256)
{
    return fits(snprintf(out, size, "usb_runtime_identity schema=v1 firmware_commit=%s app_elf_sha256=%s redacted=true", firmware_commit, app_elf_sha256), size);
}

bool bwg_line_runtime_ready(char *out, size_t size, uint64_t uptime_ms)
{
    return fits(snprintf(out, size, "usb_startup schema=v1 stage=runtime_ready state=complete first_failure=none uptime_ms=%" PRIu64 " redacted=true", uptime_ms), size);
}

bool bwg_line_allocation_failure(char *out, size_t size, uint32_t requested_bytes, uint32_t capabilities)
{
    return fits(snprintf(out, size, "allocation_failure_receipt schema=v1 requested_bytes=%" PRIu32 " capabilities=%08" PRIx32 " redacted=true", requested_bytes, capabilities), size);
}

bool bwg_line_unsafe_detail(char *out, size_t size, const bwg_unsafe_detail_t *detail)
{
    const bool has_fact = strcmp(detail->fact, "none") != 0;
    const char *state = !has_fact ? "none" : detail->has_value ? "out_of_range" : "unavailable";
    char value[16] = "unavailable";
    if (has_fact && detail->has_value && !fits(snprintf(value, sizeof(value), "%" PRId32, detail->value_milli), sizeof(value))) {
        return false;
    }
    // This firmware's samples carry no age of their own, unlike Rust's.
    return fits(snprintf(out, size,
                         "worker_revocation_detail schema=v1 generation=%" PRIu32 " reason=unsafe_observation trigger=%s fact=%s "
                         "state=%s value_milli=%s age_ms=unavailable since_safe_ms=%" PRIu32 " closed_ms=%" PRIu32 " redacted=true",
                         detail->generation, detail->trigger, detail->fact, state, value, detail->since_safe_ms, detail->closed_ms),
                size);
}
