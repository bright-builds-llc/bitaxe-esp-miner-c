#ifndef BWG_TELEMETRY_CADENCE_H
#define BWG_TELEMETRY_CADENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "bwg_cadence.h"
#include "bwg_json_writer.h"

/** Hooks in the telemetry loop. Each costs a timestamp and a try-lock, with or without a capture. */
int64_t bwg_telemetry_iteration_begin(void);
void bwg_telemetry_iteration_end(int64_t started_at_us);
void bwg_telemetry_note_publication(bwg_cadence_publication_t publication);
void bwg_telemetry_note_send(bool succeeded);

bool bwg_telemetry_cadence_arm(bwg_cadence_phase_t phase, uint32_t generation, bwg_jw_t *writer);
void bwg_telemetry_cadence_write_review(bwg_jw_t *writer);
void bwg_telemetry_cadence_note_max_probe(void);

#endif // BWG_TELEMETRY_CADENCE_H
