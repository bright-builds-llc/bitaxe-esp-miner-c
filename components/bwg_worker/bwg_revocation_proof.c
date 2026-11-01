// Test variant only (CONFIG_BWG_REVOCATION_PROOF). Stands in for the Gate, so real leases and
// stops can be timed on hardware before the serial protocol exists. Local work stands in for a
// grant's pool.
#include "bwg_revocation.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bwg_proof";

#define SETTLE_MS (180 * 1000)
#define HEARTBEAT_INTERVAL_MS 1000
#define HEARTBEAT_CYCLES 3
#define HEARTBEAT_LEASE_MS 60000
#define HEARTBEAT_FED_MS 45000
#define EXPIRY_CYCLES 2
#define EXPIRY_LEASE_MS 45000
#define STOP_TIMEOUT_MS (60 * 1000)
#define POLL_MS 100

static bool wait_for_stopped(void)
{
    for (int waited_ms = 0; waited_ms < STOP_TIMEOUT_MS; waited_ms += POLL_MS) {
        if (bwg_revocation_snapshot().state == BWG_AUTHORITY_STOPPED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    ESP_LOGE(TAG, "stop did not complete in time");
    return false;
}

static void feed_for(int fed_ms)
{
    for (int elapsed_ms = 0; elapsed_ms < fed_ms; elapsed_ms += HEARTBEAT_INTERVAL_MS) {
        if (!bwg_revocation_feed_heartbeat()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

static bool run_heartbeat_loss(int cycle)
{
    ESP_LOGW(TAG, "cycle %d: lease, heartbeats stop after %d ms", cycle, HEARTBEAT_FED_MS);
    if (!bwg_revocation_begin_lease(HEARTBEAT_LEASE_MS)) {
        return false;
    }
    feed_for(HEARTBEAT_FED_MS);
    return wait_for_stopped();
}

static bool run_lease_expiry(int cycle)
{
    ESP_LOGW(TAG, "cycle %d: lease expires at %d ms with heartbeats running", cycle, EXPIRY_LEASE_MS);
    if (!bwg_revocation_begin_lease(EXPIRY_LEASE_MS)) {
        return false;
    }
    feed_for(EXPIRY_LEASE_MS + 5000);
    return wait_for_stopped();
}

static bool run_all(void)
{
    ESP_LOGW(TAG, "handover from local mining");
    bwg_revocation_revoke(BWG_STOP_REASON_LOCAL_HANDOVER);
    if (!wait_for_stopped()) {
        return false;
    }
    for (int cycle = 0; cycle < HEARTBEAT_CYCLES; cycle++) {
        if (!run_heartbeat_loss(cycle)) {
            return false;
        }
    }
    for (int cycle = 0; cycle < EXPIRY_CYCLES; cycle++) {
        if (!run_lease_expiry(HEARTBEAT_CYCLES + cycle)) {
            return false;
        }
    }
    return bwg_revocation_release_to_local();
}

static void proof_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    ESP_LOGW(TAG, "%s", run_all() ? "finished, back to local mining" : "failed, authority left closed");
    vTaskDelete(NULL);
}

void bwg_revocation_proof_start(void)
{
    ESP_LOGW(TAG, "TEST VARIANT: revocation proof is compiled in");
    if (xTaskCreate(proof_task, "bwg_proof", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start");
    }
}
