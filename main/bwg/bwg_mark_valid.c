#include "bwg_mark_valid.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "bwg_boot_check.h"
#include "bwg_onboarding_gate.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium.h"

#define POLL_TICKS pdMS_TO_TICKS(500)
#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY 2

typedef enum {
    LINK_UNKNOWN = 0,
    LINK_READY,
    LINK_NOT_PROVISIONED,
} link_state_t;

static const char *TAG = "bwg_mark_valid";
static _Atomic int s_link = LINK_UNKNOWN;

void bwg_mark_valid_note_link_ready(void)
{
    atomic_store(&s_link, LINK_READY);
}

void bwg_mark_valid_note_no_link(void)
{
    atomic_store(&s_link, LINK_NOT_PROVISIONED);
}

static bool settings_read_back(void)
{
    // Values such as the Wi-Fi passphrase pass through the buffer.
    uint8_t *scratch = malloc(BWG_SETTINGS_VALUE_MAX_BYTES);
    if (scratch == NULL) {
        return false;
    }
    const bwg_settings_reader_t reader = {.ctx = NULL, .read = bwg_onboarding_read_setting};
    const bool readable = bwg_settings_readable(&reader, scratch);
    sodium_memzero(scratch, BWG_SETTINGS_VALUE_MAX_BYTES);
    free(scratch);
    return readable;
}

static void act_on(bwg_boot_verdict_t verdict, const bwg_boot_facts_t *facts)
{
    if (verdict == BWG_BOOT_MARK_VALID) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Startup check passed after %llu ms; marking the image valid: %s",
                 (unsigned long long) facts->elapsed_ms, esp_err_to_name(err));
        return;
    }
    if (verdict == BWG_BOOT_ROLL_BACK) {
        ESP_LOGE(TAG, "Startup check failed after %llu ms (settings readable %d, link required %d, link ready %d); "
                      "rolling back",
                 (unsigned long long) facts->elapsed_ms, facts->settings_readable, facts->link_required,
                 facts->link_ready);
        // Returns only on failure, when no other slot holds a bootable image. The image stays on trial.
        const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
    }
}

static void check_task(void *arg)
{
    const GlobalState *GLOBAL_STATE = arg;
    bwg_partition_facts_t partition;
    bwg_boot_facts_t facts = {.state = BWG_IMAGE_UNDEFINED};
    if (bwg_onboarding_partition_facts(NULL, &partition)) {
        facts.state = partition.running_state;
    }
    facts.settings_readable = settings_read_back();
    bwg_boot_verdict_t verdict;
    for (;;) {
        const int link = atomic_load(&s_link);
        // A self-test boot never starts the link. Until startup has read that flag, assume the link is needed.
        facts.link_required = link != LINK_NOT_PROVISIONED && !GLOBAL_STATE->SELF_TEST_MODULE.is_active;
        facts.link_ready = link == LINK_READY;
        facts.elapsed_ms = (uint64_t) (esp_timer_get_time() / 1000);
        verdict = bwg_boot_check_verdict(&facts);
        if (verdict != BWG_BOOT_WAIT) {
            break;
        }
        vTaskDelay(POLL_TICKS);
    }
    act_on(verdict, &facts);
    vTaskDelete(NULL);
}

void bwg_mark_valid_start(const GlobalState *GLOBAL_STATE)
{
    if (xTaskCreate(check_task, "bwg mark valid", TASK_STACK_BYTES, (void *) GLOBAL_STATE, TASK_PRIORITY, NULL) != pdPASS) {
        // Left on trial, the image rolls back at the next reset, which is the safe side.
        ESP_LOGE(TAG, "Startup check failed to start");
    }
}
