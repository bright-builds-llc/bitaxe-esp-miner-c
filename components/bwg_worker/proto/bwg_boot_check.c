#include "bwg_boot_check.h"

bwg_boot_verdict_t bwg_boot_check_verdict(const bwg_boot_facts_t *facts)
{
    if (facts->state != BWG_IMAGE_PENDING_VERIFY && facts->state != BWG_IMAGE_NEW) {
        return BWG_BOOT_NOTHING;
    }
    if (!facts->settings_readable) {
        return BWG_BOOT_ROLL_BACK;
    }
    if (facts->link_required && !facts->link_ready) {
        return facts->elapsed_ms >= BWG_BOOT_CHECK_DEADLINE_MS ? BWG_BOOT_ROLL_BACK : BWG_BOOT_WAIT;
    }
    return BWG_BOOT_MARK_VALID;
}
