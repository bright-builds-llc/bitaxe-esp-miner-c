#include "bwg_telemetry_cadence.h"

#include "bwg_gate_ops.h"
#include "bwg_revocation.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "websocket.h"

static bwg_cadence_t s_cadence;

static uint32_t subscribers(void)
{
    return (uint32_t)websocket_get_active_client_count(WS_TYPE_API);
}

/** The loop looks once a pass, so the capture begins within a pass of the lease's first work. */
static void begin_mining_capture_after_first_work(int64_t now_us)
{
    uint32_t generation = 0;
    if (!bwg_cadence_awaits_dispatch(&s_cadence, &generation) || generation != bwg_gate_ops_lease_generation()) {
        return;
    }
    const bwg_revocation_snapshot_t snapshot = bwg_revocation_snapshot();
    if (snapshot.state == BWG_AUTHORITY_LEASED && snapshot.first_work_ms != 0) {
        bwg_cadence_note_dispatch(&s_cadence, generation, (uint64_t)now_us, subscribers());
    }
}

int64_t bwg_telemetry_iteration_begin(void)
{
    const int64_t now_us = esp_timer_get_time();
    begin_mining_capture_after_first_work(now_us);
    return now_us;
}

void bwg_telemetry_iteration_end(int64_t started_at_us)
{
    const bwg_cadence_iteration_t iteration = {
        .started_at_us = (uint64_t)started_at_us,
        .finished_at_us = (uint64_t)esp_timer_get_time(),
        .subscribers = subscribers(),
        .cpu = (uint32_t)xPortGetCoreID(),
        .priority = (uint32_t)uxTaskPriorityGet(NULL),
    };
    bwg_cadence_note_iteration(&s_cadence, &iteration);
}

void bwg_telemetry_note_publication(bwg_cadence_publication_t publication)
{
    bwg_cadence_note_publication(&s_cadence, publication);
}

void bwg_telemetry_note_send(bool succeeded)
{
    bwg_cadence_note_send(&s_cadence, succeeded);
}

bool bwg_telemetry_cadence_arm(bwg_cadence_phase_t phase, uint32_t generation, bwg_jw_t *writer)
{
    return bwg_cadence_arm(&s_cadence, phase, (uint64_t)esp_timer_get_time(), generation, subscribers(), writer);
}

void bwg_telemetry_cadence_write_review(bwg_jw_t *writer)
{
    bwg_cadence_write_review(&s_cadence, writer);
}

void bwg_telemetry_cadence_note_max_probe(void)
{
    bwg_cadence_note_max_probe(&s_cadence, (uint64_t)esp_timer_get_time());
}
