#ifndef BWG_ONBOARDING_GATE_H
#define BWG_ONBOARDING_GATE_H

#include "bwg_controller.h"
#include "bwg_onboarding.h"

/** Fills the controller's onboarding commands: inspection and post-reboot verify. */
void bwg_onboarding_gate_fill_ops(bwg_controller_ops_t *ops);

/** Reads the partition table, the running slot and its OTA state. False when the running slot is unknown. */
bool bwg_onboarding_partition_facts(void *ctx, bwg_partition_facts_t *facts);

/** Reads one key of NVS namespace `main` as bwg_setting_reader_fn, opening the namespace read-only. */
bwg_setting_read_t bwg_onboarding_read_setting(void *ctx, const char *key, uint8_t *type, uint8_t *value,
                                               size_t capacity, size_t *len);

#endif // BWG_ONBOARDING_GATE_H
