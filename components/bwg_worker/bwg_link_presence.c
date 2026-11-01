#include "bwg_link_presence.h"

static bool within(bool has_stamp, uint64_t stamp_ms, uint64_t now_ms, uint64_t window_ms)
{
    return has_stamp && (now_ms <= stamp_ms || now_ms - stamp_ms < window_ms);
}

void bwg_link_presence_note_activity(bwg_link_presence_t *presence, uint64_t now_ms)
{
    presence->has_activity = true;
    presence->last_activity_ms = now_ms;
}

void bwg_link_presence_note_hello(bwg_link_presence_t *presence, uint64_t now_ms, bool admitted)
{
    bwg_link_presence_note_activity(presence, now_ms);
    presence->has_refused_hello = !admitted;
    presence->refused_hello_ms = now_ms;
}

bool bwg_link_presence_holds_local(const bwg_link_presence_t *presence, uint64_t now_ms)
{
    return within(presence->has_activity, presence->last_activity_ms, now_ms, BWG_LINK_QUIET_MS) ||
           within(presence->has_refused_hello, presence->refused_hello_ms, now_ms, BWG_HANDOVER_HOLD_MS);
}
