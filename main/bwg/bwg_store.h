#ifndef BWG_STORE_H
#define BWG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_controller.h"

#define BWG_DEVICE_SEED_BYTES 32

/**
 * Durable Gate state in the `bwg_worker` NVS namespace, with the Rust firmware's key names and formats,
 * so a device keeps its identity and its replay state. Every write is read back before it counts.
 */

/** Loads the seed, creating one if none exists. False for a corrupt seed, which is never overwritten. */
bool bwg_store_load_or_create_seed(uint8_t seed[BWG_DEVICE_SEED_BYTES]);

bool bwg_store_high_water_load(const bwg_allocator_t *allocator, const char *kid, bool *present, uint64_t *value);
bwg_store_result_t bwg_store_high_water_compare_and_store(const bwg_allocator_t *allocator, const char *kid, bool has_expected,
                                                          uint64_t expected, uint64_t next);

/** Reads the stored `lease_seq` bytes for a digest. An absent document reads as zero bytes. */
bool bwg_store_high_water_read_raw(uint8_t *text, size_t capacity, size_t *len);

bool bwg_store_effect_get(bwg_effect_state_t *state);
bool bwg_store_effect_set(bwg_effect_state_t state);

/** The crash-loop guard's streak; a device with none reads zero. */
/** Reads a budget ledger. True with `*present` false means the key is absent; false is a key that can't be read. */
bool bwg_store_ledger_load(const char *key, uint8_t *out, size_t capacity, size_t *len, bool *present);
/** Sets and commits a budget ledger. The caller reads it back. */
bool bwg_store_ledger_store(const char *key, const uint8_t *data, size_t len);

bool bwg_store_crash_streak_get(uint16_t *streak);
bool bwg_store_crash_streak_set(uint16_t streak);

#endif // BWG_STORE_H
