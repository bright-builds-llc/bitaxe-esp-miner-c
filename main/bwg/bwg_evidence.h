#ifndef BWG_EVIDENCE_H
#define BWG_EVIDENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bwg_json_writer.h"
#include "global_state.h"

/** Counts this boot and keeps the identity the board announces. Call once, before the link starts. */
void bwg_evidence_start(const char *firmware_commit, GlobalState *GLOBAL_STATE);
void bwg_evidence_set_identity(const char *app_elf_sha256);

uint32_t bwg_evidence_boot_ordinal(void);

/** The next of the lines a host watching a restart waits for. False until the Gate's link is up. */
bool bwg_evidence_format_announcement(char *out, size_t size, uint64_t now_ms);

/** Now and then gives a receipt in place of the next session diagnostic. False otherwise. */
bool bwg_evidence_format_receipt(char *out, size_t size, uint64_t now_ms);

/** Where the board's HTTP status can be read. False with no address a host could use. */
bool bwg_evidence_write_endpoint(bwg_jw_t *writer, uint32_t generation);

/** Safe from the allocator's own failure path: it only stores numbers, in memory that survives the abort. */
void bwg_evidence_note_allocation_failure(size_t requested_bytes, uint32_t capabilities);

#endif // BWG_EVIDENCE_H
