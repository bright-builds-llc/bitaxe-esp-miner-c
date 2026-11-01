#ifndef BWG_CORE_DUMP_H
#define BWG_CORE_DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Keeps the previous boot's store receipt and binds a fresh one to this firmware and boot. Call once. */
void bwg_core_dump_init(uint64_t source_hash, uint64_t boot_ordinal, bool cold_boot);

/** Whether a panic now would leave a receipt, in a partition that can take a dump. */
bool bwg_core_dump_is_ready(void);

void bwg_core_dump_mark_self_test(void);

/** The receipt line for the previous boot or this one. */
bool bwg_core_dump_format_line(char *out, size_t size, bool previous_boot);

#endif // BWG_CORE_DUMP_H
