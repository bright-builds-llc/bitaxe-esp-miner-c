#ifndef BWG_LINK_PRESENCE_H
#define BWG_LINK_PRESENCE_H

#include <stdbool.h>
#include <stdint.h>

#define BWG_LINK_QUIET_MS 15000u
// The Gate sends one Hello per Connect and never retries, and a stop can take two minutes to
// cool the chip, so a refused Hello holds local mining off until the next Connect can succeed.
#define BWG_HANDOVER_HOLD_MS 300000u

/** What the serial link has seen of the Gate. Zero-initialized means nothing yet. */
typedef struct {
    bool has_activity;
    bool has_refused_hello;
    uint64_t last_activity_ms;
    uint64_t refused_hello_ms;
} bwg_link_presence_t;

/** An authenticated heartbeat arrived. */
void bwg_link_presence_note_activity(bwg_link_presence_t *presence, uint64_t now_ms);

/** A valid Hello arrived and was admitted or refused. An admitted one ends the handover hold. */
void bwg_link_presence_note_hello(bwg_link_presence_t *presence, uint64_t now_ms, bool admitted);

/** Whether the Gate holds local mining off at `now_ms`. A stamp later than `now_ms` counts as recent. */
bool bwg_link_presence_holds_local(const bwg_link_presence_t *presence, uint64_t now_ms);

#endif // BWG_LINK_PRESENCE_H
