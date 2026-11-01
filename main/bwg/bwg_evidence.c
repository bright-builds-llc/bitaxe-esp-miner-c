#include "bwg_evidence.h"

#include <stdio.h>
#include <string.h>

#include "bwg_core_dump.h"
#include "bwg_core_dump_receipt.h"
#include "bwg_evidence_lines.h"
#include "bwg_gate_ops.h"
#include "bwg_preparation.h"
#include "esp_attr.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"

#define HTTP_PORT 80
// One receipt in this many session diagnostics, so the admission line stays the usual one.
#define RECEIPT_EVERY_LINES 16u
#define ALLOCATION_RECORD_MAGIC 0x42574741u
#define APP_ELF_TEXT_BYTES 65

typedef struct {
    uint32_t magic;
    uint32_t requested_bytes;
    uint32_t capabilities;
} allocation_record_t;

RTC_NOINIT_ATTR static bwg_boot_record_t s_boot_record;
RTC_NOINIT_ATTR static allocation_record_t s_allocation_record;

static uint32_t s_boot_ordinal;
static const char *s_firmware_commit;
static char s_app_elf_sha256[APP_ELF_TEXT_BYTES];
static uint32_t s_announcement;
static uint32_t s_session_lines;
static bool s_previous_allocation_failure;
static uint32_t s_previous_requested_bytes;
static uint32_t s_previous_capabilities;

static bool reset_is_cold(esp_reset_reason_t reason)
{
    return reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT;
}

void bwg_evidence_start(const char *firmware_commit, GlobalState *GLOBAL_STATE)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    s_firmware_commit = firmware_commit;
    s_boot_ordinal = bwg_boot_record_advance(&s_boot_record, reset_is_cold(reason));
    s_previous_allocation_failure = !reset_is_cold(reason) && s_allocation_record.magic == ALLOCATION_RECORD_MAGIC;
    s_previous_requested_bytes = s_allocation_record.requested_bytes;
    s_previous_capabilities = s_allocation_record.capabilities;
    s_allocation_record.magic = 0;
    const uint64_t source_hash = bwg_source_hash(firmware_commit);
    bwg_preparation_init(source_hash, s_boot_ordinal, reset_is_cold(reason));
    bwg_core_dump_init(source_hash, s_boot_ordinal, reset_is_cold(reason));
}

void bwg_evidence_set_identity(const char *app_elf_sha256)
{
    strlcpy(s_app_elf_sha256, app_elf_sha256, sizeof(s_app_elf_sha256));
}

uint32_t bwg_evidence_boot_ordinal(void)
{
    return s_boot_ordinal;
}

bool bwg_evidence_format_announcement(char *out, size_t size, uint64_t now_ms)
{
    if (s_app_elf_sha256[0] == '\0') {
        return false;
    }
    switch (s_announcement++ % 3) {
    case 0:
        return bwg_line_reboot(out, size, s_boot_ordinal, bwg_gate_ops_reset_cause(), now_ms);
    case 1:
        return bwg_line_identity(out, size, s_firmware_commit, s_app_elf_sha256);
    default:
        return bwg_line_runtime_ready(out, size, now_ms);
    }
}

bool bwg_evidence_format_receipt(char *out, size_t size, uint64_t now_ms)
{
    switch (s_session_lines++ % RECEIPT_EVERY_LINES) {
    case 0:
        // A host that says Hello at once sees no announcement, and a restart request has to name this boot.
        return bwg_line_reboot(out, size, s_boot_ordinal, bwg_gate_ops_reset_cause(), now_ms);
    case 1:
        return bwg_preparation_format_line(out, size, true);
    case 2:
        return bwg_core_dump_format_line(out, size, true);
    case 3:
        return s_previous_allocation_failure && bwg_line_allocation_failure(out, size, s_previous_requested_bytes, s_previous_capabilities);
    case 4:
        return bwg_preparation_format_line(out, size, false);
    case 5:
        return bwg_core_dump_format_line(out, size, false);
    default:
        return false;
    }
}

bool bwg_evidence_write_endpoint(bwg_jw_t *writer, uint32_t generation)
{
    esp_netif_t *maybe_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (maybe_netif == NULL || esp_netif_get_ip_info(maybe_netif, &info) != ESP_OK) {
        return false;
    }
    const uint32_t ip = info.ip.addr;
    const uint8_t first = ip & 0xff;
    if (first == 0 || first == 127 || first >= 224) {
        return false;
    }
    char text[16];
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    bwg_jw_object_begin(writer);
    bwg_jw_key(writer, "bootOrdinal");
    bwg_jw_u64(writer, s_boot_ordinal);
    bwg_jw_key(writer, "generation");
    bwg_jw_u64(writer, generation);
    bwg_jw_key(writer, "httpPort");
    bwg_jw_u64(writer, HTTP_PORT);
    bwg_jw_key(writer, "ipv4");
    bwg_jw_cstr(writer, text);
    bwg_jw_key(writer, "observedAtUs");
    bwg_jw_u64(writer, (uint64_t)esp_timer_get_time());
    bwg_jw_key(writer, "schema");
    bwg_jw_cstr(writer, "worker-telemetry-endpoint-v1");
    bwg_jw_object_end(writer);
    return true;
}

void bwg_evidence_note_allocation_failure(size_t requested_bytes, uint32_t capabilities)
{
    s_allocation_record.requested_bytes = (uint32_t)requested_bytes;
    s_allocation_record.capabilities = capabilities;
    s_allocation_record.magic = ALLOCATION_RECORD_MAGIC;
}
