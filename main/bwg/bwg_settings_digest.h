#ifndef BWG_SETTINGS_DIGEST_H
#define BWG_SETTINGS_DIGEST_H

#include <stdint.h>

#include "sodium.h"

/**
 * Digests the operator's public settings, the ones a stop or a lease must leave alone.
 * Reads no network, pool, host or scoreboard value, so the digest says nothing about them.
 */
void bwg_settings_digest(uint8_t digest[crypto_hash_sha256_BYTES]);

#endif // BWG_SETTINGS_DIGEST_H
