#include "bwg_owner_watchdog.h"

#include <stdatomic.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ALIVE_WITHIN_MS 1000u

static const char *TAG = "bwg_owner_wdt";

static TaskHandle_t s_owner;
static _Atomic uint32_t s_fed_ms;
static _Atomic bool s_failed;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void bwg_owner_watchdog_start(void)
{
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Owner not subscribed to the task watchdog");
        atomic_store(&s_failed, true);
        return;
    }
    atomic_store(&s_fed_ms, now_ms());
    s_owner = xTaskGetCurrentTaskHandle();
}

void bwg_owner_watchdog_feed(void)
{
    if (s_owner == NULL || xTaskGetCurrentTaskHandle() != s_owner) {
        return;
    }
    if (esp_task_wdt_reset() != ESP_OK) {
        atomic_store(&s_failed, true);
        return;
    }
    atomic_store(&s_fed_ms, now_ms());
}

bool bwg_owner_watchdog_alive(void)
{
    return s_owner != NULL && !atomic_load(&s_failed) && now_ms() - atomic_load(&s_fed_ms) <= ALIVE_WITHIN_MS;
}
